#include "mpvengine.h"

#include <QMetaObject>
#include <QtGlobal>

#include <mpv/client.h>

namespace {

// Set on the number a video-add is asked with, so its answer is never taken
// for a loadfile's. The loads count up from 1 and never reach it.
constexpr quint64 kAddVideoTag = quint64(1) << 62;
// What yt-dlp presents itself as, and therefore what the links it hands back
// must be fetched as.
const char *kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/149.0.0.0 Safari/537.36";

// Property ids passed to mpv_observe_property; echoed back on each change event.
enum ObservedProperty : uint64_t {
    PropTimePos = 1,
    PropDuration,
    PropPause,
    PropCoreIdle,
    PropCacheBuffering,
    PropMediaTitle,
    PropMetaArtist,
    PropVideoWidth,
    PropVideoHeight,
    PropAudioDeviceList,
    // What StreamInfo is read from. A change only says it is time to read
    // it all again, together (refreshStreamInfo); the values themselves are
    // not taken from the events.
    PropAudioCodec,
    PropAudioParams,
    PropAudioOutParams,
    PropCurrentAo,
    PropAudioBitrate
};

// mpv's string property `name`, or empty while it has none.
QString stringProperty(mpv_handle *mpv, const char *name)
{
    char *value = nullptr;
    if (mpv_get_property(mpv, name, MPV_FORMAT_STRING, &value) < 0 || !value)
        return {};
    const QString text = QString::fromUtf8(value);
    mpv_free(value);
    return text;
}

// mpv's number property `name`, whichever of its two number formats it has;
// false while it has none.
bool numberProperty(mpv_handle *mpv, const char *name, double *out)
{
    mpv_node node;
    if (mpv_get_property(mpv, name, MPV_FORMAT_NODE, &node) < 0)
        return false;
    bool ok = true;
    if (node.format == MPV_FORMAT_INT64)
        *out = double(node.u.int64);
    else if (node.format == MPV_FORMAT_DOUBLE)
        *out = node.u.double_;
    else
        ok = false;
    mpv_free_node_contents(&node);
    return ok;
}

// audio-params or audio-out-params: a map of the rate, sample format and
// channel layout.
void readAudioParams(mpv_handle *mpv, const char *name, int *rate, QString *format, QString *channels)
{
    *rate = 0;
    format->clear();
    channels->clear();
    mpv_node node;
    if (mpv_get_property(mpv, name, MPV_FORMAT_NODE, &node) < 0)
        return;
    if (node.format == MPV_FORMAT_NODE_MAP && node.u.list) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const char *key = node.u.list->keys[i];
            const mpv_node &value = node.u.list->values[i];
            if (qstrcmp(key, "samplerate") == 0 && value.format == MPV_FORMAT_INT64)
                *rate = int(value.u.int64);
            else if (qstrcmp(key, "format") == 0 && value.format == MPV_FORMAT_STRING)
                *format = QString::fromUtf8(value.u.string);
            else if (qstrcmp(key, "hr-channels") == 0 && value.format == MPV_FORMAT_STRING)
                *channels = QString::fromUtf8(value.u.string);
        }
    }
    mpv_free_node_contents(&node);
}

