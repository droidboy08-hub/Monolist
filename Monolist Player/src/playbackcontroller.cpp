#include "playbackcontroller.h"
#include "appdatabase.h"
#include "downloadmanager.h"
#include "mpvengine.h"
#include "streamresolver.h"
#include "trackmodel.h"

#include <QAbstractItemModel>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

// Autoplay asks for more once fewer songs than this are left, so the next one
// can be prefetched before it is needed.
constexpr int kRadioLowWater = 2;
// Radio songs added at a time: enough to run for a while, few enough that the
// queue stays readable.
constexpr int kRadioBatch = 25;

QList<QueueTrack> tracksFromModel(QAbstractItemModel *model)
{
    QList<QueueTrack> tracks;
    if (!model)
        return tracks;
    const QHash<int, QByteArray> roles = model->roleNames();
    const int rows = model->rowCount();
    tracks.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = model->index(row, 0);
        QVariantMap map;
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            map.insert(QString::fromUtf8(it.value()), model->data(index, it.key()));
        tracks.append(QueueTrack::fromMap(map));
    }
    return tracks;
}

} // namespace

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
            // page. A remembered link that no longer opens is simply stale, so
            // fetch a fresh one from the same tier; anything else goes to the
            // next tier before giving up.
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

    // Rows inserted before the current one move it; the index QML reads
    // follows.
    connect(&m_queue, &QueueModel::currentIndexChanged, this, &PlaybackController::currentTrackChanged);

    connect(&m_innerTube, &InnerTube::radioReady, this,
            [this](const QString &seed, const QList<InnerTube::Track> &found) {
                if (seed != m_radioSeed)
                    return;   // a newer context replaced the queue meanwhile
                m_radioSeed.clear();

                QList<QueueTrack> additions;
                for (const InnerTube::Track &song : found) {
                    if (song.videoId.isEmpty() || m_queue.containsVideo(song.videoId))
                        continue;   // the seed itself comes first, and no repeats
                    QueueTrack track;
                    track.videoId = song.videoId;
                    track.title = song.title;
                    track.artist = song.artist;
                    track.album = song.album;
                    track.artwork = song.artwork;
                    track.durationMs = song.durationMs;
                    track.fromRadio = true;
                    additions.append(track);
                    if (additions.size() >= kRadioBatch)
                        break;
                }
                m_queue.insert(m_queue.rowCount(), additions);

                if (m_waitingForRadio) {
                    m_waitingForRadio = false;
                    if (m_queue.upcomingCount() > 0) {
                        playIndex(m_queue.currentIndex() + 1);
                    } else {
                        setPlayingFlag(false);
                        setStatus(QStringLiteral("End of the queue"), QString(), false);
                    }
                } else {
                    prefetchUpcoming();
                }
            });

    connect(&m_innerTube, &InnerTube::radioFailed, this,
            [this](const QString &seed, const QString &reason) {
                if (seed != m_radioSeed)
                    return;
                m_radioSeed.clear();
                qWarning("Monolist: autoplay has nothing to continue with: %s", qPrintable(reason));
                if (m_waitingForRadio) {
                    m_waitingForRadio = false;
                    setPlayingFlag(false);
                    setStatus(QStringLiteral("End of the queue"), QString(), false);
                }
            });
}

bool PlaybackController::engineAvailable() const
{
    return m_engine && m_engine->isValid();
}

void PlaybackController::setLibrary(TrackModel *library)
{
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library) {
        connect(m_library, &QAbstractItemModel::modelReset, this, &PlaybackController::refreshFavourite);
        connect(m_library, &QAbstractItemModel::dataChanged, this, &PlaybackController::refreshFavourite);
    }
    refreshFavourite();
}

QString PlaybackController::currentSourceId() const
{
    return m_currentTrack.value(QStringLiteral("sourceId")).toString();
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

QString PlaybackController::artworkForSource(const QString &videoId)
{
    if (videoId.isEmpty())
        return {};
    // hqdefault exists for every video; maxresdefault often 404s on older or
    // low-resolution uploads, so it is not worth the failed request.
    return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId);
}

// ------------------------------------------------------------------ likes

int PlaybackController::libraryRow() const
{
    if (!m_library || m_currentTrack.isEmpty())
        return -1;
    const int trackId = m_currentTrack.value(QStringLiteral("trackId")).toInt();
    const int row = trackId > 0 ? m_library->indexOfTrack(trackId) : -1;
    return row >= 0 ? row : m_library->indexOfSource(currentSourceId());
}

void PlaybackController::refreshFavourite()
{
    const int row = libraryRow();
    const bool favourite = row >= 0 && m_library->get(row).value(QStringLiteral("favourite")).toBool();
    if (favourite == m_favourite)
        return;
    m_favourite = favourite;
    Q_EMIT favouriteChanged();
}

