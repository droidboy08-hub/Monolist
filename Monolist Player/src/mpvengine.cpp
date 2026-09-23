#include "mpvengine.h"

#include <QMetaObject>
#include <QtGlobal>

#include <mpv/client.h>

namespace {
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
    PropVideoHeight
};
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

    // Stream URLs are resolved by StreamResolver (yt-dlp / Piped / Invidious)
    // before they reach mpv, so mpv's own ytdl hook is redundant and would add
    // a second, slower resolution path with different failure modes.
    setOption("ytdl", "no");

    // Stop at end of file rather than holding the last frame; the controller
    // decides what plays next.
    setOption("keep-open", "no");
    setOption("idle", "yes");

    // Network buffering. Generous enough that a hiccup on a remote stream does
    // not audibly drop out.
    setOption("cache", "yes");
    setOption("demuxer-max-bytes", "64MiB");
    setOption("demuxer-readahead-secs", "20");

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
            if (!prop->data)
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
            default:
                break;
            }
            break;
        }

        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
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

void MpvEngine::load(const QString &urlOrPath, bool startPlaying, const QString &audioUrl,
                     qint64 startAt, const QVariantMap &headers)
{
    if (!m_mpv)
        return;

    // Fetch the link the way it was obtained. Always set, so one file's
    // headers are never sent for the next one's.
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

    // Forget the previous file's duration. Change events are compared against
    // it, and reloading a file of the same length would otherwise never report
    // one, leaving the controller, which resets its own copy, stuck at 0:00.
    m_duration = 0;
    // And its picture: nothing is showing until this file reports one.
    if (!m_videoSize.isEmpty()) {
        m_videoSize = QSize();
        Q_EMIT videoSizeChanged(m_videoSize);
    }

    // A stream whose sound comes separately (YouTube's larger sizes). Always
    // set, so the last video's sound is never carried into the next file —
    // and cleared as a list, because setting it to "" would leave one entry
    // that is the empty file name, which mpv then tries to open.
    if (audioUrl.isEmpty()) {
        const char *clear[] = { "change-list", "audio-files", "clr", "", nullptr };
        mpv_command(m_mpv, clear);
    } else {
        const QByteArray audio = audioUrl.toUtf8();
        const char *set[] = { "change-list", "audio-files", "set", audio.constData(), nullptr };
        mpv_command(m_mpv, set);
    }
    // Likewise always set: the next file starts where it is told, or at 0.
    mpv_set_option_string(m_mpv, "start",
                          startAt > 0 ? QByteArray::number(startAt / 1000.0, 'f', 3).constData()
                                      : "none");

    const QByteArray target = urlOrPath.toUtf8();
    // "replace" tears down the previous file; EndFile arrives with reason STOP,
    // which drainEvents deliberately ignores so it is not mistaken for EOF.
    const char *args[] = { "loadfile", target.constData(), "replace", nullptr };
    const int rc = mpv_command_async(m_mpv, 0, args);
    if (rc < 0) {
        Q_EMIT loadFailed(QString::fromUtf8(mpv_error_string(rc)));
        return;
    }
    setPaused(!startPlaying);
}

void MpvEngine::stop()
{
    if (!m_mpv)
        return;
    const char *args[] = { "stop", nullptr };
    mpv_command_async(m_mpv, 0, args);
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
    mpv_set_property_string(m_mpv, "vid", enabled ? "auto" : "no");
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