// What the open file's track list says about its bitrate: whether the file
// holds sound alone, so that its size is all sound (no picture in it, not
// even a cover; a picture mpv found beside it, such as a folder's cover.jpg,
// is another file and does not count); and the bitrate the container
// declares for the sound playing, in bits per second, 0 where it declares
// none (an Opus stream usually).
void readTrackList(mpv_handle *mpv, bool *soundOnly, double *declaredBitrate)
{
    *soundOnly = false;
    *declaredBitrate = 0.0;
    mpv_node node;
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &node) < 0)
        return;
    bool sound = false;
    bool other = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const mpv_node &track = node.u.list->values[i];
            if (track.format != MPV_FORMAT_NODE_MAP || !track.u.list)
                continue;
            bool audio = false;
            bool external = false;
            bool selected = false;
            double bitrate = 0.0;
            for (int k = 0; k < track.u.list->num; ++k) {
                const char *key = track.u.list->keys[k];
                const mpv_node &value = track.u.list->values[k];
                if (qstrcmp(key, "type") == 0 && value.format == MPV_FORMAT_STRING)
                    audio = qstrcmp(value.u.string, "audio") == 0;
                else if (qstrcmp(key, "external") == 0 && value.format == MPV_FORMAT_FLAG)
                    external = value.u.flag != 0;
                else if (qstrcmp(key, "selected") == 0 && value.format == MPV_FORMAT_FLAG)
                    selected = value.u.flag != 0;
                else if (qstrcmp(key, "demux-bitrate") == 0 && value.format == MPV_FORMAT_INT64)
                    bitrate = double(value.u.int64);
            }
            if (external)
                continue;
            if (audio)
                sound = true;
            else
                other = true;
            if (audio && selected)
                *declaredBitrate = bitrate;
        }
    }
    mpv_free_node_contents(&node);
    *soundOnly = sound && !other;
}

// The devices in mpv's audio-device-list: an array of maps, each with a
// "name" and a "description".
QVariantList audioDevicesOf(const mpv_node &list)
{
    QVariantList devices;
    if (list.format != MPV_FORMAT_NODE_ARRAY || !list.u.list)
        return devices;
    for (int i = 0; i < list.u.list->num; ++i) {
        const mpv_node &entry = list.u.list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list)
            continue;
        QString name;
        QString description;
        for (int k = 0; k < entry.u.list->num; ++k) {
            const mpv_node &value = entry.u.list->values[k];
            if (value.format != MPV_FORMAT_STRING)
                continue;
            if (qstrcmp(entry.u.list->keys[k], "name") == 0)
                name = QString::fromUtf8(value.u.string);
            else if (qstrcmp(entry.u.list->keys[k], "description") == 0)
                description = QString::fromUtf8(value.u.string);
        }
        if (!name.isEmpty()) {
            devices.append(QVariantMap{ { QStringLiteral("name"), name },
                                        { QStringLiteral("description"), description } });
        }
    }
    return devices;
}

// The playlist entry a loadfile made, from mpv's answer to it; -1 when the
// answer does not say, as an older mpv's does not.
qint64 playlistEntryOf(const mpv_node &result)
{
    if (result.format != MPV_FORMAT_NODE_MAP || !result.u.list)
        return -1;
    const mpv_node_list *map = result.u.list;
    for (int i = 0; i < map->num; ++i) {
        if (qstrcmp(map->keys[i], "playlist_entry_id") == 0
            && map->values[i].format == MPV_FORMAT_INT64)
            return map->values[i].u.int64;
    }
    return -1;
}
}

MpvEngine::MpvEngine(QObject *parent)
    : QObject(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv) {
        m_lastError = QStringLiteral("mpv_create() failed — libmpv is unavailable.");
        return;
    }

    applyBaseOptions();

    const int rc = mpv_initialize(m_mpv);
    if (rc < 0) {
        m_lastError = QStringLiteral("mpv_initialize() failed: %1")
                          .arg(QString::fromUtf8(mpv_error_string(rc)));
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    observeProperties();

    // mpv's own log is off (msg-level above). MONOLIST_MPV_LOG=warn, info, v or
    // debug routes it to the app's log instead, for diagnosing a stream that
    // will not open.
    const QByteArray logLevel = qgetenv("MONOLIST_MPV_LOG");
    if (!logLevel.isEmpty())
        mpv_request_log_messages(m_mpv, logLevel.constData());

    mpv_set_wakeup_callback(m_mpv, &MpvEngine::onWakeup, this);
}

