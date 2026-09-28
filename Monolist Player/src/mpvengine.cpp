#include "mpvengine.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

#include <mpv/client.h>

namespace {

// Set on the number a video-add is asked with, so its answer is never taken
// for a loadfile's. The loads count up from 1 and never reach it.
constexpr quint64 kAddVideoTag = quint64(1) << 62;
// And on the upgrade player's loadfile (startUpgrade), which has its own.
constexpr quint64 kUpgradeTag = quint64(1) << 61;

// QT7, the mid-song upgrade (startUpgrade). BitChord's figures are the
// starting point (BITCHORD_ENGINE_RESEARCH.md 4.6 and 15); ours differ where
// said.
//
// How far ahead of the song the new player opens. JioSaavn's CDN answered in
// 0.13 s (median; 0.8 s at p90, a file not yet in its cache) and sent whole
// files at 35 MB/s (2.2 MB/s cold), so opening, seeking and buffering 12 s of
// 320 kbps (half a megabyte) takes a second or two at worst.
constexpr qint64 kUpgradeLeadMs = 5000;
// Where it aims again when the song reached the point first: a little ahead,
// since its buffer by then holds what follows. A few times, then no.
constexpr qint64 kUpgradeReaimMs = 2500;
constexpr int kUpgradeMaxAims = 4;
// Buffered past the point before the takeover (BitChord's
// UPGRADE_PREBUFFER_MS): a stall just after it would be the glitch the whole
// exercise avoids.
constexpr qint64 kUpgradePrebufferMs = 12000;
// No takeover begun, or finished, within this of the end (the owner's 15 s;
// BitChord waits for 20).
constexpr qint64 kUpgradeLastMs = 15000;
// The new file's length against the song's, both as mpv read them from the
// files: further apart, it is not the same cut (BitChord's UPGRADE_DRIFT_SEC,
// checked against the decoder's length as it is there).
constexpr qint64 kUpgradeDriftMs = 2000;
// Unpaused this long before the point, so that its sound, which takes a
// moment to reach the device, starts about where the song is.
constexpr qint64 kUpgradeStartEarlyMs = 30;
// The two clocks are read this often while they are lined up; within
// kUpgradeAlignedMs (the median of the last five readings) they count as
// together. mpv's clock moves in steps of the output's period (10 ms on
// WASAPI), which is about as close as it can say.
constexpr int kUpgradeTickMs = 10;
constexpr double kUpgradeAlignedMs = 10.0;
// Until then the new player's speed is nudged, by at most this much, to
// close the gap over about a second. It is silent, and plays with pitch
// correction off meanwhile, so a nudge is a plain resample nobody hears.
constexpr double kUpgradeMaxNudge = 0.04;
// Longer than this and the attempt is dropped rather than heard as an echo.
constexpr qint64 kUpgradeAlignLimitMs = 3000;
// The crossfade: eight steps of 10 ms. Both players apply a volume to what
// they decode next, a fifth of a second before it is heard (audio-buffer),
// so the fade happens that much later, on both at once.
constexpr int kUpgradeFadeSteps = 8;
constexpr int kUpgradeFadeStepMs = 10;
// ...and after its last step both play on this long, until what the old one
// had decoded at its old volume has been heard.
constexpr int kUpgradeSettleMs = 400;
// An attempt still waiting after this long is dropped.
constexpr int kUpgradeGiveUpMs = 60000;
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
    PropAudioBitrate,
    // The upgrade player's alone, dropped as it becomes the player.
    PropUpgradeCacheTime = 100,
    PropUpgradeCacheIdle
};

void setOptionOn(mpv_handle *mpv, const char *name, const char *value)
{
    if (mpv)
        mpv_set_option_string(mpv, name, value);
}

// mpv's clock for the file it plays, in seconds; negative while it has none.
double timePosOf(mpv_handle *mpv)
{
    double seconds = -1.0;
    if (!mpv || mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds) < 0)
        return -1.0;
    return seconds;
}

double medianOf(QList<double> values)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const qsizetype n = values.size();
    return n % 2 ? values.at(n / 2) : (values.at(n / 2 - 1) + values.at(n / 2)) / 2.0;
}

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

