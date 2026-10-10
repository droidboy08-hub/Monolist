#include "sleeptimer.h"

#include <QVariant>

#include <algorithm>

SleepTimer::SleepTimer(QObject *player, QObject *parent)
    : QObject(parent), m_player(player)
{
    // Once a second for the countdown; the expiry itself is checked at each
    // tick too, against the elapsed clock rather than a long single-shot
    // timer, so a machine that slept wakes to the right answer.
    m_ticker.setInterval(1000);
    connect(&m_ticker, &QTimer::timeout, this, [this]() {
        if (!m_endOfSong && m_clock.elapsed() >= m_durationMs)
            expire();
        else
            Q_EMIT tick();
    });
    m_fader.setInterval(50);
    connect(&m_fader, &QTimer::timeout, this, &SleepTimer::fadeStep);

    // The song waited for has ended: the player stopped after it, and has
    // turned its switch off again.
    if (m_player)
        connect(m_player, SIGNAL(stoppedAfterCurrent()), this, SLOT(songEnded()));
}

void SleepTimer::songEnded()
{
    if (!m_active || !m_endOfSong)
        return;
    reset();
    Q_EMIT finished();
}

int SleepTimer::remainingSeconds() const
{
    if (!m_active || m_endOfSong)
        return 0;
    const qint64 left = m_durationMs - m_clock.elapsed();
    return int(std::max<qint64>(0, (left + 999) / 1000));
}

QString SleepTimer::remainingText() const
{
    if (!m_active)
        return {};
    if (m_endOfSong)
        return QStringLiteral("End of this song");
    const int seconds = remainingSeconds();
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const QString rest = QStringLiteral("%1").arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return hours > 0 ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(rest)
                     : QStringLiteral("%1:%2").arg(minutes).arg(rest);
}

void SleepTimer::start(int minutes)
{
    if (minutes > 0)
        startMs(qint64(minutes) * 60 * 1000);
}

void SleepTimer::startMs(qint64 ms)
{
    if (m_fading)
        endFade(/*pausePlayer=*/false);
    if (m_endOfSong && m_player)
        m_player->setProperty("stopAfterCurrent", false);
    m_active = true;
    m_endOfSong = false;
    m_durationMs = ms;
    m_clock.start();
    m_ticker.start();
    Q_EMIT changed();
    Q_EMIT tick();
}

void SleepTimer::startEndOfSong()
{
    if (!m_player)
        return;
    if (m_fading)
        endFade(/*pausePlayer=*/false);
    m_ticker.stop();
    m_active = true;
    m_endOfSong = true;
    m_player->setProperty("stopAfterCurrent", true);
    Q_EMIT changed();
    Q_EMIT tick();
}

void SleepTimer::extend(int minutes)
{
    if (!m_active || minutes <= 0)
        return;
    if (m_endOfSong) {
        start(minutes);
        return;
    }
    // Asked for during the fade: the music comes back up, and the clock
    // starts again from what is added.
    if (m_fading) {
        endFade(/*pausePlayer=*/false);
        startMs(qint64(minutes) * 60 * 1000);
        return;
    }
    m_durationMs += qint64(minutes) * 60 * 1000;
    Q_EMIT tick();
}

void SleepTimer::cancel()
{
    if (!m_active)
        return;
    if (m_fading)
        endFade(/*pausePlayer=*/false);
    if (m_endOfSong && m_player)
        m_player->setProperty("stopAfterCurrent", false);
    reset();
}

void SleepTimer::reset()
{
    m_ticker.stop();
    m_fader.stop();
    const bool was = m_active || m_fading;
    m_active = false;
    m_endOfSong = false;
    m_fading = false;
    m_durationMs = 0;
    if (was) {
        Q_EMIT changed();
        Q_EMIT tick();
    }
}

// The time is up. Paused already, there is nothing to fade: done at once.
void SleepTimer::expire()
{
    m_ticker.stop();
    if (!m_player || !m_player->property("playing").toBool()) {
        reset();
        Q_EMIT finished();
        return;
    }
    m_volumeBefore = m_player->property("volume").toReal();
    m_fading = true;
    m_fadeClock.start();
    m_fader.start();
    Q_EMIT changed();
    Q_EMIT tick();
}

void SleepTimer::fadeStep()
{
    if (!m_player) {
        reset();
        return;
    }
    // Paused by hand during the fade: that was the point; stop here.
    if (!m_player->property("playing").toBool()) {
        endFade(/*pausePlayer=*/false);
        reset();
        Q_EMIT finished();
        return;
    }
    const qreal done = std::min<qreal>(1.0, qreal(m_fadeClock.elapsed()) / std::max(1, m_fadeMs));
    // Down by the ear rather than by the number: the volume is already
    // cubic in mpv, so an even step in it sounds even.
    m_player->setProperty("volume", m_volumeBefore * (1.0 - done));
    if (done >= 1.0) {
        endFade(/*pausePlayer=*/true);
        reset();
        Q_EMIT finished();
    }
}

// Paused if asked, and the volume put back as it was before the fade.
void SleepTimer::endFade(bool pausePlayer)
{
    m_fader.stop();
    m_fading = false;
    if (!m_player)
        return;
    if (pausePlayer)
        QMetaObject::invokeMethod(m_player, "pause");
    m_player->setProperty("volume", m_volumeBefore);
}