MpvEngine::~MpvEngine()
{
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvEngine::setOption(const char *name, const char *value)
{
    if (m_mpv)
        mpv_set_option_string(m_mpv, name, value);
}

void MpvEngine::applyBaseOptions()
{
    // Audio only until something asks for the picture (setVideoEnabled): no
    // video decoder, no cover-art render pass.
    setOption("vid", "no");
    setOption("audio-display", "no");

    // The picture is drawn by whoever holds the render context (VideoSurface),
    // never by mpv into a window of its own.
    setOption("vo", "libmpv");
    // The picture is drawn by the CPU (VideoSurface, SystemPip), scaled from
    // the video's size to the one shown. mpv's default scaler for that
    // (lanczos, dithered) took 44 ms a frame for 1080p on an M-series Mac —
    // more than a frame lasts, so the picture fell behind and the sound could
    // run dry. Bilinear takes 1-2 ms and, at these sizes, looks the same.
    setOption("zimg-scaler", "bilinear");
    setOption("zimg-dither", "no");

    // Stream URLs are resolved by StreamResolver (yt-dlp / Piped / Invidious)
    // before they reach mpv, so mpv's own ytdl hook is redundant and would add
    // a second, slower resolution path with different failure modes.
    setOption("ytdl", "no");

    // Stop at end of file rather than holding the last frame; the controller
    // decides what plays next.
    setOption("keep-open", "no");
    setOption("idle", "yes");
    // Paused until something is played, as m_paused already assumes. mpv
    // starts unpaused, which the controller would read as playing while the
    // song a launch opens on is still resolving — so the first press of Play
    // would pause it instead.
    setOption("pause", "yes");

    // Network buffering. Generous enough that a hiccup on a remote stream does
    // not audibly drop out: with the cache on, mpv reads ahead as far as this
    // allows (its cache-secs default is all but unlimited), which holds a
    // typical song whole. demuxer-readahead-secs is not set, because the
    // cache on overrides it; the 20 s once set here did nothing.
    setOption("cache", "yes");
    setOption("demuxer-max-bytes", "64MiB");

    // Normalise loudness across tracks from different sources.
    setOption("replaygain", "track");

    setOption("audio-client-name", "Monolist");
    // The browser yt-dlp says it is when it fetches a stream URL. YouTube
    // checks: a link extracted as one client and then fetched as another is
    // refused with 403, which is what an honest "Monolist/0.1" earned.
    setOption("user-agent", kBrowserUserAgent);

    // Keep libmpv from writing to the app's stderr; errors surface as signals.
    setOption("terminal", "no");
    setOption("msg-level", "all=no");
}

void MpvEngine::observeProperties()
{
    mpv_observe_property(m_mpv, PropTimePos,        "time-pos",             MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropDuration,       "duration",             MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropPause,          "pause",                MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropCoreIdle,       "core-idle",            MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropCacheBuffering, "paused-for-cache",     MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropMediaTitle,     "media-title",          MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropMetaArtist,     "metadata/by-key/Artist", MPV_FORMAT_STRING);
    // The picture's size as it should be shown (aspect ratio applied), which
    // is also how the surface knows a picture is there at all.
    mpv_observe_property(m_mpv, PropVideoWidth,     "dwidth",               MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropVideoHeight,    "dheight",              MPV_FORMAT_INT64);
    // Observing the list is also what starts mpv watching for devices coming
    // and going; the first answer arrives with the first events, well before
    // anyone could reach the output menu.
    mpv_observe_property(m_mpv, PropAudioDeviceList, "audio-device-list",   MPV_FORMAT_NODE);
    // What is actually playing (StreamInfo). A change only prompts a read of
    // the lot; mpv may fold a quick change and change back into nothing, so
    // the start of playback prompts one too (MPV_EVENT_PLAYBACK_RESTART).
    mpv_observe_property(m_mpv, PropAudioCodec,     "audio-codec-name",     MPV_FORMAT_NONE);
    mpv_observe_property(m_mpv, PropAudioParams,    "audio-params",         MPV_FORMAT_NONE);
    mpv_observe_property(m_mpv, PropAudioOutParams, "audio-out-params",     MPV_FORMAT_NONE);
    mpv_observe_property(m_mpv, PropCurrentAo,      "current-ao",           MPV_FORMAT_NONE);
    // mpv looks at the bitrate on every tick of its clock. With a value asked
    // for, it says only when the value changes (every second or so), where a
    // bare notification would come with every tick. Only wanted until it is
    // first known.
    mpv_observe_property(m_mpv, PropAudioBitrate,   "audio-bitrate",        MPV_FORMAT_NODE);
}

// Called by libmpv from its own thread. Must not touch Qt state directly.
void MpvEngine::onWakeup(void *ctx)
{
    auto *self = static_cast<MpvEngine *>(ctx);
    QMetaObject::invokeMethod(self, &MpvEngine::drainEvents, Qt::QueuedConnection);
}

void MpvEngine::drainEvents()
{
    if (!m_mpv)
        return;

    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            // StreamInfo's properties (see observeProperties): read again,
            // for the current file only. The bitrate only until it is known.
            switch (event->reply_userdata) {
            case PropAudioBitrate:
                if (m_streamInfo.kbps > 0)
                    break;
                Q_FALLTHROUGH();
            case PropAudioCodec:
            case PropAudioParams:
            case PropAudioOutParams:
            case PropCurrentAo:
                if (currentFileStarted())
                    refreshStreamInfo();
                break;
            default:
                break;
            }
            if (!prop->data)
                break;

            // The clock, the length and the picture belong to one file. Until
            // the file last asked for has started, they are still the old
            // one's, running on under the new track's title.
            const bool perFile = event->reply_userdata != PropPause
                                 && event->reply_userdata != PropCoreIdle
                                 && event->reply_userdata != PropCacheBuffering
                                 && event->reply_userdata != PropAudioDeviceList;
            if (perFile && !currentFileStarted())
                break;

            switch (event->reply_userdata) {
            case PropTimePos:
                Q_EMIT positionChanged(qint64(*static_cast<double *>(prop->data) * 1000.0));
                break;
            case PropDuration: {
                const qint64 ms = qint64(*static_cast<double *>(prop->data) * 1000.0);
                if (ms != m_duration) {
                    m_duration = ms;
                    Q_EMIT durationChanged(ms);
                }
                break;
            }
            case PropPause: {
                const bool paused = *static_cast<int *>(prop->data) != 0;
                if (paused != m_paused) {
                    m_paused = paused;
                    Q_EMIT pausedChanged(paused);
                }
                break;
            }
            case PropCacheBuffering: {
                const bool buffering = *static_cast<int *>(prop->data) != 0;
                if (buffering != m_buffering) {
                    m_buffering = buffering;
                    Q_EMIT bufferingChanged(buffering);
                }
                break;
            }
            case PropVideoWidth:
            case PropVideoHeight: {
                const qint64 value = *static_cast<qint64 *>(prop->data);
                if (event->reply_userdata == PropVideoWidth)
                    m_videoSize.setWidth(int(value));
                else
                    m_videoSize.setHeight(int(value));
                if (m_videoSize.width() > 0 && m_videoSize.height() > 0)
                    Q_EMIT videoSizeChanged(m_videoSize);
                break;
            }
            case PropMediaTitle:
            case PropMetaArtist: {
                // Report whatever the container carries; the library row stays
                // authoritative, this only fills gaps for ad-hoc stream URLs.
                char *title = nullptr;
                char *artist = nullptr;
                mpv_get_property(m_mpv, "media-title", MPV_FORMAT_STRING, &title);
                mpv_get_property(m_mpv, "metadata/by-key/Artist", MPV_FORMAT_STRING, &artist);
                Q_EMIT metadataChanged(QString::fromUtf8(title ? title : ""),
                                       QString::fromUtf8(artist ? artist : ""));
                if (title)  mpv_free(title);
                if (artist) mpv_free(artist);
                break;
            }
            case PropAudioDeviceList: {
                if (prop->format != MPV_FORMAT_NODE)
                    break;
                const QVariantList devices = audioDevicesOf(*static_cast<mpv_node *>(prop->data));
                if (devices != m_audioDevices) {
                    m_audioDevices = devices;
                    Q_EMIT audioDevicesChanged();
                }
                break;
            }
            default:
                break;
            }
            break;
        }

        case MPV_EVENT_COMMAND_REPLY: {
            // mpv's answer to a video-add: the picture joins the file playing,
            // or it would not open and the sound plays on as it was.
            if (event->reply_userdata & kAddVideoTag) {
                if (event->reply_userdata != m_addVideoRequest)
                    break;
                m_addVideoRequest = 0;
                if (event->error < 0) {
                    Q_EMIT videoAddFailed(QString::fromUtf8(mpv_error_string(event->error)));
                    break;
                }
                // Added while nothing showed it: selecting it decoded it, so
                // put it aside until something does.
                if (!m_watched || !m_video)
                    mpv_set_property_string(m_mpv, "vid", "no");
                break;
            }
            // mpv's answer to a loadfile, naming the entry it made. Only the
            // latest load's answer counts: an earlier one was replaced, or
            // stopped, before it could matter. (stop() asks with 0, and no
            // load is ever numbered 0.)
            if (event->reply_userdata == 0 || event->reply_userdata != m_loadRequest)
                break;
            if (event->error < 0) {
                m_loadingFile = false;
                Q_EMIT loadFailed(QString::fromUtf8(mpv_error_string(event->error)));
                break;
            }
            m_currentEntry = playlistEntryOf(static_cast<mpv_event_command *>(event->data)->result);
            break;
        }

        case MPV_EVENT_FILE_LOADED: {
            // For the file last asked for only: one it replaced can still
            // report having opened, after the new load was sent.
            if (!currentFileStarted())
                break;
            m_fileLoaded = true;
            m_loadingFile = false;
            Q_EMIT fileLoaded();
            break;
        }

        case MPV_EVENT_START_FILE: {
            m_startedEntry = static_cast<mpv_event_start_file *>(event->data)->playlist_entry_id;
            if (m_currentEntry < 0)
                m_currentEntry = m_startedEntry;   // the answer did not say; this is it
            break;
        }

        // The sound has started (or started again, after a seek), or its
        // output was opened anew: what it is can be read now.
        case MPV_EVENT_PLAYBACK_RESTART:
        case MPV_EVENT_AUDIO_RECONFIG:
            if (currentFileStarted())
                refreshStreamInfo();
            break;

        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            // The end of a file that has since been replaced or stopped. Its
            // natural end would skip the track that took its place, and its
            // error would be blamed on it.
            if (m_currentEntry <= 0 || end->playlist_entry_id != m_currentEntry)
                break;
            m_fileLoaded = false;
            m_loadingFile = false;
            if (end->reason == MPV_END_FILE_REASON_ERROR) {
                Q_EMIT loadFailed(QString::fromUtf8(mpv_error_string(end->error)));
            } else if (end->reason == MPV_END_FILE_REASON_EOF) {
                Q_EMIT endOfFile();
            }
            // MPV_END_FILE_REASON_STOP is a deliberate stop/replace — silent.
            break;
        }

        case MPV_EVENT_LOG_MESSAGE: {
            const auto *message = static_cast<mpv_event_log_message *>(event->data);
            qWarning("mpv[%s] %s: %s", message->level, message->prefix,
                     QByteArray(message->text).trimmed().constData());
            break;
        }

        case MPV_EVENT_SHUTDOWN:
            return;

        default:
            break;
        }
    }
}

