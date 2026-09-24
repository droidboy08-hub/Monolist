#include "playbackcontroller.h"
#include "appdatabase.h"
#include "downloadmanager.h"
#include "library.h"
#include "mpvengine.h"
#include "streamresolver.h"
#include "trackmodel.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
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
// Tracks that may fail to resolve in a row before the queue stops walking
// itself. Three is enough to step over a patch of unavailable songs and few
// enough that a machine with no network gives up almost at once.
constexpr int kMaxConsecutiveFailures = 3;

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

        // A frame arrived: the picture plays, so an end of file after this is
        // the song ending, not the video failing.
        connect(m_engine, &MpvEngine::videoSizeChanged, this, [this](const QSize &size) {
            if (!size.isEmpty())
                m_videoUnproven = false;
        });

        connect(m_engine, &MpvEngine::loadFailed, this, [this](const QString &reason) {
            // The picture would not open: keep the song, drop the picture.
            if (abandonVideo(QStringLiteral("This video would not play — back to audio")))
                return;
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
            setStatus(QStringLiteral("Playback failed"), QString(), false, /*error=*/true);
            Q_EMIT playbackError(reason);
        });

        m_engine->setVolume(m_volume);
    }

    // The last song of a session is the one nothing else closes, and it is the
    // most recent thing the listener chose — exactly the event a recommender
    // would miss most.
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() { closePlayEvent(); });
    }

    if (m_resolver) {
        connect(m_resolver, &StreamResolver::resolved, this, &PlaybackController::handleResolved);
        connect(m_resolver, &StreamResolver::failed, this, &PlaybackController::handleResolveFailed);
        connect(m_resolver, &StreamResolver::videoResolved, this, &PlaybackController::handleVideoResolved);
        connect(m_resolver, &StreamResolver::videoFailed, this,
                [this](const QString &videoId, const QString &reason) {
                    if (videoId != m_videoPendingId)
                        return;
                    m_videoPendingId.clear();
                    // Back to sound alone, where it never stopped.
                    m_videoWanted = false;
                    setStatus(QStringLiteral("Streaming"), m_sourceLabel, false);
                    Q_EMIT videoChanged();
                    Q_EMIT playbackError(reason);
                });
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

void PlaybackController::setLibrary(Library *library)
{
    if (m_library)
        disconnect(m_library, nullptr, this, nullptr);
    m_library = library;
    if (m_library)
        connect(m_library, &Library::likesChanged, this, &PlaybackController::refreshFavourite);
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

void PlaybackController::setStatus(const QString &text, const QString &source, bool resolving,
                                   bool error)
{
    const bool changed = text != m_statusText
                      || source != m_sourceLabel
                      || resolving != m_resolving
                      || error != m_statusError;
    m_statusText = text;
    m_sourceLabel = source;
    m_resolving = resolving;
    m_statusError = error;
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

void PlaybackController::refreshFavourite()
{
    const bool favourite = m_library && m_library->isLiked(currentSourceId());
    if (favourite == m_favourite)
        return;
    m_favourite = favourite;
    Q_EMIT favouriteChanged();
}

void PlaybackController::toggleFavourite()
{
    if (!m_library || m_currentTrack.isEmpty())
        return;
    // A like is the strongest thing anyone tells a recommender, and an unlike
    // is its retraction — both are recorded, so a profile built later can
    // subtract what was taken back rather than counting it for ever.
    recordDiscreteEvent(m_currentTrack,
                        m_favourite ? QStringLiteral("unliked") : QStringLiteral("like"));
    // likesChanged refreshes the flag.
    m_library->setLiked(m_currentTrack, !m_favourite);
}

// ------------------------------------------------------------------ queue

void PlaybackController::startQueue(QList<QueueTrack> tracks, int start, bool autoPlay)
{
    // A new context: whatever the radio was fetching belonged to the old one.
    m_innerTube.cancelRadio();
    m_radioSeed.clear();
    m_waitingForRadio = false;
    // Someone has just asked for something: give the queue its three tries back.
    m_consecutiveFailures = 0;

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

void PlaybackController::playModel(QAbstractItemModel *model, int row, const QString &origin)
{
    m_source = origin;
    startQueue(tracksFromModel(model), row, /*autoPlay=*/true);
}

void PlaybackController::loadModel(QAbstractItemModel *model, int row)
{
    startQueue(tracksFromModel(model), row, /*autoPlay=*/false);
}

void PlaybackController::playTracks(const QVariantList &tracks, int start, const QString &origin)
{
    m_source = origin;
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
                                    const QString &album,
                                    bool isVideo,
                                    const QString &origin)
{
    if (videoId.isEmpty())
        return;
    m_source = origin;
    QueueTrack track;
    track.videoId = videoId;
    track.title = title;
    track.artist = artist;
    track.album = album;
    track.durationMs = durationMs;
    track.isVideo = isVideo;
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
        const int row = m_library->tracks()->indexOfSource(videoId);
        if (row >= 0)
            trackId = m_library->tracks()->get(row).value(QStringLiteral("trackId")).toInt();
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

// ------------------------------------------------------------- play events

void PlaybackController::openPlayEvent(const QVariantMap &track)
{
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    const QString title = track.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        return;                       // nothing a recommender could match on

    // Where this play came from, which the profile weights by. A track the
    // radio added was not chosen by anyone, so it counts as the queue no
    // matter which surface the queue began on — and the queue is the weakest
    // signal there is short of a resume.
    const int radioStart = m_queue.radioStartIndex();
    const bool fromRadio = radioStart >= 0 && m_queue.currentIndex() >= radioStart;
    const QString source = fromRadio ? QStringLiteral("queue") : m_source;

    QSqlQuery event(AppDatabase::connection());
    event.prepare(QStringLiteral(
        "INSERT INTO play_events (kind, video_id, title, artist, track_ms, repeat_in_session, source)"
        " VALUES ('play', ?, ?, ?, ?, ?, ?)"));
    event.addBindValue(AppDatabase::text(videoId));
    event.addBindValue(AppDatabase::text(title));
    event.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    event.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    // A song returned to in the same sitting says something a first play does
    // not, and it is only knowable while the app is running.
    const QString key = videoId.isEmpty() ? title : videoId;
    event.addBindValue(m_finalisedThisSession.contains(key) ? 1 : 0);
    event.addBindValue(AppDatabase::text(source));
    if (!event.exec()) {
        m_playEventId = 0;
        return;
    }
    m_playEventId = event.lastInsertId().toLongLong();
    m_playEventKey = key;
}

// The label ladder is the iOS one exactly (RECOMMENDATION_PORTING.md 5.5).
// Deliberately not "improved": a history imported from that player and one
// recorded here have to mean the same thing, or a taste profile built from
// both is built from two different measurements.
void PlaybackController::closePlayEvent()
{
    if (m_playEventId <= 0)
        return;
    const qint64 id = std::exchange(m_playEventId, 0);
    const qint64 listened = qMax<qint64>(0, m_position);
    const qint64 total = qMax<qint64>(0, m_duration);

    QVariant label;                   // null means "not long enough to say"
    if (total > 0) {
        const double ratio = double(listened) / double(total);
        if (ratio < 0.10)      label = 0.0;
        else if (ratio < 0.50) label = 0.2;
        else if (ratio < 0.90) label = 0.6;
        else                   label = 1.0;
    } else if (listened < 30000) {
        label = 0.0;              // gave up on something of unknown length
    }

    // A different threshold from the label on purpose: 0.9 of a song is a
    // listen, but "completed" is what the queue means by finishing one.
    const bool completed = total > 0 && listened >= qint64(total * 0.85);
    const bool skipped = !completed && listened < 30000;

    // A repeat upgrades a lukewarm label but never rescues a rejected one.
    if (m_finalisedThisSession.contains(m_playEventKey) && label.isValid()) {
        const double value = label.toDouble();
        if (value > 0.0)
            label = 1.0;
    }
    if (!m_playEventKey.isEmpty())
        m_finalisedThisSession.insert(m_playEventKey);

    // The duration is written here, not when the event opened: a track picked
    // from search carries no length until mpv has opened the stream and said
    // so, and a length of zero would make every ratio above meaningless.
    QSqlQuery finish(AppDatabase::connection());
    finish.prepare(QStringLiteral(
        "UPDATE play_events SET track_ms = ?, listened_ms = ?, completed = ?, skipped = ?,"
        " label = ? WHERE id = ?"));
    finish.addBindValue(total);
    finish.addBindValue(listened);
    finish.addBindValue(completed ? 1 : 0);
    finish.addBindValue(skipped ? 1 : 0);
    finish.addBindValue(label);
    finish.addBindValue(id);
    finish.exec();
}

void PlaybackController::recordDiscreteEvent(const QVariantMap &track, const QString &kind)
{
    const QString title = track.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        return;
    QSqlQuery event(AppDatabase::connection());
    event.prepare(QStringLiteral(
        "INSERT INTO play_events (kind, video_id, title, artist) VALUES (?, ?, ?, ?)"));
    event.addBindValue(kind);
    event.addBindValue(AppDatabase::text(track.value(QStringLiteral("sourceId")).toString()));
    event.addBindValue(AppDatabase::text(title));
    event.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    event.exec();
}

// Everything that decides *where* audio comes from lives here; the rest of the
// class only cares that something is playing.
void PlaybackController::beginTrack(const QVariantMap &track, bool autoPlay)
{
    // First, while m_position still belongs to the track being left: how much
    // of it was heard is the single most useful thing the recommender gets,
    // and it exists only in this instant.
    closePlayEvent();

    if (m_resolver && !m_pendingVideoId.isEmpty())
        m_resolver->cancel(m_pendingVideoId);
    m_pendingVideoId.clear();
    m_streamTier = -1;
    m_resumeAt = 0;

    // Every track starts as sound: the picture is asked for, never assumed.
    if (!m_videoPendingId.isEmpty() && m_resolver)
        m_resolver->cancelVideo(m_videoPendingId);
    m_videoPendingId.clear();
    m_videoUnproven = false;
    if (m_videoWanted || m_videoPlaying) {
        m_videoWanted = false;
        m_videoPlaying = false;
        if (m_engine)
            m_engine->setVideoEnabled(false);
        Q_EMIT videoChanged();
    }

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
    if (autoPlay) {
        recordHistory(m_currentTrack);
        openPlayEvent(m_currentTrack);
    }

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

    setStatus(QStringLiteral("No playable source"), QString(), false, /*error=*/true);
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
    case StreamResolver::TierInnerTube: label = QStringLiteral("InnerTube"); break;
    case StreamResolver::TierYtDlp:     label = QStringLiteral("yt-dlp");    break;
    case StreamResolver::TierPiped:     label = QStringLiteral("Piped");     break;
    case StreamResolver::TierInvidious: label = QStringLiteral("Invidious"); break;
    default:                            label = QStringLiteral("Stream");    break;
    }

    setStatus(QStringLiteral("Streaming"), label, false);
    m_consecutiveFailures = 0;
    m_streamVideoId = videoId;
    m_streamTier = tier;
    m_streamFromCache = fromCache;
    if (m_engine)
        m_engine->load(url, m_autoPlayAfterResolve, QString(), m_resumeAt);
    m_resumeAt = 0;
    prefetchUpcoming();
}

void PlaybackController::handleResolveFailed(const QString &videoId, const QString &reason)
{
    if (videoId != m_pendingVideoId)
        return;
    m_pendingVideoId.clear();

    // One track that will not play should not end the listening: say which one
    // it was and carry on down the queue. But stop after a few in a row — a
    // machine that has lost the network fails every one of them, and would
    // otherwise run the whole queue in seconds, resolving as it went.
    //
    // `m_autoPlayAfterResolve` is what says the listener was expecting sound;
    // it has to be read before anything below clears state.
    const bool wasGoingToPlay = m_autoPlayAfterResolve;
    const bool giveUp = ++m_consecutiveFailures >= kMaxConsecutiveFailures;
    const bool skip = wasGoingToPlay && !giveUp && m_queue.upcomingCount() > 0;

    // Always in the log, whichever way this goes: it is the only place the
    // real cause is written down.
    qWarning("resolve failed for %s: %s", qPrintable(videoId), qPrintable(reason));

    if (skip) {
        // The playing flag is deliberately left alone. It follows mpv's pause
        // property, and mpv is not paused here — it simply has nothing loaded.
        // Clearing it and then loading the next track would never set it back,
        // because no pause change ever happens, and the player bar would show
        // a play button over music that is playing.
        const QString title = m_currentTrack.value(QStringLiteral("title")).toString();
        Q_EMIT notice(title.isEmpty()
                          ? QStringLiteral("Couldn't play that one — skipping")
                          : QStringLiteral("Couldn't play “%1” — skipping").arg(title));
        advance(/*keepPlaying=*/true);
        return;
    }

    setPlayingFlag(false);
    setStatus(giveUp ? QStringLiteral("Nothing here will play")
                     : QStringLiteral("Source unavailable"),
              QString(), false, /*error=*/true);
    Q_EMIT playbackError(reason);
}

// True when a video that never proved itself has just been dropped, and the
// sound of the same track is on its way back.
bool PlaybackController::abandonVideo(const QString &reason)
{
    if (!m_videoPlaying || !m_videoUnproven)
        return false;

    const QString videoId = currentSourceId();
    m_videoUnproven = false;
    m_videoWanted = false;
    m_videoPlaying = false;
    if (m_engine)
        m_engine->setVideoEnabled(false);
    Q_EMIT videoChanged();
    Q_EMIT notice(reason);

    if (videoId.isEmpty() || !m_resolver)
        return true;
    m_resumeAt = m_position;
    m_autoPlayAfterResolve = true;
    m_pendingVideoId = videoId;
    setStatus(QStringLiteral("Back to the sound…"), QString(), true);
    m_resolver->resolve(videoId);
    return true;
}

void PlaybackController::handleEndOfFile()
{
    // A video that stopped before it ever showed a frame did not finish the
    // song; it failed. The queue must not move on.
    if (abandonVideo(QStringLiteral("This video would not play — back to audio")))
        return;

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
    advance(m_playing || m_resolving);
}

// `keepPlaying` is separate from m_playing because of one caller: a track that
// failed to resolve has already had the playing flag cleared by the time the
// queue steps past it, and reading the flag there would leave the replacement
// sitting paused — the listener asked for music, not for a track that fails
// quietly and a player that stops.
void PlaybackController::advance(bool keepPlaying)
{
    if (m_queue.rowCount() == 0)
        return;
    const bool wasPlaying = keepPlaying;

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

// ------------------------------------------------------------------- video

// YouTube Music's own songs are a still picture with the cover on it, so
// only what it calls a video has one worth showing.
bool PlaybackController::videoAvailable() const
{
    return !currentSourceId().isEmpty()
           && m_currentTrack.value(QStringLiteral("isVideo")).toBool();
}

void PlaybackController::setVideoHeight(int height)
{
    const int wanted = qBound(240, height, 2160);
    if (wanted == m_videoHeight)
        return;
    m_videoHeight = wanted;
    Q_EMIT videoChanged();
    // The picture on screen keeps the size it was opened with; the next one
    // gets the new one.
}

void PlaybackController::setVideoWanted(bool wanted)
{
    if (wanted == m_videoWanted)
        return;
    if (wanted && !videoAvailable())
        return;
    m_videoWanted = wanted;
    Q_EMIT videoChanged();
    playWithVideo(wanted);
}

// Swaps the stream under the same track, resuming where it was.
void PlaybackController::playWithVideo(bool video)
{
    const QString videoId = currentSourceId();
    if (videoId.isEmpty() || !m_resolver)
        return;

    if (video) {
        m_videoPendingId = videoId;
        setStatus(QStringLiteral("Loading the video…"), m_sourceLabel, true);
        m_resolver->resolveVideo(videoId, m_videoHeight);
        return;
    }

    m_resolver->cancelVideo(videoId);
    m_videoPendingId.clear();
    const bool wasShowing = m_videoPlaying;
    if (m_videoPlaying) {
        m_videoPlaying = false;
        Q_EMIT videoChanged();
    }
    if (m_engine)
        m_engine->setVideoEnabled(false);
    if (!wasShowing)
        return;   // nothing was loaded with a picture; the sound plays on
    // Straight back to the sound, from the same second.
    m_resumeAt = m_position;
    m_autoPlayAfterResolve = m_playing || m_resolving;
    m_pendingVideoId = videoId;
    setStatus(QStringLiteral("Resolving source…"), QString(), true);
    m_resolver->resolve(videoId);
}

void PlaybackController::handleVideoResolved(const QString &videoId, const QString &videoUrl,
                                             const QString &audioUrl, const QVariantMap &headers)
{
    if (videoId != m_videoPendingId)
        return;   // another track, or the switch went off again
    m_videoPendingId.clear();
    if (!m_engine)
        return;

    m_engine->setVideoEnabled(true);
    setStatus(QStringLiteral("Streaming"), QStringLiteral("yt-dlp · video"), false);
    // Unproven until a frame arrives: see abandonVideo.
    m_videoUnproven = true;
    m_engine->load(videoUrl, m_playing || m_autoPlayAfterResolve, audioUrl, m_position, headers);
    if (!m_videoPlaying) {
        m_videoPlaying = true;
        Q_EMIT videoChanged();
    }
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