// The second player of a mid-song upgrade, and where it stands.
struct MpvEngine::Upgrade {
    enum Phase {
        Opening,    // opening the link and buffering from `target`
        Waiting,    // ready, paused at `target`, for the song to get there
        Aligning,   // unpaused and silent, its clock brought onto the song's
        Fading,     // the crossfade
        Settling    // faded over; the old player's last buffer playing out
    };
    mpv_handle *mpv = nullptr;   // null once it has become the player
    Phase phase = Opening;
    // Bumped whenever a step is scheduled: a step from before a new aim, a
    // seek or a new phase finds it moved on, and does nothing.
    quint64 token = 0;
    quint64 loadRequest = 0;
    qint64 entry = 0;            // the playlist entry its loadfile made
    bool restarted = false;      // it can play from `target` (since the last aim)
    bool cacheIdle = false;      // it has read all it is going to
    double cachedUntil = -1.0;   // s: the last moment it holds
    double duration = 0.0;       // s: its own length, as mpv read it
    qint64 target = 0;           // ms: where it takes over, on the song's clock
    // ms: how much later the music is in its file than in the song's, so it
    // waits at target + offset, and is lined up that far from the song.
    double offset = 0.0;
    int minKbps = 0;             // what its file must average (0: not checked)
    int kbps = 0;                // what it does, once known
    int aims = 0;
    double pausedAt = 0.0;       // s: its clock, paused at the point
    double baseSpeed = 1.0;      // the song's speed, which it returns to
    double speed = 1.0;          // its own, as last set
    int nudges = 0;
    int ticks = 0;
    int step = 0;                // of the crossfade
    QList<double> offsets;       // its clock less the song's, ms, lately
    double firstOffset = 0.0;    // ms, once it first sounded
    double finalOffset = 0.0;    // ms, as the crossfade began
    QString url;
    // Milliseconds since startUpgrade.
    QElapsedTimer clock;
    qint64 openedAt = -1;        // first able to play
    qint64 readyAt = -1;         // buffered enough
    qint64 unpausedAt = -1;
    qint64 soundingAt = -1;      // its clock first moved
    qint64 alignedAt = -1;
};

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
    // An upgrade under way goes with the engine, said to nobody.
    if (m_upgrade) {
        if (m_upgrade->mpv) {
            mpv_set_wakeup_callback(m_upgrade->mpv, nullptr, nullptr);
            mpv_terminate_destroy(m_upgrade->mpv);
        }
        delete std::exchange(m_upgrade, nullptr);
    }
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    // Players an upgrade replaced, still shutting down.
    for (QThread *thread : std::as_const(m_retiring)) {
        thread->wait();
        delete thread;
    }
    m_retiring.clear();
}

void MpvEngine::setOption(const char *name, const char *value)
{
    setOptionOn(m_mpv, name, value);
}

