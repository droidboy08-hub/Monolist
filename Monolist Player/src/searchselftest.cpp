#include "searchselftest.h"

#include "innertube.h"
#include "mediaextractor.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

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
        qWarning("search-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("search-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

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

void pause(int ms)
{
    waitUntil([]() { return false; }, ms);
}

// YouTube Music on this computer: each /search request's body kept, and
// answered as `respond` says. One request a connection.
class StandIn : public QObject
{
public:
    struct Answer {
        int status = 200;
        QByteArray body = "{}";
        int delayMs = 0;
    };

    std::function<Answer(const QJsonObject &body)> respond;
    QList<QJsonObject> searches;

    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &StandIn::accept);
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

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
        const int end = int(buffer.indexOf("\r\n\r\n"));
        if (end < 0)
            return;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        const QByteArray path = lines.value(0).trimmed().split(' ').value(1);
        int length = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        }
        if (buffer.size() < end + 4 + length)
            return;
        const QJsonObject body = QJsonDocument::fromJson(buffer.mid(end + 4, length)).object();
        buffer.clear();

        Answer answer;
        if (path.startsWith("/youtubei/v1/search")) {
            searches.append(body);
            if (respond)
                answer = respond(body);
        }
        const QByteArray out = "HTTP/1.1 " + QByteArray::number(answer.status)
                               + (answer.status == 200 ? " OK" : " Error") + "\r\n"
                               "Content-Type: application/json; charset=UTF-8\r\n"
                               "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + answer.body;
        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(answer.delayMs, this, [guarded, out]() {
            if (!guarded)
                return;
            guarded->write(out);
            guarded->disconnectFromHost();
        });
    }

    QTcpServer m_server;
};

// — YouTube Music's answers, shaped like the real ones (October 2026) —

QJsonObject runsOf(const QString &text)
{
    return { { QStringLiteral("runs"), QJsonArray{ QJsonObject{ { QStringLiteral("text"), text } } } } };
}

QJsonObject column(const QString &text)
{
    return { { QStringLiteral("musicResponsiveListItemFlexColumnRenderer"),
               QJsonObject{ { QStringLiteral("text"), runsOf(text) } } } };
}

// A song's row: its id where a song list keeps it.
QJsonObject songRow(const QString &id)
{
    return { { QStringLiteral("musicResponsiveListItemRenderer"), QJsonObject{
                 { QStringLiteral("playlistItemData"), QJsonObject{ { QStringLiteral("videoId"), id } } },
                 { QStringLiteral("flexColumns"), QJsonArray{ column(QStringLiteral("Song ") + id),
                                                              column(QStringLiteral("Selftest Artist")) } },
             } } };
}

// An album's or a playlist's row: the page it opens.
QJsonObject cardRow(const QString &id, const char *pageType)
{
    const QJsonObject endpoint{ { QStringLiteral("browseEndpoint"), QJsonObject{
        { QStringLiteral("browseId"), id },
        { QStringLiteral("browseEndpointContextSupportedConfigs"), QJsonObject{
            { QStringLiteral("browseEndpointContextMusicConfig"), QJsonObject{
                { QStringLiteral("pageType"), QString::fromLatin1(pageType) } } } } } } } };
    return { { QStringLiteral("musicResponsiveListItemRenderer"), QJsonObject{
                 { QStringLiteral("navigationEndpoint"), endpoint },
                 { QStringLiteral("flexColumns"), QJsonArray{ column(QStringLiteral("Card ") + id),
                                                              column(QStringLiteral("Album • Selftest Artist")) } },
             } } };
}

QJsonArray rows(const QStringList &ids, const char *pageType = nullptr)
{
    QJsonArray list;
    for (const QString &id : ids)
        list.append(pageType ? cardRow(id, pageType) : songRow(id));
    return list;
}

QStringList ids(const QString &prefix, int from, int to)
{
    QStringList list;
    for (int i = from; i <= to; ++i)
        list << prefix + QString::number(i).rightJustified(3, QLatin1Char('0'));
    return list;
}

