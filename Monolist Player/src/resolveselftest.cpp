#include "resolveselftest.h"

#include "innertube.h"
#include "streamresolver.h"
#include "ytdlp.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>

namespace {

// One line per check, and a count at the end, as in connectionselftest.cpp.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qPrintable(text)); }

    int finish()
    {
        qWarning("%s: %d checks, %d failed", m_prefix, m_count, m_failed);
        return m_failed;
    }

private:
    const char *m_prefix;
    int m_count = 0;
    int m_failed = 0;
};

const QString kDefaultVideo = QStringLiteral("LrM_Y39Gmhk");

// Runs the event loop until `done` holds or `timeoutMs` passes.
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

void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The reaper's lines (ytdlp.cpp), which say when a cancelled lookup's
// processes ended and how many were left in its job.
QStringList *g_reaped = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_reaped && message.contains(QLatin1String("a cancelled lookup ended")))
        g_reaped->append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

qint64 median(QList<qint64> values)
{
    if (values.isEmpty())
        return -1;
    std::sort(values.begin(), values.end());
    const qsizetype n = values.size();
    return n % 2 ? values.at(n / 2) : (values.at(n / 2 - 1) + values.at(n / 2)) / 2;
}

qint64 largest(const QList<qint64> &values)
{
    return values.isEmpty() ? -1 : *std::max_element(values.begin(), values.end());
}

} // namespace

int runCancelSelfTest(const QString &videoIdArgument, int roundsArgument)
{
    Checks t("cancel");
    const QString videoId = videoIdArgument.isEmpty() ? kDefaultVideo : videoIdArgument;
    const int rounds = roundsArgument > 0 ? roundsArgument : 6;
    if (!t.check(YtDlp::isAvailable(), QStringLiteral("yt-dlp is installed")))
        return t.finish();

    QStringList reaped;
    g_reaped = &reaped;
    g_previousHandler = qInstallMessageHandler(captureMessage);

    // The watchdog: a tick every 5 ms on this thread, and the longest wait
    // between two ticks since it was last reset. Anything that holds the
    // thread, a cancel included, shows up as a gap.
    QElapsedTimer clock;
    clock.start();
    qint64 lastTick = 0;
    qint64 worstGap = 0;
    QTimer tick;
    tick.setTimerType(Qt::PreciseTimer);
    tick.setInterval(5);
    QObject::connect(&tick, &QTimer::timeout, [&]() {
        const qint64 now = clock.elapsed();
        worstGap = qMax(worstGap, now - lastTick);
        lastTick = now;
    });
    lastTick = clock.elapsed();
    tick.start();

    bool answered = false;
    // What a round answered with, when it did before the cancel came.
    QString answer;

    const int offsets[] = { 300, 900, 1800 };
    struct Mode {
        QList<qint64> calls;   // how long resolver.cancel() itself took
        QList<qint64> gaps;    // the watchdog's longest gap, the cancel included
        int voided = 0;        // yt-dlp had answered before the cancel came
    };
    Mode waitless;
    Mode waiting;
    int reapedExpected = 0;
    int reapedClean = 0;   // seen to end, with nothing left in the job

    t.note(QStringLiteral("%1 rounds each way on %2, the cancel at 0.3, 0.9 or 1.8 s").arg(rounds).arg(videoId));
    for (int i = 0; i < 2 * rounds; ++i) {
        // New and old in turn, so both see the same network and machine.
        const bool wait = i % 2 == 1;
        const int at = offsets[(i / 2) % 3];
        Mode &mode = wait ? waiting : waitless;
        YtDlp::setCancelWaits(wait);
        answered = false;
        // A resolver of its own each round: StreamResolver's yt-dlp time
        // limit is matched by the job's generation, which a new job for the
        // same track starts again from, so an earlier round's limit would
        // otherwise end a later round's lookup.
        auto resolver = std::make_unique<StreamResolver>();
        QObject::connect(resolver.get(), &StreamResolver::resolved, [&answered, &answer]() {
            answered = true;
            answer = QStringLiteral("a link");
        });
        QObject::connect(resolver.get(), &StreamResolver::failed,
                         [&answered, &answer](const QString &, const QString &reason) {
                             answered = true;
                             answer = reason;
                         });
        resolver->resolveVia(videoId, { StreamResolver::TierYtDlp });
        settle(at);
        if (answered) {
            ++mode.voided;
            t.note(QStringLiteral("round %1 (%2): yt-dlp answered within %3 ms (%4); nothing to cancel")
                       .arg(i + 1).arg(wait ? QStringLiteral("waits") : QStringLiteral("no wait")).arg(at)
                       .arg(answer.left(80)));
            continue;
        }
        const qsizetype reapedBefore = reaped.size();
        lastTick = clock.elapsed();
        worstGap = 0;
        QElapsedTimer call;
        call.start();
        resolver->cancel(videoId);   // what a skip does (PlaybackController::beginTrack)
        const qint64 callMs = call.elapsed();
        settle(1500);
        const qint64 gap = worstGap;
        mode.calls << callMs;
        mode.gaps << gap;
        QString ended;
        if (!wait) {
            ++reapedExpected;
            // Its processes end on their own; the next round waits for them,
            // so no two are ever running at once.
            const bool seen = waitUntil([&]() { return reaped.size() > reapedBefore; }, 8000);
            const QString line = seen ? reaped.last() : QString();
            if (seen && line.contains(QLatin1String("(0 of its processes left)")))
                ++reapedClean;
            ended = seen ? QStringLiteral("; ") + line.section(QLatin1String("yt-dlp: "), 1)
                         : QStringLiteral("; its processes were not seen to end within 8 s");
        }
        t.note(QStringLiteral("round %1 (%2), cancelled at %3 ms: the call took %4 ms, the longest gap %5 ms%6")
                   .arg(i + 1).arg(wait ? QStringLiteral("waits") : QStringLiteral("no wait")).arg(at)
                   .arg(callMs).arg(gap).arg(ended));
    }
    YtDlp::setCancelWaits(false);
    tick.stop();
    g_reaped = nullptr;
    qInstallMessageHandler(g_previousHandler);

    const auto summary = [](const Mode &mode) {
        return QStringLiteral("n=%1 (%2 answered first): the call median %3 ms, max %4; the longest gap median %5 ms, "
                              "max %6")
            .arg(mode.calls.size()).arg(mode.voided).arg(median(mode.calls)).arg(largest(mode.calls))
            .arg(median(mode.gaps)).arg(largest(mode.gaps));
    };
    t.note(QStringLiteral("without waiting (now): ") + summary(waitless));
    t.note(QStringLiteral("waiting (ytdlp.cancel=wait, as before): ") + summary(waiting));
    t.check(!waitless.calls.isEmpty(), QStringLiteral("at least one cancel came while yt-dlp was still running"));
    t.check(largest(waitless.gaps) >= 0 && largest(waitless.gaps) <= 100,
            QStringLiteral("a cancel that does not wait never holds this thread over 100 ms (longest %1 ms)")
                .arg(largest(waitless.gaps)));
    t.check(reapedExpected > 0 && reapedClean == reapedExpected,
            QStringLiteral("and every one of those saw all its processes end (%1 of %2)")
                .arg(reapedClean).arg(reapedExpected),
            QStringLiteral("a process was left, or its end was not seen"));
    return t.finish();
}