namespace {

// Every player's options, the upgrade's (startUpgrade) as much as this one's.
void applyBaseOptionsTo(mpv_handle *mpv)
{
    // Audio only until something asks for the picture (setVideoEnabled): no
    // video decoder, no cover-art render pass.
    setOptionOn(mpv, "vid", "no");
    setOptionOn(mpv, "audio-display", "no");

    // The picture is drawn by whoever holds the render context (VideoSurface),
    // never by mpv into a window of its own.
    setOptionOn(mpv, "vo", "libmpv");
    // The picture is drawn by the CPU (VideoSurface, SystemPip), scaled from
    // the video's size to the one shown. mpv's default scaler for that
    // (lanczos, dithered) took 44 ms a frame for 1080p on an M-series Mac —
    // more than a frame lasts, so the picture fell behind and the sound could
    // run dry. Bilinear takes 1-2 ms and, at these sizes, looks the same.
    setOptionOn(mpv, "zimg-scaler", "bilinear");
    setOptionOn(mpv, "zimg-dither", "no");

    // Stream URLs are resolved by StreamResolver (yt-dlp / Piped / Invidious)
    // before they reach mpv, so mpv's own ytdl hook is redundant and would add
    // a second, slower resolution path with different failure modes.
    setOptionOn(mpv, "ytdl", "no");

    // Stop at end of file rather than holding the last frame; the controller
    // decides what plays next.
    setOptionOn(mpv, "keep-open", "no");
    setOptionOn(mpv, "idle", "yes");
    // Paused until something is played, as m_paused already assumes. mpv
    // starts unpaused, which the controller would read as playing while the
    // song a launch opens on is still resolving — so the first press of Play
    // would pause it instead.
    setOptionOn(mpv, "pause", "yes");

    // Network buffering. Generous enough that a hiccup on a remote stream does
    // not audibly drop out: with the cache on, mpv reads ahead as far as this
    // allows (its cache-secs default is all but unlimited), which holds a
    // typical song whole. demuxer-readahead-secs is not set, because the
    // cache on overrides it; the 20 s once set here did nothing.
    setOptionOn(mpv, "cache", "yes");
    setOptionOn(mpv, "demuxer-max-bytes", "64MiB");

    // Normalise loudness across tracks from different sources.
    setOptionOn(mpv, "replaygain", "track");

    setOptionOn(mpv, "audio-client-name", "Monolist");
    // The browser yt-dlp says it is when it fetches a stream URL. YouTube
    // checks: a link extracted as one client and then fetched as another is
    // refused with 403, which is what an honest "Monolist/0.1" earned.
    setOptionOn(mpv, "user-agent", kBrowserUserAgent);

    // Keep libmpv from writing to the app's stderr; errors surface as signals.
    setOptionOn(mpv, "terminal", "no");
    setOptionOn(mpv, "msg-level", "all=no");
}

// What every player reports, the upgrade's too: once it becomes the player,
// drainEvents hears from it exactly as from the one it replaced.
void observePropertiesOn(mpv_handle *mpv)
{
    mpv_observe_property(mpv, PropTimePos,        "time-pos",             MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropDuration,       "duration",             MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropPause,          "pause",                MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropCoreIdle,       "core-idle",            MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropCacheBuffering, "paused-for-cache",     MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropMediaTitle,     "media-title",          MPV_FORMAT_STRING);
    mpv_observe_property(mpv, PropMetaArtist,     "metadata/by-key/Artist", MPV_FORMAT_STRING);
    // The picture's size as it should be shown (aspect ratio applied), which
    // is also how the surface knows a picture is there at all.
    mpv_observe_property(mpv, PropVideoWidth,     "dwidth",               MPV_FORMAT_INT64);
    mpv_observe_property(mpv, PropVideoHeight,    "dheight",              MPV_FORMAT_INT64);
    // Observing the list is also what starts mpv watching for devices coming
    // and going; the first answer arrives with the first events, well before
    // anyone could reach the output menu.
    mpv_observe_property(mpv, PropAudioDeviceList, "audio-device-list",   MPV_FORMAT_NODE);
    // What is actually playing (StreamInfo). A change only prompts a read of
    // the lot; mpv may fold a quick change and change back into nothing, so
    // the start of playback prompts one too (MPV_EVENT_PLAYBACK_RESTART).
    mpv_observe_property(mpv, PropAudioCodec,     "audio-codec-name",     MPV_FORMAT_NONE);
    mpv_observe_property(mpv, PropAudioParams,    "audio-params",         MPV_FORMAT_NONE);
    mpv_observe_property(mpv, PropAudioOutParams, "audio-out-params",     MPV_FORMAT_NONE);
    mpv_observe_property(mpv, PropCurrentAo,      "current-ao",           MPV_FORMAT_NONE);
    // mpv looks at the bitrate on every tick of its clock. With a value asked
    // for, it says only when the value changes (every second or so), where a
    // bare notification would come with every tick. Only wanted until it is
    // first known.
    mpv_observe_property(mpv, PropAudioBitrate,   "audio-bitrate",        MPV_FORMAT_NODE);
}

} // namespace

void MpvEngine::applyBaseOptions()
{
    applyBaseOptionsTo(m_mpv);
}

void MpvEngine::observeProperties()
{
    observePropertiesOn(m_mpv);
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
            case PropTimePos: {
                const double seconds = *static_cast<double *>(prop->data);
                Q_EMIT positionChanged(qint64(seconds * 1000.0));
                if (!m_audioStarted && seconds > 0.0) {
                    m_audioStarted = true;
                    Q_EMIT audioStarted();
                }
                break;
            }
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
            if (!currentFileStarted())
                break;
            refreshStreamInfo();
            if (event->event_id == MPV_EVENT_PLAYBACK_RESTART && !m_audioStarted && !m_paused) {
                m_audioStarted = true;
                Q_EMIT audioStarted();
            }
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
    // Whatever was on its way to take over the last file is not wanted for
    // this one.
    cancelUpgrade(QStringLiteral("another file was loaded"));

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
    m_audioStarted = false;

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
    cancelUpgrade(QStringLiteral("playback stopped"));
    ++m_loadRequest;
    m_currentEntry = 0;
    m_duration = 0;
    m_fileLoaded = false;
    m_loadingFile = false;
    m_audioStarted = false;
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
    // The picture is YouTube's, and comes with YouTube's sound.
    if (enabled)
        cancelUpgrade(QStringLiteral("the picture was turned on"));
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
    // Paused while the two sound together: the takeover is off, and the song
    // back at its own volume, before it pauses.
    if (paused && m_upgrade && m_upgrade->phase >= Upgrade::Aligning)
        cancelUpgrade(QStringLiteral("paused during the takeover"));
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvEngine::seekAbsolute(qint64 ms)
{
    if (!m_mpv)
        return;
    if (m_upgrade && m_upgrade->phase >= Upgrade::Aligning)
        cancelUpgrade(QStringLiteral("the song was moved during the takeover"));
    double seconds = double(ms) / 1000.0;
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
    // Still getting ready: it gets ready for the new place instead.
    if (m_upgrade && m_upgrade->phase <= Upgrade::Waiting)
        aimUpgrade(ms + kUpgradeLeadMs);
}

void MpvEngine::setVolume(qreal volume)
{
    if (!m_mpv)
        return;
    m_volume = qBound(0.0, double(volume), 1.0);
    double percent = m_volume * 100.0;
    // Mid-crossfade the two volumes are the fade's to set, from this one;
    // once it is over, the new player is the one that sounds.
    if (m_upgrade && m_upgrade->phase >= Upgrade::Fading) {
        if (m_upgrade->phase == Upgrade::Settling)
            mpv_set_property_async(m_upgrade->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &percent);
        return;
    }
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &percent);
}