void MpvEngine::applyHeaders(const QVariantMap &headers)
{
    const QString agent = headers.value(QStringLiteral("User-Agent")).toString();
    mpv_set_option_string(m_mpv, "user-agent",
                          agent.isEmpty() ? kBrowserUserAgent : agent.toUtf8().constData());
    const char *clearHeaders[] = { "change-list", "http-header-fields", "clr", "", nullptr };
    mpv_command(m_mpv, clearHeaders);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0)
            continue;   // its own option
        const QByteArray field = (it.key() + QStringLiteral(": ") + it.value().toString()).toUtf8();
        const char *add[] = { "change-list", "http-header-fields", "append", field.constData(), nullptr };
        mpv_command(m_mpv, add);
    }
}

void MpvEngine::addVideo(const QString &url, const QVariantMap &headers)
{
    if (!m_mpv || url.isEmpty())
        return;
    applyHeaders(headers);
    // Asynchronously: opening the link takes as long as the network does,
    // and the window must not wait for it.
    m_addVideoRequest = kAddVideoTag | ++m_addVideoCount;
    const QByteArray target = url.toUtf8();
    const char *args[] = { "video-add", target.constData(), "select", nullptr };
    const int rc = mpv_command_async(m_mpv, m_addVideoRequest, args);
    if (rc < 0)
        Q_EMIT videoAddFailed(QString::fromUtf8(mpv_error_string(rc)));
}

