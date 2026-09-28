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

// The player's own choices, kept in the settings table so a launch picks up
// where the last one left off.
const QString kVolumeKey = QStringLiteral("player.volume");
const QString kShuffleKey = QStringLiteral("player.shuffle");
const QString kRepeatKey = QStringLiteral("player.repeat");
const QString kAutoplayKey = QStringLiteral("player.autoplay");
const QString kAudioDeviceKey = QStringLiteral("player.audio_device");
const QString kAudioDeviceNameKey = QStringLiteral("player.audio_device_name");
const QString kAutoDevice = QStringLiteral("auto");
const QString kSaavnKey = QStringLiteral("jiosaavn.enabled");
const QString kSaavnIndiaKey = QStringLiteral("jiosaavn.india_headers");
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
            // A URL can resolve and still be refused when mpv opens it: a link
            // the CDN rejects now and then, or an instance serving an error
            // page. A remembered link that no longer opens is simply stale, so
            // a fresh one from the same tier comes first; after that, the
            // tiers this track has not yet had refused, in the order the
            // resolver gives for a refusal. After InnerTube's sound-only
            // stream that is the track's muxed stream — itag 18, or the best
            // other stream with the sound and a small picture in one file —
            // fetched for this track alone, before anything else. Whichever
            // it is, the song carries on from where it was: a stream refused
            // part-way through does not start it again.
            if (m_streamTier >= 0 && m_resolver) {
                const int refused = std::exchange(m_streamTier, -1);
                QList<int> tiers;
                if (m_streamFromCache)
                    tiers.append(refused);
                else
                    m_refusedTiers.insert(refused);
                for (const int tier : StreamResolver::afterRefusal(refused)) {
                    if (!m_refusedTiers.contains(tier) && !tiers.contains(tier))
                        tiers.append(tier);
                }
                if (!tiers.isEmpty()) {
                    m_resolver->invalidate(m_streamVideoId);
                    m_pendingVideoId = m_streamVideoId;
                    m_resumeAt = m_position;
                    if (tiers.first() == StreamResolver::TierMuxed) {
                        qInfo("playback: trying %s again as its muxed stream (itag 18), for this track alone",
                              qPrintable(m_pendingVideoId));
                    }
                    setStatus(m_streamFromCache ? QStringLiteral("Refreshing the source…")
                                                : QStringLiteral("Trying another source…"),
                              QString(), true);
                    m_resolver->resolveVia(m_pendingVideoId, tiers);
                    return;
                }
            }
            m_streamTier = -1;
            haltPlayback();
            setStatus(QStringLiteral("Playback failed"), QString(), false, /*error=*/true);
            // As for a failed resolve: a toast only for someone waiting to
            // hear it.
            if (m_autoPlayAfterResolve)
                Q_EMIT playbackError(reason);
        });

        m_engine->setVolume(m_volume);

        connect(m_engine, &MpvEngine::audioDevicesChanged, this, &PlaybackController::applyAudioDevice);

        // What mpv says it is playing, which fills in once the sound starts.
        connect(m_engine, &MpvEngine::streamInfoChanged, this, [this]() {
            logStreamInfo();
            Q_EMIT streamInfoChanged();
        });
    }
    applyAudioDevice();

    connect(&m_listen, &ListenTracker::listenStarted, this, &PlaybackController::listenStarted);
    connect(&m_listen, &ListenTracker::listenQualified, this, &PlaybackController::listenQualified);
    connect(&m_listen, &ListenTracker::listenResumed, this, &PlaybackController::listenResumed);

    m_volumeSave.setSingleShot(true);
    m_volumeSave.setInterval(kVolumeSaveDelayMs);
    connect(&m_volumeSave, &QTimer::timeout, this, &PlaybackController::saveVolume);

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
        });
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

    // JioSaavn only once the listener picks High sound quality: a setting
    // never written reads as off. Its Indian headers, which matter only then,
    // are on unless turned off.
    if (m_resolver) {
        m_resolver->setSaavnEnabled(m_library->settingValue(kSaavnKey) == QLatin1String("1"));
        m_resolver->setSaavnIndiaHeaders(m_library->settingValue(kSaavnIndiaKey) != QLatin1String("0"),
                                         /*forgetNoMatches=*/false);
        Q_EMIT saavnChanged();
    }

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

bool PlaybackController::saavnEnabled() const
{
    return m_resolver && m_resolver->saavnEnabled();
}

// From the next song on: the one playing keeps the sound it has.
void PlaybackController::setSaavnEnabled(bool on)
{
    if (!m_resolver || on == m_resolver->saavnEnabled())
        return;
    m_resolver->setSaavnEnabled(on);
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
}

void PlaybackController::addToQueue(const QVariantMap &map)
{
    QueueTrack track = QueueTrack::fromMap(map);
    track.fromRadio = false;
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

    if (m_resolver && !m_pendingVideoId.isEmpty())
        m_resolver->cancel(m_pendingVideoId);
    m_pendingVideoId.clear();
    m_streamTier = -1;
    m_streamIsVideo = false;
    m_refusedTiers.clear();
    m_resumeAt = 0;

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

    m_position = 0;
    m_autoPlayAfterResolve = autoPlay;
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
    // refused outright: loadFailed has said that.
    if (!localPath.isEmpty()) {
        if (!m_engine->load(localPath, autoPlay))
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
        if (!m_engine->load(source, autoPlay))
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
    m_consecutiveFailures = 0;
    m_streamVideoId = videoId;
    m_streamTier = tier;
    m_streamFromCache = fromCache;
    m_streamIsVideo = false;
    // mpv first: it opens the link on its own thread while everything below
    // is done on this one. The status line, the listen written down and Home
    // and History refreshed with it used to come first, and every song start
    // waited 40-60 ms for them before mpv was even asked, over half a second
    // once History held a few hundred songs (ROADMAP F35). A link some tiers
    // only serve to the client that asked for it (the muxed stream's) comes
    // with the headers to ask as; JioSaavn's CDN wants none, and must not be
    // sent a YouTube client's.
    const qint64 resumeAt = std::exchange(m_resumeAt, 0);
    if (!m_engine
        || !m_engine->load(url, m_autoPlayAfterResolve, QString(), resumeAt,
                           m_resolver && !fromSaavn ? m_resolver->headersFor(videoId) : QVariantMap()))
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
    const QString localPath = localCopyOf(m_currentTrack);
    if (!localPath.isEmpty()) {
        m_pendingVideoId.clear();
        m_streamIsVideo = false;
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

void PlaybackController::handleEndOfFile()
{
    // A video that stopped before it ever showed a frame did not finish the
    // song; it failed. The queue must not move on.
    if (abandonVideo(QStringLiteral("This video would not play — back to audio")))
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
    // as one the listener is waiting for, which says so if it fails.
    if (m_statusError) {
        m_consecutiveFailures = 0;
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