void MpvEngine::setSpeed(qreal speed)
{
    if (!m_mpv)
        return;
    double value = qBound(0.25, double(speed), 4.0);
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &value);
    if (m_upgrade && m_upgrade->phase <= Upgrade::Waiting) {
        m_upgrade->baseSpeed = m_upgrade->speed = value;
        mpv_set_property_async(m_upgrade->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &value);
    }
}

void MpvEngine::setReplayGainEnabled(bool enabled)
{
    setOption("replaygain", enabled ? "track" : "no");
    if (m_upgrade)
        mpv_set_property_string(m_upgrade->mpv, "replaygain", enabled ? "track" : "no");
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
    // The player waiting to take over sounds where the song does.
    if (m_upgrade)
        mpv_set_property_async(m_upgrade->mpv, 0, "audio-device", MPV_FORMAT_STRING, &value);
}

// ------------------------------------------------------------------ upgrade

void MpvEngine::onUpgradeWakeup(void *ctx)
{
    auto *self = static_cast<MpvEngine *>(ctx);
    QMetaObject::invokeMethod(self, [self]() { self->drainUpgrade(); }, Qt::QueuedConnection);
}

bool MpvEngine::startUpgrade(const QString &url, double offsetMs, int minKbps, const QVariantMap &headers)
{
    if (!m_mpv || m_upgrade || url.isEmpty())
        return false;
    // A file that is playing its sound alone, whose length mpv knows.
    if (!currentFileStarted() || !m_fileLoaded || !m_audioStarted || m_video || m_duration <= 0)
        return false;
    const double now = timePosOf(m_mpv);
    if (now < 0.0)
        return false;
    const qint64 target = qint64(now * 1000.0) + kUpgradeLeadMs;
    if (m_duration - target < kUpgradeLastMs)
        return false;

    mpv_handle *mpv = mpv_create();
    if (!mpv)
        return false;
    applyBaseOptionsTo(mpv);
    // Silent until the crossfade brings it in; and until then its speed can
    // be nudged as a plain resample (see alignTick), pitch correction being
    // put back once it is the player.
    setOptionOn(mpv, "volume", "0");
    setOptionOn(mpv, "audio-pitch-correction", "no");
    // Where, and how, the song sounds now.
    const QString device = stringProperty(m_mpv, "audio-device");
    if (!device.isEmpty())
        setOptionOn(mpv, "audio-device", device.toUtf8().constData());
    const QString gain = stringProperty(m_mpv, "replaygain");
    if (!gain.isEmpty())
        setOptionOn(mpv, "replaygain", gain.toUtf8().constData());
    double speed = 1.0;
    numberProperty(m_mpv, "speed", &speed);
    setOptionOn(mpv, "speed", QByteArray::number(speed, 'f', 6).constData());
    const QString agent = headers.value(QStringLiteral("User-Agent")).toString();
    if (!agent.isEmpty())
        setOptionOn(mpv, "user-agent", agent.toUtf8().constData());
    setOptionOn(mpv, "start", QByteArray::number(qMax(0.0, (target + offsetMs) / 1000.0), 'f', 3).constData());
    if (mpv_initialize(mpv) < 0) {
        mpv_destroy(mpv);
        return false;
    }
    observePropertiesOn(mpv);
    mpv_observe_property(mpv, PropUpgradeCacheTime, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropUpgradeCacheIdle, "demuxer-cache-idle", MPV_FORMAT_FLAG);
    const QByteArray logLevel = qgetenv("MONOLIST_MPV_LOG");
    if (!logLevel.isEmpty())
        mpv_request_log_messages(mpv, logLevel.constData());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0)
            continue;
        const QByteArray field = (it.key() + QStringLiteral(": ") + it.value().toString()).toUtf8();
        const char *add[] = { "change-list", "http-header-fields", "append", field.constData(), nullptr };
        mpv_command(mpv, add);
    }

    auto *upgrade = new Upgrade;
    upgrade->mpv = mpv;
    upgrade->url = url;
    upgrade->target = target;
    upgrade->offset = offsetMs;
    upgrade->minKbps = minKbps;
    upgrade->aims = 1;
    upgrade->baseSpeed = upgrade->speed = speed;
    upgrade->clock.start();
    static quint64 loads = 0;
    upgrade->loadRequest = kUpgradeTag | ++loads;
    m_upgrade = upgrade;
    mpv_set_wakeup_callback(mpv, &MpvEngine::onUpgradeWakeup, this);

    const QByteArray target8 = url.toUtf8();
    const char *args[] = { "loadfile", target8.constData(), "replace", nullptr };
    if (mpv_command_async(mpv, upgrade->loadRequest, args) < 0) {
        endUpgrade(false, QStringLiteral("mpv would not take the new link"));
        return true;   // said, through upgradeFinished
    }
    qInfo("upgrade: opening the new link %lld ms ahead of the song, to take over at %.3f s of %.3f s "
          "(%.3f s in the new file, %+.1f ms apart)",
          static_cast<long long>(kUpgradeLeadMs), target / 1000.0, m_duration / 1000.0,
          (target + offsetMs) / 1000.0, offsetMs);
    // Never left waiting for good: a song paused for an hour, a link that
    // stalls without failing.
    const quint64 id = upgrade->loadRequest;
    QTimer::singleShot(kUpgradeGiveUpMs, this, [this, id]() {
        if (m_upgrade && m_upgrade->loadRequest == id && m_upgrade->phase <= Upgrade::Waiting)
            cancelUpgrade(QStringLiteral("not taken over within a minute"));
    });
    return true;
}

