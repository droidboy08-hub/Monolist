#pragma once

#include <QObject>
#include <QVariantMap>

#include <functional>

// When a listen counts, for Last.fm: how much of a track was actually heard.
//
// The playhead cannot answer that. play_events.listened_ms is where the track
// was left, so seeking to the last second reads as a whole listen, and a song
// paused half-way for an hour reads as heard for an hour. Last.fm's rule is
// about time spent listening (half the track or four minutes, whichever comes
// first, for anything longer than 30 seconds: last.fm/api/scrobbling), so this
// adds up the steps of the clock that were real playing, and nothing else:
//
//  - only while playing, and not while waiting on the network (buffering);
//  - only forward steps of at most 2.5 seconds. The engine reports the clock
//    several times a second, so a larger step is a jump, not listening: a
//    seek, or the picture's stream opening a moment behind the sound's.
//  - a seek from the interface moves the playhead here first (seeked()), so
//    the jump itself is never a step at all.
//
// PlaybackController owns one and feeds it what the engine reports; a listen
// begins with each track it begins, including the same one again under
// repeat-one. The listen *starts* at its first heard step rather than when the
// track was asked for: a track that never resolves, or the one a launch opens
// on and nobody plays, was never heard. It qualifies once, at the step that
// crosses the threshold.
class ListenTracker : public QObject
{
    Q_OBJECT
public:
    static constexpr qint64 kMinimumTrackMs = 30000;   // this long or shorter never counts
    static constexpr qint64 kEnoughMs = 240000;        // four minutes always does
    static constexpr qint64 kMaxStepMs = 2500;
    // After a pause this long, Last.fm's "now playing" has long lapsed and is
    // worth saying again when the music resumes.
    static constexpr qint64 kLongPauseMs = 5 * 60 * 1000;

    // What has to be heard of a track this long, or -1 when it never counts.
    // An unknown length (0) never counts until it is known.
    static qint64 thresholdMs(qint64 durationMs);

    explicit ListenTracker(QObject *parent = nullptr);

    // A new listen of `track`, which may or may not be heard. `chosenByUser`
    // is false for what autoplay picked.
    void begin(const QVariantMap &track, bool chosenByUser);
    // The same track again from the start, as a listen of its own (Previous).
    void restart();

    void setPlaying(bool playing);
    void setBuffering(bool buffering);
    void setDuration(qint64 ms);
    // A report of the clock from the engine.
    void positionChanged(qint64 ms);
    // The playhead moved because someone moved it: not heard.
    void seeked(qint64 ms);

    qint64 heardMs() const { return m_heardMs; }
    bool started() const { return m_started; }
    bool qualified() const { return m_qualified; }

    // Milliseconds since the epoch, UTC. Swappable so a self-test can let ten
    // minutes pass in an instant; nothing else changes it.
    static void setClock(std::function<qint64()> clock);
    static qint64 now();

Q_SIGNALS:
    // The track as it began, with "durationMs" brought up to date when the
    // engine knows better; `startedAt` is when it started, in UTC seconds.
    void listenStarted(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    void listenQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser);
    // Playing again after a long pause.
    void listenResumed(const QVariantMap &track, qint64 startedAt, bool chosenByUser);

private:
    QVariantMap current() const;
    void checkQualified();

    QVariantMap m_track;
    bool m_chosenByUser = true;
    bool m_active = false;       // a track has begun
    bool m_playing = false;
    bool m_buffering = false;
    qint64 m_durationMs = 0;
    qint64 m_lastPositionMs = 0;
    qint64 m_heardMs = 0;
    bool m_started = false;
    bool m_qualified = false;
    qint64 m_startedAt = 0;      // UTC seconds
    qint64 m_pausedSince = 0;    // wall clock ms; 0 while playing
};
