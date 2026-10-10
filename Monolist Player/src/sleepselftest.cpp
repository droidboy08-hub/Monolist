#include "sleepselftest.h"

#include "sleeptimer.h"

#include <QEventLoop>
#include <QTimer>

#include <functional>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("sleep-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("sleep-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

} // namespace

// The player as the timer sees it: a volume, playing or not, the switch to
// stop after the song, pause(), and the word that the song has ended.
class StandInPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal volume MEMBER volume)
    Q_PROPERTY(bool playing MEMBER playing)
    Q_PROPERTY(bool stopAfterCurrent MEMBER stopAfterCurrent)

public:
    qreal volume = 0.8;
    bool playing = true;
    bool stopAfterCurrent = false;
    int pauses = 0;
    qreal lowest = 1.0;

public Q_SLOTS:
    void pause()
    {
        ++pauses;
        playing = false;
        lowest = std::min(lowest, volume);
    }

Q_SIGNALS:
    void stoppedAfterCurrent();
};

int runSleepSelfTest()
{
    Checks t;
    StandInPlayer player;
    SleepTimer timer(&player);
    timer.setFadeMs(400);
    int finished = 0;
    QObject::connect(&timer, &SleepTimer::finished, [&finished]() { ++finished; });
    qreal quietest = 1.0;
    QObject::connect(&timer, &SleepTimer::changed, [&]() {});

    t.check(!timer.active() && timer.remainingText().isEmpty(), QStringLiteral("off to begin with"));

    // — the clock —
    timer.start(30);
    t.check(timer.active() && !timer.endOfSong() && timer.remainingSeconds() == 1800
                && timer.remainingText() == QLatin1String("30:00"),
            QStringLiteral("30 minutes: on, 30:00 left"), timer.remainingText());
    timer.extend(10);
    t.check(timer.remainingSeconds() == 2400 && timer.remainingText() == QLatin1String("40:00"),
            QStringLiteral("  +10 minutes: 40:00"), timer.remainingText());
    timer.start(60);
    t.check(timer.remainingText() == QLatin1String("1:00:00"), QStringLiteral("  an hour reads 1:00:00"),
            timer.remainingText());
    timer.cancel();
    t.check(!timer.active() && player.pauses == 0 && qFuzzyCompare(player.volume, 0.8),
            QStringLiteral("turned off: nothing paused, the volume untouched"));

    // — running out, with the fade —
    timer.startMs(1500);
    QTimer sampler;
    sampler.setInterval(20);
    QObject::connect(&sampler, &QTimer::timeout, [&]() { quietest = std::min(quietest, player.volume); });
    sampler.start();
    const bool faded = waitUntil([&]() { return timer.fading(); }, 4000);
    t.check(faded, QStringLiteral("the time up: the music fading"));
    waitUntil([&]() { return finished == 1; }, 3000);
    sampler.stop();
    t.check(finished == 1 && player.pauses == 1 && !player.playing, QStringLiteral("  then paused, and said once"));
    t.check(quietest < 0.2 && player.lowest < 0.05,
            QStringLiteral("  it went down to nothing before the pause"),
            QString::number(player.lowest, 'f', 3));
    t.check(qFuzzyCompare(player.volume, 0.8), QStringLiteral("  and the volume is put back for the next play"),
            QString::number(player.volume, 'f', 3));
    t.check(!timer.active() && !timer.fading(), QStringLiteral("  the timer off"));

    // — already paused when the time is up —
    player.playing = false;
    player.pauses = 0;
    finished = 0;
    timer.startMs(1200);
    waitUntil([&]() { return finished == 1; }, 4000);
    t.check(finished == 1 && player.pauses == 0 && qFuzzyCompare(player.volume, 0.8),
            QStringLiteral("paused already: nothing to fade, the timer just ends"));

    // — +10 minutes during the fade —
    player.playing = true;
    finished = 0;
    timer.startMs(1100);
    waitUntil([&]() { return timer.fading(); }, 4000);
    timer.extend(10);
    t.check(timer.active() && !timer.fading() && qFuzzyCompare(player.volume, 0.8) && player.pauses == 0
                && timer.remainingSeconds() == 600,
            QStringLiteral("+10 minutes while fading: the music back up, 10:00 on the clock"), timer.remainingText());
    timer.cancel();

    // — paused by hand during the fade —
    player.playing = true;
    finished = 0;
    timer.startMs(1100);
    waitUntil([&]() { return timer.fading(); }, 4000);
    player.playing = false;
    waitUntil([&]() { return finished == 1; }, 2000);
    t.check(finished == 1 && qFuzzyCompare(player.volume, 0.8) && !timer.active(),
            QStringLiteral("paused by hand while fading: done, the volume put back"));

    // — the end of the song —
    player.playing = true;
    player.pauses = 0;
    finished = 0;
    timer.startEndOfSong();
    t.check(timer.active() && timer.endOfSong() && player.stopAfterCurrent
                && timer.remainingText() == QLatin1String("End of this song"),
            QStringLiteral("end of this song: the player told to stop after it"));
    timer.cancel();
    t.check(!timer.active() && !player.stopAfterCurrent, QStringLiteral("  turned off: and told not to"));
    timer.startEndOfSong();
    timer.start(15);
    t.check(!player.stopAfterCurrent && timer.remainingText() == QLatin1String("15:00"),
            QStringLiteral("  a clock chosen instead: the song no longer waited for"));
    timer.startEndOfSong();
    // The player stops after the song and says so, its switch off again.
    player.stopAfterCurrent = false;
    player.playing = false;
    Q_EMIT player.stoppedAfterCurrent();
    t.check(finished == 1 && !timer.active(), QStringLiteral("  the song over: the timer done, said once"));
    Q_EMIT player.stoppedAfterCurrent();
    t.check(finished == 1, QStringLiteral("  and not again for another song's end"));

    return t.finish();
}

#include "sleepselftest.moc"