void MpvEngine::cancelUpgrade(const QString &why)
{
    if (m_upgrade)
        endUpgrade(false, why);
}

// The next step in `ms`, unless another has been scheduled since (the token).
void MpvEngine::upgradeAfter(int ms, void (MpvEngine::*step)())
{
    if (!m_upgrade)
        return;
    const quint64 token = ++m_upgrade->token;
    const quint64 id = m_upgrade->loadRequest;   // this attempt's, never reused
    QTimer::singleShot(qMax(0, ms), Qt::PreciseTimer, this, [this, id, token, step]() {
        if (m_upgrade && m_upgrade->loadRequest == id && m_upgrade->token == token)
            (this->*step)();
    });
}

void MpvEngine::drainUpgrade()
{
    // After it has become the player its events are drainEvents'; after it
    // has gone, nobody's.
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || !upgrade->mpv)
        return;
    mpv_handle *mpv = upgrade->mpv;
    while (m_upgrade == upgrade && upgrade->mpv == mpv) {
        mpv_event *event = mpv_wait_event(mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_COMMAND_REPLY:
            if (event->reply_userdata != upgrade->loadRequest)
                break;
            if (event->error < 0) {
                endUpgrade(false, QStringLiteral("the new link would not open: %1")
                                      .arg(QString::fromUtf8(mpv_error_string(event->error))));
                return;
            }
            upgrade->entry = playlistEntryOf(static_cast<mpv_event_command *>(event->data)->result);
            break;
        case MPV_EVENT_START_FILE:
            if (upgrade->entry <= 0)
                upgrade->entry = static_cast<mpv_event_start_file *>(event->data)->playlist_entry_id;
            break;
        // Paused, it has decoded where it was told to start: it can play
        // from there the moment it is asked.
        case MPV_EVENT_PLAYBACK_RESTART:
            upgrade->restarted = true;
            if (upgrade->openedAt < 0)
                upgrade->openedAt = upgrade->clock.elapsed();
            judgeUpgrade();
            break;
        case MPV_EVENT_END_FILE: {
            const auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (upgrade->entry > 0 && end->playlist_entry_id != upgrade->entry)
                break;
            endUpgrade(false, end->reason == MPV_END_FILE_REASON_ERROR
                                  ? QStringLiteral("the new link stopped: %1")
                                        .arg(QString::fromUtf8(mpv_error_string(end->error)))
                                  : QStringLiteral("the new link ended before it could take over"));
            return;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *prop = static_cast<mpv_event_property *>(event->data);
            if (!prop->data)
                break;
            switch (event->reply_userdata) {
            case PropDuration:
                upgrade->duration = *static_cast<double *>(prop->data);
                judgeUpgrade();
                break;
            case PropUpgradeCacheTime:
                upgrade->cachedUntil = *static_cast<double *>(prop->data);
                judgeUpgrade();
                break;
            case PropUpgradeCacheIdle:
                upgrade->cacheIdle = *static_cast<int *>(prop->data) != 0;
                judgeUpgrade();
                break;
            default:
                break;
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto *message = static_cast<mpv_event_log_message *>(event->data);
            qWarning("mpv[%s] upgrade %s: %s", message->level, message->prefix,
                     QByteArray(message->text).trimmed().constData());
            break;
        }
        default:
            break;
        }
    }
}