bool MpvEngine::load(const QString &urlOrPath, bool startPlaying, const QString &audioUrl,
                     qint64 startAt, const QVariantMap &headers)
{
    if (!m_mpv)
        return false;

    // Fetch the link the way it was obtained. Always set, so one file's
    // headers are never sent for the next one's.
    applyHeaders(headers);

    // Forget the previous file's duration. Change events are compared against
    // it, and reloading a file of the same length would otherwise never report
    // one, leaving the controller, which resets its own copy, stuck at 0:00.
    m_duration = 0;
    // And its picture: nothing is showing until this file reports one.
    if (!m_videoSize.isEmpty()) {
        m_videoSize = QSize();
        Q_EMIT videoSizeChanged(m_videoSize);
    }
    // And what it was: this file says for itself once its sound starts.
    clearStreamInfo();

    // A stream whose sound comes separately (YouTube's larger sizes). Always
    // cleared first, so the last video's sound is never carried into the next
    // file — as a list, because setting it to "" would leave one entry that is
    // the empty file name, which mpv then tries to open.
    //
    // Then APPENDED, never "set": audio-files is a path list, and "set" splits
    // its value on the platform's path separator. That is ';' on Windows, which
    // a googlevideo link never contains, but ':' on macOS and Linux, where it
    // cut "https://…" into "https" and "//…" — the picture played, the sound
    // never opened. "append" adds exactly one item, unsplit.
    const char *clear[] = { "change-list", "audio-files", "clr", "", nullptr };
    mpv_command(m_mpv, clear);
    if (!audioUrl.isEmpty()) {
        const QByteArray audio = audioUrl.toUtf8();
        const char *add[] = { "change-list", "audio-files", "append", audio.constData(), nullptr };
        mpv_command(m_mpv, add);
    }
    // Likewise always set: the next file starts where it is told, or at 0.
    mpv_set_option_string(m_mpv, "start",
                          startAt > 0 ? QByteArray::number(startAt / 1000.0, 'f', 3).constData()
                                      : "none");

    m_fileLoaded = false;
    m_loadingFile = true;

    const QByteArray target = urlOrPath.toUtf8();
    // "replace" tears down the previous file. From here until mpv answers
    // with the new entry, no file is current, so whatever the previous one
    // still reports goes nowhere.
    ++m_loadRequest;
    m_currentEntry = 0;
    const char *args[] = { "loadfile", target.constData(), "replace", nullptr };
    const int rc = mpv_command_async(m_mpv, m_loadRequest, args);
    if (rc < 0) {
        Q_EMIT loadFailed(QString::fromUtf8(mpv_error_string(rc)));
        return false;
    }
    setPaused(!startPlaying);
    return true;
}