// A search's first page: one shelf, its next page's token as search has it.
QByteArray firstPage(const QJsonArray &shelfRows, const QString &next)
{
    QJsonObject shelf{ { QStringLiteral("contents"), shelfRows } };
    if (!next.isEmpty())
        shelf.insert(QStringLiteral("continuations"), QJsonArray{ QJsonObject{
            { QStringLiteral("nextContinuationData"), QJsonObject{ { QStringLiteral("continuation"), next } } } } });
    const QJsonObject section{ { QStringLiteral("musicShelfRenderer"), shelf } };
    const QJsonObject list{ { QStringLiteral("contents"), QJsonArray{ section } } };
    const QJsonObject content{ { QStringLiteral("sectionListRenderer"), list } };
    const QJsonObject tab{ { QStringLiteral("tabRenderer"), QJsonObject{ { QStringLiteral("content"), content } } } };
    const QJsonObject tabs{ { QStringLiteral("tabs"), QJsonArray{ tab } } };
    const QJsonObject root{ { QStringLiteral("contents"),
                              QJsonObject{ { QStringLiteral("tabbedSearchResultsRenderer"), tabs } } } };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// A page after: the shelf's continuation, as /search answers a token.
QByteArray nextPage(const QJsonArray &shelfRows, const QString &next)
{
    QJsonObject shelf{ { QStringLiteral("contents"), shelfRows } };
    if (!next.isEmpty())
        shelf.insert(QStringLiteral("continuations"), QJsonArray{ QJsonObject{
            { QStringLiteral("nextContinuationData"), QJsonObject{ { QStringLiteral("continuation"), next } } } } });
    const QJsonObject root{ { QStringLiteral("continuationContents"),
                              QJsonObject{ { QStringLiteral("musicShelfContinuation"), shelf } } } };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

int count(const QList<QJsonObject> &searches, const char *key)
{
    int n = 0;
    for (const QJsonObject &body : searches)
        n += body.contains(QLatin1String(key)) ? 1 : 0;
    return n;
}

} // namespace

int runSearchSelfTest()
{
    Checks t;
    StandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in YouTube Music listens on this computer")))
        return t.finish();

    standIn.respond = [](const QJsonObject &body) {
        StandIn::Answer answer;
        const QString token = body.value(QLatin1String("continuation")).toString();
        const QString query = body.value(QLatin1String("query")).toString();
        const QString params = body.value(QLatin1String("params")).toString();
        if (!token.isEmpty()) {
            // Songs: the second page brings 15 new and 5 already shown; the
            // third only ones already shown, and a token that goes nowhere.
            if (token == QLatin1String("songs-2"))
                answer.body = nextPage(rows(ids(QStringLiteral("s"), 16, 35)), QStringLiteral("songs-3"));
            else if (token == QLatin1String("songs-3"))
                answer.body = nextPage(rows(ids(QStringLiteral("s"), 1, 20)), QStringLiteral("songs-4"));
            else if (token == QLatin1String("slow-2")) {
                answer.body = nextPage(rows(ids(QStringLiteral("old"), 21, 40)), QString());
                answer.delayMs = 600;
            } else if (token == QLatin1String("albums-2"))
                answer.body = nextPage(rows(ids(QStringLiteral("a"), 20, 30), "MUSIC_PAGE_TYPE_ALBUM"), QString());
            else if (token == QLatin1String("featured-2"))
                answer.body = nextPage(rows(ids(QStringLiteral("f"), 4, 6), "MUSIC_PAGE_TYPE_PLAYLIST"), QString());
            else if (token == QLatin1String("community-2"))
                answer.body = nextPage(rows(ids(QStringLiteral("c"), 21, 40), "MUSIC_PAGE_TYPE_PLAYLIST"), QString());
            else
                answer.status = 500;   // "broken-2"
            return answer;
        }
        if (params.startsWith(QLatin1String("EgWKAQIY")))
            answer.body = firstPage(rows(ids(QStringLiteral("a"), 1, 20), "MUSIC_PAGE_TYPE_ALBUM"), QStringLiteral("albums-2"));
        else if (params.startsWith(QLatin1String("EgeKAQQoADgB")))
            answer.body = firstPage(rows(ids(QStringLiteral("f"), 1, 3), "MUSIC_PAGE_TYPE_PLAYLIST"), QStringLiteral("featured-2"));
        else if (params.startsWith(QLatin1String("EgeKAQQoAEAB")))
            answer.body = firstPage(rows(ids(QStringLiteral("c"), 1, 20), "MUSIC_PAGE_TYPE_PLAYLIST"), QStringLiteral("community-2"));
        else if (query == QLatin1String("broken"))
            answer.body = firstPage(rows(ids(QStringLiteral("b"), 1, 20)), QStringLiteral("broken-2"));
        else if (query == QLatin1String("slow"))
            answer.body = firstPage(rows(ids(QStringLiteral("old"), 1, 20)), QStringLiteral("slow-2"));
        else if (query == QLatin1String("newer"))
            answer.body = firstPage(rows(ids(QStringLiteral("new"), 1, 20)), QString());
        else
            answer.body = firstPage(rows(ids(QStringLiteral("s"), 1, 20)), QStringLiteral("songs-2"));
        return answer;
    };
    // Before the extractor exists, so its InnerTube opens no connection to
    // YouTube; every request goes to the stand-in.
    InnerTube::setTestServer(standIn.base());
    qWarning("search-test: YouTube Music is %s; nothing leaves this computer", qPrintable(standIn.base()));

    MediaExtractor extractor;
    QList<QPair<int, int>> appended;   // {section, cards}
    QObject::connect(&extractor, &MediaExtractor::cardsAppended, &extractor,
                     [&appended](int section, const QVariantList &cards) {
                         appended.append({ section, int(cards.size()) });
                     });
    const auto settled = [&extractor]() { return !extractor.busy() && !extractor.loadingMore(); };

    // — songs —
    extractor.search(QStringLiteral("monolist"));
    waitUntil([&]() { return !extractor.busy() && extractor.results()->rowCount() > 0; }, 5000);
    t.check(extractor.results()->rowCount() == 20 && extractor.hasMore(),
            QStringLiteral("the first page: 20 songs, and more to come"),
            QString::number(extractor.results()->rowCount()));
    extractor.loadMore();
    t.check(extractor.loadingMore(), QStringLiteral("  the next page asked for"));
    extractor.loadMore();   // while one is on its way: nothing
    waitUntil(settled, 5000);
    t.check(extractor.results()->rowCount() == 35 && extractor.hasMore(),
            QStringLiteral("  15 more added, the 5 already shown left out"),
            QString::number(extractor.results()->rowCount()));
    t.check(count(standIn.searches, "continuation") == 1
                && standIn.searches.constLast().value(QLatin1String("continuation")).toString() == QLatin1String("songs-2")
                && !standIn.searches.constLast().contains(QLatin1String("query")),
            QStringLiteral("  one request for it, with the token and no query"));
    t.check(extractor.results()->get(20).value(QStringLiteral("sourceId")).toString() == QLatin1String("s021"),
            QStringLiteral("  the new ones below the others, in their order"));
    extractor.loadMore();
    waitUntil(settled, 5000);
    t.check(extractor.results()->rowCount() == 35 && !extractor.hasMore(),
            QStringLiteral("  a page with nothing new ends it"), QString::number(extractor.results()->rowCount()));
    const int asked = int(standIn.searches.size());
    extractor.loadMore();
    pause(200);
    t.check(int(standIn.searches.size()) == asked && !extractor.loadingMore(),
            QStringLiteral("  and nothing more is asked"));

    // — a page that fails —
    extractor.search(QStringLiteral("broken"));
    waitUntil([&]() { return !extractor.busy() && extractor.hasMore(); }, 5000);
    extractor.loadMore();
    waitUntil(settled, 15000);
    t.check(extractor.results()->rowCount() == 20 && !extractor.hasMore() && extractor.lastError().isEmpty(),
            QStringLiteral("a page that fails: the results kept, the end there, no error over them"),
            QString::number(extractor.results()->rowCount()));

    // — an older search's page —
    extractor.search(QStringLiteral("slow"));
    waitUntil([&]() { return !extractor.busy() && extractor.hasMore(); }, 5000);
    extractor.loadMore();
    extractor.search(QStringLiteral("newer"));
    waitUntil([&]() { return !extractor.busy(); }, 5000);
    pause(900);   // past the slow page's answer
    t.check(extractor.results()->rowCount() == 20
                && extractor.results()->get(0).value(QStringLiteral("sourceId")).toString() == QLatin1String("new001")
                && !extractor.hasMore() && !extractor.loadingMore(),
            QStringLiteral("a page asked for an older search is dropped"),
            QString::number(extractor.results()->rowCount()));

    // — albums: one section —
    extractor.setFilter(QStringLiteral("albums"));
    extractor.search(QStringLiteral("monolist"));
    waitUntil([&]() { return !extractor.busy() && !extractor.cardSections().isEmpty(); }, 5000);
    t.check(extractor.cardSections().size() == 1 && extractor.hasMore()
                && extractor.cardMore() == QVariantList{ true },
            QStringLiteral("albums: one section of 20, with more"));
    appended.clear();
    extractor.loadMore();
    waitUntil(settled, 5000);
    t.check(appended.size() == 1 && appended.at(0) == qMakePair(0, 10) && !extractor.hasMore(),
            QStringLiteral("  10 more added to it (the one already there left out), and the end"),
            appended.isEmpty() ? QStringLiteral("none") : QString::number(appended.at(0).second));

    // — playlists: two sections, each continued on its own —
    extractor.setFilter(QStringLiteral("playlists"));
    waitUntil([&]() { return !extractor.busy() && extractor.cardSections().size() == 2; }, 5000);
    t.check(extractor.cardMore() == QVariantList({ true, true }) && extractor.hasMore(),
            QStringLiteral("playlists: two sections, both with more"));
    appended.clear();
    extractor.loadMore();
    waitUntil(settled, 5000);
    t.check(appended.size() == 1 && appended.at(0) == qMakePair(1, 20),
            QStringLiteral("  the page's end continues the last section"));
    t.check(extractor.cardMore() == QVariantList({ true, false }) && !extractor.hasMore(),
            QStringLiteral("  which has no more; the first still has"));
    extractor.loadMore(0);
    waitUntil(settled, 5000);
    t.check(appended.size() == 2 && appended.at(1) == qMakePair(0, 3)
                && extractor.cardMore() == QVariantList({ false, false }),
            QStringLiteral("  its own \"show more\" continues the first"));

    InnerTube::setTestServer(QString());
    return t.finish();
}
