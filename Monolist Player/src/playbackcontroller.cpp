#include "playbackcontroller.h"
#include "appdatabase.h"
#include "audioalign.h"
#include "downloadmanager.h"
#include "library.h"
#include "loudness.h"
#include "mpvengine.h"
#include "streamresolver.h"
#include "trackmodel.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>

#include <utility>

namespace {

// The status line while a picture is fetched for the song playing.
QString loadingVideoText()
{
    return QStringLiteral("Loading the video…");
}

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
// Previous restarts a song played for longer than this, and goes back to the
// one before a song that has barely begun.
constexpr qint64 kRestartAfterMs = 3000;
// A stream that ends more than this before its own length, or 3% of it if
// that is more, stopped short rather than finished (resumeEarlyEnd). Wide
// enough for the last position mpv reported lagging the real end, and for
// a length that is a little off.
constexpr qint64 kEarlyEndMarginMs = 5000;

// The player's own choices, kept in the settings table so a launch picks up
// where the last one left off.
const QString kVolumeKey = QStringLiteral("player.volume");
const QString kShuffleKey = QStringLiteral("player.shuffle");
const QString kRepeatKey = QStringLiteral("player.repeat");
const QString kAutoplayKey = QStringLiteral("player.autoplay");
const QString kLevelLoudnessKey = QStringLiteral("player.level_loudness");
// The queue and the place in its song, for the next launch (restoreSession).
const QString kQueueKey = QStringLiteral("player.queue");
const QString kPlaceKey = QStringLiteral("player.place");
// At most this many of the queue's songs are kept, mostly those still to
// come: a queue autoplay has run on for a day is otherwise written whole.
constexpr int kQueueKeptRows = 500;
constexpr int kQueueSaveDelayMs = 2000;
constexpr int kPlaceSaveIntervalMs = 10000;
// A place this near either end of a song is not worth going back to: the
// song starts from the top.
constexpr qint64 kPlaceMinMs = 5000;
constexpr qint64 kPlaceTailMs = 10000;
const QString kAudioDeviceKey = QStringLiteral("player.audio_device");
const QString kAudioDeviceNameKey = QStringLiteral("player.audio_device_name");
const QString kAutoDevice = QStringLiteral("auto");
const QString kSaavnKey = QStringLiteral("jiosaavn.enabled");
const QString kSaavnIndiaKey = QStringLiteral("jiosaavn.india_headers");
const QString kSaavnUpgradeKey = QStringLiteral("jiosaavn.upgrade");
// A move to JioSaavn mid-song must be worth hearing: its copy at least this
// much above the bitrate playing (BitChord's UPGRADE_MIN_GAIN_KBPS). 320
// against YouTube's 130-160 clears it; JioSaavn's 160 does not.
constexpr int kUpgradeMinGainKbps = 96;
// YouTube's Opus as mpv usually reads it, where it has not said yet.
constexpr int kUpgradeAssumedKbps = 160;
// JioSaavn's length against the song's as mpv read it (the engine checks the
// file's own length again before it takes over).
constexpr qint64 kUpgradeDriftMs = 2000;
// Where the two copies are compared (AudioAlign): 4 s from 16 s in, looked
// for up to 3 s either way. Near the start, since each is decoded from its
// beginning and how far apart they are holds through the song (the same at
// 30 s and at 120 s on the songs measured). A song must be long enough.
constexpr qint64 kAlignAtMs = 16000;
constexpr qint64 kAlignRangeMs = 3000;
constexpr qint64 kAlignShortestMs = 30000;
// The switches back for the recovery ladder's newer rungs, each restoring
// the way it was before: "muxed", "keep" and "next" (see restoreSettings).
const QString kRefusedKey = QStringLiteral("playback.refused");
const QString kRescueLinkKey = QStringLiteral("playback.rescue_link");
const QString kEarlyEndKey = QStringLiteral("playback.early_end");
// Long enough to outlast one drag of the slider.
constexpr int kVolumeSaveDelayMs = 400;

// Where a stream came from, as the status line and the log name it.
QString tierLabel(int tier)
{
    switch (tier) {
    case StreamResolver::TierInnerTube: return QStringLiteral("InnerTube");
    case StreamResolver::TierYtDlp:     return QStringLiteral("yt-dlp");
    case StreamResolver::TierMuxed:     return QStringLiteral("yt-dlp · muxed");
    case StreamResolver::TierPiped:     return QStringLiteral("Piped");
    case StreamResolver::TierInvidious: return QStringLiteral("Invidious");
    case StreamResolver::TierJioSaavn:  return QStringLiteral("JioSaavn");
    // Played with the YouTube Music account, because YouTube would not
    // play it signed out: said, so nobody wonders why it took longer.
    case StreamResolver::TierSignedIn:  return QStringLiteral("YouTube · signed in");
    default:                            return QStringLiteral("Stream");
    }
}

// A codec as a listener knows it, from mpv's name for its decoder.
QString codecName(const QString &mpvName)
{
    static const QHash<QString, QString> names{
        { QStringLiteral("opus"), QStringLiteral("Opus") },
        { QStringLiteral("aac"), QStringLiteral("AAC") },
        { QStringLiteral("mp3"), QStringLiteral("MP3") },
        { QStringLiteral("mp3float"), QStringLiteral("MP3") },
        { QStringLiteral("vorbis"), QStringLiteral("Vorbis") },
        { QStringLiteral("flac"), QStringLiteral("FLAC") },
        { QStringLiteral("alac"), QStringLiteral("ALAC") },
    };
    if (mpvName.startsWith(QLatin1String("pcm_")))
        return QStringLiteral("PCM");
    return names.value(mpvName, mpvName);
}

// Whether a file of this kind, as mpv names its container (fileFormat), says
// its own length, rather than having it guessed from its size and the bitrate
// of its first frames as FFmpeg does for an MP3 with no index or for ADTS AAC:
// MP4 and M4A, Matroska and WebM, Ogg, FLAC and WAV.
bool lengthIsExact(const QString &fileFormat)
{
    const QStringList names = fileFormat.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &name : names) {
        if (name == QLatin1String("mp4") || name == QLatin1String("m4a") || name == QLatin1String("mov")
            || name == QLatin1String("matroska") || name == QLatin1String("webm") || name == QLatin1String("ogg")
            || name == QLatin1String("flac") || name == QLatin1String("wav"))
            return true;
    }
    return false;
}

// 48000 as "48", 44100 as "44.1".
QString kiloHertz(int hertz)
{
    return QString::number(hertz / 1000.0, 'g', 6);
}

