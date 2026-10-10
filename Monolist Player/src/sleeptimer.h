#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

// The sleep timer (ROADMAP P05; the owner's, 2026-10-10): 15, 30, 45 or 60
// minutes, or the end of the song playing, with +10 minutes while it runs.
// When the time is up the music fades out over ten seconds and pauses, and
// the volume is put back as it was, so the next play is not silent. "End of
// this song" lets the song finish and leaves the next one ready, not playing
// (PlaybackController's stopAfterCurrent). Never kept across a restart: a
// timer set last night has no business pausing anything today.
//
// It drives the player only through its properties and slots — "volume",
// "playing", "stopAfterCurrent", pause() — so the self-test can give it a
// stand-in player with no mpv at all.
//
// Exposed to QML as the "SleepTimer" singleton.
class SleepTimer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // Waiting for the song to end rather than for the clock.
    Q_PROPERTY(bool endOfSong READ endOfSong NOTIFY changed)
    // Seconds left on the clock (0 for the end of the song).
    Q_PROPERTY(int remainingSeconds READ remainingSeconds NOTIFY tick)
    // "12:04", or "End of this song".
    Q_PROPERTY(QString remainingText READ remainingText NOTIFY tick)
    // The last ten seconds, the music going down.
    Q_PROPERTY(bool fading READ fading NOTIFY changed)

public:
    explicit SleepTimer(QObject *player, QObject *parent = nullptr);

    bool active() const { return m_active; }
    bool endOfSong() const { return m_endOfSong; }
    int remainingSeconds() const;
    QString remainingText() const;
    bool fading() const { return m_fading; }

    Q_INVOKABLE void start(int minutes);
    Q_INVOKABLE void startEndOfSong();
    // More time on a clock that is running; from the end of the song, the
    // clock starts from now.
    Q_INVOKABLE void extend(int minutes);
    Q_INVOKABLE void cancel();

    // For the self-test: the clock in milliseconds, and the fade's length.
    void startMs(qint64 ms);
    void setFadeMs(int ms) { m_fadeMs = ms; }

Q_SIGNALS:
    void changed();
    void tick();
    // The time is up and the music paused (or the song ended): for a word
    // from the app ("Sleep timer: paused").
    void finished();

private Q_SLOTS:
    // The player's stoppedAfterCurrent(): the song waited for has ended.
    void songEnded();

private:
    void reset();
    void expire();
    void fadeStep();
    void endFade(bool pausePlayer);

    QPointer<QObject> m_player;
    bool m_active = false;
    bool m_endOfSong = false;
    bool m_fading = false;
    qint64 m_durationMs = 0;
    QElapsedTimer m_clock;
    QTimer m_ticker;
    QTimer m_fader;
    QElapsedTimer m_fadeClock;
    int m_fadeMs = 10000;
    qreal m_volumeBefore = 1.0;
};
