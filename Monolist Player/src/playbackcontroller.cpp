#include "playbackcontroller.h"
#include "appdatabase.h"
#include "downloadmanager.h"
#include "mpvengine.h"
#include "streamresolver.h"
#include "trackmodel.h"

#include <QFileInfo>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QVariant>

PlaybackController::PlaybackController(MpvEngine *engine,
                                       StreamResolver *resolver,
                                       DownloadManager *downloads,
                                       QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_resolver(resolver)
    , m_downloads(downloads)
{
    if (m_engine) {
        connect(m_engine, &MpvEngine::positionChanged, this, [this](qint64 ms) {
            if (ms == m_position)
                return;
            m_position = ms;
            Q_EMIT positionChanged();
        });

        connect(m_engine, &MpvEngine::durationChanged, this, &PlaybackController::setDuration);

        connect(m_engine, &MpvEngine::pausedChanged, this, [this](bool paused) {
            setPlayingFlag(!paused);
        });

        connect(m_engine, &MpvEngine::bufferingChanged, this, [this](bool buffering) {
            if (buffering == m_buffering)
                return;
            m_buffering = buffering;
            Q_EMIT bufferingChanged();
        });

        connect(m_engine, &MpvEngine::endOfFile, this, &PlaybackController::handleEndOfFile);

        connect(m_engine, &MpvEngine::loadFailed, this, [this](const QString &reason) {
            // A URL can resolve and still be refused when mpv opens it: a link
            // the CDN rejects now and then, or an instance serving an error
            // page. Ask the next tier before giving up.
            // A remembered link that no longer opens is simply stale: fetch a
            // fresh one from the same tier before trying a worse one.
            if (m_streamTier >= 0 && m_resolver && m_streamFromCache) {
                const int sameTier = m_streamTier;
                m_streamTier = -1;
                m_resolver->invalidate(m_streamVideoId);
                m_pendingVideoId = m_streamVideoId;
                setStatus(QStringLiteral("Refreshing the source…"), QString(), true);
                m_resolver->resolve(m_pendingVideoId, sameTier);
                return;
            }
            if (m_streamTier >= 0 && m_resolver
                && m_streamTier + 1 < StreamResolver::TierExhausted) {
                const int nextTier = m_streamTier + 1;
                m_streamTier = -1;
                m_resolver->invalidate(m_streamVideoId);
                m_pendingVideoId = m_streamVideoId;
                setStatus(QStringLiteral("Trying another source…"), QString(), true);
                m_resolver->resolve(m_pendingVideoId, nextTier);
                return;
            }
            m_streamTier = -1;
            setPlayingFlag(false);
            setStatus(QStringLiteral("Playback failed"), QString(), false);
            Q_EMIT playbackError(reason);
        });

        m_engine->setVolume(m_volume);
    }

    if (m_resolver) {
        connect(m_resolver, &StreamResolver::resolved, this, &PlaybackController::handleResolved);
        connect(m_resolver, &StreamResolver::failed, this, &PlaybackController::handleResolveFailed);
    }
}

bool PlaybackController::engineAvailable() const
{
    return m_engine && m_engine->isValid();
}

void PlaybackController::setQueue(TrackModel *model)
{
    if (m_queue)
        disconnect(m_queue, nullptr, this, nullptr);
    m_queue = model;
    if (m_queue)
        connect(m_queue, &QAbstractItemModel::modelReset, this, &PlaybackController::resyncIndex);
}

// The library reloads whenever a download lands or is removed, so the row the
// player is on may have moved — and a search result that has just been saved
// now has a row of its own. Find the current track again by its identity.
void PlaybackController::resyncIndex()
{
    if (!m_queue || m_currentTrack.isEmpty())
        return;
    int row = -1;
    const int trackId = m_currentTrack.value(QStringLiteral("trackId")).toInt();
    if (trackId > 0)
        row = m_queue->indexOfTrack(trackId);
    if (row < 0)
        row = m_queue->indexOfSource(m_currentTrack.value(QStringLiteral("sourceId")).toString());
    if (row == m_index)
        return;
    m_index = row;
    Q_EMIT currentTrackChanged();
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

void PlaybackController::setStatus(const QString &text, const QString &source, bool resolving)
{
    const bool changed = text != m_statusText
                      || source != m_sourceLabel
                      || resolving != m_resolving;
    m_statusText = text;
    m_sourceLabel = source;
    m_resolving = resolving;
    if (changed)
        Q_EMIT statusChanged();
}

void PlaybackController::setPlayingFlag(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    Q_EMIT playingChanged();
}

void PlaybackController::setDuration(qint64 ms)
{
    if (ms == m_duration)
        return;
    m_duration = ms;
    Q_EMIT durationChanged();
}

void PlaybackController::recordHistory(int trackId)
{
    if (trackId <= 0)
        return;
    QSqlQuery history(AppDatabase::connection());
    history.prepare(QStringLiteral("INSERT INTO history (track_id) VALUES (?)"));
    history.addBindValue(trackId);
    history.exec();
}

void PlaybackController::loadIndex(int index)
{
    if (!m_queue)
        return;
    const int count = m_queue->rowCount();
    if (count == 0 || index < 0 || index >= count)
        return;

    m_index = index;
    beginTrack(m_queue->get(index), /*autoPlay=*/false);
}

void PlaybackController::playIndex(int index)
{
    if (!m_queue)
        return;
    const int count = m_queue->rowCount();
    if (count == 0 || index < 0 || index >= count)
        return;

    m_index = index;
    beginTrack(m_queue->get(index), /*autoPlay=*/true);
}

QString PlaybackController::artworkForSource(const QString &videoId)
{
    if (videoId.isEmpty())
        return {};
    // hqdefault exists for every video; maxresdefault often 404s on older or
    // low-resolution uploads, so it is not worth the failed request.
    return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId);
}

void PlaybackController::playSource(const QString &videoId,
                                    const QString &title,
                                    const QString &artist,
                                    const QString &artwork,
                                    qint64 durationMs,
                                    const QString &album)
{
    if (videoId.isEmpty())
        return;

    m_index = -1;
    beginTrack(QVariantMap{
                   { QStringLiteral("trackId"),    0 },
                   { QStringLiteral("sourceId"),   videoId },
                   { QStringLiteral("title"),      title },
                   { QStringLiteral("artist"),     artist },
                   { QStringLiteral("album"),      album },
                   { QStringLiteral("durationMs"), durationMs },
                   { QStringLiteral("sourceUrl"),  QString() },
                   { QStringLiteral("artwork"),    artwork.isEmpty()
                                                       ? artworkForSource(videoId)
                                                       : artwork }
               },
               /*autoPlay=*/true);
}

// The source ladder. Everything that decides *where* audio comes from lives
// here; the rest of the class only cares that something is playing.
void PlaybackController::beginTrack(const QVariantMap &track, bool autoPlay)
{
    if (m_resolver && !m_pendingVideoId.isEmpty())
        m_resolver->cancel(m_pendingVideoId);
    m_pendingVideoId.clear();
    m_streamTier = -1;

    m_currentTrack = track;

    // Library rows written before artwork was captured still have a source id;
    // derive the thumbnail rather than showing an empty plate.
    if (m_currentTrack.value(QStringLiteral("artwork")).toString().isEmpty()) {
        const QString derived = artworkForSource(
            m_currentTrack.value(QStringLiteral("sourceId")).toString());
        if (!derived.isEmpty())
            m_currentTrack.insert(QStringLiteral("artwork"), derived);
    }

    m_favourite = track.value(QStringLiteral("favourite")).toBool();
    m_position = 0;
    m_autoPlayAfterResolve = autoPlay;
    setDuration(track.value(QStringLiteral("durationMs")).toLongLong());

    Q_EMIT currentTrackChanged();
    Q_EMIT favouriteChanged();
    Q_EMIT positionChanged();

    recordHistory(track.value(QStringLiteral("trackId")).toInt());

    if (!engineAvailable()) {
        setStatus(QStringLiteral("Audio engine unavailable"), QString(), false);
        Q_EMIT playbackError(m_engine ? m_engine->lastError()
                                      : QStringLiteral("libmpv was not initialised."));
        return;
    }

    const QString videoId = track.value(QStringLiteral("sourceId")).toString();

    // 1 — a downloaded copy, or any source that is already a local file.
    QString localPath;
    if (m_downloads && !videoId.isEmpty())
        localPath = m_downloads->localPathFor(videoId);
    if (localPath.isEmpty()) {
        const QString source = track.value(QStringLiteral("sourceUrl")).toString();
        if (!source.isEmpty() && QFileInfo::exists(source))
            localPath = source;
    }

    if (!localPath.isEmpty()) {
        setStatus(QStringLiteral("Offline"), QStringLiteral("Local file"), false);
        m_engine->load(localPath, autoPlay);
        if (autoPlay)
            prefetchUpcoming();
        return;
    }

    // 2 — resolve a stream. Nothing plays until the resolver answers.
    if (!videoId.isEmpty() && m_resolver) {
        m_pendingVideoId = videoId;
        setStatus(QStringLiteral("Resolving source…"), QString(), true);
        m_resolver->resolve(videoId);
        return;
    }

    // 3 — a plain remote URL stored on the row.
    const QString source = track.value(QStringLiteral("sourceUrl")).toString();
    if (!source.isEmpty()) {
        setStatus(QStringLiteral("Streaming"), QStringLiteral("Direct URL"), false);
        m_engine->load(source, autoPlay);
        return;
    }

    setStatus(QStringLiteral("No playable source"), QString(), false);
    Q_EMIT playbackError(QStringLiteral("This track has no local file and no source id."));
}

void PlaybackController::handleResolved(const QString &videoId, const QString &url, int tier,
                                        bool fromCache)
{
    if (videoId != m_pendingVideoId)
        return;                       // a later track superseded this one
    m_pendingVideoId.clear();

    QString label;
    switch (tier) {
    case StreamResolver::TierYtDlp:     label = QStringLiteral("yt-dlp");    break;
    case StreamResolver::TierPiped:     label = QStringLiteral("Piped");     break;
    case StreamResolver::TierInvidious: label = QStringLiteral("Invidious"); break;
    default:                            label = QStringLiteral("Stream");    break;
    }

    setStatus(QStringLiteral("Streaming"), label, false);
    m_streamVideoId = videoId;
    m_streamTier = tier;
    m_streamFromCache = fromCache;
    if (m_engine)
        m_engine->load(url, m_autoPlayAfterResolve);
    prefetchUpcoming();
}

// While one track plays, resolve the next, so skipping to it does not wait on
// yt-dlp. Downloaded tracks need nothing, and under shuffle there is no
// telling which track is next.
void PlaybackController::prefetchUpcoming()
{
    if (!m_resolver || !m_queue || m_shuffle || m_index < 0)
        return;
    const int count = m_queue->rowCount();
    int upcoming = m_index + 1;
    if (upcoming >= count) {
        if (m_repeatMode != RepeatAll || count < 2)
            return;
        upcoming = 0;
    }
    const QString videoId = m_queue->get(upcoming).value(QStringLiteral("sourceId")).toString();
    if (videoId.isEmpty() || (m_downloads && !m_downloads->localPathFor(videoId).isEmpty()))
        return;
    m_resolver->prefetch(videoId);
}

void PlaybackController::handleResolveFailed(const QString &videoId, const QString &reason)
{
    if (videoId != m_pendingVideoId)
        return;
    m_pendingVideoId.clear();

    setPlayingFlag(false);
    setStatus(QStringLiteral("Source unavailable"), QString(), false);
    Q_EMIT playbackError(reason);
}

void PlaybackController::handleEndOfFile()
{
    if (m_repeatMode == RepeatOne) {
        m_position = 0;
        Q_EMIT positionChanged();
        if (m_index >= 0)
            playIndex(m_index);
        return;
    }
    next();
}

void PlaybackController::play()
{
    if (!engineAvailable())
        return;
    if (m_index < 0 && m_currentTrack.isEmpty()) {
        playIndex(0);
        return;
    }
    m_engine->setPaused(false);
}

void PlaybackController::pause()
{
    if (engineAvailable())
        m_engine->setPaused(true);
}

void PlaybackController::togglePlay()
{
    m_playing ? pause() : play();
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
        if (m_repeatMode == RepeatOff) {
            pause();
            return;                   // end of queue — stop rather than restart
        }
        target = 0;
    }

    const bool wasPlaying = m_playing || m_resolving;
    if (wasPlaying)
        playIndex(target);
    else
        loadIndex(target);
}

void PlaybackController::previous()
{
    if (!m_queue)
        return;

    // Restart the current track first, the way every other player behaves.
    if (m_position > 3000) {
        setPosition(0);
        return;
    }

    const bool wasPlaying = m_playing || m_resolving;
    const int target = m_index > 0 ? m_index - 1 : 0;
    if (wasPlaying)
        playIndex(target);
    else
        loadIndex(target);
}

void PlaybackController::setPosition(qint64 ms)
{
    const qint64 clamped = m_duration > 0 ? qBound<qint64>(0, ms, m_duration)
                                          : qMax<qint64>(0, ms);
    m_position = clamped;
    if (engineAvailable())
        m_engine->seekAbsolute(clamped);
    Q_EMIT positionChanged();
}

void PlaybackController::seekFraction(qreal fraction)
{
    setPosition(qint64(qBound(0.0, fraction, 1.0) * qreal(m_duration)));
}

void PlaybackController::setVolume(qreal volume)
{
    const qreal clamped = qBound(0.0, volume, 1.0);
    if (qFuzzyCompare(clamped + 1.0, m_volume + 1.0))
        return;
    m_volume = clamped;
    if (m_engine)
        m_engine->setVolume(clamped);
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
