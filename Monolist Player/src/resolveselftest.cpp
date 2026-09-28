#include "resolveselftest.h"

#include "downloadmanager.h"
#include "innertube.h"
#include "library.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "streamresolver.h"
#include "ytdlp.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

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

// ------------------------------------------------------------ --bounds-test

namespace {

// A stand-in for www.youtube.com: sw.js_data at once, and /player as
// `player` says. Every answer closes its connection, so a request's client
// port tells which connection it came on.
class BoundsStandIn : public QObject
{
public:
    struct Request {
        QByteArray path;
        QString videoId;
        qint64 at = 0;          // ms since listen()
        quint16 peerPort = 0;   // the client's end of its connection
    };
    struct Answer {
        int delayMs = 0;        // answered this long after it came
        bool hold = false;      // never answered
        QByteArray body = "{}";
    };
    std::function<Answer(const Request &)> player;
    QList<Request> requests;

    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &BoundsStandIn::accept);
        m_clock.start();
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    QList<Request> playersSince(qsizetype from) const
    {
        QList<Request> found;
        for (qsizetype i = from; i < requests.size(); ++i) {
            if (requests.at(i).path.contains("/player"))
                found << requests.at(i);
        }
        return found;
    }

private:
    void accept()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                *buffer += socket->readAll();
                handle(socket, *buffer);
            });
        }
    }

    void handle(QTcpSocket *socket, QByteArray &buffer)
    {
        const qsizetype end = buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        qsizetype length = 0;
        for (const QByteArray &line : lines) {
            if (line.trimmed().toLower().startsWith("content-length:"))
                length = line.trimmed().mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < end + 4 + length)
            return;
        Request request;
        request.path = lines.value(0).trimmed().split(' ').value(1);
        request.videoId = QJsonDocument::fromJson(buffer.mid(end + 4, length)).object()
                              .value(QStringLiteral("videoId")).toString();
        request.at = m_clock.elapsed();
        request.peerPort = socket->peerPort();
        buffer.clear();
        requests.append(request);

        Answer answer;
        if (request.path.startsWith("/sw.js_data"))
            answer.body = visitorAnswer();
        else if (request.path.contains("/player") && player)
            answer = player(request);
        if (answer.hold)
            return;
        const QByteArray out = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\n"
                               "Content-Length: " + QByteArray::number(answer.body.size())
                               + "\r\nConnection: close\r\n\r\n" + answer.body;
        const QPointer<QTcpSocket> guard(socket);
        const auto send = [guard, out]() {
            if (guard && guard->state() == QAbstractSocket::ConnectedState) {
                guard->write(out);
                guard->disconnectFromHost();
            }
        };
        if (answer.delayMs > 0)
            QTimer::singleShot(answer.delayMs, socket, send);
        else
            send();
    }

    // sw.js_data's shape, as innertube.cpp reads it: ")]}'", then the id at
    // [0][2][6].
    static QByteArray visitorAnswer()
    {
        QJsonArray config;
        config.append(QJsonArray());
        config.append(QStringLiteral("TESTKEY"));
        for (int i = 2; i < 6; ++i)
            config.append(QJsonValue());
        config.append(QStringLiteral("TESTVISITORbounds0001"));
        const QJsonArray entry{ QStringLiteral("yt.sw.adr"), QJsonValue(), config };
        return ")]}'\n" + QJsonDocument(QJsonArray{ entry }).toJson(QJsonDocument::Compact);
    }

    QTcpServer m_server;
    QElapsedTimer m_clock;
};

// A proxy that takes every connection and never says a word: yt-dlp sent
// through it (HTTP_PROXY and the rest) finds YouTube gone dark, and nothing
// it asks for reaches the network.
class BlackHole : public QObject
{
public:
    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.nextPendingConnection())
                ++m_connections;   // kept open, unread, until the test ends
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    int connections() const { return m_connections; }

private:
    QTcpServer m_server;
    int m_connections = 0;
};