// Silence now, while the next thing is found. Nothing the stopped file still
// reports — its clock, its end, a late answer to its loadfile — is passed on.
void MpvEngine::stop()
{
    if (!m_mpv)
        return;
    ++m_loadRequest;
    m_currentEntry = 0;
    m_duration = 0;
    m_fileLoaded = false;
    m_loadingFile = false;
    if (!m_videoSize.isEmpty()) {
        m_videoSize = QSize();
        Q_EMIT videoSizeChanged(m_videoSize);
    }
    clearStreamInfo();
    const char *args[] = { "stop", nullptr };
    mpv_command_async(m_mpv, 0, args);
}

void MpvEngine::clearStreamInfo()
{
    if (m_streamInfo == StreamInfo())
        return;
    m_streamInfo = StreamInfo();
    Q_EMIT streamInfoChanged();
}

// All of it read again, together, whenever any part may have changed: a
// codec without its output's rate would be half a sentence.
void MpvEngine::refreshStreamInfo()
{
    StreamInfo info;
    info.codec = stringProperty(m_mpv, "audio-codec-name");
    readAudioParams(m_mpv, "audio-params", &info.sampleRate, &info.sampleFormat, &info.channels);
    readAudioParams(m_mpv, "audio-out-params", &info.outputRate, &info.outputFormat, &info.outputChannels);
    info.output = stringProperty(m_mpv, "current-ao");
    numberProperty(m_mpv, "volume", &info.volume);

    // The bitrate is kept once known: it describes the file, and the
    // measure behind a picture's would otherwise wander from second to
    // second on the screen.
    if (m_streamInfo.kbps > 0) {
        info.kbps = m_streamInfo.kbps;
        info.kbpsIsFileAverage = m_streamInfo.kbpsIsFileAverage;
        info.kbpsIsDeclared = m_streamInfo.kbpsIsDeclared;
    } else {
        bool soundOnly = false;
        double declared = 0.0;
        double bytes = 0.0;
        double seconds = 0.0;
        double measured = 0.0;
        readTrackList(m_mpv, &soundOnly, &declared);
        if (soundOnly && numberProperty(m_mpv, "file-size", &bytes)
            && numberProperty(m_mpv, "duration", &seconds) && bytes > 0.0 && seconds > 1.0) {
            info.kbps = qRound(bytes * 8.0 / seconds / 1000.0);
            info.kbpsIsFileAverage = true;
        } else if (declared > 0.0) {
            info.kbps = qRound(declared / 1000.0);
            info.kbpsIsDeclared = true;
        } else if (numberProperty(m_mpv, "audio-bitrate", &measured) && measured > 0.0) {
            info.kbps = qRound(measured / 1000.0);
        }
    }

    if (info == m_streamInfo)
        return;
    m_streamInfo = info;
    Q_EMIT streamInfoChanged();
}

