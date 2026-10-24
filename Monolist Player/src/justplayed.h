#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

#include "innertube.h"
#include "rec/sessionrows.h"

class ArtistLinks;
class Library;

// Home's first rows, from the songs just heard: "More like <song>" for the
// newest, for one of another kind and for the taste the page should not
// lose, and "Based on your recent listening" between them (rec/sessionrows.h
// composes them).
//
// Each row's songs are YouTube Music's own radio for one song, asked signed
// out and without cookies on an InnerTube of this object's own (no slot, so
// autoplay's radio is never cancelled; Auth::Anonymous, so AccountGuard and
// the account are never reached). Autoplay's own answers are kept too, free.
// What is asked, and when:
//
//  - a song the user chose, once it has been heard for min(45 s, the
//    scrobble threshold) (listenSettled); one autoplay added, once it counts
//    (listenQualified). A skip is never a seed and sends nothing;
//  - only when its radio is not kept, or (for the newest) kept more than 3
//    days or for another country; an empty or failed answer is not asked
//    again for 3 days;
//  - one at a time, 3 s after the seed (a newer seed going first), 20 s and
//    a random 0-5 s after the last answer (one dropped as stale too), 30 in
//    any hour and 200 in any day, counted back from now across restarts
//    (home.justPlayed.pace); at most 3 waiting, the newest first, the rest
//    dropped;
//  - never again at once after a failure: a rest of 15 min, then 1 h, then
//    3 h, and 1 h after three empty answers in a row; the rows stay as kept.
//    A 400 is a failure too: the country is not dropped for it;
//  - nothing while an import runs, while the account rests, while the window
//    is hidden, minimised or in the tray, or once quitting;
//  - at launch, nothing for 15 s, then at most 3 for seeds with nothing kept.
//
// Home visits are local: visit() draws the rows again from what is kept.
// Switched off (home.justPlayed = 0) it records, asks and keeps nothing.
//
// Exposed to QML as the "JustPlayed" singleton.
class JustPlayed : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // [{ key, kind, title, seedVideoId, items: [cards] }], in reading order
    // (s1, mix, s2, s3); a card is Home's quick-pick card shape, with
    // suggestion: true, rowKey and rowIndex.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(bool hasRows READ hasRows NOTIFY rowsChanged)
    // Who Home's More like <artist> should follow: [{ name, browseId }],
    // only artists' own pages, the head changing at most once in 15 min.
    Q_PROPERTY(QVariantList moreLikeCandidates READ moreLikeCandidates NOTIFY rowsChanged)
    // The window is on screen (not hidden, minimised or in the tray).
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged)
public:
    struct Timing {
        qint64 launchDelayMs = 15000;
        qint64 debounceMs = 3000;
        qint64 gapMs = 20000;
        qint64 jitterMs = 5000;
        int hourCap = 30;
        int dayCap = 200;
        // After the first, second and third (or later) failure in a row.
        QList<qint64> restsMs{ 15 * 60 * 1000, 60 * 60 * 1000, 180 * 60 * 1000 };
        qint64 emptyStreakRestMs = 60 * 60 * 1000;
        qint64 freshS1Secs = 3 * 24 * 3600;
        qint64 displaySecs = 14 * 24 * 3600;
        // The longest the pacer sleeps before it looks at the clock again.
        qint64 recheckMs = 60000;
    };

    explicit JustPlayed(QObject *parent = nullptr);
    ~JustPlayed() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QVariantList rows() const { return m_rows; }
    bool hasRows() const { return !m_rows.isEmpty(); }
    QVariantList moreLikeCandidates() const { return m_candidates; }
    bool watching() const { return m_watching; }
    void setWatching(bool on);

    // A Home visit: the rows drawn again, from what is kept. Nothing is asked.
    Q_INVOKABLE void visit();

    // What the rows leave out (Recommender::unwanted).
    void setFilter(std::function<bool(const QString &videoId, const QString &title, const QString &artist)> filter);
    void setHideExplicit(bool hide);
    // A playlist import is running.
    void setPaused(bool paused);
    // AccountGuard is resting: YouTube has this address in its bad books.
    void setAccountResting(bool resting);
    // Which names are artists' own pages, for moreLikeCandidates.
    void setArtists(ArtistLinks *artists) { m_artists = artists; }
    void setTiming(const Timing &timing) { m_timing = timing; }
    // Milliseconds since the epoch, UTC; swappable for a self-test.
    static void setClock(std::function<qint64()> clock);
    static qint64 now();

    // The setting, the pace and the seeds, once the database is open; then,
    // after the launch delay, what is missing for the first rows.
    void load();
    // Quitting: nothing more is asked, written or answered.
    void shutdown();

    // For the self-test.
    QVector<Rec::SessionSeed> seeds() const { return m_seeds; }
    QStringList queued() const { return m_queue; }
    bool inFlight() const { return !m_inFlight.isEmpty(); }
    bool timersActive() const { return m_timer.isActive() || m_launchTimer.isActive(); }