// Points this process's children at a proxy until it goes; what the
// variables were is put back.
class ProxyEnvironment
{
public:
    explicit ProxyEnvironment(const QString &proxy)
    {
        for (const char *name : { "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy" })
            set(name, proxy.toUtf8());
        for (const char *name : { "NO_PROXY", "no_proxy" })
            set(name, "127.0.0.1,localhost");
    }
    ~ProxyEnvironment()
    {
        // Backwards: on Windows the upper and lower case names are one.
        for (auto it = m_saved.crbegin(); it != m_saved.crend(); ++it) {
            if (it->had)
                qputenv(it->name.constData(), it->value);
            else
                qunsetenv(it->name.constData());
        }
    }

private:
    void set(const char *name, const QByteArray &value)
    {
        m_saved.push_back({ name, qEnvironmentVariableIsSet(name), qgetenv(name) });
        qputenv(name, value);
    }
    struct Saved {
        QByteArray name;
        bool had;
        QByteArray value;
    };
    std::vector<Saved> m_saved;
};

QStringList *g_boundsLog = nullptr;
QtMessageHandler g_boundsPrevious = nullptr;

void keepBoundsMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_boundsLog)
        g_boundsLog->append(message);
    if (g_boundsPrevious)
        g_boundsPrevious(type, context, message);
}

const QByteArray kPlayable =
    R"({"playabilityStatus":{"status":"OK"},"streamingData":{"adaptiveFormats":[)"
    R"({"itag":251,"url":"http://127.0.0.1:9/bounds-audio","mimeType":"audio/webm; codecs=\"opus\"","bitrate":130000}]}})";
const QByteArray kUnplayable = R"({"playabilityStatus":{"status":"UNPLAYABLE","reason":"Not in this test"}})";

struct PlayerAnswer {
    bool done = false;
    QString url;
    QString error;
};

std::shared_ptr<PlayerAnswer> askPlayer(InnerTube &tube, const QString &videoId)
{
    auto answer = std::make_shared<PlayerAnswer>();
    tube.player(videoId, [answer](const QString &url, int, const QString &error) {
        answer->done = true;
        answer->url = url;
        answer->error = error;
    });
    return answer;
}

qint64 percentile90(QList<qint64> values)
{
    if (values.isEmpty())
        return -1;
    std::sort(values.begin(), values.end());
    return values.at(qMin<qsizetype>(values.size() - 1, qsizetype(std::ceil(0.9 * values.size())) - 1));
}

} // namespace

