#include "mpvengine.h"

#include <QMetaObject>
#include <QtGlobal>

#include <mpv/client.h>

namespace {
// Property ids passed to mpv_observe_property; echoed back on each change event.
enum ObservedProperty : uint64_t {
    PropTimePos = 1,
    PropDuration,
    PropPause,
    PropCoreIdle,
    PropCacheBuffering,
    PropMediaTitle,
    PropMetaArtist
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
    // Audio only — no video decoder, no window, no cover-art render pass.
    setOption("vid", "no");
    setOption("audio-display", "no");

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
    setOption("user-agent", "Mozilla/5.0 (compatible; Monolist/0.1)");

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

        case MPV_EVENT_SHUTDOWN:
            return;

        default:
            break;
        }
    }
}

void MpvEngine::load(const QString &urlOrPath, bool startPlaying)
{
    if (!m_mpv)
        return;

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