void PlaybackController::toggleFavourite()
{
    if (!m_library || m_currentTrack.isEmpty())
        return;
    const int row = libraryRow();
    if (row >= 0)
        m_library->toggleFavourite(row);   // dataChanged refreshes the flag
    else
        // Liking a song that is not in the library saves it there, the way a
        // like saves a song in any streaming app.
        m_library->addTrack(m_currentTrack, /*favourite=*/true);
}

// ------------------------------------------------------------------ queue

void PlaybackController::startQueue(QList<QueueTrack> tracks, int start, bool autoPlay)
{
    // A new context: whatever the radio was fetching belonged to the old one.
    m_innerTube.cancelRadio();
    m_radioSeed.clear();
    m_waitingForRadio = false;

    if (tracks.isEmpty())
        return;
    start = qBound(0, start, int(tracks.size()) - 1);

    if (m_shuffle) {
        // Shuffling a list starts with the song that was picked and plays the
        // rest of the list, before it and after it, in random order.
        tracks.prepend(tracks.takeAt(start));
        start = 0;
        m_queue.replace(tracks, start);
        m_queue.shuffleUpcoming();
    } else {
        m_queue.replace(tracks, start);
    }
    beginCurrent(autoPlay);
}

void PlaybackController::beginCurrent(bool autoPlay)
{
    const QueueTrack *track = m_queue.current();
    if (!track)
        return;
    beginTrack(track->toMap(), autoPlay);
    if (autoPlay && m_autoplay && m_repeatMode != RepeatAll
        && m_queue.upcomingCount() < kRadioLowWater)
        extendWithRadio();
}

bool PlaybackController::extendWithRadio()
{
    if (!m_radioSeed.isEmpty())
        return true;   // already on its way
    // Seed from the last song, so the radio carries on from where the queue
    // ends rather than from where it began.
    for (int row = m_queue.rowCount() - 1; row >= 0; --row) {
        const QueueTrack *track = m_queue.at(row);
        if (track && !track->videoId.isEmpty()) {
            m_radioSeed = track->videoId;
            m_innerTube.radio(m_radioSeed);
            return true;
        }
    }
    return false;
}

void PlaybackController::loadIndex(int index)
{
    if (!m_queue.at(index))
        return;
    m_queue.setCurrentIndex(index);
    beginCurrent(/*autoPlay=*/false);
}

void PlaybackController::playIndex(int index)
{
    if (!m_queue.at(index))
        return;
    m_queue.setCurrentIndex(index);
    beginCurrent(/*autoPlay=*/true);
}

void PlaybackController::playModel(QAbstractItemModel *model, int row)
{
    startQueue(tracksFromModel(model), row, /*autoPlay=*/true);
}

void PlaybackController::loadModel(QAbstractItemModel *model, int row)
{
    startQueue(tracksFromModel(model), row, /*autoPlay=*/false);
}

void PlaybackController::playTracks(const QVariantList &tracks, int start)
{
    QList<QueueTrack> queue;
    queue.reserve(tracks.size());
    for (const QVariant &track : tracks)
        queue.append(QueueTrack::fromMap(track.toMap()));
    startQueue(queue, start, /*autoPlay=*/true);
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
    QueueTrack track;
    track.videoId = videoId;
    track.title = title;
    track.artist = artist;
    track.album = album;
    track.durationMs = durationMs;
    track.artwork = artwork.isEmpty() ? artworkForSource(videoId) : artwork;
    startQueue({ track }, 0, /*autoPlay=*/true);
}

void PlaybackController::playNext(const QVariantMap &map)
{
    const QueueTrack track = QueueTrack::fromMap(map);
    if (track.videoId.isEmpty() && track.sourceUrl.isEmpty())
        return;
    if (m_queue.rowCount() == 0) {
        startQueue({ track }, 0, /*autoPlay=*/true);
        return;
    }
    m_queue.insert(m_queue.currentIndex() + 1, { track });
    prefetchUpcoming();
}

void PlaybackController::addToQueue(const QVariantMap &map)
{
    const QueueTrack track = QueueTrack::fromMap(map);
    if (track.videoId.isEmpty() && track.sourceUrl.isEmpty())
        return;
    if (m_queue.rowCount() == 0) {
        startQueue({ track }, 0, /*autoPlay=*/false);
        return;
    }
    // What you queue goes ahead of what autoplay found.
    int row = m_queue.rowCount();
    for (int candidate = m_queue.currentIndex() + 1; candidate < m_queue.rowCount(); ++candidate) {
        if (m_queue.at(candidate)->fromRadio) {
            row = candidate;
            break;
        }
    }
    m_queue.insert(row, { track });
    prefetchUpcoming();
}

void PlaybackController::removeFromQueue(int index)
{
    m_queue.removeAt(index);
    prefetchUpcoming();
}

void PlaybackController::clearUpcoming()
{
    m_innerTube.cancelRadio();
    m_radioSeed.clear();
    m_waitingForRadio = false;
    m_queue.clearUpcoming();
}

