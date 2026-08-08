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

void MpvEngine::load(const QString &urlOrPath, bool startPlaying)
{
    Q_UNUSED(urlOrPath)
    Q_UNUSED(startPlaying)
    Q_EMIT loadFailed(m_lastError);
}

void MpvEngine::stop() {}

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
    Q_UNUSED(speed)
}

void MpvEngine::setReplayGainEnabled(bool enabled)
{
    Q_UNUSED(enabled)
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