// What JioSaavn needs to find a song: its name, who made it and how long it
// is — the length being what tells one recording from another.
Saavn::Target saavnTarget(const QVariantMap &track)
{
    Saavn::Target target;
    target.videoId = track.value(QStringLiteral("sourceId")).toString();
    target.title = track.value(QStringLiteral("title")).toString();
    target.artist = track.value(QStringLiteral("artist")).toString();
    target.album = track.value(QStringLiteral("album")).toString();
    target.durationMs = track.value(QStringLiteral("durationMs")).toLongLong();
    return target;
}

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
            // Heard time, for scrobbling: the tracker counts only the small
            // forward steps of real playing. A seek from setPosition has
            // already moved m_position, so the jump never arrives here as a
            // step; nor does a picture's stream opening where the sound was.
            m_listen.positionChanged(ms);
            Q_EMIT positionChanged();

            // A song restarted with Previous is a listen again once it plays
            // past the point where Previous would go back instead (see
            // previous()). Only after mpv has reported the jump back: until
            // then it may still say where the song was.
            if (m_replay == Replay::Rewinding && ms <= kRestartAfterMs) {
                m_replay = Replay::Replaying;
            } else if (m_replay == Replay::Replaying && m_playing && ms > kRestartAfterMs) {
                m_replay = Replay::None;
                startListening();
            }
        });

        connect(m_engine, &MpvEngine::durationChanged, this, &PlaybackController::setDuration);

        connect(m_engine, &MpvEngine::pausedChanged, this, [this](bool paused) {
            setPlayingFlag(!paused);
        });

        connect(m_engine, &MpvEngine::bufferingChanged, this, [this](bool buffering) {
            if (buffering == m_buffering)
                return;
            m_buffering = buffering;
            m_listen.setBuffering(buffering);
            Q_EMIT bufferingChanged();
        });

        connect(m_engine, &MpvEngine::endOfFile, this, &PlaybackController::handleEndOfFile);

        // A song is heard: the songs in a row that would not play are
        // counted from nothing again. Not when its link arrived — a link mpv
        // then refuses, and a rescue that fails, played nothing, and counting
        // them as a success let a queue walk itself past the three-in-a-row
        // stop.
        connect(m_engine, &MpvEngine::audioStarted, this, [this]() {
            // After a refusal or an early end: what the song was rescued
            // with, and how long it went without sound. The one line that
            // says whether a rung earns its place.
            if (m_rescueClock.isValid()) {
                // UTF-8: a title, and "yt-dlp · muxed", are not ASCII.
                const QString name = currentSourceId().isEmpty()
                                         ? m_currentTrack.value(QStringLiteral("title")).toString()
                                         : currentSourceId();
                qInfo("playback: %s rescued: its sound came from %s, %lld ms after the first failure",
                      qUtf8Printable(name),
                      qUtf8Printable(m_streamTier >= 0 ? tierLabel(m_streamTier) : QStringLiteral("its own link")),
                      static_cast<long long>(m_rescueClock.elapsed()));
                m_rescueClock.invalidate();
            }
            // A late JioSaavn match that was waiting for this.
            if (!m_upgradeOffer.videoId.isEmpty()) {
                const UpgradeOffer offer = std::exchange(m_upgradeOffer, {});
                // After the rest of this handler, and after mpv has said
                // what it plays.
                QTimer::singleShot(0, this, [this, offer]() {
                    offerSaavnUpgrade(offer.videoId, offer.url, offer.kbps, offer.durationSec);
                });
            }
            if (m_consecutiveFailures == 0)
                return;
            qInfo("playback: %s has started, so the count of songs in a row that would not play starts again"
                  " (it was %d)", qPrintable(currentSourceId()), m_consecutiveFailures);
            m_consecutiveFailures = 0;
        });

        // A frame arrived: the picture plays, so an end of file after this is
        // the song ending, not the video failing.
        connect(m_engine, &MpvEngine::videoSizeChanged, this, [this](const QSize &size) {
            if (!size.isEmpty())
                m_videoUnproven = false;
        });

        // The sound of a song begun with the switch on has loaded: its
        // picture, if it came first, joins it now.
        connect(m_engine, &MpvEngine::fileLoaded, this, [this]() {
            if (m_pictureAwaitingSound.videoId.isEmpty())
                return;
            const PictureAwaitingSound picture = std::exchange(m_pictureAwaitingSound, {});
            if (picture.videoId != currentSourceId() || !m_videoWanted)
                return;
            applyVideo(picture.videoUrl, picture.audioUrl, picture.headers);
        });

        // An added picture that would not open: the sound never stopped.
        connect(m_engine, &MpvEngine::videoAddFailed, this, [this](const QString &reason) {
            if (!m_videoAdded)
                return;
            qWarning("video: the picture would not open: %s", qPrintable(reason));
            dropAddedVideo();
            Q_EMIT notice(QStringLiteral("This video would not play — back to audio"));
        });

        connect(m_engine, &MpvEngine::loadFailed, this, [this](const QString &reason) {
            // The picture would not open: keep the song, drop the picture.
            if (abandonVideo(QStringLiteral("This video would not play — back to audio")))
                return;
            // Whichever way this goes: the only place the player's own reason
            // is written down.
            if (m_streamTier >= 0) {
                qWarning("playback: %s would not play from %s: %s", qPrintable(m_streamVideoId),
                         qPrintable(m_streamIsVideo ? QStringLiteral("yt-dlp · video") : tierLabel(m_streamTier)),
                         qPrintable(reason));
            }
            // The song's sound is JioSaavn's, but what failed was the video
            // laid over it, which is YouTube's: the match stands, and the
            // sound comes back from it at the same second.
            if (m_streamTier == StreamResolver::TierJioSaavn && m_streamIsVideo && m_resolver) {
                m_streamTier = -1;
                m_streamIsVideo = false;
                m_resolver->invalidate(m_streamVideoId);
                m_pendingVideoId = m_streamVideoId;
                m_resumeAt = m_position;
                setStatus(QStringLiteral("Refreshing the source…"), QString(), true);
                m_resolver->resolveTrack(saavnTarget(m_currentTrack));
                return;
            }
            // JioSaavn's link refused: the match is forgotten, and YouTube's
            // ladder takes the song from the top, from the second it had
            // reached. The ladder most likely has its link already: its half
            // of the race carried on into the cache, and a remembered match
            // is played with YouTube's link fetched beside it.
            if (m_streamTier == StreamResolver::TierJioSaavn && m_resolver) {
                if (!m_rescueClock.isValid())
                    m_rescueClock.start();
                m_streamTier = -1;
                m_refusedTiers.insert(StreamResolver::TierJioSaavn);
                m_resolver->refuseSaavn(m_streamVideoId);
                m_pendingVideoId = m_streamVideoId;
                m_resumeAt = m_position;
                qInfo("playback: %s back to YouTube from %lld ms", qPrintable(m_pendingVideoId),
                      static_cast<long long>(m_resumeAt));
                setStatus(QStringLiteral("Trying another source…"), QString(), true);
                m_resolver->resolve(m_pendingVideoId);
                return;
            }
            // A downloaded file, or the row's own file, that mpv will not
            // open (damaged, cut short, replaced by something else): the song
            // is streamed instead, from where it was, and its file is left
            // alone for the rest of this track. Stopping there, as it did,
            // ended the listening over one bad file. With nothing to stream
            // it from, it is passed over as a song that will not resolve is.
            if (!m_localPath.isEmpty()) {
                const QString file = std::exchange(m_localPath, QString());
                m_localRefused = true;
                qWarning("playback: the file %s would not play: %s", qUtf8Printable(QDir::toNativeSeparators(file)),
                         qPrintable(reason));
                const QString videoId = currentSourceId();
                if (!videoId.isEmpty() && m_resolver) {
                    if (!m_rescueClock.isValid())
                        m_rescueClock.start();
                    m_pendingVideoId = videoId;
                    m_resumeAt = m_position;
                    qInfo("playback: streaming %s instead of its file, from %lld ms", qPrintable(videoId),
                          static_cast<long long>(m_resumeAt));
                    setStatus(QStringLiteral("Resolving source…"), QString(), true);
                    m_resolver->resolveTrack(saavnTarget(m_currentTrack));
                    return;
                }
                failTrack(reason);
                return;
            }
            if (retryRefused())
                return;
            m_streamTier = -1;
            // Every link refused: passed over as a song that will not resolve
            // is, and counted toward the three in a row that stop the queue,
            // rather than ending the listening here.
            failTrack(reason);
        });

        m_engine->setVolume(m_volume);

        connect(m_engine, &MpvEngine::audioDevicesChanged, this, &PlaybackController::applyAudioDevice);

        // What mpv says it is playing, which fills in once the sound starts.
        connect(m_engine, &MpvEngine::streamInfoChanged, this, [this]() {
            logStreamInfo();
            Q_EMIT streamInfoChanged();
        });

        connect(m_engine, &MpvEngine::upgradeFinished, this, &PlaybackController::upgradeFinished);
        // A sound effect's speed (Slowed, Nightcore), for the media controls.
        connect(m_engine, &MpvEngine::playbackRateChanged, this, &PlaybackController::playbackRateChanged);
        m_align = new AudioAlign(this);
        connect(m_align, &AudioAlign::measured, this, &PlaybackController::saavnAligned);
    }
    applyAudioDevice();

    connect(&m_listen, &ListenTracker::listenStarted, this, &PlaybackController::listenStarted);
    connect(&m_listen, &ListenTracker::listenQualified, this, &PlaybackController::listenQualified);
    connect(&m_listen, &ListenTracker::listenResumed, this, &PlaybackController::listenResumed);

    m_volumeSave.setSingleShot(true);
    m_volumeSave.setInterval(kVolumeSaveDelayMs);
    connect(&m_volumeSave, &QTimer::timeout, this, &PlaybackController::saveVolume);

    m_queueSave.setSingleShot(true);
    m_queueSave.setInterval(kQueueSaveDelayMs);
    connect(&m_queueSave, &QTimer::timeout, this, &PlaybackController::saveQueue);
    // Every change there is: a list played, a song added, moved or taken
    // out, shuffle, and the queue moving on to its next song.
    connect(&m_queue, &QueueModel::countChanged, &m_queueSave, qOverload<>(&QTimer::start));
    connect(&m_queue, &QueueModel::upcomingChanged, &m_queueSave, qOverload<>(&QTimer::start));
    m_placeSave.setInterval(kPlaceSaveIntervalMs);
    connect(&m_placeSave, &QTimer::timeout, this, &PlaybackController::savePlace);
    m_placeSave.start();

    // The last song of a session is the one nothing else closes, and it is the
    // most recent thing the listener chose — exactly the event a recommender
    // would miss most.
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            closePlayEvent();
            // Now, with any closed a moment ago: there is no next turn of
            // the event loop to write them on.
            writePlayEvents();
            // A volume still settling as the app closes is written now
            // rather than lost with the timer.
            if (m_volumeSave.isActive()) {
                m_volumeSave.stop();
                saveVolume();
            }
            // And where the listener was, for the next launch to open on.
            if (m_queueSave.isActive()) {
                m_queueSave.stop();
                saveQueue();
            }
            savePlace();
        });
    }

    if (m_resolver) {
        connect(m_resolver, &StreamResolver::resolved, this, &PlaybackController::handleResolved);
        connect(m_resolver, &StreamResolver::failed, this, &PlaybackController::handleResolveFailed);
        connect(m_resolver, &StreamResolver::videoResolved, this, &PlaybackController::handleVideoResolved);
        connect(m_resolver, &StreamResolver::saavnLateMatch, this, &PlaybackController::offerSaavnUpgrade);
        connect(m_resolver, &StreamResolver::videoFailed, this,
                [this](const QString &videoId, const QString &reason) {
                    if (videoId != m_videoPendingId)
                        return;
                    m_videoPendingId.clear();
                    // Back to sound alone, where it never stopped. The switch
                    // stays on for the songs to come (m_videoPreferred).
                    m_videoWanted = false;
                    endVideoStatus();
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
                    // Turned down in Search, or by an artist turned down there:
                    // "Not interested" holds for the radio too.
                    if (m_radioUnwanted && m_radioUnwanted(song.videoId, song.title, song.artist))
                        continue;
                    QueueTrack track;
                    track.videoId = song.videoId;
                    track.title = song.title;
                    track.artist = song.artist;
                    track.album = song.album;
                    track.artwork = song.artwork;
                    track.durationMs = song.durationMs;
                    track.primaryArtist = song.primaryArtist;
                    track.credits = InnerTube::creditsToVariant(song.credits);
                    track.albumId = song.albumId;
                    track.isVideo = song.isVideo;
                    track.fromRadio = true;
                    additions.append(track);
                    if (additions.size() >= kRadioBatch)
                        break;
                }
                m_queue.insert(m_queue.rowCount(), additions);
                qInfo("autoplay: %d songs added after %s", int(additions.size()), qPrintable(seed));

                if (m_waitingForRadio) {
                    m_waitingForRadio = false;
                    if (m_queue.upcomingCount() > 0) {
                        playIndex(m_queue.currentIndex() + 1);
                    } else {
                        haltPlayback();
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
                    haltPlayback();
                    setStatus(QStringLiteral("End of the queue"), QString(), false);
                }
            });
}