// While one song plays, resolve the next, so skipping to it does not wait on
// yt-dlp. Downloaded songs need nothing. Shuffle has already put the upcoming
// songs in their played order, so "next" is known either way.
void PlaybackController::prefetchUpcoming()
{
    if (!m_resolver)
        return;
    const QueueTrack *upcoming = m_queue.at(m_queue.currentIndex() + 1);
    if (!upcoming && m_repeatMode == RepeatAll)
        upcoming = m_queue.at(0);
    if (!upcoming || upcoming->videoId.isEmpty())
        return;
    if (m_downloads && !m_downloads->localPathFor(upcoming->videoId).isEmpty())
        return;
    m_resolver->prefetch(upcoming->videoId);
}

// ------------------------------------------------------------ source ladder

void PlaybackController::recordHistory(const QVariantMap &track)
{
    int trackId = track.value(QStringLiteral("trackId")).toInt();
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (trackId <= 0 && m_library && !videoId.isEmpty()) {
        const int row = m_library->indexOfSource(videoId);
        if (row >= 0)
            trackId = m_library->get(row).value(QStringLiteral("trackId")).toInt();
    }
    if (trackId > 0) {
        QSqlQuery history(AppDatabase::connection());
        history.prepare(QStringLiteral("INSERT INTO history (track_id) VALUES (?)"));
        history.addBindValue(trackId);
        history.exec();
    }

    // Every song played, in the library or not, for Home's "Recently played".
    if (videoId.isEmpty())
        return;
    QSqlQuery recent(AppDatabase::connection());
    recent.prepare(QStringLiteral(
        "INSERT INTO recent (video_id, title, artist, album, artwork, duration_ms)"
        " VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(video_id) DO UPDATE SET"
        "   title = excluded.title, artist = excluded.artist, album = excluded.album,"
        "   artwork = excluded.artwork, duration_ms = excluded.duration_ms,"
        "   played_at = datetime('now'), play_count = play_count + 1"));
    recent.addBindValue(videoId);
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("artwork")).toString()));
    recent.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    if (!recent.exec())
        qWarning("Monolist: could not record a play: %s", qPrintable(recent.lastError().text()));
}

// Everything that decides *where* audio comes from lives here; the rest of the
// class only cares that something is playing.
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
        const QString derived = artworkForSource(currentSourceId());
        if (!derived.isEmpty())
            m_currentTrack.insert(QStringLiteral("artwork"), derived);
    }

    m_position = 0;
    m_autoPlayAfterResolve = autoPlay;
    setDuration(track.value(QStringLiteral("durationMs")).toLongLong());

    // Before announcing the track, so anything that reloads history on the
    // announcement already finds it there.
    if (autoPlay)
        recordHistory(m_currentTrack);

    Q_EMIT currentTrackChanged();
    Q_EMIT positionChanged();
    refreshFavourite();

    if (!engineAvailable()) {
        setStatus(QStringLiteral("Audio engine unavailable"), QString(), false);
        Q_EMIT playbackError(m_engine ? m_engine->lastError()
                                      : QStringLiteral("libmpv was not initialised."));
        return;
    }

    const QString videoId = currentSourceId();

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
        beginCurrent(/*autoPlay=*/true);
        return;
    }
    next();
}

// -------------------------------------------------------------- transport

void PlaybackController::play()
{
    if (!engineAvailable())
        return;
    if (m_currentTrack.isEmpty()) {
        if (m_queue.rowCount() > 0)
            playIndex(qMax(0, m_queue.currentIndex()));
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
    if (m_queue.rowCount() == 0)
        return;
    const bool wasPlaying = m_playing || m_resolving;

    int target = m_queue.currentIndex() + 1;
    if (target >= m_queue.rowCount()) {
        if (m_repeatMode == RepeatAll) {
            target = 0;
        } else if (m_autoplay && extendWithRadio()) {
            // Out of songs: carry on with the radio as soon as it answers.
            m_waitingForRadio = true;
            setStatus(QStringLiteral("Finding more songs…"), QString(), true);
            return;
        } else {
            pause();                  // end of the queue: stop rather than restart
            return;
        }
    }

    if (wasPlaying)
        playIndex(target);
    else
        loadIndex(target);
}

void PlaybackController::previous()
{
    // Restart the current song first, the way every other player behaves.
    if (m_position > 3000 || m_queue.currentIndex() <= 0) {
        setPosition(0);
        return;
    }

    const bool wasPlaying = m_playing || m_resolving;
    const int target = m_queue.currentIndex() - 1;
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
    if (shuffle)
        m_queue.shuffleUpcoming();
    else
        m_queue.restoreOrder();
    Q_EMIT shuffleChanged();
    prefetchUpcoming();
}

void PlaybackController::cycleRepeat()
{
    m_repeatMode = (m_repeatMode + 1) % 3;
    Q_EMIT repeatModeChanged();
}

void PlaybackController::setAutoplay(bool autoplay)
{
    if (autoplay == m_autoplay)
        return;
    m_autoplay = autoplay;
    if (!autoplay) {
        m_innerTube.cancelRadio();
        m_radioSeed.clear();
    }
    Q_EMIT autoplayChanged();
}