// Whether the new player can take over yet: the same length as the song,
// able to play from the point, and holding kUpgradePrebufferMs past it (or
// everything there is).
void MpvEngine::judgeUpgrade()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Opening)
        return;
    if (upgrade->duration > 0.0 && qAbs(qint64(upgrade->duration * 1000.0) - m_duration) > kUpgradeDriftMs) {
        endUpgrade(false, QStringLiteral("the new file is %1 s long and the song %2 s: not the same cut")
                              .arg(upgrade->duration, 0, 'f', 1).arg(m_duration / 1000.0, 0, 'f', 1));
        return;
    }
    // Its own length is needed, for the check above and the one below.
    if (!upgrade->restarted || upgrade->cachedUntil < 0.0 || upgrade->duration <= 1.0)
        return;
    // What it really is, its size over its length: a link named for 320
    // kbps has been seen to serve 98. A file that does not say its size
    // (no Content-Length) is let through.
    double bytes = 0.0;
    if (upgrade->kbps == 0 && numberProperty(upgrade->mpv, "file-size", &bytes) && bytes > 0.0) {
        upgrade->kbps = qRound(bytes * 8.0 / upgrade->duration / 1000.0);
        if (upgrade->kbps < upgrade->minKbps) {
            endUpgrade(false, QStringLiteral("the new file averages %1 kbps, short of the %2 it has to be worth")
                                  .arg(upgrade->kbps).arg(upgrade->minKbps));
            return;
        }
    }
    // In its own file's time.
    const qint64 point = upgrade->target + qint64(upgrade->offset);
    const qint64 cached = qint64(upgrade->cachedUntil * 1000.0);
    const qint64 wanted = upgrade->duration > 0.0
                              ? qMin(point + kUpgradePrebufferMs, qint64(upgrade->duration * 1000.0) - 1000)
                              : point + kUpgradePrebufferMs;
    if (cached < wanted && !(upgrade->cacheIdle && cached > point))
        return;
    upgrade->phase = Upgrade::Waiting;
    if (upgrade->readyAt < 0)
        upgrade->readyAt = upgrade->clock.elapsed();
    awaitTakeover();
}

void MpvEngine::aimUpgrade(qint64 at)
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade)
        return;
    if (++upgrade->aims > kUpgradeMaxAims) {
        endUpgrade(false, QStringLiteral("the song reached the point first %1 times").arg(kUpgradeMaxAims));
        return;
    }
    if (m_duration - at < kUpgradeLastMs) {
        endUpgrade(false, QStringLiteral("too near the end by now"));
        return;
    }
    upgrade->target = at;
    upgrade->restarted = false;
    upgrade->phase = Upgrade::Opening;
    ++upgrade->token;   // whatever was scheduled for the old point
    // Paused, it reports a restart once it can play from there; from its
    // buffer when that holds the place, which it usually does by now.
    const QByteArray seconds = QByteArray::number(qMax(0.0, (at + upgrade->offset) / 1000.0), 'f', 3);
    const char *args[] = { "seek", seconds.constData(), "absolute+exact", nullptr };
    mpv_command_async(upgrade->mpv, 0, args);
    qInfo("upgrade: aiming again, at %.3f s", at / 1000.0);
}

// Waits for the song to reach the point, looking again at its clock shortly
// before: the song may be paused or moved meanwhile, and the clock read
// nearer the moment is the truer one.
void MpvEngine::awaitTakeover()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Waiting)
        return;
    const double now = timePosOf(m_mpv);
    const qint64 at = qint64(now * 1000.0);
    const qint64 start = upgrade->target - kUpgradeStartEarlyMs;
    if (now < 0.0 || at > start - 5) {
        aimUpgrade(qMax<qint64>(at, 0) + kUpgradeReaimMs);
        return;
    }
    if (m_duration - upgrade->target < kUpgradeLastMs) {
        endUpgrade(false, QStringLiteral("too near the end by now"));
        return;
    }
    if (m_paused) {
        upgradeAfter(250, &MpvEngine::awaitTakeover);
        return;
    }
    double speed = 1.0;
    numberProperty(m_mpv, "speed", &speed);
    const qint64 wait = qint64(double(start - at) / qMax(0.25, speed));
    if (wait > 400) {
        upgradeAfter(int(wait - 250), &MpvEngine::awaitTakeover);
        return;
    }
    upgradeAfter(int(wait), &MpvEngine::beginTakeover);
}