// A listen closed just before the controller goes (a self-test's player at
// the end of its scope) is still written.
PlaybackController::~PlaybackController()
{
    writePlayEvents();
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

// Straight into the members rather than through the setters: those write the
// value back, and setShuffle would shuffle a queue that has not been loaded
// yet. A value never saved, or one that does not read, keeps its default.
void PlaybackController::restoreSettings()
{
    if (!m_library)
        return;

    bool ok = false;
    const qreal volume = m_library->settingValue(kVolumeKey).toDouble(&ok);
    if (ok) {
        m_volume = qBound(0.0, volume, 1.0);
        if (m_engine)
            m_engine->setVolume(m_volume);
        Q_EMIT volumeChanged();
    }

    const QString shuffle = m_library->settingValue(kShuffleKey);
    if (!shuffle.isEmpty()) {
        m_shuffle = shuffle == QLatin1String("1");
        Q_EMIT shuffleChanged();
    }

    const int repeat = m_library->settingValue(kRepeatKey).toInt(&ok);
    if (ok && repeat >= RepeatOff && repeat <= RepeatOne) {
        m_repeatMode = repeat;
        Q_EMIT repeatModeChanged();
    }

    const QString autoplay = m_library->settingValue(kAutoplayKey);
    if (!autoplay.isEmpty()) {
        m_autoplay = autoplay != QLatin1String("0");
        Q_EMIT autoplayChanged();
    }

    m_levelLoudness = m_library->settingValue(kLevelLoudnessKey) != QLatin1String("0");
    Q_EMIT levelLoudnessChanged();
    applyLoudness();

    // JioSaavn only once the listener picks High sound quality: a setting
    // never written reads as off. Its Indian headers, which matter only then,
    // are on unless turned off.
    if (m_resolver) {
        m_resolver->setSaavnEnabled(m_library->settingValue(kSaavnKey) == QLatin1String("1"));
        m_resolver->setSaavnIndiaHeaders(m_library->settingValue(kSaavnIndiaKey) != QLatin1String("0"),
                                         /*forgetNoMatches=*/false);
        // The move mid-song, on unless turned off; and only ever with High.
        m_saavnUpgrade = m_library->settingValue(kSaavnUpgradeKey) != QLatin1String("0");
        Q_EMIT saavnChanged();
    }

    // The recovery ladder's switches back, none of them in Settings: each
    // puts one part of it back as it was, should the new way misbehave
    // somewhere. Said in the log when set, since they change what it shows.
    m_freshLinkFirst = m_library->settingValue(kRefusedKey) != QLatin1String("muxed");
    m_earlyEndCheck = m_library->settingValue(kEarlyEndKey) != QLatin1String("next");
    const bool keepRescueLinks = m_library->settingValue(kRescueLinkKey) == QLatin1String("keep");
    if (m_resolver)
        m_resolver->setKeepRescueLinks(keepRescueLinks);
    if (!m_freshLinkFirst)
        qInfo("playback: playback.refused=muxed: a refused InnerTube link goes to its muxed stream, as before");
    if (keepRescueLinks)
        qInfo("playback: playback.rescue_link=keep: a rescue link is kept as the song's link, as before");
    if (!m_earlyEndCheck)
        qInfo("playback: playback.early_end=next: a stream that ends early is taken as the end, as before");

    // Used as soon as mpv has said which devices are there; until then, and
    // for as long as the device is not among them, Auto plays.
    const QString device = m_library->settingValue(kAudioDeviceKey);
    if (!device.isEmpty()) {
        m_audioChoice = device;
        m_audioChoiceName = m_library->settingValue(kAudioDeviceNameKey);
        if (!m_audioChoiceName.isEmpty())
            m_deviceNames.insert(m_audioChoice, m_audioChoiceName);
        applyAudioDevice();
    }
}

void PlaybackController::saveSetting(const QString &key, const QString &value)
{
    if (m_library)
        m_library->setSetting(key, value);
}

void PlaybackController::saveVolume()
{
    saveSetting(kVolumeKey, QString::number(m_volume, 'f', 3));
}

namespace {

// Which song a saved place belongs to: its id, or its file for a row with
// none, or its title for a row with neither.
QString placeKey(const QVariantMap &track)
{
    for (const char *key : { "sourceId", "sourceUrl", "title" }) {
        const QString value = track.value(QLatin1String(key)).toString();
        if (!value.isEmpty())
            return value;
    }
    return {};
}

QVariantMap settingObject(Library *library, const QString &key)
{
    return QJsonDocument::fromJson(library->settingValue(key).toUtf8()).object().toVariantMap();
}

QString compactJson(const QVariantMap &map)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

} // namespace

void PlaybackController::saveQueue()
{
    if (!m_library)
        return;
    QVariantMap snapshot = m_queue.snapshot(kQueueKeptRows);
    // An empty queue is written as nothing: the next launch opens as a
    // first one does.
    if (!snapshot.isEmpty())
        snapshot.insert(QStringLiteral("origin"), m_source);
    saveSetting(kQueueKey, snapshot.isEmpty() ? QString() : compactJson(snapshot));
}

// Kept apart from the queue, which can be long: this is the part that moves
// while a song plays, and is small.
void PlaybackController::savePlace()
{
    if (!m_library || m_currentTrack.isEmpty())
        return;
    const QString place = compactJson({ { QStringLiteral("key"), placeKey(m_currentTrack) },
                                        { QStringLiteral("ms"), qMax<qint64>(0, m_position) } });
    if (place == m_placeSaved)
        return;
    m_placeSaved = place;
    saveSetting(kPlaceKey, place);
}

void PlaybackController::saveSession()
{
    m_queueSave.stop();
    saveQueue();
    savePlace();
}

bool PlaybackController::restoreSession()
{
    if (!m_library)
        return false;
    const QVariantMap saved = settingObject(m_library, kQueueKey);
    if (saved.isEmpty() || !m_queue.restoreSnapshot(saved))
        return false;
    // A queue of its own, as startQueue makes one, but as it was: in the
    // order it was left, shuffled or not, and not shuffled again.
    ++m_queueGeneration;
    m_consecutiveFailures = 0;
    m_source = saved.value(QStringLiteral("origin")).toString();

    // The place, if it is this song's and not at either end of it. A place
    // written for the song after, by a launch that ended before the queue
    // was written, is some other song's and is left alone.
    const QueueTrack *track = m_queue.current();
    const QVariantMap place = settingObject(m_library, kPlaceKey);
    qint64 at = 0;
    if (track && place.value(QStringLiteral("key")).toString() == placeKey(track->toMap()))
        at = place.value(QStringLiteral("ms")).toLongLong();
    if (at < kPlaceMinMs || (track && track->durationMs > 0 && at > track->durationMs - kPlaceTailMs))
        at = 0;

    qInfo("session: the queue back as it was left, %d songs, at song %d, from %lld ms",
          int(m_queue.rowCount()), m_queue.currentIndex() + 1, static_cast<long long>(at));
    m_openAt = at;
    beginCurrent(/*autoPlay=*/false);
    m_openAt = 0;
    // Written as read, until either moves.
    m_placeSaved = compactJson(place);
    m_queueSave.stop();
    return true;
}

bool PlaybackController::saavnEnabled() const
{
    return m_resolver && m_resolver->saavnEnabled();
}

// From the next song on: the one playing keeps the sound it has. So a move to
// JioSaavn under way for it stops, whatever stage it has reached (the two
// copies being compared, the new link opening, or the crossfade), and the
// song stays on YouTube: on Standard nothing more is fetched from JioSaavn.
void PlaybackController::setSaavnEnabled(bool on)
{
    if (!m_resolver || on == m_resolver->saavnEnabled())
        return;
    m_resolver->setSaavnEnabled(on);
    if (!on) {
        if (m_align)
            m_align->cancel();
        if (!m_upgradeProbe.videoId.isEmpty() || !m_upgradeOffer.videoId.isEmpty())
            qInfo("upgrade: %s stays on YouTube: JioSaavn was turned off (Standard sound quality)",
                  qPrintable(!m_upgradeProbe.videoId.isEmpty() ? m_upgradeProbe.videoId : m_upgradeOffer.videoId));
        m_upgradeProbe = {};
        m_upgradeOffer = {};
        if (!m_upgradeVideoId.isEmpty() && m_engine)
            m_engine->cancelUpgrade(QStringLiteral("JioSaavn was turned off (Standard sound quality)"));
    }
    saveSetting(kSaavnKey, on ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT saavnChanged();
}

bool PlaybackController::saavnIndiaHeaders() const
{
    return m_resolver && m_resolver->saavnIndiaHeaders();
}

void PlaybackController::setSaavnIndiaHeaders(bool on)
{
    if (!m_resolver || on == m_resolver->saavnIndiaHeaders())
        return;
    m_resolver->setSaavnIndiaHeaders(on);
    saveSetting(kSaavnIndiaKey, on ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT saavnChanged();
}

// From the next late answer on. One already under way finishes: turning the
// switch off does not move the song back.
void PlaybackController::setLevelLoudness(bool on)
{
    if (on == m_levelLoudness)
        return;
    m_levelLoudness = on;
    saveSetting(kLevelLoudnessKey, on ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT levelLoudnessChanged();
    // The song playing too, at once.
    m_loudnessLogged.clear();
    applyLoudness();
}

double PlaybackController::playbackRate() const
{
    return m_engine ? m_engine->playbackRate() : 1.0;
}

void PlaybackController::applyLoudness()
{
    if (!m_engine)
        return;
    const QString videoId = currentSourceId();
    const double db = Loudness::of(videoId);
    const double gain = Loudness::gainFor(db);
    m_engine->setLevelling(m_levelLoudness, gain);
    // Once a song, when it is known: the one line that says how loud the
    // song was measured and what was done about it.
    if (!videoId.isEmpty() && !qIsNaN(db) && m_loudnessLogged != videoId) {
        m_loudnessLogged = videoId;
        qInfo("loudness: %s measured %+.1f dB, %s", qPrintable(videoId), db,
              !m_levelLoudness ? "levelling is off"
              : gain < 0.0     ? qPrintable(QStringLiteral("played %1 dB quieter").arg(-gain, 0, 'f', 1))
                               : "played as it is (never turned up)");
    }
}

void PlaybackController::setSaavnUpgrade(bool on)
{
    if (on == m_saavnUpgrade)
        return;
    m_saavnUpgrade = on;
    saveSetting(kSaavnUpgradeKey, on ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT saavnChanged();
}

void PlaybackController::setAudioDevice(const QString &name)
{
    const QString chosen = name.isEmpty() ? kAutoDevice : name;
    // What the system calls it, kept with the choice so the menu can still
    // say which device it was once it has been unplugged.
    const QString description = chosen == kAutoDevice ? QString() : m_deviceNames.value(chosen);
    if (chosen != m_audioChoice || description != m_audioChoiceName) {
        m_audioChoice = chosen;
        m_audioChoiceName = description;
        saveSetting(kAudioDeviceKey, chosen);
        saveSetting(kAudioDeviceNameKey, description);
    }
    applyAudioDevice();
}

void PlaybackController::applyAudioDevice()
{
    const QVariantList found = m_engine ? m_engine->audioDevices() : QVariantList();
    // mpv always lists "auto", so nothing at all means it has not looked yet,
    // and a choice missing from nothing is not missing.
    const bool known = !found.isEmpty();

    QVariantList devices;
    devices.append(QVariantMap{ { QStringLiteral("name"), kAutoDevice },
                                { QStringLiteral("description"), QStringLiteral("Auto · system default") },
                                { QStringLiteral("missing"), false } });

    // mpv lists the devices of every sound driver it was built with, so on
    // Windows the same speakers can come again through OpenAL or SDL. Only
    // the first driver's are offered — mpv's own first choice, WASAPI on
    // Windows and CoreAudio on a Mac, which is also what Auto plays through.
    QString driver;
    bool choiceHere = m_audioChoice == kAutoDevice;
    for (const QVariant &value : found) {
        const QVariantMap device = value.toMap();
        const QString name = device.value(QStringLiteral("name")).toString();
        const qsizetype slash = name.indexOf(QLatin1Char('/'));
        if (name == kAutoDevice || slash <= 0)
            continue;
        if (driver.isEmpty())
            driver = name.left(slash);
        if (name.left(slash) != driver)
            continue;
        QString description = device.value(QStringLiteral("description")).toString();
        if (description.isEmpty())
            description = name.mid(slash + 1);
        m_deviceNames.insert(name, description);
        devices.append(QVariantMap{ { QStringLiteral("name"), name },
                                    { QStringLiteral("description"), description },
                                    { QStringLiteral("missing"), false } });
        if (name == m_audioChoice)
            choiceHere = true;
    }
    // Chosen before and unplugged since, or on another computer: still the
    // choice, shown so the listener can see why Auto is playing instead.
    if (known && !choiceHere) {
        devices.append(QVariantMap{ { QStringLiteral("name"), m_audioChoice },
                                    { QStringLiteral("description"),
                                      m_audioChoiceName.isEmpty() ? m_audioChoice : m_audioChoiceName },
                                    { QStringLiteral("missing"), true } });
    }

    const QString use = choiceHere ? m_audioChoice : kAutoDevice;
    if (devices == m_audioDevices && use == m_audioDevice)
        return;
    if (known && !choiceHere) {
        qInfo("audio: %s is not connected; Auto plays until it is",
              qPrintable(m_audioChoiceName.isEmpty() ? m_audioChoice : m_audioChoiceName));
    }
    m_audioDevices = devices;
    if (use != m_audioDevice) {
        m_audioDevice = use;
        if (m_engine)
            m_engine->setAudioDevice(use);
        qInfo("audio: playing through %s", qPrintable(use));
    }
    Q_EMIT audioDevicesChanged();
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
    const bool sourceShown = !m_sourceLabel.isEmpty();
    m_statusText = text;
    m_sourceLabel = source;
    m_resolving = resolving;
    m_statusError = error;
    if (changed)
        Q_EMIT statusChanged();
    // The stream-info line stands only while the status names a source.
    if (sourceShown != !m_sourceLabel.isEmpty())
        Q_EMIT streamInfoChanged();
}

QString PlaybackController::streamInfo() const
{
    // Nothing while the status line names no source (resolving, failed, the
    // queue run out), whatever was loaded last.
    if (m_soundOrigin.isEmpty() || m_sourceLabel.isEmpty())
        return {};
    QStringList parts{ m_soundOrigin };
    if (m_engine) {
        const MpvEngine::StreamInfo sound = m_engine->streamInfo();
        if (!sound.codec.isEmpty())
            parts << codecName(sound.codec);
        if (sound.sampleRate > 0) {
            parts << (sound.outputRate > 0 && sound.outputRate != sound.sampleRate
                          ? QStringLiteral("%1 → %2 kHz").arg(kiloHertz(sound.sampleRate), kiloHertz(sound.outputRate))
                          : QStringLiteral("%1 kHz").arg(kiloHertz(sound.sampleRate)));
        }
        if (sound.kbps > 0)
            parts << QStringLiteral("%1 kbps").arg(sound.kbps);
    }
    return parts.join(QStringLiteral(" · "));
}

void PlaybackController::setSoundOrigin(const QString &origin)
{
    // A new load, whatever it is: its line is still to be written.
    m_streamLogged = false;
    if (origin == m_soundOrigin)
        return;
    m_soundOrigin = origin;
    Q_EMIT streamInfoChanged();
}

// One line per file, once mpv has said what it decodes and what the device
// was opened with: the instrument the engine work is measured by (did the
// Opus stream arrive, is it resampled, at what gain). mpv -v says the same
// in its ao and swresample lines, spread over dozens more.
void PlaybackController::logStreamInfo()
{
    if (m_streamLogged || m_soundOrigin.isEmpty() || !m_engine)
        return;
    const MpvEngine::StreamInfo s = m_engine->streamInfo();
    if (s.codec.isEmpty() || s.sampleRate <= 0 || s.outputRate <= 0 || s.output.isEmpty())
        return;
    m_streamLogged = true;
    const QString videoId = currentSourceId();
    const QString bitrate = s.kbps <= 0 ? QStringLiteral("bitrate not measured yet")
                          : QStringLiteral("%1 kbps (%2)").arg(s.kbps).arg(
                                s.kbpsIsFileAverage ? QStringLiteral("file average")
                                : s.kbpsIsDeclared  ? QStringLiteral("as the file declares it")
                                                    : QStringLiteral("measured"));
    const QString resampling = s.outputRate != s.sampleRate
                                   ? QStringLiteral("resampled %1 -> %2 Hz").arg(s.sampleRate).arg(s.outputRate)
                                   : QStringLiteral("not resampled");
    // UTF-8 throughout: the origin has a middle dot, which the message
    // handler would otherwise get in the system's code page and print as "?".
    qInfo("stream: %s from %s: %s %d Hz %s %s, %s -> %s %d Hz %s %s, %s, volume %s",
          qUtf8Printable(videoId.isEmpty() ? m_currentTrack.value(QStringLiteral("title")).toString() : videoId),
          qUtf8Printable(m_soundOrigin), qUtf8Printable(s.codec), s.sampleRate, qUtf8Printable(s.channels),
          qUtf8Printable(s.sampleFormat), qUtf8Printable(bitrate), qUtf8Printable(s.output), s.outputRate,
          qUtf8Printable(s.outputChannels), qUtf8Printable(s.outputFormat), qUtf8Printable(resampling),
          qUtf8Printable(s.volume >= 0.0 ? QStringLiteral("%1%").arg(s.volume, 0, 'f', 0) : QStringLiteral("unknown")));
}

void PlaybackController::setPlayingFlag(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    m_listen.setPlaying(playing);
    Q_EMIT playingChanged();
}

// Stopped with nothing more on its way: a track that will not play, or a queue
// that ran out. mpv is paused along with the flag. The flag follows mpv's pause
// property, and a file that failed or ended leaves mpv idle but unpaused; were
// only the flag cleared, the next load's "play" would change nothing in mpv,
// no pause change would ever come back, and the bar would show Play over
// music that Play/Pause could then not pause.
void PlaybackController::haltPlayback()
{
    setPlayingFlag(false);
    if (engineAvailable())
        m_engine->setPaused(true);
}

void PlaybackController::setDuration(qint64 ms)
{
    // Told even when unchanged: a new listen starts from the track's listed
    // length, and mpv's own may be the same number.
    m_listen.setDuration(ms);
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
    // The recommender's record of this is written by Library::setLiked, where
    // every heart in the app arrives — not here, where only this one did.
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
    ++m_queueGeneration;
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
            // One line a request: the log is the only place that says where
            // the radio took over, and from which song.
            qInfo("autoplay: asking for songs like \"%s\" (%s), %d in the queue",
                  qPrintable(track->title), qPrintable(m_radioSeed), int(m_queue.rowCount()));
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
                                    const QString &origin,
                                    const QString &primaryArtist)
{
    if (videoId.isEmpty())
        return;
    m_source = origin;
    QueueTrack track;
    track.videoId = videoId;
    track.title = title;
    track.artist = artist;
    track.primaryArtist = primaryArtist;
    track.album = album;
    track.durationMs = durationMs;
    track.isVideo = isVideo;
    track.artwork = artwork.isEmpty() ? artworkForSource(videoId) : artwork;
    startQueue({ track }, 0, /*autoPlay=*/true);
}

// Both of these are the listener's own choice, whatever the song was before:
// a copy of something autoplay added (the player bar's menu on the song
// playing, a queue row's) must not arrive marked as autoplay's, or it would
// sit after the AUTOPLAY heading, be recorded as a radio play and go with the
// next "Don't suggest".
void PlaybackController::playNext(const QVariantMap &map)
{
    QueueTrack track = QueueTrack::fromMap(map);
    track.fromRadio = false;
    if (track.videoId.isEmpty() && track.sourceUrl.isEmpty())
        return;
    if (m_queue.rowCount() == 0) {
        startQueue({ track }, 0, /*autoPlay=*/true);
        return;
    }
    m_queue.insert(m_queue.currentIndex() + 1, { track });
    prefetchUpcoming();
    // Said, since with the queue closed nothing else shows it worked.
    Q_EMIT notice(track.title.isEmpty() ? QStringLiteral("Playing next")
                                        : QStringLiteral("Playing next: %1").arg(track.title));
}

void PlaybackController::addToQueue(const QVariantMap &map)
{
    addAllToQueue({ map });
}

void PlaybackController::addAllToQueue(const QVariantList &maps)
{
    QList<QueueTrack> tracks;
    tracks.reserve(maps.size());
    for (const QVariant &map : maps) {
        QueueTrack track = QueueTrack::fromMap(map.toMap());
        track.fromRadio = false;
        if (!track.videoId.isEmpty() || !track.sourceUrl.isEmpty())
            tracks.append(track);
    }
    if (tracks.isEmpty())
        return;
    const QString said = tracks.size() == 1
        ? (tracks.first().title.isEmpty() ? QStringLiteral("Added to the queue")
                                          : QStringLiteral("Added to the queue: %1").arg(tracks.first().title))
        : QStringLiteral("Added %1 songs to the queue").arg(tracks.size());
    if (m_queue.rowCount() == 0) {
        startQueue(tracks, 0, /*autoPlay=*/false);
        Q_EMIT notice(said);
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
    m_queue.insert(row, tracks);
    prefetchUpcoming();
    Q_EMIT notice(said);
}

void PlaybackController::removeFromQueue(int index)
{
    m_queue.removeAt(index);
    prefetchUpcoming();
}

// Only among what is still to come: what has played stays where it was, and
// the song playing is not moved out from under itself. The next song may be
// another one now, so it is the one fetched ahead.
void PlaybackController::moveInQueue(int from, int to)
{
    const int first = m_queue.currentIndex() + 1;
    if (from < first || to < first)
        return;
    if (m_queue.move(from, to))
        prefetchUpcoming();
}

void PlaybackController::pruneRadio()
{
    if (!m_radioUnwanted)
        return;
    bool removed = false;
    for (int row = m_queue.rowCount() - 1; row > m_queue.currentIndex(); --row) {
        const QueueTrack *track = m_queue.at(row);
        if (track && track->fromRadio && m_radioUnwanted(track->videoId, track->title, track->artist)) {
            m_queue.removeAt(row);
            removed = true;
        }
    }
    if (removed)
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
    // With the switch on, the next song's picture too, so it is there as
    // that song begins. A downloaded song still streams its picture.
    if (m_videoPreferred && upcoming->isVideo)
        m_resolver->resolveVideo(upcoming->videoId, m_videoHeight);
    if (m_downloads && !m_downloads->localPathFor(upcoming->videoId).isEmpty())
        return;
    // JioSaavn too, so the next song finds its answer already there.
    m_resolver->prefetchTrack(saavnTarget(upcoming->toMap()));
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
    // A video stays one: replayed from somewhere that does not know it is
    // (a download, a suggestion), it keeps the picture History offers.
    QSqlQuery recent(AppDatabase::connection());
    recent.prepare(QStringLiteral(
        "INSERT INTO recent (video_id, title, artist, album, artwork, duration_ms, is_video)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(video_id) DO UPDATE SET"
        "   title = excluded.title, artist = excluded.artist, album = excluded.album,"
        "   artwork = excluded.artwork, duration_ms = excluded.duration_ms,"
        "   is_video = MAX(is_video, excluded.is_video),"
        "   played_at = datetime('now'), play_count = play_count + 1"));
    recent.addBindValue(videoId);
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
    recent.addBindValue(AppDatabase::text(track.value(QStringLiteral("artwork")).toString()));
    recent.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    recent.addBindValue(track.value(QStringLiteral("isVideo")).toBool() ? 1 : 0);
    if (!recent.exec())
        qWarning("Monolist: could not record a play: %s", qPrintable(recent.lastError().text()));
}

// ------------------------------------------------------------- play events

// A listen begins when a track someone wants to hear has its sound on the way:
// as it is loaded to play, for one started with Play, or at the first press of
// Play for one that was only loaded; for one restarted with Previous, a few
// seconds into hearing it again. Not when it is asked for — a track that
// never resolves was never heard, and a play in History and a skip in the
// taste profile would both be false. Either way it is written down once.
void PlaybackController::startListening()
{
    if (!m_listenPending || m_currentTrack.isEmpty() || m_replay != Replay::None)
        return;
    m_listenPending = false;
    recordHistory(m_currentTrack);
    openPlayEvent(m_currentTrack);
    Q_EMIT playRecorded();
}

void PlaybackController::openPlayEvent(const QVariantMap &track)
{
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    const QString title = track.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        return;                       // nothing a recommender could match on

    // Where this play came from, which the profile weights by. A track the
    // radio added was not chosen by anyone, so it is "radio" no matter which
    // surface the queue began on, and weighs as little as a queue running on.
    // The row's own flag says so; QueueModel::radioStartIndex cannot, since
    // it only looks ahead of the song playing, never at it.
    const bool fromRadio = track.value(QStringLiteral("fromRadio")).toBool();
    const QString source = fromRadio ? QStringLiteral("radio") : m_source;

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
void PlaybackController::closePlayEvent(bool restarting)
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
    // A song restarted with Previous is not one returned to later in the
    // sitting, which is what the upgrade above rewards: counted as one, a
    // restart then skipped a few seconds in would read as a full listen.
    if (!m_playEventKey.isEmpty() && !restarting)
        m_finalisedThisSession.insert(m_playEventKey);

    // The duration is written with the rest, not when the event opened: a
    // track picked from search carries no length until mpv has opened the
    // stream and said so, and a length of zero would make every ratio above
    // meaningless.
    //
    // Written on the next turn of the event loop rather than here: whoever
    // closes a listen is on the way to starting something (the next song, the
    // same one again), which should not wait on the database. Everything is
    // taken now, while the clock still belongs to the song being left, so a
    // song that then fails to resolve cannot change it; a quit writes it at
    // once (writePlayEvents).
    m_playEventWrites.append({ id, total, listened, completed, skipped, label });
    if (m_playEventWrites.size() == 1)
        QTimer::singleShot(0, this, &PlaybackController::writePlayEvents);
}

void PlaybackController::writePlayEvents()
{
    const QList<PlayEventClose> writes = std::exchange(m_playEventWrites, {});
    for (const PlayEventClose &close : writes) {
        QSqlQuery finish(AppDatabase::connection());
        finish.prepare(QStringLiteral(
            "UPDATE play_events SET track_ms = ?, listened_ms = ?, completed = ?, skipped = ?,"
            " label = ? WHERE id = ?"));
        finish.addBindValue(close.trackMs);
        finish.addBindValue(close.listenedMs);
        finish.addBindValue(close.completed ? 1 : 0);
        finish.addBindValue(close.skipped ? 1 : 0);
        finish.addBindValue(close.label);
        finish.addBindValue(close.id);
        if (!finish.exec())
            qWarning("Monolist: could not close a play event: %s", qPrintable(finish.lastError().text()));
    }
}

// Everything that decides *where* audio comes from lives here; the rest of the
// class only cares that something is playing.
void PlaybackController::beginTrack(const QVariantMap &track, bool autoPlay)
{
    // First, while m_position still belongs to the track being left: how much
    // of it was heard is the single most useful thing the recommender gets,
    // and it exists only in this instant.
    closePlayEvent();
    // Anything but the song a launch opens on starts from the top.
    const qint64 openAt = std::exchange(m_openAt, 0);

    if (m_resolver && !m_pendingVideoId.isEmpty())
        m_resolver->cancel(m_pendingVideoId);
    m_pendingVideoId.clear();
    m_streamTier = -1;
    m_streamIsVideo = false;
    m_refusedTiers.clear();
    m_homeTier = -1;
    m_rescueTier = -1;
    m_innerTubeAskedAgain = false;
    m_earlyEndResumed = false;
    m_upgradeTried = false;
    m_upgradeOffer = {};
    m_upgradeProbe = {};
    if (m_align)
        m_align->cancel();
    m_streamUrl.clear();
    m_streamHeaders.clear();
    m_rescueClock.invalidate();
    m_localPath.clear();
    m_directUrl.clear();
    m_localRefused = false;
    m_resumeAt = openAt;
    // Whatever rescued the last play, this one starts from the top: a song
    // played again is Opus again, not the last play's itag 18. Unless
    // googlevideo refused its InnerTube links lately, which the resolver
    // remembers: then the rescue link is kept, and is what it plays.
    if (m_resolver)
        m_resolver->dropRescueLinks();

    // The song being left stops now, not when the next one has resolved.
    // Until then it would play on under the new title, and the engine passes
    // on nothing more from it: not its clock, and not an ending that would
    // skip the new track or an error that would be blamed on it.
    if (engineAvailable())
        m_engine->stop();
    setSoundOrigin(QString());

    // Every track starts as sound. Its picture follows only if the listener
    // left the switch on (m_videoPreferred) and the song has one: asked for
    // beside the sound, below, and joined to it once that has loaded.
    if (!m_videoPendingId.isEmpty() && m_resolver)
        m_resolver->cancelVideo(m_videoPendingId);
    m_videoPendingId.clear();
    m_pictureAwaitingSound = {};
    m_videoUnproven = false;
    m_videoAdded = false;
    const bool hadVideo = m_videoWanted || m_videoPlaying;
    m_videoWanted = false;
    m_videoPlaying = false;
    if (hadVideo && m_engine)
        m_engine->setVideoEnabled(false);

    m_currentTrack = track;
    // Set once, for the new song, so what shows the picture sees the switch
    // stay on across the change rather than go off and on again.
    m_videoWanted = m_videoPreferred && videoAvailable();
    if (hadVideo || m_videoWanted)
        Q_EMIT videoChanged();

    // Library rows written before artwork was captured still have a source id;
    // derive the thumbnail rather than showing an empty plate.
    if (m_currentTrack.value(QStringLiteral("artwork")).toString().isEmpty()) {
        const QString derived = artworkForSource(currentSourceId());
        if (!derived.isEmpty())
            m_currentTrack.insert(QStringLiteral("artwork"), derived);
    }

    m_position = openAt;
    m_autoPlayAfterResolve = autoPlay;
    // As loud as it was measured, before a file of it loads here; a stream
    // is told again once its answer has said (handleResolved).
    applyLoudness();
    // A new listen for Last.fm, whether or not it will be heard: repeat-one
    // arrives here too, and is a listen of its own. A song autoplay added was
    // not chosen by anyone, which Last.fm is told. The queue row's own flag
    // is read, not QueueModel::radioStartIndex, which only looks ahead of
    // the current song.
    m_listen.begin(m_currentTrack, !m_currentTrack.value(QStringLiteral("fromRadio")).toBool());
    setDuration(track.value(QStringLiteral("durationMs")).toLongLong());

    // Recorded below once there is something to play, and for a stream once
    // it resolves (handleResolved). A track only loaded — the one a launch
    // opens on, or one reached with Next while paused — is a listen once Play
    // is pressed (see play()).
    m_listenPending = true;
    m_replay = Replay::None;

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

    // The switch left on: the picture is asked for now, beside the sound,
    // rather than after it, so it is ready sooner.
    if (m_videoWanted && m_resolver && !videoId.isEmpty()) {
        m_videoPendingId = videoId;
        m_resolver->resolveVideo(videoId, m_videoHeight);
    }

    // 1 — a downloaded copy, or any source that is already a local file.
    const QString localPath = localCopyOf(track);
    // mpv first here too (see handleResolved), and nothing said of a file it
    // refused outright: loadFailed has said that. Which file it is, first,
    // so that loadFailed can stream the song instead of it.
    if (!localPath.isEmpty()) {
        m_localPath = localPath;
        m_resumeAt = 0;
        if (!m_engine->load(localPath, autoPlay, QString(), openAt))
            return;
        setStatus(QStringLiteral("Offline"), QStringLiteral("Local file"), false);
        setSoundOrigin(QStringLiteral("Offline · Local file"));
        if (autoPlay) {
            startListening();
            prefetchUpcoming();
        }
        return;
    }

    // 2 — resolve a stream. Nothing plays until the resolver answers.
    if (!videoId.isEmpty() && m_resolver) {
        m_pendingVideoId = videoId;
        setStatus(QStringLiteral("Resolving source…"), QString(), true);
        m_resolver->resolveTrack(saavnTarget(m_currentTrack));
        return;
    }

    // 3 — a plain remote URL stored on the row.
    const QString source = track.value(QStringLiteral("sourceUrl")).toString();
    if (!source.isEmpty()) {
        // A link on the network can stop short as a resolved one can
        // (resumeEarlyEnd), and is loaded again from where it stopped.
        const QString scheme = QUrl(source).scheme();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
            m_directUrl = source;
        m_resumeAt = 0;
        if (!m_engine->load(source, autoPlay, QString(), openAt))
            return;
        setStatus(QStringLiteral("Streaming"), QStringLiteral("Direct URL"), false);
        setSoundOrigin(QStringLiteral("Streaming · Direct URL"));
        if (autoPlay)
            startListening();
        return;
    }

    setStatus(QStringLiteral("No playable source"), QString(), false, /*error=*/true);
    if (autoPlay)
        Q_EMIT playbackError(QStringLiteral("This track has no local file and no source id."));
}

void PlaybackController::handleResolved(const QString &videoId, const QString &url, int tier,
                                        bool fromCache)
{
    if (videoId != m_pendingVideoId)
        return;                       // a later track superseded this one
    m_pendingVideoId.clear();

    const bool fromSaavn = tier == StreamResolver::TierJioSaavn;
    // m_consecutiveFailures is left as it is: a link is not yet a song
    // heard (see MpvEngine::audioStarted).
    m_streamVideoId = videoId;
    m_streamTier = tier;
    m_streamFromCache = fromCache;
    m_streamIsVideo = false;
    m_localPath.clear();
    m_directUrl.clear();
    // The rung this play began on, and whether this link is a rescue from
    // below it: as StreamResolver judged it, so the sound can go back to the
    // same link later in the play (backToSound).
    if (m_homeTier < 0 && !fromSaavn)
        m_homeTier = tier;
    m_rescueTier = !fromSaavn && tier != StreamResolver::TierInnerTube && tier != m_homeTier ? tier : -1;
    // mpv first: it opens the link on its own thread while everything below
    // is done on this one. The status line, the listen written down and Home
    // and History refreshed with it used to come first, and every song start
    // waited 40-60 ms for them before mpv was even asked, over half a second
    // once History held a few hundred songs (ROADMAP F35). A link some tiers
    // only serve to the client that asked for it (the muxed stream's) comes
    // with the headers to ask as; JioSaavn's CDN wants none, and must not be
    // sent a YouTube client's.
    const qint64 resumeAt = std::exchange(m_resumeAt, 0);
    // The answer that brought this link measured the song, where it was
    // YouTube's: level it by that before it loads.
    applyLoudness();
    const QVariantMap headers = m_resolver && !fromSaavn ? m_resolver->headersFor(videoId, url) : QVariantMap();
    m_streamUrl = url;
    m_streamHeaders = headers;
    if (!m_engine || !m_engine->load(url, m_autoPlayAfterResolve, QString(), resumeAt, headers))
        return;   // refused outright: loadFailed has already taken it elsewhere, and nothing was heard

    // JioSaavn's links name their bitrate, and that is the point of them, so
    // the label says it: "JioSaavn · 320 kbps".
    const int kbps = fromSaavn && m_resolver ? m_resolver->saavnKbps(videoId) : 0;
    setStatus(QStringLiteral("Streaming"),
              kbps > 0 ? QStringLiteral("JioSaavn · %1 kbps").arg(kbps) : tierLabel(tier), false);
    setSoundOrigin(QStringLiteral("Streaming · ") + tierLabel(tier));
    // Someone is waiting to hear it, and now it is on its way: a listen.
    // (Once only — a stream refreshed or re-resolved mid-song arrives here
    // again.)
    if (m_autoPlayAfterResolve)
        startListening();
    prefetchUpcoming();
}

void PlaybackController::handleResolveFailed(const QString &videoId, const QString &reason)
{
    if (videoId != m_pendingVideoId)
        return;
    m_pendingVideoId.clear();

    // Always in the log, whichever way this goes: it is the only place the
    // real cause is written down.
    qWarning("resolve failed for %s: %s", qPrintable(videoId), qPrintable(reason));
    failTrack(reason);
}

// JioSaavn said yes after YouTube had already begun. The song moves over
// where it is, if the copy is worth moving to and nothing about this play
// says not to: the listener's switch, the picture on, a file, a move already
// made or refused. The two copies are compared first (AudioAlign), then the
// engine gets JioSaavn's ready unheard and takes over without a gap, or
// leaves the song as it was.
void PlaybackController::offerSaavnUpgrade(const QString &videoId, const QString &url, int kbps,
                                           int durationSec)
{
    const auto stay = [&videoId](const QString &why) {
        qInfo("upgrade: %s stays on YouTube: %s", qPrintable(videoId), qUtf8Printable(why));
    };
    if (!m_resolver || !m_resolver->saavnEnabled() || !engineAvailable())
        return;
    if (!m_saavnUpgrade) {
        stay(QStringLiteral("moving mid-song is turned off (jiosaavn.upgrade=0)"));
        return;
    }
    if (videoId != currentSourceId()) {
        stay(QStringLiteral("it is no longer the song playing"));
        return;
    }
    if (m_upgradeTried || m_refusedTiers.contains(StreamResolver::TierJioSaavn)) {
        stay(QStringLiteral("it has had its one move, or JioSaavn's link was refused, this play"));
        return;
    }
    // Before the song has made a sound, or while it is being fetched again
    // after a refused link, there is nothing to judge by or to take over
    // from: weighed once it sounds. A song rescued by its 96 kbps muxed
    // stream is exactly the one to move.
    if (!m_pendingVideoId.isEmpty() || !m_engine->hasAudioStarted()) {
        m_upgradeOffer = { videoId, url, kbps, durationSec };
        qInfo("upgrade: %s: JioSaavn's match came before YouTube's sound had started; "
              "it is weighed once that has", qPrintable(videoId));
        return;
    }
    if (videoId != m_streamVideoId || m_streamTier < 0 || m_streamTier == StreamResolver::TierJioSaavn) {
        stay(QStringLiteral("it is not playing from a YouTube stream"));
        return;
    }
    if (m_videoPlaying || m_videoWanted || m_streamIsVideo) {
        stay(QStringLiteral("its video is on, and the picture comes with YouTube's sound"));
        return;
    }
    const MpvEngine::StreamInfo sound = m_engine->streamInfo();
    const int playing = sound.kbps > 0 ? sound.kbps : kUpgradeAssumedKbps;
    if (kbps <= 0 || kbps - playing < kUpgradeMinGainKbps) {
        stay(kbps <= 0 ? QStringLiteral("JioSaavn's link does not say its bitrate")
                       : QStringLiteral("JioSaavn's %1 kbps is not enough above the %2 kbps playing")
                             .arg(kbps).arg(playing));
        return;
    }
    const qint64 length = m_engine->duration();
    if (durationSec > 0 && length > 0 && qAbs(qint64(durationSec) * 1000 - length) > kUpgradeDriftMs) {
        stay(QStringLiteral("JioSaavn's copy is %1 s long and the stream playing %2 s")
                 .arg(durationSec).arg(length / 1000.0, 0, 'f', 1));
        return;
    }
    if (length < kAlignShortestMs) {
        stay(QStringLiteral("too short to compare the two copies"));
        return;
    }
    // The one attempt this play, from here on whatever becomes of it.
    m_upgradeTried = true;
    // Where the music sits in each file first: they are not always the same
    // file, and a move by the clock alone would skip or repeat the
    // difference.
    m_upgradeProbe = { videoId, url, kbps, durationSec };
    m_upgradeProbeStream = m_streamUrl;
    const AudioAlign::Source current{ m_streamUrl, m_streamHeaders };
    const AudioAlign::Source saavn{ url, QVariantMap() };
    if (!m_align || !m_align->measure(current, saavn, kAlignAtMs, kAlignRangeMs)) {
        m_upgradeProbe = {};
        stay(QStringLiteral("the two copies cannot be compared (FFmpeg, which does it, is not installed)"));
        return;
    }
    qInfo("upgrade: %s: JioSaavn's %d kbps copy arrived at %s of %s, over %s at %d kbps; comparing the two",
          qPrintable(videoId), kbps, qPrintable(TrackModel::formatDuration(m_position)),
          qPrintable(TrackModel::formatDuration(length)), qUtf8Printable(tierLabel(m_streamTier)), playing);
}

void PlaybackController::saavnAligned(bool ok, double offsetMs, double peak, const QString &detail)
{
    const UpgradeOffer offer = std::exchange(m_upgradeProbe, {});
    if (offer.videoId.isEmpty())
        return;
    const auto stay = [&offer](const QString &why) {
        qInfo("upgrade: %s stays on YouTube: %s", qPrintable(offer.videoId), qUtf8Printable(why));
    };
    // JioSaavn, and the move, still wanted: the listener may have picked
    // Standard, or turned the move off, while the two were compared.
    if (!m_resolver || !m_resolver->saavnEnabled() || !m_saavnUpgrade) {
        stay(QStringLiteral("JioSaavn, or moving to it mid-song, was turned off while the two copies were compared"));
        return;
    }
    // Still the same song, from the same stream, with nothing else begun.
    if (offer.videoId != currentSourceId() || m_streamUrl != m_upgradeProbeStream || !m_pendingVideoId.isEmpty()
        || m_streamTier < 0 || m_streamTier == StreamResolver::TierJioSaavn || m_videoPlaying || m_videoWanted
        || m_streamIsVideo) {
        stay(QStringLiteral("the song changed while the two copies were compared"));
        return;
    }
    if (!ok) {
        stay(QStringLiteral("the two copies could not be lined up: %1").arg(detail));
        return;
    }
    if (qAbs(offsetMs) > double(kUpgradeDriftMs)) {
        stay(QStringLiteral("the music is %1 ms apart in the two files").arg(offsetMs, 0, 'f', 0));
        return;
    }
    // Worth it by what the file really holds, not only by what its link says.
    const int playing = m_engine->streamInfo().kbps > 0 ? m_engine->streamInfo().kbps : kUpgradeAssumedKbps;
    if (!m_engine->startUpgrade(offer.url, offsetMs, playing + kUpgradeMinGainKbps)) {
        stay(QStringLiteral("too near the end, or not playing now (at %1 of %2)")
                 .arg(TrackModel::formatDuration(m_position), TrackModel::formatDuration(m_engine->duration())));
        return;
    }
    m_upgradeVideoId = offer.videoId;
    m_upgradeKbps = offer.kbps;
    m_upgradeListedSec = offer.durationSec;
    qInfo("upgrade: %s: the music is %+.1f ms later in JioSaavn's file (%.3f alike there); getting it ready",
          qPrintable(offer.videoId), offsetMs, peak);
}

void PlaybackController::upgradeFinished(bool swapped, const QString &detail, int fileKbps, bool otherLength)
{
    const QString videoId = std::exchange(m_upgradeVideoId, QString());
    if (videoId.isEmpty())
        return;
    if (!swapped) {
        qInfo("upgrade: %s not moved to JioSaavn: %s", qPrintable(videoId), qUtf8Printable(detail));
        // What was found out about JioSaavn's file, where it is the file's
        // own fault, holds for the song's next start too: the race would
        // play the same file from 0 s, labelled with the bitrate its link
        // claims. Thinner than the YouTube stream it was to replace (a
        // "_320" link has been seen to average 98 kbps), or another length
        // than JioSaavn lists it at, which offerSaavnUpgrade found to match
        // the stream playing. Not for a file merely short of the move's
        // margin, which is still better than YouTube's, and not for the two
        // copies failing to line up, where the stream playing (a music
        // video's longer cut, say) may be the odd one.
        const int playing = m_engine ? m_engine->streamInfo().kbps : 0;
        const bool thin = fileKbps > 0 && playing > 0 && fileKbps < playing;
        const bool misListed = otherLength && m_upgradeListedSec > 0;
        if ((thin || misListed) && m_resolver) {
            m_resolver->passOverSaavn(videoId, thin ? QStringLiteral("its file averages %1 kbps, less than the %2 "
                                                                     "kbps YouTube stream").arg(fileKbps).arg(playing)
                                                    : QStringLiteral("its file is not the length JioSaavn lists"));
        }
        return;
    }
    const QString from = tierLabel(m_streamTier);
    m_streamTier = StreamResolver::TierJioSaavn;
    m_streamFromCache = false;
    m_rescueTier = -1;
    ++m_upgradesDone;
    // The one line to count these by: how often a song moved, and how.
    qInfo("upgrade: %s moved from %s to JioSaavn (%d kbps) mid-song at %s, move %d this session: %s",
          qPrintable(videoId), qUtf8Printable(from), m_upgradeKbps, qPrintable(TrackModel::formatDuration(m_position)),
          m_upgradesDone, qUtf8Printable(detail));
    setStatus(QStringLiteral("Streaming"), QStringLiteral("JioSaavn · %1 kbps").arg(m_upgradeKbps), false);
    setSoundOrigin(QStringLiteral("Streaming · ") + tierLabel(StreamResolver::TierJioSaavn));
    // What mpv has already said of the new file is written now: it may have
    // nothing more to say.
    logStreamInfo();
    Q_EMIT streamInfoChanged();
}

void PlaybackController::failTrack(const QString &reason)
{
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
    qInfo("playback: %d in a row that would not play%s", m_consecutiveFailures,
          giveUp ? ", so the queue stops here" : "");

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

    haltPlayback();
    setStatus(giveUp ? QStringLiteral("Nothing here will play")
                     : QStringLiteral("Source unavailable"),
              QString(), false, /*error=*/true);
    // Out loud only to someone waiting for sound. A track nobody asked to
    // play — the one a launch opens on, with the network down — says so on
    // the status line alone, and Play tries it again.
    if (wasGoingToPlay)
        Q_EMIT playbackError(reason);
}

QString PlaybackController::localCopyOf(const QVariantMap &track) const
{
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (m_downloads && !videoId.isEmpty()) {
        const QString downloaded = m_downloads->localPathFor(videoId);
        if (!downloaded.isEmpty())
            return downloaded;
    }
    const QString source = track.value(QStringLiteral("sourceUrl")).toString();
    return !source.isEmpty() && QFileInfo::exists(source) ? source : QString();
}

// The picture has gone and the sound comes back, from the second it had
// reached. A song kept on disk comes back from the file, as it began: the
// picture was the only part of it that needed the network, and a stream in
// its place would leave the rest of the song at the network's mercy.
void PlaybackController::backToSound(bool keepPlaying, const QString &resolvingText)
{
    // Not a file mpv has already refused for this track.
    const QString localPath = m_localRefused ? QString() : localCopyOf(m_currentTrack);
    if (!localPath.isEmpty()) {
        m_pendingVideoId.clear();
        m_streamIsVideo = false;
        m_localPath = localPath;
        setStatus(QStringLiteral("Offline"), QStringLiteral("Local file"), false);
        if (m_engine && m_engine->load(localPath, keepPlaying, QString(), m_position))
            setSoundOrigin(QStringLiteral("Offline · Local file"));
        return;
    }
    const QString videoId = currentSourceId();
    if (videoId.isEmpty() || !m_resolver)
        return;
    m_resumeAt = m_position;
    m_autoPlayAfterResolve = keepPlaying;
    m_pendingVideoId = videoId;
    setStatus(resolvingText, QString(), true);
    // A play rescued from below its own rung goes back to the link that
    // rescued it, which the resolver keeps for this play: the top of the
    // ladder is where the refused link came from.
    if (m_rescueTier >= 0 && !m_resolver->keepsRescueLinks()) {
        QList<int> tiers{ m_rescueTier };
        for (const int tier : StreamResolver::afterRefusal(m_rescueTier)) {
            if (!m_refusedTiers.contains(tier) && !tiers.contains(tier))
                tiers.append(tier);
        }
        m_resolver->resolveVia(videoId, tiers, m_homeTier);
        return;
    }
    // The sound as it would be for the song begun afresh: JioSaavn's copy
    // where it has one and it was not refused.
    m_resolver->resolveTrack(saavnTarget(m_currentTrack));
}

// The added picture put aside (vid=no), the file and its sound left as they
// are. The next song starts as sound anyway.
void PlaybackController::dropAddedVideo()
{
    m_videoAdded = false;
    m_videoUnproven = false;
    const bool changed = m_videoWanted || m_videoPlaying;
    m_videoWanted = false;
    m_videoPlaying = false;
    if (m_engine)
        m_engine->setVideoEnabled(false);
    endVideoStatus();
    if (changed)
        Q_EMIT videoChanged();
}

// True when a video that never proved itself has just been dropped, and the
// sound of the same track is on its way back.
bool PlaybackController::abandonVideo(const QString &reason)
{
    if (!m_videoPlaying || !m_videoUnproven)
        return false;
    // An added picture is not what ended or failed: that was the file
    // playing, which the caller goes on to handle as it would without one.
    if (m_videoAdded) {
        dropAddedVideo();
        return false;
    }

    m_videoUnproven = false;
    m_videoWanted = false;
    m_videoPlaying = false;
    if (m_engine)
        m_engine->setVideoEnabled(false);
    Q_EMIT videoChanged();
    Q_EMIT notice(reason);

    backToSound(/*keepPlaying=*/true, QStringLiteral("Back to the sound…"));
    return true;
}

// A URL can resolve and still be refused when mpv opens it: a link the CDN
// rejects now and then, or an instance serving an error page. A remembered
// link that no longer opens is simply stale, so a fresh one from the same tier
// comes first. So does a fresh InnerTube link, once a track (the order the
// owner chose, U01): /player's next answer comes in a fifth of a second and
// is Opus, where the muxed stream below it takes three and is 96-128 kbps
// AAC. Measured on real refusals, though, the fresh link was refused too
// every time (0 rescued of 28): googlevideo refuses the song, not the link.
// So once both are refused the song is remembered as refused for an hour
// (StreamResolver::noteInnerTubeRefused): its next plays start from the link
// that rescued it, and InnerTube is not asked twice again. With
// playback.refused=muxed the fresh link is not asked for at all, and one
// refusal is remembered the same way. After that, the tiers this track has not
// yet had refused, in the order the resolver gives for a refusal: after
// InnerTube's sound-only stream that is the track's muxed stream — itag 18,
// or the best other stream with the sound and a small picture in one file —
// fetched for this track alone. Whichever it is, the song carries on from
// where it was: a stream refused part-way through does not start it again.
bool PlaybackController::retryRefused()
{
    if (m_streamTier < 0 || !m_resolver)
        return false;
    const int refused = std::exchange(m_streamTier, -1);
    bool again = m_streamFromCache;
    bool refusedLately = false;
    if (refused == StreamResolver::TierInnerTube) {
        // Once, whichever the reason, so a session YouTube doubts cannot go
        // round in circles; not at all with the switch back, unless stale;
        // and not for a song whose InnerTube links were refused lately,
        // stale or not.
        refusedLately = m_resolver->innerTubeRefusedLately(m_streamVideoId);
        again = (m_streamFromCache || m_freshLinkFirst) && !m_innerTubeAskedAgain && !refusedLately;
        if (again) {
            m_innerTubeAskedAgain = true;
        } else if (!m_streamFromCache && !refusedLately) {
            // A fresh link refused, and InnerTube not asked again: the
            // refusal is the song's, and its next plays go straight to what
            // rescues it now (StreamResolver::noteInnerTubeRefused).
            m_resolver->noteInnerTubeRefused(m_streamVideoId);
        }
    }
    QList<int> tiers;
    if (again)
        tiers.append(refused);
    if (!m_streamFromCache)
        m_refusedTiers.insert(refused);
    for (const int tier : StreamResolver::afterRefusal(refused)) {
        if (!m_refusedTiers.contains(tier) && !tiers.contains(tier))
            tiers.append(tier);
    }
    if (tiers.isEmpty())
        return false;
    if (!m_rescueClock.isValid())
        m_rescueClock.start();
    m_resolver->invalidate(m_streamVideoId);
    m_pendingVideoId = m_streamVideoId;
    m_resumeAt = m_position;
    if (again && refused == StreamResolver::TierInnerTube && !m_streamFromCache) {
        qInfo("playback: asking InnerTube once more for %s, for a fresh link, before its muxed stream",
              qPrintable(m_pendingVideoId));
    } else if (refusedLately) {
        qInfo("playback: %s's InnerTube links were refused lately, so InnerTube is not asked again; %s next",
              qPrintable(m_pendingVideoId), qUtf8Printable(tierLabel(tiers.first())));
    } else if (tiers.first() == StreamResolver::TierMuxed) {
        qInfo("playback: trying %s again as its muxed stream (itag 18), for this track alone",
              qPrintable(m_pendingVideoId));
    }
    setStatus(again ? QStringLiteral("Refreshing the source…") : QStringLiteral("Trying another source…"),
              QString(), true);
    m_resolver->resolveVia(m_pendingVideoId, tiers, m_homeTier);
    return true;
}

// A stream that ends well short of its own length did not finish: the
// connection gave out, or the CDN served the link's first request and
// refused the reconnect that should have fetched the rest, which mpv reads as
// the end of the file. The rest of the song was skipped without a word. So an
// end more than kEarlyEndMarginMs (or 3%) before the length mpv read from the
// stream itself — not the catalogue's, which a different upload can differ
// from — counts as the link failing: a fresh one from the same rung, and the
// song carries on from the second it stopped at, once a track. A second early
// end is taken as the end, for a file whose length is simply wrong.
//
// Streams only. A file on disk says its own length, which for some (a VBR MP3
// with no index) is only an estimate; and with the picture on, the sound's
// file and the picture's are more than this puts back together. A row's own
// link is such a file on someone else's server, so it is picked up only
// where its container carries its length (lengthIsExact): an MP3 with no
// index is given the length its first frames' bitrate suggests and is sought
// by the same guess, so its real end would be taken for an early one, and
// the reload would land well before it and play a stretch again.
bool PlaybackController::resumeEarlyEnd()
{
    if (!m_earlyEndCheck || !m_engine || m_videoPlaying)
        return false;
    if ((m_streamTier < 0 || !m_resolver) && m_directUrl.isEmpty())
        return false;
    const qint64 length = m_engine->duration();
    if (length <= 0)
        return false;
    const qint64 margin = qMax(kEarlyEndMarginMs, length * 3 / 100);
    if (m_position >= length - margin)
        return false;
    const QString name = currentSourceId().isEmpty() ? m_currentTrack.value(QStringLiteral("title")).toString()
                                                     : currentSourceId();
    if (!m_directUrl.isEmpty() && !lengthIsExact(m_engine->fileFormat())) {
        qInfo("playback: %s ended at %s of %s, but its link is %s, whose length may be only a guess; taken as "
              "its end", qUtf8Printable(name), qPrintable(TrackModel::formatDuration(m_position)),
              qPrintable(TrackModel::formatDuration(length)),
              qPrintable(m_engine->fileFormat().isEmpty() ? QStringLiteral("a file of unknown kind")
                                                           : QStringLiteral("\"%1\"").arg(m_engine->fileFormat())));
        return false;
    }
    if (m_earlyEndResumed) {
        qWarning("playback: %s ended early again, at %s of %s; taken as its end", qUtf8Printable(name),
                 qPrintable(TrackModel::formatDuration(m_position)), qPrintable(TrackModel::formatDuration(length)));
        return false;
    }
    m_earlyEndResumed = true;
    if (!m_rescueClock.isValid())
        m_rescueClock.start();
    const QString from = !m_directUrl.isEmpty() ? QStringLiteral("its own link") : tierLabel(m_streamTier);
    qWarning("playback: %s ended at %s of %s, streamed from %s: it stopped short, and carries on from there",
             qUtf8Printable(name), qPrintable(TrackModel::formatDuration(m_position)),
             qPrintable(TrackModel::formatDuration(length)), qUtf8Printable(from));
    m_autoPlayAfterResolve = true;

    // The row's own link has no other to go to: loaded again at the second.
    if (!m_directUrl.isEmpty()) {
        if (m_engine->load(m_directUrl, true, QString(), m_position))
            setSoundOrigin(QStringLiteral("Streaming · Direct URL"));
        return true;
    }
    const QString videoId = m_streamVideoId;
    const int tier = std::exchange(m_streamTier, -1);
    m_pendingVideoId = videoId;
    m_resumeAt = m_position;
    setStatus(QStringLiteral("Refreshing the source…"), QString(), true);
    // JioSaavn's links are plain paths on its CDN that last: the match
    // answers with the same one again.
    if (tier == StreamResolver::TierJioSaavn) {
        m_resolver->resolveTrack(saavnTarget(m_currentTrack));
        return true;
    }
    // The same rung, and a fresh link from it; the rungs below should it not
    // answer.
    QList<int> tiers{ tier };
    for (const int next : StreamResolver::afterRefusal(tier)) {
        if (!m_refusedTiers.contains(next) && !tiers.contains(next))
            tiers.append(next);
    }
    m_resolver->invalidate(videoId);
    m_resolver->resolveVia(videoId, tiers, m_homeTier);
    return true;
}

void PlaybackController::handleEndOfFile()
{
    // A video that stopped before it ever showed a frame did not finish the
    // song; it failed. The queue must not move on.
    if (abandonVideo(QStringLiteral("This video would not play — back to audio")))
        return;

    // A stream that stopped short is picked up where it stopped.
    if (resumeEarlyEnd())
        return;

    // The clock is left at the end for beginTrack to close the listen with, as
    // for any song that ends: set back to 0 first, every repeat was written
    // down as a skip. beginTrack puts it back to 0 itself.
    if (m_repeatMode == RepeatOne) {
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
    // A track that would not load has nothing to unpause: try it again, now
    // as one the listener is waiting for, which says so if it fails. From
    // where it was: the place a launch put back, with the network down then,
    // is still the place to go back to.
    if (m_statusError) {
        m_consecutiveFailures = 0;
        m_openAt = m_position;
        beginCurrent(/*autoPlay=*/true);
        return;
    }
    // Someone wants to hear this one now: if it is still resolving it starts
    // when it arrives rather than landing paused, and is recorded then; if it
    // was only loaded it counts as played from here.
    m_autoPlayAfterResolve = true;
    if (m_pendingVideoId.isEmpty())
        startListening();
    m_engine->setPaused(false);
}

void PlaybackController::pause()
{
    // A track still resolving arrives paused, as asked.
    m_autoPlayAfterResolve = false;
    if (engineAvailable())
        m_engine->setPaused(true);
}

void PlaybackController::togglePlay()
{
    m_playing ? pause() : play();
}

// A track still resolving counts as playing only if someone is waiting for its
// sound: one reached with Next while paused is resolving too, and a second
// Next on it must load the one after paused as well, not start it.
void PlaybackController::next()
{
    advance(m_playing || (m_resolving && m_autoPlayAfterResolve));
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
    if (m_position > kRestartAfterMs || m_queue.currentIndex() <= 0) {
        // The listen ends here, while the clock still says how much of it was
        // heard. Left open, it was closed by whatever came next — and pressed
        // twice, Previous goes back a song a second later, which recorded the
        // song as heard for no time at all. Hearing it again is a listen of
        // its own, but only once it is past this point again: until then
        // Previous means "the song before", and the restart was on the way.
        if (m_position > kRestartAfterMs) {
            closePlayEvent(/*restarting=*/true);
            m_listenPending = true;
            m_replay = Replay::Rewinding;
            // For Last.fm too: heard again from the start, it is a new
            // listen, and counts again once enough of it has been heard.
            m_listen.restart();
        }
        setPosition(0);
        return;
    }

    // As in next(): resolving is playing only for a track someone is waiting for.
    const bool wasPlaying = m_playing || (m_resolving && m_autoPlayAfterResolve);
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
    m_listen.seeked(clamped);
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
    m_volumeSave.start();
}

void PlaybackController::setShuffle(bool shuffle)
{
    if (shuffle == m_shuffle)
        return;
    m_shuffle = shuffle;
    saveSetting(kShuffleKey, shuffle ? QStringLiteral("1") : QStringLiteral("0"));
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
    saveSetting(kRepeatKey, QString::number(m_repeatMode));
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
    // The switch is the listener's choice for the songs to come as well:
    // off turns it off for them even on a song with no picture to stop.
    if (!wanted && m_videoPreferred) {
        m_videoPreferred = false;
        if (!m_videoWanted) {
            Q_EMIT videoChanged();
            return;
        }
    }
    if (wanted == m_videoWanted)
        return;
    if (wanted && !videoAvailable())
        return;
    m_videoWanted = wanted;
    if (wanted)
        m_videoPreferred = true;
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
        if (m_statusText != loadingVideoText())
            m_statusBeforeVideo = m_statusText;
        setStatus(loadingVideoText(), m_sourceLabel, true);
        m_resolver->resolveVideo(videoId, m_videoHeight);
        return;
    }

    m_resolver->cancelVideo(videoId);
    m_videoPendingId.clear();
    m_pictureAwaitingSound = {};
    endVideoStatus();
    // Added to the file playing: put aside, and the sound plays on untouched.
    if (m_videoAdded) {
        dropAddedVideo();
        return;
    }
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
    backToSound(m_playing || m_resolving, QStringLiteral("Resolving source…"));
}

void PlaybackController::handleVideoResolved(const QString &videoId, const QString &videoUrl,
                                             const QString &audioUrl, const QVariantMap &headers)
{
    if (videoId != m_videoPendingId)
        return;   // another track, the next one's fetched ahead, or the switch went off
    m_videoPendingId.clear();
    if (!m_engine)
        return;

    // A song just begun, its sound still on its way: the picture waits for
    // it and joins it once it has loaded. Loaded together instead, the
    // sound's own arrival would then replace them both.
    if (!m_engine->hasLoadedFile() && (m_engine->isLoadingFile() || !m_pendingVideoId.isEmpty())) {
        m_pictureAwaitingSound = { videoId, videoUrl, audioUrl, headers };
        return;
    }
    applyVideo(videoUrl, audioUrl, headers);
}

// The picture onto what is playing: added to the file already open, so the
// sound plays on without a gap, or loaded with its own sound where nothing
// is (the song's sound would not load, say).
void PlaybackController::applyVideo(const QString &videoUrl, const QString &audioUrl,
                                    const QVariantMap &headers)
{
    m_engine->setVideoEnabled(true);
    // Unproven until a frame arrives: see abandonVideo.
    m_videoUnproven = true;
    // The picture is laid over the sound already playing, so nothing restarts
    // — unless that sound is JioSaavn's. Its master is not YouTube's and can
    // differ by seconds, so the picture would drift from it; the video then
    // loads whole, with YouTube's own sound, and the match stands for when
    // the picture is turned off again.
    const bool overSaavn = m_streamTier == StreamResolver::TierJioSaavn;
    if (m_engine->hasLoadedFile() && !overSaavn) {
        m_videoAdded = true;
        endVideoStatus();
        m_engine->addVideo(videoUrl, headers);
    } else {
        m_videoAdded = false;
        m_streamIsVideo = true;
        // What mpv has open now is the picture's stream, not a file.
        m_localPath.clear();
        m_directUrl.clear();
        setStatus(QStringLiteral("Streaming"), QStringLiteral("yt-dlp · video"), false);
        if (m_engine->load(videoUrl, m_playing || m_autoPlayAfterResolve, audioUrl, m_position, headers))
            setSoundOrigin(QStringLiteral("Streaming · yt-dlp · video"));
    }
    if (!m_videoPlaying) {
        m_videoPlaying = true;
        Q_EMIT videoChanged();
    }
}

// The status line back to what it said before "Loading the video…", if it
// still says that.
void PlaybackController::endVideoStatus()
{
    if (m_statusText != loadingVideoText())
        return;
    setStatus(m_statusBeforeVideo.isEmpty() ? QStringLiteral("Streaming") : m_statusBeforeVideo,
              m_sourceLabel, false);
}

void PlaybackController::setAutoplay(bool autoplay)
{
    if (autoplay == m_autoplay)
        return;
    m_autoplay = autoplay;
    saveSetting(kAutoplayKey, autoplay ? QStringLiteral("1") : QStringLiteral("0"));
    if (!autoplay) {
        m_innerTube.cancelRadio();
        m_radioSeed.clear();
    }
    Q_EMIT autoplayChanged();
}
