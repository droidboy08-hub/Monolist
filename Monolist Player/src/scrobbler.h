#pragma once

#include "lastfm.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <functional>

class Library;
class PlaybackController;

// Last.fm, from the Settings row to the last accepted scrobble.
//
// Connecting uses Last.fm's desktop sign-in: a token from auth.getToken, the
// approval page opened in the user's own browser, and auth.getSession asked
// until the approval has happened. Nobody tells the app when that is, so it
// asks when the window becomes active again (the user is back from the
// browser), every 3 seconds for the first 10 minutes, and whenever "I've
// approved it" is pressed. The password is only ever typed into last.fm; the
// session key that comes back goes to SecretStore, never to the database, and
// only the user name is kept in the settings table.
//
// Scrobbling is a queue. A listen that qualifies (PlaybackController, by
// ListenTracker's rule) becomes a row of scrobble_queue at once, and rows
// leave only when Last.fm has answered for them, oldest first, up to 50 a
// request, one request at a time. What an answer means decides the rest:
//
//   accepted, or ignored 1-4       the row goes (1-4 will never be accepted)
//   ignored 5, the daily limit     kept; nothing more is sent until tomorrow
//   no answer, 5xx, 11, 16         kept; tried again after 30 s, doubling to 30 min
//   29, too many requests          kept; a quarter of an hour's pause
//   9, the session was revoked     kept; the key is deleted and the row says Reconnect
//   10, 13, 26, the key refused    kept; paused, since dropping on these is how
//                                  other scrobblers have lost whole queues
//   6, 8 and the rest on a batch   each item sent again alone, once; only an
//                                  item refused on its own is dropped
//
// Recording goes on while the session has expired, so nothing heard is lost
// before the user reconnects; each row carries the account it was heard
// under, so a backlog is never sent to someone else. "Now playing" is sent
// when a listen starts, and again after a long pause, and is never queued.
//
// Exposed to QML as the "Scrobbler" singleton.
class Scrobbler : public QObject
{
    Q_OBJECT
    // off | waiting | connected | expired | error | unavailable
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    // Where things stand, in a sentence; may hold a link.
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY changed)
    // The Scrobble switch: off, nothing is sent or recorded.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    // Scrobbles waiting for this account.
    Q_PROPERTY(int pending READ pending NOTIFY changed)

public:
    enum class State { Unavailable, Off, Waiting, Connected, Expired, Error };
    enum class Pause { None, Backoff, RateLimit, DailyLimit, Hold };

    // How long things wait. These are the real values; only a self-test
    // changes them.
    struct Timing {
        int pollMs = 3000;
        int pollForMs = 10 * 60 * 1000;
        qint64 tokenLifeMs = 60 * 60 * 1000;   // Last.fm's tokens last an hour
        int afterEnqueueMs = 5000;
        int afterLaunchMs = 20000;
        int backoffMinMs = 30 * 1000;
        int backoffMaxMs = 30 * 60 * 1000;
        int rateLimitMs = 15 * 60 * 1000;
        int holdMs = 30 * 60 * 1000;
    };

    // The queue is capped; beyond it the oldest rows go. Last.fm refuses
    // scrobbles more than a couple of weeks old anyway.
    static constexpr int kMaxQueued = 10000;

    Scrobbler(LastFmApi *api, Library *library, QObject *parent = nullptr);

    void setPlayer(PlaybackController *player);
    // Where things stood at the last run: connected, expired or off. Then
    // the first send, 20 seconds in, and a send whenever the network comes
    // back.
    void start();

    QString state() const;
    State stateValue() const { return m_state; }
    QString accountName() const { return m_user; }
    QString statusLine() const { return m_statusLine; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    int pending() const { return m_pending; }

    Q_INVOKABLE void connectAccount();
    Q_INVOKABLE void cancelConnect();
    // "I've approved it": ask for the session now.
    Q_INVOKABLE void checkApproval();
    // Deletes the session key; the account itself is managed at last.fm.
    Q_INVOKABLE void disconnectAccount();

    // The artist Last.fm is sent for a track: the first credit alone where
    // it is known, and never YouTube's " - Topic" channel suffix.
    static QString scrobbleArtist(const QVariantMap &track);

    // — for the self-tests —
    void setTiming(const Timing &timing);
    // In place of opening the browser; returns whether it "opened".
    void setUrlOpener(std::function<bool(const QUrl &)> opener);
    // Connected as `user` with `key`, in memory only: nothing is stored.
    void useTestSession(const QString &user, const QByteArray &key);
    Pause pause() const { return m_pause; }
    qint64 pausedUntil() const { return m_pausedUntil; }   // ms since the epoch
    static QString pauseName(Pause pause);

public Q_SLOTS:
    void listenStarted(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    void listenQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    void listenResumed(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    // Sends the next batch, if one may go now.
    void flush();

Q_SIGNALS:
    void changed();
    // A track.scrobble request was answered: how many it carried, and the
    // outcome (LastFmApi::outcomeName).
    void batchAnswered(int items, const QString &outcome);
    void nowPlayingAnswered(const QString &outcome);

private:
    enum class Wait { Asking, Approve, NotYet, StillWaiting, NoBrowser };

    void setState(State state);
    void updateStatus();
    void refreshPending();
    State restingState() const;

    // Signing in.
    void tokenArrived(const LastFmApi::Reply &reply);
    void pollSession(bool manual);
    void sessionAnswered(const LastFmApi::Reply &reply, bool manual);
    void finishConnecting(const QString &user, const QByteArray &key);
    void failConnecting(const QString &message);
    void stopWaiting();
    static QString connectError(const LastFmApi::Reply &reply);

    // Recording and sending.
    bool recording() const;
    bool canSend() const;
    void enqueue(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    void sendNowPlaying(const QVariantMap &track);
    void handleBatch(const QList<qint64> &ids, const LastFmApi::Reply &reply);
    void markAttempt(const QList<qint64> &ids, const QString &error);
    void removeRows(const QList<qint64> &ids);
    void pauseFor(Pause pause, qint64 ms);
    void scheduleFlush(qint64 ms);
    qint64 backoffMs() const;
    void expire();
    void watchNetwork();

    LastFmApi *m_api = nullptr;
    Library *m_library = nullptr;
    Timing m_timing;
    std::function<bool(const QUrl &)> m_openUrl;

    State m_state = State::Off;
    QString m_user;
    QByteArray m_sessionKey;   // never logged, never in the database
    bool m_enabled = true;
    int m_pending = 0;
    QString m_statusLine;
    QString m_error;           // what the row says in the error state
    QString m_notice;          // what it says when off, after a disconnect

    // The sign-in under way.
    QString m_token;           // never logged
    QUrl m_authUrl;            // holds the key and token: never logged
    qint64 m_tokenAt = 0;
    Wait m_wait = Wait::Asking;
    QTimer m_pollTimer;
    bool m_sessionInFlight = false;
    bool m_manualCheck = false;
    quint64 m_connectGeneration = 0;

    // The sending.
    QTimer m_flushTimer;
    bool m_inFlight = false;
    quint64 m_sendGeneration = 0;
    Pause m_pause = Pause::None;
    qint64 m_pausedUntil = 0;
    int m_failures = 0;
    int m_holdError = 0;
    // A batch Last.fm refused as a whole, sent again an item at a time.
    QList<qint64> m_singles;
    bool m_watchingNetwork = false;
};
