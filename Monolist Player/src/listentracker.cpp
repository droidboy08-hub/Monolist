#include "listentracker.h"

#include <QDateTime>

namespace {

std::function<qint64()> &clockOverride()
{
    static std::function<qint64()> instance;
    return instance;
}

}

void ListenTracker::setClock(std::function<qint64()> clock)
{
    clockOverride() = std::move(clock);
}

qint64 ListenTracker::now()
{
    const std::function<qint64()> &clock = clockOverride();
    return clock ? clock() : QDateTime::currentMSecsSinceEpoch();
}

qint64 ListenTracker::thresholdMs(qint64 durationMs)
{
    if (durationMs <= kMinimumTrackMs)
        return -1;
    return qMin(durationMs / 2, kEnoughMs);
}

ListenTracker::ListenTracker(QObject *parent)
    : QObject(parent)
{
}

void ListenTracker::begin(const QVariantMap &track, bool chosenByUser)
{
    m_track = track;
    m_chosenByUser = chosenByUser;
    m_active = !track.isEmpty();
    m_durationMs = track.value(QStringLiteral("durationMs")).toLongLong();
    m_lastPositionMs = 0;
    m_heardMs = 0;
    m_started = false;
    m_qualified = false;
    m_startedAt = 0;
    m_pausedSince = 0;
}

void ListenTracker::restart()
{
    // The length the engine reported is kept: it says it once per file, and
    // a restart is a seek, not a new file. Without it, a song that came
    // with no length (a card on Home) could never count the second time.
    const qint64 known = m_durationMs;
    begin(m_track, m_chosenByUser);
    if (known > 0)
        m_durationMs = known;
}

void ListenTracker::setPlaying(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    if (!playing) {
        m_pausedSince = now();
        return;
    }
    const qint64 pausedFor = m_pausedSince > 0 ? now() - m_pausedSince : 0;
    m_pausedSince = 0;
    if (m_active && m_started && pausedFor > kLongPauseMs)
        Q_EMIT listenResumed(current(), m_startedAt, m_chosenByUser);
}

void ListenTracker::setBuffering(bool buffering)
{
    m_buffering = buffering;
}

void ListenTracker::setDuration(qint64 ms)
{
    if (ms <= 0 || ms == m_durationMs)
        return;
    m_durationMs = ms;
    // A length learned late (a search result carries none until the stream
    // opens) can make what was already heard enough.
    checkQualified();
}

void ListenTracker::positionChanged(qint64 ms)
{
    const qint64 step = ms - m_lastPositionMs;
    m_lastPositionMs = ms;
    if (!m_active || !m_playing || m_buffering || step <= 0 || step > kMaxStepMs)
        return;

    m_heardMs += step;
    if (!m_started) {
        m_started = true;
        // When it began to be heard: now, less what this step already heard.
        m_startedAt = (now() - m_heardMs) / 1000;
        Q_EMIT listenStarted(current(), m_startedAt, m_chosenByUser);
    }
    checkQualified();
}

void ListenTracker::seeked(qint64 ms)
{
    m_lastPositionMs = ms;
}

QVariantMap ListenTracker::current() const
{
    QVariantMap track = m_track;
    if (m_durationMs > 0)
        track.insert(QStringLiteral("durationMs"), m_durationMs);
    return track;
}

void ListenTracker::checkQualified()
{
    if (!m_active || !m_started || m_qualified)
        return;
    const qint64 needed = thresholdMs(m_durationMs);
    if (needed < 0 || m_heardMs < needed)
        return;
    m_qualified = true;
    Q_EMIT listenQualified(current(), m_startedAt, m_chosenByUser);
}