public Q_SLOTS:
    // A song the user chose has settled (only then); one autoplay added has
    // qualified (only then).
    void noteSettled(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    void noteQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    // Autoplay's radio for `seed`, kept as if asked.
    void noteRadio(const QString &seed, const QList<InnerTube::Track> &tracks);
    // The plays, likes and Recently played read again (a listen closed, the
    // history edited or cleared).
    void reloadSeeds();
    // The rows composed again (a turn-down, the explicit filter).
    void recompose();
    void onRegionChanged();

Q_SIGNALS:
    void rowsChanged();
    void enabledChanged();
    void watchingChanged();

private:
    struct Radio {
        QVector<Rec::RadioTrack> tracks;
        QString region;
        qint64 fetchedAt = 0;    // UTC seconds
        bool ok = false;
    };
    // The pacer's counts, kept in home.justPlayed.pace (UTC seconds).
    struct Pace {
        QVector<qint64> sent;    // each look-up of the last day, oldest first
        qint64 restUntil = 0;
        int failStreak = 0;
        int emptyStreak = 0;
    };

    void noteSeed(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    // m_seeds again from the live seed and the plays read.
    void mergeSeeds();
    bool suitableSeed(const Rec::SessionSeed &seed) const;
    // What rec_radio keeps for these ids, read into m_radios once.
    void ensureRadios(const QStringList &ids);
    void storeRadio(const QString &videoId, const QList<InnerTube::Track> &tracks, bool ok);
    // The seed S1 would take first: the live one, else the newest play.
    QString newestSeed() const;
    // Whether a seed's radio should be asked for now.
    bool wantsFetch(const QString &videoId, bool newest);
    void enqueue(const QString &videoId);
    void pump();
    void send(const QString &videoId);
    void answered(const QString &videoId, const QList<InnerTube::Track> &tracks, const QString &error);
    // An answer of an older generation, not used: only paced, and rested on.
    void dropped(const QString &error);
    // The next look-up no sooner than the gap (and the jitter) from now.
    void spaceNext();
    // One failure more in a row, and its rest.
    void failed(const QString &error);
    void rest(qint64 ms, const QString &reason);
    void pruneSent(qint64 nowSecs);
    qint64 capWaitMs(qint64 atMs, qint64 windowSecs, int cap) const;
    void backfill();
    void loadPace();
    void savePace() const;
    QString seedTitle(const QString &videoId) const;
    QString setting(const QString &key) const;
    void storeSetting(const QString &key, const QString &value) const;
    void logOverlaps(const QString &videoId);

    InnerTube m_innerTube{ this, /*warmUp=*/false };
    ArtistLinks *m_artists = nullptr;
    std::function<bool(const QString &, const QString &, const QString &)> m_filter;
    Timing m_timing;
    bool m_enabled = false;
    bool m_loaded = false;
    bool m_hideExplicit = false;
    bool m_paused = false;
    bool m_accountResting = false;
    bool m_watching = true;
    bool m_quitting = false;
    bool m_jarMode = false;

    Rec::SessionSeed m_live;
    bool m_hasLive = false;
    qint64 m_liveStartedAt = 0;          // UTC seconds, as ListenTracker said
    QVector<Rec::SessionSeed> m_plays;   // the qualifying plays read, newest first
    QVector<Rec::SessionSeed> m_seeds;   // newest first, at most 60
    QSet<QString> m_recent;              // Recently played
    QSet<QString> m_recentHeard;         // the part of it heard in the last 48 h
    QHash<QString, Radio> m_radios;
    QSet<QString> m_radioAbsent;         // ids rec_radio was asked for and has not

    QStringList m_queue;                 // newest first, at most 3
    QString m_inFlight;
    quint64 m_generation = 0;
    Pace m_pace;
    qint64 m_launchUntil = 0;            // ms
    qint64 m_debounceUntil = 0;
    qint64 m_nextAllowedAt = 0;
    QTimer m_timer;
    QTimer m_launchTimer;

    int m_visit = 0;
    QVariantList m_rows;
    QVariantList m_candidates;
    QString m_signature;
    QString m_headKey;                   // moreLikeCandidates' head, by leadKey
    qint64 m_headChangedAt = 0;          // ms
};

// --just-played-probe <id>...: YouTube Music's radio for each (at most 5),
// asked as JustPlayed asks, signed out and cookieless (home.justPlayed.jar =
// 1: the shared anonymous jar), 20 s apart; prints each one's songs, how many
// the rows could use and how many lead artists. In MONOLIST_DATA_DIR only.
int runJustPlayedProbe(Library *library, const QStringList &videoIds);