// Off by default: with no video track selected mpv decodes nothing, which is
// what a music player wants. Turned on, the picture goes to the render context
// a VideoSurface holds.
void MpvEngine::setVideoEnabled(bool enabled)
{
    if (!m_mpv || enabled == m_video)
        return;
    m_video = enabled;
    // Hardware decoding where the driver offers it, copied back to memory
    // because the frames are rendered by the CPU into a Qt Quick texture.
    mpv_set_option_string(m_mpv, "hwdec", enabled ? "auto-copy-safe" : "no");
    mpv_set_property_string(m_mpv, "vid", enabled && m_watched ? "auto" : "no");
}

void MpvEngine::setVideoWatched(bool watched)
{
    if (!m_mpv || watched == m_watched)
        return;
    m_watched = watched;
    if (!m_video)
        return;   // no picture either way; the next one starts as this says
    mpv_set_property_string(m_mpv, "vid", watched ? "auto" : "no");
    qInfo("video: %s", watched ? "on screen again, decoding (vid=auto)"
                               : "nothing shows the picture, decoding stops (vid=no)");
}

void MpvEngine::setPaused(bool paused)
{
    if (!m_mpv)
        return;
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvEngine::seekAbsolute(qint64 ms)
{
    if (!m_mpv)
        return;
    double seconds = double(ms) / 1000.0;
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvEngine::setVolume(qreal volume)
{
    if (!m_mpv)
        return;
    double percent = qBound(0.0, double(volume), 1.0) * 100.0;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &percent);
}

void MpvEngine::setSpeed(qreal speed)
{
    if (!m_mpv)
        return;
    double value = qBound(0.25, double(speed), 4.0);
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &value);
}

void MpvEngine::setReplayGainEnabled(bool enabled)
{
    setOption("replaygain", enabled ? "track" : "no");
}

// Asked without waiting for the answer: during a song, mpv reopens the sound
// output on the new device before it answers, which is long enough to be
// felt if the interface stood still for it.
void MpvEngine::setAudioDevice(const QString &name)
{
    if (!m_mpv)
        return;
    const QByteArray device = (name.isEmpty() ? QStringLiteral("auto") : name).toUtf8();
    const char *value = device.constData();
    mpv_set_property_async(m_mpv, 0, "audio-device", MPV_FORMAT_STRING, &value);
}
