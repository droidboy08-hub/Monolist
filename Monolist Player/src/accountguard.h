#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

// How much Monolist may ask of YouTube with the signed-in account, and when
// it must stop asking altogether.
//
// Signing in ties what the app sends to a real Google account, and the
// owner's rule is that this must never put the account at risk. What is known
// of how accounts get into trouble with outside clients (yt-dlp's wiki and
// issues, Metrolist, OuterTune, Koda; docs/research/account-safety.md) comes
// down to volume, bursts and pushing on after YouTube has said to slow down.
// So every call that carries the account comes through here first, whoever
// makes it: YtmSession's check, Home's feed, the library import, the listen
// reports, and yt-dlp's lookups with the account's cookies.
//
//  - One at a time. A call waits while another one with the account is out,
//    so the account never has two requests in flight from this computer.
//  - Spaced. At least `gapMs` plus a random share of `jitterMs` between the
//    start of one and the next, and a bucket of `burst` calls that fills
//    again one every `refillMs`: a few may go close together, a long run
//    settles to about fifteen a minute.
//  - Counted. At most `perHour` and `perDay` of them, every kind together,
//    and tighter shares for listen reports and yt-dlp's lookups. The counts
//    are kept in the settings table, so a restart does not start them afresh.
//  - Stopped. When YouTube says to slow down (429, a bot check, "unusual
//    traffic", a 403 on a call it had answered before), every use of the
//    account pauses: an hour the first time, three hours the second within a
//    day, a day the third, or longer when YouTube names a longer wait. The
//    app carries on signed out meanwhile, and says so.
//
// The limits are conservative on purpose, a few per cent of the ~4000
// requests an hour yt-dlp's wiki gives as where YouTube throttles an
// account. None of them is ever relaxed by a retry: a call refused here is
// not asked again sooner.
class AccountGuard : public QObject
{
    Q_OBJECT
public:
    // What a call is, for the limits kept per kind and for the log.
    enum class Kind { Check, Browse, Listen, YtDlp };

    struct Limits {
        int burst = 6;
        int refillMs = 4000;
        int gapMs = 1500;
        int jitterMs = 1000;
        int perHour = 150;
        int perDay = 800;
        int listensPerHour = 40;
        int listensPerDay = 400;
        // A lookup is several requests to www.youtube.com (the watch page,
        // /player per client, the player's script), so it counts as several.
        int ytdlpWeight = 4;
        int ytdlpGapMs = 20 * 1000;
        int ytdlpPerHour = 15;
        int ytdlpPerDay = 60;
        // A call not heard back from in this long gives up its turn: a reply
        // Qt never finishes must not hold the account for good.
        int holdMs = 45 * 1000;
        qint64 firstPauseMs = 60LL * 60 * 1000;
        qint64 secondPauseMs = 3LL * 60 * 60 * 1000;
        qint64 thirdPauseMs = 24LL * 60 * 60 * 1000;
        // How far back earlier pauses count when choosing the next one.
        qint64 tripMemoryMs = 24LL * 60 * 60 * 1000;
    };

    // Where the counts and the pause are kept between launches: the settings
    // table (nothing in them is secret: times, kinds and a reason).
    struct Store {
        std::function<QString(const QString &key)> read;
        std::function<void(const QString &key, const QString &value)> write;
    };

    explicit AccountGuard(QObject *parent = nullptr);
    ~AccountGuard() override;

    void setLimits(const Limits &limits);
    const Limits &limits() const { return m_limits; }
    // Reads what an earlier run kept, and keeps what happens from now on.
    void setStore(Store store);

    // Milliseconds since the epoch. Swappable so a self-test can let an hour
    // pass in an instant; nothing else changes it.
    static void setClock(std::function<qint64()> clock);
    static qint64 now();

    // Asked just before a call of `kind` would go with the account. 0: it
    // may go now (and must then be reported with started()). A positive
    // number: wait that many milliseconds and ask again. -1: not now at all,
    // with `why` saying why in words for the log and the interface.
    qint64 admit(Kind kind, QString *why = nullptr);
    // The call left with the account: it holds the turn until finished(),
    // and it counts.
    void started(Kind kind);
    // It is over: its HTTP status (0 for no answer) and, where the answer
    // named one, how long YouTube asked to be left alone. A 429 pauses
    // everything; anything else only frees the turn.
    void finished(Kind kind, int status, qint64 retryAfterSecs = 0);
    // YouTube said to slow down in some other way (a bot check, "unusual
    // traffic", a 403): everything with the account pauses.
    void trip(const QString &why, qint64 retryAfterMs = 0);

    // For the self-tests only: every rest and every count forgotten, as if
    // the account had never been used.
    void forgive();

    bool paused() const;
    qint64 pausedUntil() const { return m_pausedUntil; }
    QString pauseReason() const { return m_pauseWhy; }
    // How the budget stands, for the self-test and the log: calls counted
    // (by weight) in the last hour and the last day, all kinds or one.
    int usedLastHour() const;
    int usedLastDay() const;
    int countLastHour(Kind kind) const;
    int countLastDay(Kind kind) const;
    bool busy() const;

    static const char *name(Kind kind);

Q_SIGNALS:
    // A pause began or ended.
    void pausedChanged();

private:
    struct Use {
        qint64 at = 0;
        Kind kind = Kind::Browse;
        int weight = 1;
    };

    int weightOf(Kind kind) const;
    int sum(qint64 since, bool anyKind, Kind kind) const;
    void prune();
    void refill();
    void load();
    void save();
    void saveSoon();
    void armResume();
    void resumed();

    Limits m_limits;
    Store m_store;
    QList<Use> m_uses;           // the last day's, oldest first
    QList<qint64> m_trips;       // when pauses began, within tripMemoryMs
    qint64 m_pausedUntil = 0;
    QString m_pauseWhy;
    double m_tokens = 0;
    qint64 m_refilledAt = 0;
    qint64 m_lastStart = 0;
    qint64 m_lastYtDlp = 0;
    int m_nextJitter = 0;
    bool m_inFlight = false;
    qint64 m_inFlightSince = 0;
    Kind m_inFlightKind = Kind::Browse;
    QTimer m_resumeTimer;
    QTimer m_saveTimer;
};
