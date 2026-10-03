// Null implementation of MpvEngine, compiled instead of mpvengine.cpp when the
// project is configured with -DMONOLIST_NO_MPV=ON.
//
// Purpose: decouple the first build from libmpv, which is the one dependency
// with no clean Windows package. Everything except audio output works in this
// mode — the interface, the database, yt-dlp search, and downloads (which write
// files through yt-dlp and never touch mpv).
//
// The header is unchanged, so switching back is a configure flag, not a code
// change. m_mpv stays null, which makes isValid() false, which makes
// PlaybackController report "Audio engine unavailable" instead of appearing to
// play silence.

#include "mpvengine.h"

MpvEngine::MpvEngine(QObject *parent)
    : QObject(parent)
{
    m_lastError = QStringLiteral(
        "Built without libmpv (MONOLIST_NO_MPV=ON) — playback is disabled. "
        "Reconfigure with -DMONOLIST_NO_MPV=OFF and -DMPV_ROOT=<path> to enable audio.");
}

MpvEngine::~MpvEngine() = default;

bool MpvEngine::load(const QString &urlOrPath, bool startPlaying, const QString &audioUrl, qint64 startAt,
                     const QVariantMap &headers)
{
    Q_UNUSED(urlOrPath)
    Q_UNUSED(startPlaying)
    Q_UNUSED(audioUrl)
    Q_UNUSED(startAt)
    Q_UNUSED(headers)
    Q_EMIT loadFailed(m_lastError);
    return false;
}

// Nothing ever plays, so there is never anything to say about it.
void MpvEngine::refreshStreamInfo() {}

void MpvEngine::clearStreamInfo() {}

// Nothing plays, so there is never a picture: the switch is remembered and
// changes nothing.
void MpvEngine::setVideoEnabled(bool enabled)
{
    m_video = enabled;
}

void MpvEngine::setVideoWatched(bool watched)
{
    m_watched = watched;
}

void MpvEngine::stop() {}

// Nothing plays, so nothing is ever taken over.
bool MpvEngine::startUpgrade(const QString &url, double offsetMs, int minKbps, const QVariantMap &headers)
{
    Q_UNUSED(url)
    Q_UNUSED(offsetMs)
    Q_UNUSED(minKbps)
    Q_UNUSED(headers)
    return false;
}

void MpvEngine::cancelUpgrade(const QString &why)
{
    Q_UNUSED(why)
}

void MpvEngine::addVideo(const QString &url, const QVariantMap &headers)
{
    Q_UNUSED(url)
    Q_UNUSED(headers)
    Q_EMIT videoAddFailed(m_lastError);
}

void MpvEngine::applyHeaders(const QVariantMap &headers)
{
    Q_UNUSED(headers)
}

void MpvEngine::setPaused(bool paused)
{
    Q_UNUSED(paused)
}

void MpvEngine::seekAbsolute(qint64 ms)
{
    Q_UNUSED(ms)
}

void MpvEngine::setVolume(qreal volume)
{
    Q_UNUSED(volume)
}

void MpvEngine::setSpeed(qreal speed)
{
    m_userSpeed = qBound(0.25, double(speed), 4.0);
}

// Nothing plays, so the effects are only kept: effects() answers with what
// was set, and mpv's side of them is empty.
void MpvEngine::setEffects(const SoundChain::Settings &settings)
{
    m_fx = SoundChain::sanitized(settings);
}

bool MpvEngine::setAudioFilters(const QString &afValue)
{
    Q_UNUSED(afValue)
    return false;
}

QString MpvEngine::audioFilterProperty() const
{
    return QString();
}

double MpvEngine::speedProperty() const
{
    return 1.0;
}

bool MpvEngine::pitchCorrection() const
{
    return true;
}

bool MpvEngine::effectsAnswer()
{
    return false;
}

QStringList MpvEngine::effectsRefused()
{
    return QStringList();
}

bool MpvEngine::hasSoundTrack() const
{
    return false;
}

QString MpvEngine::levellingPreamp() const
{
    return QString();
}

void MpvEngine::setReplayGainEnabled(bool enabled)
{
    Q_UNUSED(enabled)
}

void MpvEngine::setLevelling(bool on, double fallbackDb)
{
    Q_UNUSED(on)
    Q_UNUSED(fallbackDb)
}

double MpvEngine::fallbackGain() const
{
    return 0.0;
}

// No devices are ever listed, so the output menu offers Auto alone.
void MpvEngine::setAudioDevice(const QString &name)
{
    Q_UNUSED(name)
}

// Never invoked — nothing installs a wakeup callback in this build — but moc
// emits a call into drainEvents for the private slot, so it needs a body.
void MpvEngine::drainEvents() {}

void MpvEngine::onWakeup(void *ctx)
{
    Q_UNUSED(ctx)
}

void MpvEngine::observeProperties() {}

void MpvEngine::applyBaseOptions() {}

void MpvEngine::setOption(const char *name, const char *value)
{
    Q_UNUSED(name)
    Q_UNUSED(value)
}