int runBoundsSelfTest(Library *library, int roundsArgument, bool before)
{
    Checks t("bounds");
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: the player part plays on the library there"));
        return t.finish();
    }
    const int rounds = roundsArgument > 0 ? roundsArgument : 1;

    QStringList log;
    g_boundsLog = &log;
    g_boundsPrevious = qInstallMessageHandler(keepBoundsMessage);
    const auto restoreLog = qScopeGuard([]() {
        qInstallMessageHandler(g_boundsPrevious);
        g_boundsLog = nullptr;
    });
    const auto loggedSince = [&log](qsizetype from, const QString &text) {
        for (qsizetype i = from; i < log.size(); ++i) {
            if (log.at(i).contains(text))
                return true;
        }
        return false;
    };
    const auto countSince = [&log](qsizetype from, const QString &text) {
        int count = 0;
        for (qsizetype i = from; i < log.size(); ++i) {
            if (log.at(i).contains(text))
                ++count;
        }
        return count;
    };

    BoundsStandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in YouTube on this computer")))
        return t.finish();
    InnerTube::setTestServer(standIn.base());
    // A visitor id store of its own, in memory: the library's is not touched.
    QHash<QString, QString> settings;
    InnerTube::setVisitorStore({ [&settings](const QString &key) { return settings.value(key); },
                                 [&settings](const QString &key, const QString &value) { settings.insert(key, value); } });
    const auto restoreInnerTube = qScopeGuard([]() {
        InnerTube::setVisitorStore({});
        InnerTube::setTestServer(QString());
    });
    t.note(QStringLiteral("/player goes to %1; yt-dlp to YouTube, or to a proxy that never answers").arg(standIn.base()));

    const auto hold = []() {
        BoundsStandIn::Answer answer;
        answer.hold = true;
        return answer;
    };
    const auto playable = [](int delayMs) {
        BoundsStandIn::Answer answer;
        answer.delayMs = delayMs;
        answer.body = kPlayable;
        return answer;
    };

    // — 1. the first /player stalls: the hedge, on its own connection, answers —
    {
        InnerTube tube;
        int seen = 0;
        standIn.player = [&](const BoundsStandIn::Request &) { return ++seen == 1 ? hold() : playable(0); };
        const qsizetype from = standIn.requests.size();
        const qsizetype logFrom = log.size();
        QElapsedTimer took;
        took.start();
        const auto answer = askPlayer(tube, QStringLiteral("boundsHedge"));
        waitUntil([answer]() { return answer->done; }, 10000);
        const qint64 ms = took.elapsed();
        const QList<BoundsStandIn::Request> asked = standIn.playersSince(from);
        const qint64 gap = asked.size() >= 2 ? asked.at(1).at - asked.at(0).at : -1;
        t.check(!answer->url.isEmpty() && asked.size() == 2 && ms >= 1100 && ms < 2500,
                QStringLiteral("a first /player that never answers: asked once more, which answers, in %1 ms").arg(ms),
                QStringLiteral("%1 /player requests; %2").arg(asked.size()).arg(answer->error));
        t.check(gap >= 1100 && gap < 1700, QStringLiteral("the second went %1 ms after the first (1200 set)").arg(gap));
        t.check(asked.size() == 2 && asked.at(0).peerPort != asked.at(1).peerPort,
                QStringLiteral("on a connection of its own (client ports %1 and %2)")
                    .arg(asked.value(0).peerPort).arg(asked.value(1).peerPort));
        t.check(loggedSince(logFrom, QStringLiteral("asking once more on a connection of its own"))
                    && loggedSince(logFrom, QStringLiteral("answered on the second request's own connection")),
                QStringLiteral("the log says so"));
    }

    // — 2. every /player slow: given up at 3 s, after two requests, no more —
    {
        InnerTube tube;
        standIn.player = [&](const BoundsStandIn::Request &) { return playable(10000); };
        const qsizetype from = standIn.requests.size();
        QElapsedTimer took;
        took.start();
        const auto answer = askPlayer(tube, QStringLiteral("boundsSlow1"));
        waitUntil([answer]() { return answer->done; }, 15000);
        const qint64 ms = took.elapsed();
        const int asked = int(standIn.playersSince(from).size());
        t.check(answer->done && answer->url.isEmpty() && ms >= 2800 && ms < 3800 && asked == 2
                    && answer->error.contains(QLatin1String("no answer within 3000 ms")),
                QStringLiteral("every /player delayed 10 s: given up after %1 ms and %2 requests").arg(ms).arg(asked),
                answer->error);
    }

    // — 3. the switch back: youtube.player_deadline=off —
    {
        InnerTube::setPlayerDeadline(false);
        InnerTube tube;
        standIn.player = [&](const BoundsStandIn::Request &) { return hold(); };
        const qsizetype from = standIn.requests.size();
        const auto answer = askPlayer(tube, QStringLiteral("boundsOff01"));
        settle(4000);
        const int asked = int(standIn.playersSince(from).size());
        t.check(!answer->done && asked == 1,
                QStringLiteral("youtube.player_deadline=off: 4 s in, still one request and no answer, as before"),
                QStringLiteral("%1 requests, %2").arg(asked).arg(answer->done ? QStringLiteral("answered") : QStringLiteral("waiting")));
        InnerTube::setPlayerDeadline(true);
    }

    // — 4. through the resolver, every /player delayed 10 s: yt-dlp at ~3 s —
    if (!t.check(YtDlp::isAvailable(), QStringLiteral("yt-dlp is installed"))) {
        return t.finish();
    }
    {
        // One round: the real song resolved while every /player takes 10 s.
        // `asBefore` turns every bound off (the three switches back), for
        // the numbers these replace.
        struct Arm {
            QList<qint64> starts;    // when yt-dlp was started
            QList<qint64> answers;   // when yt-dlp's answer came
            int viaYtDlp = 0;
        };
        const auto slowRound = [&](Arm &arm, bool asBefore, int round) {
            InnerTube::setPlayerDeadline(!asBefore);
            StreamResolver resolver;
            resolver.setSaavnEnabled(false);
            resolver.setDeadline(!asBefore);
            resolver.setYtDlpOneAtATime(!asBefore);
            QElapsedTimer clock;
            qint64 ytdlpAt = -1;
            qint64 answeredAt = -1;
            int tier = -1;
            QString failure;
            QObject::connect(&resolver, &StreamResolver::tierChanged, &resolver, [&](const QString &, int now) {
                if (now == StreamResolver::TierYtDlp && ytdlpAt < 0)
                    ytdlpAt = clock.elapsed();
            });
            QObject::connect(&resolver, &StreamResolver::resolved, &resolver,
                             [&](const QString &, const QString &, int answeredTier, bool) {
                                 answeredAt = clock.elapsed();
                                 tier = answeredTier;
                             });
            QObject::connect(&resolver, &StreamResolver::failed, &resolver, [&](const QString &, const QString &why) {
                answeredAt = clock.elapsed();
                failure = why;
            });
            clock.start();
            resolver.resolve(kDefaultVideo);
            waitUntil([&answeredAt]() { return answeredAt >= 0; }, 40000);
            t.note(QStringLiteral("round %1%2: yt-dlp started at %3 ms, %4 at %5 ms%6")
                       .arg(round).arg(asBefore ? QStringLiteral(" as before") : QString()).arg(ytdlpAt)
                       .arg(tier == StreamResolver::TierYtDlp ? QStringLiteral("answered by yt-dlp")
                                                              : QStringLiteral("ended"))
                       .arg(answeredAt).arg(failure.isEmpty() ? QString() : QStringLiteral(": ") + failure.left(120)));
            if (ytdlpAt >= 0)
                arm.starts << ytdlpAt;
            if (tier == StreamResolver::TierYtDlp) {
                ++arm.viaYtDlp;
                arm.answers << answeredAt;
            }
            InnerTube::setPlayerDeadline(true);
        };
        Arm bounded;
        Arm unbounded;
        standIn.player = [&](const BoundsStandIn::Request &) { return playable(10000); };
        for (int round = 1; round <= rounds; ++round) {
            slowRound(bounded, false, round);
            if (before)
                slowRound(unbounded, true, round);
        }
        t.check(bounded.starts.size() == rounds
                    && std::all_of(bounded.starts.begin(), bounded.starts.end(),
                                   [](qint64 at) { return at >= 2800 && at < 4000; }),
                QStringLiteral("every /player delayed 10 s: yt-dlp starts at %1 ms (median of %2)")
                    .arg(median(bounded.starts)).arg(bounded.starts.size()));
        t.check(bounded.viaYtDlp == rounds && largest(bounded.answers) <= 10000,
                QStringLiteral("and answers: median %1 ms, p90 %2 ms, n=%3 (the plan's target ~6,000)")
                    .arg(median(bounded.answers)).arg(percentile90(bounded.answers)).arg(bounded.answers.size()));
        if (before) {
            t.note(QStringLiteral("as before (every switch back): yt-dlp starts at median %1 ms, p90 %2; answers at "
                                  "median %3 ms, p90 %4; n=%5 of %6")
                       .arg(median(unbounded.starts)).arg(percentile90(unbounded.starts))
                       .arg(median(unbounded.answers)).arg(percentile90(unbounded.answers))
                       .arg(unbounded.answers.size()).arg(rounds));
        }
    }

    // From here yt-dlp goes through a proxy that never answers.
    BlackHole blackHole;
    if (!t.check(blackHole.listen(), QStringLiteral("a proxy that never answers")))
        return t.finish();
    ProxyEnvironment proxy(blackHole.url());

    // — 5. everything dark: the skip notice within 20 s —
    //
    // The real player, on a queue of two invented songs: the first with
    // YouTube and yt-dlp both dark, the second failing at once so the queue
    // stops there. `asBefore` turns every bound off; `alongside` runs as
    // Play is pressed.
    struct DarkRun {
        bool engine = false;
        qint64 noticeAt = -1;
        QString notice;
        qint64 failedAt = -1;
        QString failure;
        QList<QPair<int, qint64>> walked;   // tier, when
        QString rungs() const
        {
            QStringList names;
            for (const auto &step : walked)
                names << QStringLiteral("%1 at %2 ms").arg(step.first).arg(step.second);
            return names.join(QStringLiteral(", "));
        }
    };
    const auto playDark = [&](const QString &dark, const QString &after, bool asBefore, int waitMs,
                              const std::function<void()> &alongside) {
        DarkRun run;
        InnerTube::setPlayerDeadline(!asBefore);
        MpvEngine engine;
        run.engine = engine.isValid();
        if (!run.engine)
            return run;
        StreamResolver resolver;
        DownloadManager downloads;
        PlaybackController player(&engine, &resolver, &downloads);
        player.setLibrary(library);
        resolver.setSaavnEnabled(false);   // invented songs: JioSaavn is never asked about them
        resolver.setDeadline(!asBefore);
        resolver.setYtDlpOneAtATime(!asBefore);
        player.setAutoplay(false);
        player.setVolume(0.2);
        for (const int tier : { StreamResolver::TierInnerTube, StreamResolver::TierYtDlp, StreamResolver::TierMuxed,
                                StreamResolver::TierPiped, StreamResolver::TierInvidious })
            resolver.setTestAnswer(after, tier, QString());
        QElapsedTimer clock;
        QObject::connect(&resolver, &StreamResolver::tierChanged, &resolver, [&](const QString &id, int tier) {
            if (id == dark)
                run.walked.append({ tier, clock.elapsed() });
        });
        QObject::connect(&resolver, &StreamResolver::failed, &resolver, [&](const QString &id, const QString &why) {
            if (id == dark && run.failedAt < 0) {
                run.failedAt = clock.elapsed();
                run.failure = why;
            }
        });
        QObject::connect(&player, &PlaybackController::notice, &player, [&](const QString &text) {
            if (run.noticeAt < 0) {
                run.noticeAt = clock.elapsed();
                run.notice = text;
            }
        });
        const auto song = [](const QString &id, const QString &title) {
            return QVariantMap{ { QStringLiteral("videoId"), id },
                                { QStringLiteral("title"), title },
                                { QStringLiteral("artist"), QStringLiteral("Selftest") },
                                { QStringLiteral("durationMs"), 180000 } };
        };
        clock.start();
        player.playTracks({ song(dark, QStringLiteral("Gone dark")), song(after, QStringLiteral("After it")) }, 0);
        if (alongside)
            alongside();
        waitUntil([&run]() { return run.noticeAt >= 0; }, waitMs);
        resolver.cancelAll();
        InnerTube::setPlayerDeadline(true);
        return run;
    };
    standIn.player = [&](const BoundsStandIn::Request &) { return hold(); };
    {
        // Beside it, the same song on a resolver with the switch back.
        const QString dark = QStringLiteral("selftestB05");
        StreamResolver unbounded;
        unbounded.setSaavnEnabled(false);
        unbounded.setDeadline(false);
        bool unboundedEnded = false;
        QObject::connect(&unbounded, &StreamResolver::resolved, &unbounded, [&]() { unboundedEnded = true; });
        QObject::connect(&unbounded, &StreamResolver::failed, &unbounded, [&]() { unboundedEnded = true; });
        const DarkRun run = playDark(dark, QStringLiteral("selftestB06"), false, 30000,
                                     [&unbounded, &dark]() { unbounded.resolve(dark); });
        if (!t.check(run.engine, QStringLiteral("an audio engine to drive")))
            return t.finish();
        t.check(run.noticeAt >= 0 && run.noticeAt <= 20500 && run.notice.contains(QLatin1String("skipping"))
                    && run.notice.contains(QLatin1String("Gone dark")),
                QStringLiteral("YouTube and yt-dlp both dark: the skip notice %1 ms after Play (20 s is the limit)")
                    .arg(run.noticeAt),
                run.notice);
        const QList<QPair<int, qint64>> &walked = run.walked;
        const bool ladder = walked.size() >= 3 && walked.at(0).first == StreamResolver::TierInnerTube
                            && walked.at(1).first == StreamResolver::TierYtDlp && walked.at(1).second >= 2800
                            && walked.at(1).second < 4000 && walked.at(2).first == StreamResolver::TierMuxed;
        t.check(ladder && run.failedAt >= 19500 && run.failure.contains(QLatin1String("within 20 s")),
                QStringLiteral("the ladder on the way: InnerTube, yt-dlp at ~3 s, the muxed stream, then given up "
                               "at %1 ms").arg(run.failedAt),
                QStringLiteral("rungs (tier at time) %1; %2").arg(run.rungs(), run.failure.left(200)));
        t.check(blackHole.connections() > 0, QStringLiteral("and yt-dlp did reach the dark proxy (%1 connections)")
                                                 .arg(blackHole.connections()));
        settle(1000);
        t.check(!unboundedEnded, QStringLiteral("playback.resolve_deadline=off: the same song still resolving "
                                                "past 21 s, as before"));
        unbounded.cancelAll();
    }
    if (before) {
        const DarkRun run = playDark(QStringLiteral("selftestB07"), QStringLiteral("selftestB08"), true, 60000, {});
        t.note(QStringLiteral("as before (every switch back), everything dark: the skip notice %1 ms after Play; "
                              "rungs (tier at time) %2; %3")
                   .arg(run.noticeAt).arg(run.rungs(), run.failure.left(200)));
    }

    // — 6. one yt-dlp at a time, the song someone is waiting for first —
    {
        StreamResolver resolver;
        resolver.setSaavnEnabled(false);
        standIn.player = [](const BoundsStandIn::Request &) {
            BoundsStandIn::Answer answer;
            answer.body = kUnplayable;
            return answer;
        };
        const auto launches = [&countSince](qsizetype from, const QString &id) {
            return countSince(from, QStringLiteral("resolver: %1: yt-dlp asked for").arg(id));
        };
        const qsizetype from = log.size();
        const QString first = QStringLiteral("selftestQ01");
        const QString second = QStringLiteral("selftestQ02");
        const QString wanted = QStringLiteral("selftestQ03");
        resolver.prefetch(first);
        const bool firstRan = waitUntil([&]() { return launches(from, first) == 1; }, 5000);
        resolver.prefetch(second);
        const bool secondWaits = waitUntil([&]() {
            return loggedSince(from, second + QStringLiteral(" waits its turn for yt-dlp"));
        }, 5000);
        settle(300);
        t.check(firstRan && secondWaits && launches(from, second) == 0 && !YtDlp::playbackResolving(),
                QStringLiteral("two songs fetched ahead: the second waits for the first's lookup, and no download "
                               "waits for either"));
        resolver.resolve(wanted);
        const bool wantedRan = waitUntil([&]() { return launches(from, wanted) == 1; }, 5000);
        settle(300);
        t.check(wantedRan && loggedSince(from, first + QStringLiteral("'s yt-dlp lookup, ahead of time, stopped"))
                    && launches(from, first) == 1 && launches(from, second) == 0,
                QStringLiteral("a song someone is waiting for: the first's lookup is stopped, and it goes first"));
        t.check(YtDlp::playbackResolving(), QStringLiteral("and while it resolves, new downloads wait"));
        bool released = false;
        YtDlp::whenPlaybackResolved(&resolver, [&released]() { released = true; });
        resolver.cancel(wanted);
        const bool firstAgain = waitUntil([&]() { return launches(from, first) == 2; }, 5000);
        settle(300);
        t.check(firstAgain && launches(from, second) == 0,
                QStringLiteral("once it has gone, the stopped one starts again, ahead of the one behind it"));
        waitUntil([&released]() { return released; }, 2000);
        t.check(released && !YtDlp::playbackResolving(), QStringLiteral("and the downloads' wait is over"));
        resolver.cancelAll();
    }

    // — 7. the switch back: ytdlp.resolves=parallel —
    {
        StreamResolver resolver;
        resolver.setSaavnEnabled(false);
        resolver.setYtDlpOneAtATime(false);
        const qsizetype from = log.size();
        const QString ahead = QStringLiteral("selftestQ04");
        const QString wanted = QStringLiteral("selftestQ05");
        resolver.prefetch(ahead);
        waitUntil([&]() { return countSince(from, ahead + QStringLiteral(": yt-dlp asked for")) == 1; }, 5000);
        resolver.resolve(wanted);
        waitUntil([&]() { return countSince(from, wanted + QStringLiteral(": yt-dlp asked for")) == 1; }, 5000);
        settle(300);
        t.check(countSince(from, ahead + QStringLiteral(": yt-dlp asked for")) == 1
                    && countSince(from, wanted + QStringLiteral(": yt-dlp asked for")) == 1
                    && !loggedSince(from, QStringLiteral("waits its turn")) && !loggedSince(from, QStringLiteral("stopped for"))
                    && !YtDlp::playbackResolving(),
                QStringLiteral("ytdlp.resolves=parallel: both lookups at once, and no download waits, as before"));
        resolver.cancelAll();
    }
    settle(200);
    return t.finish();
}