// The song is at the point: the new player starts, silent.
void MpvEngine::beginTakeover()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Waiting)
        return;
    const double now = timePosOf(m_mpv);
    const qint64 at = qint64(now * 1000.0);
    // Paused or moved since the timer was set, or the timer late: look again.
    if (m_paused || now < 0.0 || qAbs(at - (upgrade->target - kUpgradeStartEarlyMs)) > 40) {
        awaitTakeover();
        return;
    }
    upgrade->phase = Upgrade::Aligning;
    upgrade->pausedAt = timePosOf(upgrade->mpv);
    numberProperty(m_mpv, "speed", &upgrade->baseSpeed);
    upgrade->speed = upgrade->baseSpeed;
    int no = 0;
    mpv_set_property_async(upgrade->mpv, 0, "pause", MPV_FORMAT_FLAG, &no);
    upgrade->unpausedAt = upgrade->clock.elapsed();
    upgradeAfter(kUpgradeTickMs, &MpvEngine::alignTick);
}

// Its clock onto the song's. Both clocks say what is being heard, each net of
// its own output's delay, so their difference is how far apart the two
// sounds are. A nudge to its speed closes it.
void MpvEngine::alignTick()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Aligning)
        return;
    const double fresh = timePosOf(upgrade->mpv);
    const double old = timePosOf(m_mpv);
    const qint64 now = upgrade->clock.elapsed();
    if (upgrade->soundingAt < 0) {
        if (fresh <= upgrade->pausedAt + 0.0005) {
            if (now - upgrade->unpausedAt > 1500) {
                endUpgrade(false, QStringLiteral("the new player did not start"));
                return;
            }
            upgradeAfter(kUpgradeTickMs, &MpvEngine::alignTick);
            return;
        }
        upgrade->soundingAt = now;
    }
    if (fresh < 0.0 || old < 0.0) {
        endUpgrade(false, QStringLiteral("a clock went missing during the takeover"));
        return;
    }
    // How far the new one is from where it should be: its clock less the
    // song's, less how much later the music comes in its file (+: ahead).
    const double offset = (fresh - old) * 1000.0 - upgrade->offset;
    if (upgrade->offsets.isEmpty())
        upgrade->firstOffset = offset;
    upgrade->offsets.append(offset);
    if (upgrade->offsets.size() > 5)
        upgrade->offsets.removeFirst();
    ++upgrade->ticks;
    if (upgrade->offsets.size() >= 3) {
        const double typical = medianOf(upgrade->offsets);
        if (qAbs(typical) <= kUpgradeAlignedMs) {
            if (upgrade->speed != upgrade->baseSpeed) {
                double back = upgrade->baseSpeed;
                mpv_set_property_async(upgrade->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &back);
                upgrade->speed = back;
            }
            upgrade->finalOffset = typical;
            upgrade->alignedAt = now;
            upgrade->phase = Upgrade::Fading;
            upgrade->step = 0;
            upgradeAfter(0, &MpvEngine::fadeTick);
            return;
        }
        if (now - upgrade->soundingAt > kUpgradeAlignLimitMs) {
            endUpgrade(false, QStringLiteral("the two could not be lined up (still %1 ms apart after %2 s)")
                                  .arg(typical, 0, 'f', 0).arg(kUpgradeAlignLimitMs / 1000));
            return;
        }
        // Every 50 ms: a speed that would close the gap in about a second.
        if (upgrade->ticks % 5 == 0) {
            double speed = upgrade->baseSpeed
                           * (1.0 - qBound(-kUpgradeMaxNudge, typical / 1000.0, kUpgradeMaxNudge));
            speed = std::round(speed * 1000.0) / 1000.0;
            if (qAbs(speed - upgrade->speed) >= 0.001) {
                mpv_set_property_async(upgrade->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
                upgrade->speed = speed;
                ++upgrade->nudges;
            }
        }
    }
    upgradeAfter(kUpgradeTickMs, &MpvEngine::alignTick);
}

// One step of the crossfade. mpv's volume is cubic (a setting of p plays at
// (p/100)^3), so each is set to the cube root of a straight line: lined up,
// the same sound from the two adds up to the song at its own level
// throughout, with no dip in the middle and no rise.
void MpvEngine::fadeTick()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Fading)
        return;
    ++upgrade->step;
    const double full = m_volume * 100.0;
    const double share = double(upgrade->step) / double(kUpgradeFadeSteps);
    double in = full * std::cbrt(share);
    double out = full * std::cbrt(1.0 - share);
    mpv_set_property_async(upgrade->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &in);
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &out);
    if (upgrade->step < kUpgradeFadeSteps) {
        upgradeAfter(kUpgradeFadeStepMs, &MpvEngine::fadeTick);
        return;
    }
    upgrade->phase = Upgrade::Settling;
    upgradeAfter(kUpgradeSettleMs, &MpvEngine::promoteUpgrade);
}

