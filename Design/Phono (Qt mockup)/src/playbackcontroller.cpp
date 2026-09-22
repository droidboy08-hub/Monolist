#include "playbackcontroller.h"
#include "trackmodel.h"
#include "appdatabase.h"

#include <QRandomGenerator>
#include <QSqlQuery>
#include <QVariant>

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
    m_clock.setInterval(200);
    connect(&m_clock, &QTimer::timeout, this, &PlaybackController::tick);
}

void PlaybackController::setQueue(TrackModel *model)
{
    m_queue = model;
}

qreal PlaybackController::progress() const
{
    return m_duration > 0 ? qreal(m_position) / qreal(m_duration) : 0.0;
}

QString PlaybackController::positionText() const
{
    return TrackModel::formatDuration(m_position);
}

QString PlaybackController::durationText() const
{
    return TrackModel::formatDuration(m_duration);
}

void PlaybackController::play()
{
    if (m_index < 0)
        loadIndex(0);
    if (m_playing)
        return;
    m_playing = true;
    m_clock.start();
    Q_EMIT playingChanged();
}

void PlaybackController::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_clock.stop();
    Q_EMIT playingChanged();
}

void PlaybackController::togglePlay()
{
    m_playing ? pause() : play();
}

void PlaybackController::loadIndex(int index)
{
    if (!m_queue)
        return;
    const int count = m_queue->rowCount();
    if (count == 0 || index < 0 || index >= count)
        return;

    m_index = index;
    m_currentTrack = m_queue->get(index);
    m_favourite = false;
    m_position = 0;
    setDuration(m_currentTrack.value(QStringLiteral("durationMs")).toLongLong());

    QSqlQuery history(AppDatabase::connection());
    history.prepare(QStringLiteral("INSERT INTO history (track_id) VALUES (?)"));
    history.addBindValue(m_currentTrack.value(QStringLiteral("trackId")).toInt());
    history.exec();

    Q_EMIT currentTrackChanged();
    Q_EMIT favouriteChanged();
    Q_EMIT positionChanged();
}

void PlaybackController::playIndex(int index)
{
    loadIndex(index);
    play();
}

void PlaybackController::next()
{
    if (!m_queue || m_queue->rowCount() == 0)
        return;
    const int count = m_queue->rowCount();
    int target = m_index + 1;
    if (m_shuffle && count > 1) {
        do {
            target = QRandomGenerator::global()->bounded(count);
        } while (target == m_index);
    } else if (target >= count) {
        target = m_repeatMode == RepeatOff ? count - 1 : 0;
    }
    const bool wasPlaying = m_playing;
    loadIndex(target);
    if (wasPlaying)
        play();
}

void PlaybackController::previous()
{
    if (!m_queue)
        return;
    if (m_position > 3000) {
        setPosition(0);
        return;
    }
    const bool wasPlaying = m_playing;
    loadIndex(m_index > 0 ? m_index - 1 : 0);
    if (wasPlaying)
        play();
}

void PlaybackController::setPosition(qint64 ms)
{
    const qint64 clamped = qBound<qint64>(0, ms, m_duration);
    if (clamped == m_position)
        return;
    m_position = clamped;
    Q_EMIT positionChanged();
}

void PlaybackController::seekFraction(qreal fraction)
{
    setPosition(qint64(qBound(0.0, fraction, 1.0) * qreal(m_duration)));
}

void PlaybackController::setDuration(qint64 ms)
{
    if (ms == m_duration)
        return;
    m_duration = ms;
    Q_EMIT durationChanged();
}

void PlaybackController::setVolume(qreal volume)
{
    const qreal clamped = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(clamped + 1.0, m_volume + 1.0))
        return;
    m_volume = clamped;
    Q_EMIT volumeChanged();
}

void PlaybackController::setShuffle(bool shuffle)
{
    if (shuffle == m_shuffle)
        return;
    m_shuffle = shuffle;
    Q_EMIT shuffleChanged();
}

void PlaybackController::cycleRepeat()
{
    m_repeatMode = (m_repeatMode + 1) % 3;
    Q_EMIT repeatModeChanged();
}

void PlaybackController::toggleFavourite()
{
    m_favourite = !m_favourite;
    if (m_queue && m_index >= 0)
        m_queue->toggleFavourite(m_index);
    Q_EMIT favouriteChanged();
}

void PlaybackController::tick()
{
    if (m_duration <= 0)
        return;
    m_position += m_clock.interval();
    if (m_position >= m_duration) {
        if (m_repeatMode == RepeatOne) {
            m_position = 0;
            Q_EMIT positionChanged();
            return;
        }
        m_position = m_duration;
        Q_EMIT positionChanged();
        next();
        return;
    }
    Q_EMIT positionChanged();
}