int runPlayerCanary(const QStringList &videoIdsArgument)
{
    Checks t("canary");
    const QStringList videoIds = videoIdsArgument.isEmpty() ? QStringList{ kDefaultVideo } : videoIdsArgument;
    InnerTube tube;
    QNetworkAccessManager network;

    // One range of a link, as the player would ask for it: the status and
    // how many bytes came.
    const auto range = [&network](const QString &url, qint64 from, qint64 to) {
        QNetworkRequest request{ QUrl(url) };
        request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(from) + '-' + QByteArray::number(to));
        request.setTransferTimeout(15000);
        QNetworkReply *reply = network.get(request);
        waitUntil([reply]() { return reply->isFinished(); }, 20000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 bytes = reply->readAll().size();
        reply->deleteLater();
        return std::make_pair(status, bytes);
    };

    for (const QString &videoId : videoIds) {
        for (const InnerTube::PlayerClients which : { InnerTube::PlayerClients::First, InnerTube::PlayerClients::Second }) {
            const QString name = which == InnerTube::PlayerClients::First ? QStringLiteral("VISIONOS 1.02")
                                                                           : QStringLiteral("VISIONOS 0.1");
            InnerTube::setPlayerClients(which);
            auto answer = std::make_shared<std::tuple<bool, QString, int, QString>>();
            QElapsedTimer took;
            took.start();
            tube.player(videoId, [answer](const QString &url, int itag, const QString &error) {
                *answer = { true, url, itag, error };
            });
            waitUntil([answer]() { return std::get<0>(*answer); }, 30000);
            const qint64 ms = took.elapsed();
            const QString url = std::get<1>(*answer);
            if (url.isEmpty()) {
                const QString error = std::get<0>(*answer) ? std::get<3>(*answer) : QStringLiteral("no answer in 30 s");
                t.check(false, QStringLiteral("%1 answers %2 with a link that serves").arg(name, videoId),
                        QStringLiteral("no link, in %1 ms: %2").arg(ms).arg(error));
                continue;
            }
            const QUrlQuery query{ QUrl(url) };
            const qint64 length = query.queryItemValue(QStringLiteral("clen")).toLongLong();
            constexpr qint64 kMiB = 1024 * 1024;
            const auto first = range(url, 0, kMiB - 1);
            const bool firstOk = first.first == 206 && first.second == qMin(kMiB, length > 0 ? length : kMiB);
            QString second = QStringLiteral("no second MiB");
            bool secondOk = true;
            if (length > kMiB) {
                const auto next = range(url, kMiB, 2 * kMiB - 1);
                secondOk = next.first == 206 && next.second == qMin(kMiB, length - kMiB);
                second = QStringLiteral("2nd MiB %1 (%2 bytes)").arg(next.first).arg(next.second);
            }
            const QString detail =
                QStringLiteral("itag %1 in %2 ms, %3, %4, %5; 1st MiB %6 (%7 bytes), %8")
                    .arg(std::get<2>(*answer)).arg(ms)
                    .arg(QUrl(url).host().endsWith(QLatin1String(".googlevideo.com")) ? QStringLiteral("googlevideo")
                                                                                        : QStringLiteral("another host"))
                    .arg(query.hasQueryItem(QStringLiteral("n")) ? QStringLiteral("n= present") : QStringLiteral("no n="))
                    .arg(query.hasQueryItem(QStringLiteral("pot")) ? QStringLiteral("pot= present") : QStringLiteral("no pot="))
                    .arg(first.first).arg(first.second).arg(second);
            t.check(firstOk && secondOk, QStringLiteral("%1 answers %2 with a link that serves: %3").arg(name, videoId, detail));
        }
    }
    InnerTube::setPlayerClients(InnerTube::PlayerClients::Both);
    return t.finish();
}