// The new player becomes this one: every signal from here on is about it,
// and the old one is shut down out of the way.
void MpvEngine::promoteUpgrade()
{
    Upgrade *upgrade = m_upgrade;
    if (!upgrade || upgrade->phase != Upgrade::Settling || !upgrade->mpv)
        return;
    // What each said it was playing as the old one stopped: the step in the
    // song, if any, at the moment it could still be heard.
    const double fresh = timePosOf(upgrade->mpv);
    const double old = timePosOf(m_mpv);
    int yes = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &yes);

    // Its render context first: freed on the old player, which must outlive
    // it, and made again on the new one below.
    Q_EMIT handleAboutToChange();
    mpv_handle *previous = m_mpv;
    mpv_set_wakeup_callback(previous, nullptr, nullptr);
    mpv_handle *mpv = std::exchange(upgrade->mpv, nullptr);
    mpv_set_wakeup_callback(mpv, nullptr, nullptr);
    mpv_unobserve_property(mpv, PropUpgradeCacheTime);
    mpv_unobserve_property(mpv, PropUpgradeCacheIdle);
    mpv_set_property_string(mpv, "audio-pitch-correction", "yes");
    m_mpv = mpv;
    retire(previous);

    // Its file is the current one, open, sounding and unpaused.
    m_currentEntry = m_startedEntry = upgrade->entry > 0 ? upgrade->entry : -1;
    if (m_currentEntry < 0)
        m_currentEntry = m_startedEntry = 1;
    m_fileLoaded = true;
    m_loadingFile = false;
    m_audioStarted = true;
    if (m_paused) {
        m_paused = false;
        Q_EMIT pausedChanged(false);
    }
    if (m_buffering) {
        m_buffering = false;
        Q_EMIT bufferingChanged(false);
    }
    const qint64 length = qint64(upgrade->duration * 1000.0);
    if (length > 0 && length != m_duration) {
        m_duration = length;
        Q_EMIT durationChanged(length);
    }
    mpv_set_wakeup_callback(mpv, &MpvEngine::onWakeup, this);
    Q_EMIT handleChanged();
    clearStreamInfo();
    refreshStreamInfo();

    ++m_upgradesDone;
    // Every "apart" is net of how much later the music is in the new file:
    // 0 is the same moment of music in both.
    const QString detail = QStringLiteral(
        "the music %1 ms later in the new file; opened in %2 ms and buffered in %3 ms, aimed %4 time(s); "
        "its sound started %5 ms after it was unpaused, %6 ms from the song's, and was lined up to %7 ms "
        "in %8 ms with %9 speed nudge(s); crossfade %10 ms; at the handover the new clock read %11 s and "
        "the old %12 s (%13 ms apart), with no silence between them")
        .arg(upgrade->offset, 0, 'f', 1)
        .arg(upgrade->openedAt).arg(upgrade->readyAt).arg(upgrade->aims)
        .arg(upgrade->soundingAt - upgrade->unpausedAt)
        .arg(upgrade->firstOffset, 0, 'f', 0).arg(upgrade->finalOffset, 0, 'f', 0)
        .arg(upgrade->alignedAt - upgrade->soundingAt).arg(upgrade->nudges)
        .arg(kUpgradeFadeSteps * kUpgradeFadeStepMs)
        .arg(fresh, 0, 'f', 3).arg(old, 0, 'f', 3).arg((fresh - old) * 1000.0 - upgrade->offset, 0, 'f', 0);
    endUpgrade(true, (upgrade->kbps > 0 ? QStringLiteral("the new file averages %1 kbps; ").arg(upgrade->kbps)
                                        : QString()) + detail);
    // What it said while it was taking over, and since, is heard now.
    QMetaObject::invokeMethod(this, &MpvEngine::drainEvents, Qt::QueuedConnection);
}

void MpvEngine::endUpgrade(bool swapped, const QString &detail)
{
    Upgrade *upgrade = std::exchange(m_upgrade, nullptr);
    if (!upgrade)
        return;
    if (upgrade->mpv) {
        // Not taken over: it goes, and the song is as it was, at its own
        // volume should the crossfade have begun.
        mpv_set_wakeup_callback(upgrade->mpv, nullptr, nullptr);
        if (upgrade->phase >= Upgrade::Fading && m_mpv) {
            double percent = m_volume * 100.0;
            mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &percent);
        }
        retire(upgrade->mpv);
    }
    delete upgrade;
    Q_EMIT upgradeFinished(swapped, detail);
}

void MpvEngine::retire(mpv_handle *mpv)
{
    if (!mpv)
        return;
    QThread *thread = QThread::create([mpv]() { mpv_terminate_destroy(mpv); });
    m_retiring.append(thread);
    connect(thread, &QThread::finished, this, [this, thread]() {
        m_retiring.removeOne(thread);
        thread->deleteLater();
    });
    thread->start();
}
