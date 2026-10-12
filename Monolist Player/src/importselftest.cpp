#include "importselftest.h"

#include "accountguard.h"
#include "appdatabase.h"
#include "importmatcher.h"
#include "importsource.h"
#include "innertube.h"
#include "jiosaavn.h"
#include "library.h"
#include "playlistimport.h"
#include "secretstore.h"
#include "ytmsession.h"

#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <iterator>
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
        qWarning("import-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }
    void note(const QString &text) { qWarning("import-test: %s", qPrintable(text)); }
    int finish()
    {
        qWarning("import-test: %d checks, %d failed", m_count, m_failed);
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

// Every service on this computer: each request kept as it arrived (its
// headers byte for byte, and when), and answered as `respond` says. One
// request a connection. Every answer also sets a cookie, so that one sent
// back would show.
class StandIn : public QObject
{
public:
    struct Request {
        QByteArray method;
        QByteArray path;       // with the query
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;   // names in lower case
        qint64 atMs = 0;

        QByteArray header(const QByteArray &name) const
        {
            for (const auto &pair : headers) {
                if (pair.first == name)
                    return pair.second;
            }
            return QByteArray();
        }
        bool has(const QByteArray &name) const
        {
            for (const auto &pair : headers) {
                if (pair.first == name)
                    return true;
            }
            return false;
        }
        QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
        bool authed() const { return has("authorization"); }
    };
    struct Answer {
        int status = 200;
        QByteArray body = "{}";
        QByteArray type = "application/json; charset=UTF-8";
    };

    std::function<Answer(const Request &)> respond;
    QList<Request> requests;
    QElapsedTimer clock;

    bool listen()
    {
        clock.start();
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
        Request request;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        request.method = first.value(0);
        request.path = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = int(line.indexOf(':'));
            if (colon > 0)
                request.headers.append({ line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed() });
        }
        const int length = request.header("content-length").toInt();
        if (buffer.size() < end + 4 + length)
            return;
        request.body = buffer.mid(end + 4, length);
        request.atMs = clock.elapsed();
        buffer.clear();
        requests.append(request);

        const Answer answer = respond ? respond(request) : Answer();
        const QByteArray out = "HTTP/1.1 " + QByteArray::number(answer.status)
                               + (answer.status == 200 ? " OK" : answer.status == 429 ? " Too Many Requests"
                                                                 : " Error") + "\r\n"
                               "Content-Type: " + answer.type + "\r\n"
                               "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\n"
                               "Set-Cookie: IMPORTTEST=anon-" + QByteArray::number(requests.size()) + "; Path=/\r\n"
                               "Connection: close\r\n\r\n" + answer.body;
        socket->write(out);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// — YouTube Music's answers, shaped like the real ones (October 2026) —

QJsonObject run(const QString &text)
{
    return { { QStringLiteral("text"), text } };
}

QJsonObject linkRun(const QString &text, const QString &browseId, const char *pageType)
{
    return { { QStringLiteral("text"), text },
             { QStringLiteral("navigationEndpoint"), QJsonObject{ { QStringLiteral("browseEndpoint"), QJsonObject{
                 { QStringLiteral("browseId"), browseId },
                 { QStringLiteral("browseEndpointContextSupportedConfigs"), QJsonObject{
                     { QStringLiteral("browseEndpointContextMusicConfig"), QJsonObject{
                         { QStringLiteral("pageType"), QString::fromLatin1(pageType) } } } } } } } } } };
}

QJsonObject flex(const QJsonArray &runs)
{
    return { { QStringLiteral("musicResponsiveListItemFlexColumnRenderer"),
               QJsonObject{ { QStringLiteral("text"), QJsonObject{ { QStringLiteral("runs"), runs } } } } } };
}

QString clockOf(int seconds)
{
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString idOf(const QString &text)
{
    QString id = text;
    id.remove(QLatin1Char(' '));
    return id.left(20);
}

QJsonObject playButton(const QString &videoId, const QString &type)
{
    const QJsonObject watch{
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("watchEndpointMusicSupportedConfigs"), QJsonObject{ { QStringLiteral("watchEndpointMusicConfig"),
            QJsonObject{ { QStringLiteral("musicVideoType"), QStringLiteral("MUSIC_VIDEO_TYPE_") + type } } } } } };
    return { { QStringLiteral("musicItemThumbnailOverlayRenderer"), QJsonObject{ { QStringLiteral("content"), QJsonObject{
        { QStringLiteral("musicPlayButtonRenderer"), QJsonObject{ { QStringLiteral("playNavigationEndpoint"),
            QJsonObject{ { QStringLiteral("watchEndpoint"), watch } } } } } } } } } };
}

// One result of a search.
struct Hit {
    QString videoId;
    QString title;
    QString artist;
    QString album;
    int seconds = 0;          // 0: the row gives no length (a live stream)
    QString type = QStringLiteral("ATV");
    bool video = false;
};

QJsonObject searchRow(const Hit &hit)
{
    QJsonArray second;
    second.append(linkRun(hit.artist, QStringLiteral("UCTEST") + idOf(hit.artist),
                          hit.video ? "MUSIC_PAGE_TYPE_USER_CHANNEL" : "MUSIC_PAGE_TYPE_ARTIST"));
    if (hit.video) {
        second.append(run(QStringLiteral(" • ")));
        second.append(run(QStringLiteral("1.2M views")));
    } else if (!hit.album.isEmpty()) {
        second.append(run(QStringLiteral(" • ")));
        second.append(linkRun(hit.album, QStringLiteral("MPREb_TEST") + idOf(hit.album), "MUSIC_PAGE_TYPE_ALBUM"));
    }
    if (hit.seconds > 0) {
        second.append(run(QStringLiteral(" • ")));
        second.append(run(clockOf(hit.seconds)));
    }
    return { { QStringLiteral("musicResponsiveListItemRenderer"), QJsonObject{
        { QStringLiteral("playlistItemData"), QJsonObject{ { QStringLiteral("videoId"), hit.videoId } } },
        { QStringLiteral("overlay"), playButton(hit.videoId, hit.type) },
        { QStringLiteral("flexColumns"), QJsonArray{ flex(QJsonArray{ run(hit.title) }), flex(second) } },
    } } };
}

QByteArray searchAnswer(const QList<Hit> &hits)
{
    QJsonArray rows;
    for (const Hit &hit : hits)
        rows.append(searchRow(hit));
    QJsonArray sections;
    if (!rows.isEmpty())
        sections.append(QJsonObject{ { QStringLiteral("musicShelfRenderer"), QJsonObject{ { QStringLiteral("contents"), rows } } } });
    const QJsonObject list{ { QStringLiteral("sectionListRenderer"), QJsonObject{ { QStringLiteral("contents"), sections } } } };
    const QJsonObject tab{ { QStringLiteral("tabRenderer"), QJsonObject{ { QStringLiteral("content"), list } } } };
    const QJsonObject root{ { QStringLiteral("contents"), QJsonObject{ { QStringLiteral("tabbedSearchResultsRenderer"),
        QJsonObject{ { QStringLiteral("tabs"), QJsonArray{ tab } } } } } } };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QJsonObject playlistRow(const QString &videoId, const QString &title, const QString &artist, const QString &album,
                        int seconds)
{
    return { { QStringLiteral("musicResponsiveListItemRenderer"), QJsonObject{
        { QStringLiteral("playlistItemData"), QJsonObject{ { QStringLiteral("videoId"), videoId },
                                                           { QStringLiteral("playlistSetVideoId"), QStringLiteral("SET") + videoId } } },
        { QStringLiteral("overlay"), playButton(videoId, QStringLiteral("ATV")) },
        { QStringLiteral("flexColumns"), QJsonArray{
            flex(QJsonArray{ run(title) }),
            flex(QJsonArray{ linkRun(artist, QStringLiteral("UCTEST") + idOf(artist), "MUSIC_PAGE_TYPE_ARTIST") }),
            flex(QJsonArray{ linkRun(album, QStringLiteral("MPREb_TEST") + idOf(album), "MUSIC_PAGE_TYPE_ALBUM") }) } },
        { QStringLiteral("fixedColumns"), QJsonArray{ QJsonObject{ { QStringLiteral("musicResponsiveListItemFixedColumnRenderer"),
            QJsonObject{ { QStringLiteral("text"), QJsonObject{ { QStringLiteral("runs"), QJsonArray{ run(clockOf(seconds)) } } } } } } } } },
    } } };
}

QString ytId(int n)
{
    return QStringLiteral("YT") + QString::number(n).rightJustified(9, QLatin1Char('0'));
}

QJsonArray playlistRows(int from, int count)
{
    QJsonArray rows;
    for (int i = from; i < from + count; ++i)
        rows.append(playlistRow(ytId(i), QStringLiteral("Listed Song %1").arg(i), QStringLiteral("List Artist"),
                                QStringLiteral("List Album"), 180 + i % 60));
    return rows;
}

QByteArray playlistPage(const QString &name, const QJsonArray &rows, const QString &next, const QString &details)
{
    QJsonArray contents = rows;
    if (!next.isEmpty())
        contents.append(QJsonObject{ { QStringLiteral("continuationItemRenderer"), QJsonObject{
            { QStringLiteral("continuationEndpoint"), QJsonObject{ { QStringLiteral("continuationCommand"),
                QJsonObject{ { QStringLiteral("token"), next } } } } } } } });
    const QJsonObject header{ { QStringLiteral("musicResponsiveHeaderRenderer"), QJsonObject{
        { QStringLiteral("title"), QJsonObject{ { QStringLiteral("runs"), QJsonArray{ run(name) } } } },
        { QStringLiteral("straplineTextOne"), QJsonObject{ { QStringLiteral("runs"), QJsonArray{ run(QStringLiteral("Test Curator")) } } } },
        { QStringLiteral("secondSubtitle"), QJsonObject{ { QStringLiteral("runs"), QJsonArray{ run(details) } } } } } } };
    const QJsonObject tab{ { QStringLiteral("tabRenderer"), QJsonObject{ { QStringLiteral("content"), QJsonObject{
        { QStringLiteral("sectionListRenderer"), QJsonObject{ { QStringLiteral("contents"), QJsonArray{ header } } } } } } } } };
    const QJsonObject shelf{ { QStringLiteral("musicPlaylistShelfRenderer"), QJsonObject{ { QStringLiteral("contents"), contents } } } };
    const QJsonObject root{
        { QStringLiteral("responseContext"), QJsonObject{ { QStringLiteral("visitorData"), QStringLiteral("IMPVISITOR01") } } },
        { QStringLiteral("contents"), QJsonObject{ { QStringLiteral("twoColumnBrowseResultsRenderer"), QJsonObject{
            { QStringLiteral("tabs"), QJsonArray{ tab } },
            { QStringLiteral("secondaryContents"), QJsonObject{ { QStringLiteral("sectionListRenderer"), QJsonObject{
                { QStringLiteral("contents"), QJsonArray{ shelf } } } } } } } } } } };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray appendedPage(const QJsonArray &rows)
{
    const QJsonObject root{ { QStringLiteral("onResponseReceivedActions"), QJsonArray{ QJsonObject{
        { QStringLiteral("appendContinuationItemsAction"), QJsonObject{ { QStringLiteral("continuationItems"), rows } } } } } } };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// An answer's responseContext, as the session's check reads it.
QByteArray trackingAnswer(const QByteArray &loggedIn)
{
    return R"({"responseContext":{"serviceTrackingParams":[{"service":"GFEEDBACK","params":[{"key":"logged_in","value":")"
           + loggedIn + R"("}]}]},"contents":{}})";
}

QByteArray menuAnswer()
{
    return R"({"actions":[{"openPopupAction":{"popup":{"multiPageMenuRenderer":{"header":{"activeAccountHeaderRenderer":)"
           R"({"accountName":{"runs":[{"text":"Import Test"}]},"channelHandle":{"runs":[{"text":"@importtest"}]}}}}}}}]})";
}

// — Spotify, JioSaavn, Deezer, Apple Music —

QString spotifyTrackId(int n)
{
    return QStringLiteral("TESTTRACK") + QString::number(n).rightJustified(13, QLatin1Char('0'));
}

// A playlist's embed has its pictures as its coverArt; an album's and a
// song's (`coverArt` false) has none, and its pictures are its
// visualIdentity's, as the real ones were in October 2026.
QByteArray spotifyEmbed(const QString &type, const QString &id, const QString &name, const QJsonArray &tracks,
                        bool coverArt = true)
{
    QJsonObject entity{
        { QStringLiteral("type"), type }, { QStringLiteral("name"), name }, { QStringLiteral("title"), name },
        { QStringLiteral("id"), id }, { QStringLiteral("uri"), QStringLiteral("spotify:%1:%2").arg(type, id) },
        { QStringLiteral("subtitle"), QStringLiteral("Test Owner") },
        { QStringLiteral("trackList"), tracks },
    };
    if (coverArt) {
        entity.insert(QStringLiteral("coverArt"), QJsonObject{ { QStringLiteral("sources"), QJsonArray{
            QJsonObject{ { QStringLiteral("url"), QStringLiteral("https://i.scdn.co/image/small") }, { QStringLiteral("width"), 64 } },
            QJsonObject{ { QStringLiteral("url"), QStringLiteral("https://i.scdn.co/image/large") }, { QStringLiteral("width"), 640 } } } } });
    } else {
        const auto image = [](const char *size, int width) {
            return QJsonObject{ { QStringLiteral("url"), QStringLiteral("https://image-cdn-ak.spotifycdn.com/image/") + QLatin1String(size) },
                                { QStringLiteral("maxWidth"), width }, { QStringLiteral("maxHeight"), width } };
        };
        entity.insert(QStringLiteral("visualIdentity"), QJsonObject{ { QStringLiteral("image"), QJsonArray{
            image("medium", 300), image("large", 640), image("small", 64) } } });
    }
    const QJsonObject data{ { QStringLiteral("props"), QJsonObject{ { QStringLiteral("pageProps"), QJsonObject{
        { QStringLiteral("state"), QJsonObject{ { QStringLiteral("data"), QJsonObject{ { QStringLiteral("entity"), entity } } } } } } } } } };
    return "<!DOCTYPE html><html><head><title>Spotify</title></head><body><div id=\"__next\"></div>"
           "<script id=\"__NEXT_DATA__\" type=\"application/json\">"
           + QJsonDocument(data).toJson(QJsonDocument::Compact) + "</script></body></html>";
}

QJsonObject spotifyTrack(int n, const QString &title, const QString &artists, int seconds)
{
    return { { QStringLiteral("uri"), QStringLiteral("spotify:track:") + spotifyTrackId(n) },
             { QStringLiteral("title"), title }, { QStringLiteral("subtitle"), artists },
             { QStringLiteral("duration"), seconds * 1000 }, { QStringLiteral("isPlayable"), true } };
}

QByteArray spotifyPublic(const QString &id, int total, const QString &firstAlbum)
{
    const QJsonObject item{ { QStringLiteral("itemV2"), QJsonObject{ { QStringLiteral("data"), QJsonObject{
        { QStringLiteral("uri"), QStringLiteral("spotify:track:") + spotifyTrackId(0) },
        { QStringLiteral("albumOfTrack"), QJsonObject{ { QStringLiteral("name"), firstAlbum } } } } } } } };
    const QJsonObject state{ { QStringLiteral("entities"), QJsonObject{ { QStringLiteral("items"), QJsonObject{
        { QStringLiteral("spotify:playlist:") + id, QJsonObject{ { QStringLiteral("content"), QJsonObject{
            { QStringLiteral("totalCount"), total }, { QStringLiteral("items"), QJsonArray{ item } } } } } } } } } } };
    return "<html><body><script id=\"initialState\" type=\"text/plain\">"
           + QJsonDocument(state).toJson(QJsonDocument::Compact).toBase64() + "</script></body></html>";
}

QJsonObject saavnSong(const QString &id, const QString &title, const QString &artist, const QString &album, int seconds)
{
    return { { QStringLiteral("id"), id }, { QStringLiteral("title"), title }, { QStringLiteral("type"), QStringLiteral("song") },
             { QStringLiteral("more_info"), QJsonObject{
                 { QStringLiteral("album"), album }, { QStringLiteral("duration"), QString::number(seconds) },
                 { QStringLiteral("artistMap"), QJsonObject{ { QStringLiteral("primary_artists"), QJsonArray{
                     QJsonObject{ { QStringLiteral("name"), artist } } } } } } } } };
}

QJsonObject deezerTrack(qint64 id, const QString &title, const QString &artist, const QString &album, int seconds,
                        const QString &isrc)
{
    QJsonObject track{ { QStringLiteral("id"), id }, { QStringLiteral("title"), title },
                       { QStringLiteral("duration"), seconds },
                       { QStringLiteral("artist"), QJsonObject{ { QStringLiteral("name"), artist } } },
                       { QStringLiteral("album"), QJsonObject{ { QStringLiteral("title"), album } } },
                       { QStringLiteral("type"), QStringLiteral("track") } };
    if (!isrc.isEmpty())
        track.insert(QStringLiteral("isrc"), isrc);
    return track;
}

QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray applePage(const QString &name, const QString &curator, const QJsonArray &items, bool more, bool album)
{
    const QJsonObject header{ { QStringLiteral("itemKind"), QStringLiteral("containerDetailHeaderLockup") },
                              { QStringLiteral("items"), QJsonArray{ QJsonObject{
                                  { QStringLiteral("title"), name },
                                  { QStringLiteral("subtitleLinks"), QJsonArray{ QJsonObject{ { QStringLiteral("title"), curator } } } },
                                  { QStringLiteral("artwork"), QJsonObject{ { QStringLiteral("dictionary"), QJsonObject{
                                      { QStringLiteral("url"), QStringLiteral("https://is1-ssl.mzstatic.com/image/thumb/test/{w}x{h}bb.{f}") } } } } } } } } };
    const QJsonObject tracks{ { QStringLiteral("itemKind"), QStringLiteral("trackLockup") }, { QStringLiteral("items"), items } };
    // An album's footer has its date before the count and its ℗ after, both
    // with a year in them, as After Hours' real one has.
    const QString count = QStringLiteral("%1 songs, %2").arg(items.size()).arg(album ? QStringLiteral("56 minutes")
                                                                                     : QStringLiteral("18 hours 16 minutes"));
    const QString description = album ? QStringLiteral("March 20, 2020\n") + count
                                            + QStringLiteral("\n℗ 2020 The Weeknd XO, Inc., marketed by Republic Records")
                                      : count;
    const QJsonObject footer{ { QStringLiteral("itemKind"), QStringLiteral("containerDetailTracklistFooterLockup") },
                              { QStringLiteral("items"), QJsonArray{ QJsonObject{ { QStringLiteral("description"), description } } } } };
    QJsonObject data{ { QStringLiteral("sections"), QJsonArray{ header, tracks, footer } } };
    if (more)
        data.insert(QStringLiteral("nextIntent"), QJsonObject{ { QStringLiteral("$kind"), QStringLiteral("PlaylistPaginationIntent") } });
    const QJsonObject root{ { QStringLiteral("data"), QJsonArray{ QJsonObject{ { QStringLiteral("intent"), QJsonObject() },
                                                                              { QStringLiteral("data"), data } } } } };
    return "<html><head><script type=\"application/json\" id=\"serialized-server-data\">" + json(root)
           + "</script></head></html>";
}

QJsonObject appleTrack(const QString &id, const QString &title, const QString &artist, const QString &album, int ms,
                       bool albumRow)
{
    QJsonObject item{ { QStringLiteral("title"), title }, { QStringLiteral("duration"), ms },
                      { QStringLiteral("contentDescriptor"), QJsonObject{ { QStringLiteral("identifiers"),
                          QJsonObject{ { QStringLiteral("storeAdamID"), id } } } } } };
    if (albumRow) {
        item.insert(QStringLiteral("artistName"), artist);
    } else {
        item.insert(QStringLiteral("subtitleLinks"), QJsonArray{ QJsonObject{ { QStringLiteral("title"), artist } } });
        item.insert(QStringLiteral("tertiaryLinks"), QJsonArray{ QJsonObject{ { QStringLiteral("title"), album } } });
    }
    return item;
}

// — the session the import must never carry: invented, every value marked TEST —

const char *const kSessionHeader =
    "Cookie: LOGIN_INFO=TESTIMPORT-login-1; SAPISID=TESTIMPORTSAPISID2; __Secure-3PAPISID=TESTIMPORT3PAPISID3; "
    "SID=TESTIMPORT-sid-4; HSID=TESTIMPORT-hsid-5; SSID=TESTIMPORT-ssid-6";

AccountGuard::Limits unpaced()
{
    AccountGuard::Limits limits;
    limits.burst = 1000000;
    limits.refillMs = 1;
    limits.gapMs = 0;
    limits.jitterMs = 0;
    limits.perHour = limits.perDay = 1000000;
    limits.listensPerHour = limits.listensPerDay = 1000000;
    limits.ytdlpGapMs = 0;
    limits.ytdlpPerHour = limits.ytdlpPerDay = 1000000;
    return limits;
}

// The playlist's songs, in order.
QStringList playlistVideoIds(int playlistId)
{
    QStringList ids;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT video_id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position, id"));
    q.addBindValue(playlistId);
    if (q.exec()) {
        while (q.next())
            ids.append(q.value(0).toString());
    }
    return ids;
}

QStringList statuses(int jobId)
{
    QStringList list;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT status FROM import_rows WHERE job_id = ? ORDER BY position"));
    q.addBindValue(jobId);
    if (q.exec()) {
        while (q.next())
            list.append(q.value(0).toString());
    }
    return list;
}

QString jobState(int jobId)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT state FROM import_jobs WHERE id = ?"));
    q.addBindValue(jobId);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

int rowIdAt(int jobId, int position)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT id FROM import_rows WHERE job_id = ? AND position = ?"));
    q.addBindValue(jobId);
    q.addBindValue(position);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

QString rowField(int jobId, int position, const char *column)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT %1 FROM import_rows WHERE job_id = ? AND position = ?").arg(QLatin1String(column)));
    q.addBindValue(jobId);
    q.addBindValue(position);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

// How many of a job's rows `where` says.
int countRows(int jobId, const QString &where)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM import_rows WHERE job_id = ? AND (%1)").arg(where));
    q.addBindValue(jobId);
    return q.exec() && q.next() ? q.value(0).toInt() : -1;
}

int searchesFor(const QList<StandIn::Request> &requests, const QString &query, int from = 0)
{
    int n = 0;
    for (int i = from; i < requests.size(); ++i) {
        const StandIn::Request &request = requests.at(i);
        if (request.path.startsWith("/youtubei/v1/search")
            && request.json().value(QStringLiteral("query")).toString() == query)
            ++n;
    }
    return n;
}

ImportMatch::Candidate candidate(const QString &title, const QString &artist, const QString &album, int seconds,
                                 const QString &type = QStringLiteral("MUSIC_VIDEO_TYPE_ATV"), bool video = false)
{
    ImportMatch::Candidate c;
    c.videoId = QStringLiteral("CAND") + idOf(title);
    c.title = title;
    c.artists = QStringList{ artist };
    c.artistLine = artist;
    c.album = album;
    c.durationSec = seconds;
    c.videoType = type;
    c.video = video;
    return c;
}

ImportMatch::Wanted wanted(const QString &title, const QString &artist, int seconds, const QString &album = QString())
{
    ImportMatch::Wanted w;
    w.title = title;
    w.artists = QStringList{ artist };
    w.album = album;
    w.durationSec = seconds;
    return w;
}

ImportMatch::Confidence verdictOf(const ImportMatch::Wanted &w, const ImportMatch::Candidate &c)
{
    return ImportMatch::judge(w, c).confidence;
}

} // namespace

int runImportSelfTest(Library *library)
{
    Checks t;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test makes playlists and imports a session"));
        return t.finish();
    }
    using C = ImportMatch::Confidence;

    // ------------------------------------------------------------ 1. links
    {
        struct Case {
            const char *text;
            bool ok;
            const char *platform;
            const char *type;
            const char *id;       // when ok; else a word the reason holds
        };
        const Case cases[] = {
            { "https://music.youtube.com/playlist?list=PLtest123abc", true, "ytmusic", "playlist", "VLPLtest123abc" },
            { "https://www.youtube.com/playlist?list=PLtest123abc", true, "youtube", "playlist", "VLPLtest123abc" },
            { "https://m.youtube.com/playlist?list=PLtest123abc&si=x", true, "youtube", "playlist", "VLPLtest123abc" },
            { "https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=PLtest123abc&index=3", true, "youtube", "playlist", "VLPLtest123abc" },
            { "https://youtu.be/dQw4w9WgXcQ?list=PLtest123abc", true, "youtube", "playlist", "VLPLtest123abc" },
            { "music.youtube.com/playlist?list=PLtest123abc", true, "ytmusic", "playlist", "VLPLtest123abc" },
            { "https://music.youtube.com/browse/MPREb_TESTalbum1", true, "ytmusic", "album", "MPREb_TESTalbum1" },
            { "https://music.youtube.com/browse/VLPLtest123abc", true, "ytmusic", "playlist", "VLPLtest123abc" },
            { "https://music.youtube.com/playlist?list=OLAK5uy_TESTalbum", true, "ytmusic", "album", "VLOLAK5uy_TESTalbum" },
            { "https://music.youtube.com/playlist?list=RDCLAK5uy_TESTeditorial", true, "ytmusic", "playlist", "VLRDCLAK5uy_TESTeditorial" },
            { "https://music.youtube.com/watch?v=dQw4w9WgXcQ&list=RDAMVMdQw4w9WgXcQ", false, "ytmusic", nullptr, "mix" },
            { "https://www.youtube.com/playlist?list=RDdQw4w9WgXcQ", false, "youtube", nullptr, "mix" },
            { "https://www.youtube.com/playlist?list=LL", false, "youtube", nullptr, "account" },
            { "https://www.youtube.com/playlist?list=WL", false, "youtube", nullptr, "Watch later" },
            { "https://music.youtube.com/playlist?list=LM", false, "ytmusic", nullptr, "account" },
            { "https://www.youtube.com/watch?v=dQw4w9WgXcQ", false, "youtube", nullptr, "one video" },
            { "https://www.youtube.com/@someone", false, "youtube", nullptr, "channel" },
            { "https://music.youtube.com/browse/UCtestchannel", false, "ytmusic", nullptr, "artist" },
            { "https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M?si=abc123", true, "spotify", "playlist", "37i9dQZF1DXcBWIGoYBM5M" },
            { "https://open.spotify.com/intl-de/album/4yP0hdKOZPNshxUOjY0cZj", true, "spotify", "album", "4yP0hdKOZPNshxUOjY0cZj" },
            { "https://open.spotify.com/track/6b5P51m8xx2XA6U7sdNZ5E", true, "spotify", "song", "6b5P51m8xx2XA6U7sdNZ5E" },
            { "https://open.spotify.com/embed/playlist/37i9dQZF1DXcBWIGoYBM5M", true, "spotify", "playlist", "37i9dQZF1DXcBWIGoYBM5M" },
            { "https://open.spotify.com/user/someone/playlist/37i9dQZF1DXcBWIGoYBM5M", true, "spotify", "playlist", "37i9dQZF1DXcBWIGoYBM5M" },
            { "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M", true, "spotify", "playlist", "37i9dQZF1DXcBWIGoYBM5M" },
            { "https://open.spotify.com/artist/1Xyo4u8uXC1ZmMpatF05PJ", false, "spotify", nullptr, "artist" },
            { "https://open.spotify.com/show/1Xyo4u8uXC1ZmMpatF05PJ", false, "spotify", nullptr, "podcast" },
            { "https://spotify.link/AbCdEf", false, "spotify", nullptr, "short link" },
            { "https://www.jiosaavn.com/featured/100-hits-of-spb/USpboLHSnvmO0eMLZZxqsA__", true, "jiosaavn", "playlist", "USpboLHSnvmO0eMLZZxqsA__" },
            { "https://www.jiosaavn.com/s/playlist/a1b2c3/my-songs/XyZ12ab34_", true, "jiosaavn", "playlist", "XyZ12ab34_" },
            { "https://www.jiosaavn.com/album/after-hours/8Ps4qqBA6,Y_", true, "jiosaavn", "album", "8Ps4qqBA6,Y_" },
            { "https://www.jiosaavn.com/song/abbanee/JgtSVhlDXnQ", true, "jiosaavn", "song", "JgtSVhlDXnQ" },
            { "https://www.jiosaavn.com/artist/arijit-singh-songs/LlRWpHzy3Hk_", false, "jiosaavn", nullptr, "playlist, album or song" },
            { "https://www.deezer.com/en/playlist/8317752022", true, "deezer", "playlist", "8317752022" },
            { "https://www.deezer.com/album/302127", true, "deezer", "album", "302127" },
            { "https://deezer.com/fr/track/3135556", true, "deezer", "song", "3135556" },
            { "https://deezer.page.link/AbCd", false, "deezer", nullptr, "short link" },
            { "https://www.deezer.com/en/artist/27", false, "deezer", nullptr, "artist" },
            { "https://music.apple.com/us/playlist/todays-hits/pl.f4d106fed2bd41149aaacabb233eb5eb", true, "apple", "playlist", "pl.f4d106fed2bd41149aaacabb233eb5eb" },
            { "https://music.apple.com/gb/album/after-hours/1499378108", true, "apple", "album", "1499378108" },
            { "https://music.apple.com/us/album/after-hours/1499378108?i=1499378115", true, "apple", "song", "1499378108" },
            { "https://music.apple.com/us/artist/the-weeknd/479756766", false, "apple", nullptr, "artist" },
            { "https://music.apple.com/us/song/alone-again/1499378115", false, "apple", nullptr, "album" },
            { "https://soundcloud.com/someone/sets/a-list", false, "soundcloud", nullptr, "SoundCloud" },
            { "https://tidal.com/browse/playlist/abc", false, "tidal", nullptr, "Tidal" },
            { "https://music.amazon.com/playlists/B0TEST", false, "amazon", nullptr, "Amazon Music" },
            { "https://example.com/playlist/1", false, "", nullptr, "doesn't know" },
        };
        int wrong = 0;
        QString first;
        for (const Case &c : cases) {
            const Import::Link link = Import::parseLink(QString::fromUtf8(c.text));
            bool right = link.ok == c.ok && link.platform == QLatin1String(c.platform);
            if (c.ok)
                right = right && link.type == QLatin1String(c.type) && link.id == QLatin1String(c.id) && !link.label.isEmpty();
            else
                right = right && link.reason.contains(QLatin1String(c.id), Qt::CaseInsensitive);
            if (!right) {
                ++wrong;
                if (first.isEmpty())
                    first = QStringLiteral("%1 -> ok=%2 %3 %4 %5 \"%6\"").arg(QString::fromUtf8(c.text)).arg(link.ok ? 1 : 0)
                                .arg(link.platform, link.type, link.id, link.reason);
            }
        }
        t.check(wrong == 0, QStringLiteral("%1 links read or refused as they should, each refusal with its plain reason")
                                .arg(int(std::size(cases))), first);
        const Import::Link apple = Import::parseLink(QStringLiteral("https://music.apple.com/gb/album/after-hours/1499378108?i=1499378115"));
        t.check(apple.region == QLatin1String("gb") && apple.songId == QLatin1String("1499378115")
                    && apple.slug == QLatin1String("after-hours"),
                QStringLiteral("  an Apple Music song: its album, its storefront and the song's own id"));
        const Import::Link typing = Import::parseLink(QStringLiteral("blinding lig"));
        const Import::Link lines = Import::parseLink(QStringLiteral("Coldplay - Yellow\nThe Weeknd - Blinding Lights"));
        t.check(!typing.ok && typing.reason.isEmpty() && !lines.ok && lines.looksLikeList,
                QStringLiteral("  words typed are not yet a link (no reason shown); several lines are a list of songs"));
    }

    // ------------------------------------------------------------ 2. lists and CSV files
    {
        const Import::Page page = Import::readList(QStringLiteral(
            "# my songs\n\nColdplay - Yellow (4:26)\nBlinding Lights by The Weeknd\n3. Daft Punk – One More Time\n"
            "Bohemian Rhapsody\nTitle In Tab\tSome Artist\tAlbum In Tab\n"));
        const auto row = [&page](int i) { return page.rows.value(i); };
        t.check(page.rows.size() == 5, QStringLiteral("a pasted list: 5 songs, the comment and the blank line left out"),
                QString::number(page.rows.size()));
        t.check(row(0).title == QLatin1String("Yellow") && row(0).artists == QStringList{ QStringLiteral("Coldplay") }
                    && row(0).durationMs == 266000 && row(0).eitherWay,
                QStringLiteral("  \"Coldplay - Yellow (4:26)\": the length taken off, either half may be the title"));
        t.check(row(1).title == QLatin1String("Blinding Lights") && row(1).artists.value(0) == QLatin1String("The Weeknd")
                    && !row(1).eitherWay,
                QStringLiteral("  \"Title by Artist\""));
        t.check(row(2).title == QLatin1String("One More Time") && row(2).artists.value(0) == QLatin1String("Daft Punk"),
                QStringLiteral("  numbering off, an en dash read as a dash"));
        t.check(row(3).title == QLatin1String("Bohemian Rhapsody") && row(3).artists.isEmpty()
                    && page.warnings.join(QLatin1Char(' ')).contains(QLatin1String("1 of the lines name no artist")),
                QStringLiteral("  a title alone, and a warning that it waits to be checked"), page.warnings.join(QLatin1Char('|')));
        t.check(row(4).title == QLatin1String("Title In Tab") && row(4).artists.value(0) == QLatin1String("Some Artist")
                    && row(4).album == QLatin1String("Album In Tab"),
                QStringLiteral("  columns pasted from a spreadsheet"));

        const QByteArray exportify =
            "\xEF\xBB\xBF\"Track URI\",\"Track Name\",\"Artist URI(s)\",\"Artist Name(s)\",\"Album Name\",\"Track Duration (ms)\",\"ISRC\"\r\n"
            "\"spotify:track:0VjIjW4GlUZAMYd2vXMi3b\",\"Blinding Lights\",\"spotify:artist:1\",\"The Weeknd\",\"After Hours\",\"200040\",\"USUG11904206\"\r\n"
            "\"spotify:track:1\",\"Under Pressure - Remastered 2011\",\"spotify:artist:2,spotify:artist:3\",\"Queen,David Bowie\",\"Hot Space\",\"248440\",\"GBUM71029605\"\r\n"
            "\"spotify:track:2\",\"A \"\"quoted\"\" title, with a comma\",\"spotify:artist:4\",\"Test Band\",\"Line one\nline two\",\"180000\",\"\"\r\n";
        const Import::Page csv = Import::readCsv(exportify, QStringLiteral("my_road_trip"));
        t.check(csv.error.isEmpty() && csv.rows.size() == 3 && csv.name == QLatin1String("my road trip"),
                QStringLiteral("Exportify's CSV: 3 songs, the playlist named after the file"), csv.error);
        t.check(csv.rows.value(0).isrc == QLatin1String("USUG11904206") && csv.rows.value(0).durationMs == 200040
                    && csv.rows.value(0).sourceId == QLatin1String("spotify:track:0VjIjW4GlUZAMYd2vXMi3b"),
                QStringLiteral("  its ISRC, its length in ms and the Spotify id, by the headers' names"));
        t.check(csv.rows.value(1).artists == QStringList({ QStringLiteral("Queen"), QStringLiteral("David Bowie") }),
                QStringLiteral("  two artists in one field"), csv.rows.value(1).artists.join(QLatin1Char('|')));
        t.check(csv.rows.value(2).title == QLatin1String("A \"quoted\" title, with a comma")
                    && csv.rows.value(2).album == QLatin1String("Line one\nline two"),
                QStringLiteral("  quotes, a comma and a line break inside fields"));
        const Import::Page tmm = Import::readCsv(
            "Track name,Artist name,Album,Playlist name,Type,ISRC\nYellow,Coldplay,Parachutes,Mine,Playlist,GBAYE0000351\n",
            QStringLiteral("tunemymusic"));
        t.check(tmm.rows.size() == 1 && tmm.rows.value(0).title == QLatin1String("Yellow")
                    && tmm.rows.value(0).artists.value(0) == QLatin1String("Coldplay") && tmm.rows.value(0).isrc == QLatin1String("GBAYE0000351"),
                QStringLiteral("TuneMyMusic's CSV, by its own headers"));
        const Import::Page bad = Import::readCsv("Foo,Bar\n1,2\n", QStringLiteral("x"));
        t.check(!bad.error.isEmpty() && bad.error.contains(QLatin1String("Track Name")),
                QStringLiteral("a CSV with no title column: refused, saying which columns it reads"), bad.error);
    }

    // ------------------------------------------------------------ 3. the matcher's rule
    {
        t.check(verdictOf(wanted(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), 200),
                          candidate(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), QStringLiteral("After Hours"), 201)) == C::Sure,
                QStringLiteral("matcher: the same title, artist and length on YouTube Music's own song: sure"));
        const ImportMatch::Verdict cover = ImportMatch::judge(
            wanted(QStringLiteral("You Needed Me"), QStringLiteral("Anne Murray"), 222),
            candidate(QStringLiteral("You Needed Me (Originally By Anne Murray)"), QStringLiteral("Various Musique"),
                      QStringLiteral("Hits Made Famous by Anne Murray"), 222));
        t.check(cover.confidence == C::None && cover.reason.contains(QLatin1String("cover")),
                QStringLiteral("  \"(Originally By Anne Murray)\" by Various Musique: a cover, refused"), cover.reason);
        t.check(verdictOf(wanted(QStringLiteral("Yellow"), QStringLiteral("Coldplay"), 266),
                          candidate(QStringLiteral("Yellow (Live)"), QStringLiteral("Coldplay"), QString(), 268)) == C::None
                    && verdictOf(wanted(QStringLiteral("Yellow - Live in Buenos Aires"), QStringLiteral("Coldplay"), 300),
                                 candidate(QStringLiteral("Yellow"), QStringLiteral("Coldplay"), QString(), 300)) == C::None,
                QStringLiteral("  a live take for the studio song refused, and the other way round"));
        t.check(verdictOf(wanted(QStringLiteral("Song Twenty"), QStringLiteral("Test Band"), 180),
                          candidate(QStringLiteral("Song Twenty"), QStringLiteral("Test Band"), QString(), 200)) == C::None,
                QStringLiteral("  20 s apart: refused"));
        t.check(verdictOf(wanted(QStringLiteral("Song Ten"), QStringLiteral("Test Band"), 180),
                          candidate(QStringLiteral("Song Ten"), QStringLiteral("Test Band"), QString(), 190)) == C::Likely
                    && verdictOf(wanted(QStringLiteral("Song Ten"), QStringLiteral("Test Band"), 180),
                                 candidate(QStringLiteral("Song Ten"), QStringLiteral("Test Band"), QString(), 181,
                                           QStringLiteral("MUSIC_VIDEO_TYPE_UGC"), true)) == C::Likely,
                QStringLiteral("  10 s apart: likely; someone's upload within a second: likely at most"));
        t.check(verdictOf(wanted(QStringLiteral("Exact Title"), QStringLiteral("Someone"), 200),
                          candidate(QStringLiteral("Exact Title"), QStringLiteral("Another Name"), QString(), 201)) == C::Likely
                    && verdictOf(wanted(QStringLiteral("Exact Title"), QStringLiteral("Someone"), 200),
                                 candidate(QStringLiteral("Exact Title"), QStringLiteral("Another Name"), QString(), 204)) == C::None,
                QStringLiteral("  the exact title by another name: likely within 2 s, refused at 4"));
        t.check(verdictOf(wanted(QStringLiteral("Video Only"), QStringLiteral("Test Band"), 210),
                          candidate(QStringLiteral("Test Band - Video Only (Official Video)"), QStringLiteral("Test Band"), QString(),
                                    0, QStringLiteral("MUSIC_VIDEO_TYPE_OMV"), true)) == C::Check,
                QStringLiteral("  a video with no length: to check (its \"Artist - Title (Official Video)\" read)"));
        t.check(verdictOf(wanted(QStringLiteral("Bohemian Rhapsody - Remastered 2011"), QStringLiteral("Queen"), 355),
                          candidate(QStringLiteral("Bohemian Rhapsody"), QStringLiteral("Queen"), QStringLiteral("A Night at the Opera"), 355)) == C::Sure,
                QStringLiteral("  Spotify's \" - Remastered 2011\" is release noise"));
        t.check(verdictOf(wanted(QStringLiteral("Srivalli (Hindi)"), QStringLiteral("Javed Ali"), 220),
                          candidate(QStringLiteral("Srivalli"), QStringLiteral("Javed Ali"), QString(), 220)) == C::None,
                QStringLiteral("  a song sung in another language is another recording"));
        t.check(verdictOf(wanted(QStringLiteral("Kesariya (From \"Brahmastra\")"), QStringLiteral("Pritam"), 268, QStringLiteral("Brahmastra")),
                          candidate(QStringLiteral("Kesariya"), QStringLiteral("Pritam"), QStringLiteral("Brahmastra"), 268)) == C::Sure
                    && verdictOf(wanted(QStringLiteral("Night Song"), QStringLiteral("Test Band"), 200),
                                 candidate(QStringLiteral("Night Song (Midnight Ride)"), QStringLiteral("Test Band"), QString(), 200)) == C::Check,
                QStringLiteral("  a film's name is no version; an aside nobody can place is left to check"));
        ImportMatch::Wanted either;
        either.title = QStringLiteral("Coldplay");
        either.artists = QStringList{ QStringLiteral("Yellow") };
        either.durationSec = 266;
        either.eitherWay = true;
        t.check(verdictOf(either, candidate(QStringLiteral("Yellow"), QStringLiteral("Coldplay"), QString(), 266)) == C::Sure,
                QStringLiteral("  a pasted \"Title - Artist\" line read the other way round"));
        ImportMatch::Wanted withIsrc = wanted(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), 200,
                                              QStringLiteral("After Hours (Deluxe)"));
        withIsrc.isrc = QStringLiteral("usug11904206");
        const QList<ImportMatch::Search> searches = ImportMatch::searches(withIsrc);
        t.check(searches.size() == 4 && searches.at(0).isrc && searches.at(0).query == QLatin1String("USUG11904206")
                    && searches.at(1).query == QLatin1String("Blinding Lights The Weeknd") && !searches.at(1).videos
                    && searches.at(2).query == QLatin1String("Blinding Lights After Hours") && searches.at(3).videos
                    && searches.at(3).query == searches.at(1).query,
                QStringLiteral("  the searches in order: the ISRC, title and lead artist, title and album, then videos"),
                [&searches]() { QStringList q; for (const auto &s : searches) q << s.query; return q.join(QLatin1Char('|')); }());
        const QList<ImportMatch::Candidate> isrcAnswer{
            candidate(QStringLiteral("Something Else"), QStringLiteral("Other"), QString(), 200),
            candidate(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), QString(), 200) };
        const ImportMatch::Pick isrcPick = ImportMatch::pick(withIsrc, isrcAnswer, searches.at(0));
        const ImportMatch::Pick plainPick = ImportMatch::pick(withIsrc, isrcAnswer, searches.at(1));
        t.check(isrcPick.index < 0 && plainPick.index == 1,
                QStringLiteral("  an ISRC search's first result alone counts; the same answer to a plain search finds the second"));
        const QList<ImportMatch::Candidate> ranked{
            candidate(QStringLiteral("Ranked"), QStringLiteral("Test Band"), QString(), 200, QStringLiteral("MUSIC_VIDEO_TYPE_UGC")),
            candidate(QStringLiteral("Ranked"), QStringLiteral("Test Band"), QString(), 203, QStringLiteral("MUSIC_VIDEO_TYPE_OMV")),
            candidate(QStringLiteral("Ranked"), QStringLiteral("Test Band"), QString(), 204, QStringLiteral("MUSIC_VIDEO_TYPE_ATV")) };
        const ImportMatch::Pick rankedPick = ImportMatch::pick(wanted(QStringLiteral("Ranked"), QStringLiteral("Test Band"), 200),
                                                               ranked, searches.at(1));
        t.check(rankedPick.index == 2 && rankedPick.verdict.confidence == C::Sure,
                QStringLiteral("  sure before likely, YouTube Music's own song before the video, never the nearest length alone"),
                QString::number(rankedPick.index));

        // Sung or played by several, as YouTube Music credits a film song.
        const auto sung = [](const QString &title, const QStringList &artists, int seconds, const QString &album = QString()) {
            ImportMatch::Wanted w = wanted(title, artists.value(0), seconds, album);
            w.artists = artists;
            return w;
        };
        const auto credited = [](const QString &title, const QStringList &artists, const QString &album, int seconds) {
            ImportMatch::Candidate c = candidate(title, artists.value(0), album, seconds);
            c.artists = artists;
            c.artistLine = artists.join(QStringLiteral(", "));
            return c;
        };
        // A tribute album's titles are the songs' own: only its name says
        // what it is, and that name credits nobody.
        const ImportMatch::Verdict tribute = ImportMatch::judge(
            wanted(QStringLiteral("You Needed Me"), QStringLiteral("Anne Murray"), 222),
            candidate(QStringLiteral("You Needed Me"), QStringLiteral("Various Musique"), QStringLiteral("Hits Made Famous by Anne Murray"), 222));
        t.check(tribute.confidence == C::None
                    && verdictOf(wanted(QStringLiteral("Yellow"), QStringLiteral("Coldplay"), 266),
                                 candidate(QStringLiteral("Yellow"), QStringLiteral("Vitamin String Quartet"),
                                           QStringLiteral("The String Quartet Tribute to Coldplay"), 268)) == C::None
                    && verdictOf(wanted(QStringLiteral("Yellow"), QStringLiteral("Coldplay"), 266),
                                 candidate(QStringLiteral("Yellow"), QStringLiteral("Rockabye Baby!"),
                                           QStringLiteral("Lullaby Renditions of Coldplay"), 275)) == C::None,
                QStringLiteral("  a plain title on a tribute album (\"Hits Made Famous by Anne Murray\", \"Tribute to Coldplay\", "
                               "\"Lullaby Renditions of Coldplay\"): refused, the album's name no artist's credit"),
                tribute.reason);
        t.check(verdictOf(wanted(QStringLiteral("Yellow - Live in Buenos Aires"), QStringLiteral("Coldplay"), 300),
                          candidate(QStringLiteral("Yellow (Live)"), QStringLiteral("Coldplay"), QStringLiteral("Live 2003"), 302)) == C::None
                    && verdictOf(wanted(QStringLiteral("Yellow - Live in Buenos Aires"), QStringLiteral("Coldplay"), 300),
                                 candidate(QStringLiteral("Yellow (Live)"), QStringLiteral("Coldplay"),
                                           QStringLiteral("Live in Buenos Aires"), 301)) == C::Sure,
                QStringLiteral("  one live take is not another: Buenos Aires refuses \"Live 2003\", and takes the album named for it"));
        t.check(verdictOf(wanted(QStringLiteral("Blinding Lights (Chromatics Remix)"), QStringLiteral("The Weeknd"), 300),
                          candidate(QStringLiteral("Blinding Lights (Major Lazer Remix)"), QStringLiteral("The Weeknd"), QString(), 300)) == C::None
                    && verdictOf(wanted(QStringLiteral("Blinding Lights - Chromatics Remix"), QStringLiteral("The Weeknd"), 300),
                                 candidate(QStringLiteral("Blinding Lights (Chromatics Remix)"), QStringLiteral("The Weeknd"), QString(), 301)) == C::Sure,
                QStringLiteral("  whose remix it is: Chromatics' is not Major Lazer's"));
        const QStringList pushpa{ QStringLiteral("Devi Sri Prasad"), QStringLiteral("Sid Sriram") };
        const ImportMatch::Verdict dub = ImportMatch::judge(
            sung(QStringLiteral("Srivalli"), pushpa, 224, QStringLiteral("Pushpa - The Rise (Telugu)")),
            credited(QStringLiteral("Srivalli"), { QStringLiteral("Javed Ali"), QStringLiteral("Devi Sri Prasad") },
                     QStringLiteral("Pushpa - The Rise (Hindi)"), 224));
        t.check(dub.confidence == C::None
                    && verdictOf(sung(QStringLiteral("Srivalli"), pushpa, 224, QStringLiteral("Pushpa - The Rise (Telugu)")),
                                 credited(QStringLiteral("Srivalli"), pushpa, QStringLiteral("Pushpa - The Rise - Part 01 (Telugu)"), 224)) == C::Sure,
                QStringLiteral("  a language the albums name: the Telugu album's song is not the Hindi album's, the Telugu one is"),
                dub.reason);
        // YouTube Music's "(Movie: X)" is JioSaavn's and Spotify's '(From "X")'
        // (three of fifteen Hot Hits Hindi songs, live, October 2026).
        const ImportMatch::Verdict movie = ImportMatch::judge(
            sung(QStringLiteral("Tum Ho Toh (From \"Saiyaara\")"),
                 { QStringLiteral("Vishal Mishra"), QStringLiteral("Hansika Pareek"), QStringLiteral("Raj Shekhar") }, 318),
            credited(QStringLiteral("Tum Ho Toh (Movie: Saiyaara)"),
                     { QStringLiteral("Vishal Mishra"), QStringLiteral("Raj Shekhar"), QStringLiteral("Hansika Pareek") },
                     QStringLiteral("Saiyaara"), 319));
        const QStringList war{ QStringLiteral("Pritam"), QStringLiteral("Arijit Singh"), QStringLiteral("Nikhita Gandhi"),
                               QStringLiteral("Amitabh Bhattacharya") };
        t.check(movie.confidence == C::Sure
                    && verdictOf(sung(QStringLiteral("Aavan Jaavan (From \"WAR 2\")"), war, 226),
                                 credited(QStringLiteral("Aavan Jaavan (Movie: WAR 2)"), war, QStringLiteral("War 2"), 226)) == C::Sure
                    && verdictOf(sung(QStringLiteral("Saiyaara (From \"Saiyaara\")"),
                                      { QStringLiteral("Tanishk Bagchi"), QStringLiteral("Faheem Abdullah"),
                                        QStringLiteral("Arslan Nizami"), QStringLiteral("Irshad Kamil") }, 371),
                                 credited(QStringLiteral("Saiyaara (Movie: Saiyaara)"),
                                          { QStringLiteral("Faheem Abdullah"), QStringLiteral("Arslan Nizami"), QStringLiteral("Irshad Kamil") },
                                          QStringLiteral("Saiyaara"), 371)) == C::Sure,
                QStringLiteral("  \"(Movie: Saiyaara)\" read as '(From \"Saiyaara\")': sure, not one to check"), movie.reason);
        // What a match made once is kept by (import_matches' "tal:" key): the
        // version is in it, so it is never used for another.
        const auto key = [](const QString &title, const QString &artist, const QString &album = QString()) {
            return ImportMatch::songKey(wanted(title, artist, 0, album));
        };
        t.check(key(QStringLiteral("Yellow"), QStringLiteral("Coldplay")) != key(QStringLiteral("Yellow (Live)"), QStringLiteral("Coldplay"))
                    && key(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"))
                           != key(QStringLiteral("Blinding Lights (Instrumental)"), QStringLiteral("The Weeknd"))
                    && key(QStringLiteral("Love Story"), QStringLiteral("Taylor Swift"))
                           != key(QStringLiteral("Love Story (Taylor's Version)"), QStringLiteral("Taylor Swift"))
                    && key(QStringLiteral("Srivalli"), QStringLiteral("Sid Sriram"), QStringLiteral("Pushpa - The Rise (Telugu)"))
                           != key(QStringLiteral("Srivalli"), QStringLiteral("Sid Sriram"), QStringLiteral("Pushpa - The Rise (Hindi)"))
                    && key(QStringLiteral("Bohemian Rhapsody - Remastered 2011"), QStringLiteral("Queen"))
                           == key(QStringLiteral("Bohemian Rhapsody"), QStringLiteral("Queen")),
                QStringLiteral("  a match is kept by title, version and artist: live, instrumental, re-recorded and a dub apart; "
                               "a remaster the same song"),
                key(QStringLiteral("Yellow (Live)"), QStringLiteral("Coldplay")));
    }

    // ------------------------------------------------------------ the stand-ins
    StandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("stand-ins for YouTube Music, Spotify, JioSaavn, Deezer and Apple Music listen on this computer")))
        return t.finish();
    InnerTube::setTestServer(standIn.base());
    PlaylistImport::setTestServer(standIn.base());
    InnerTube::setRegion(QString());
    t.note(QStringLiteral("every request goes to ") + standIn.base() + QStringLiteral("; nothing leaves this computer"));

    // What YouTube Music answers, by query; and how many times a query is
    // refused (429) before it is answered.
    QHash<QString, QList<Hit>> songs;
    QHash<QString, QList<Hit>> videos;
    QHash<QString, int> refuse;
    const auto song = [](const QString &id, const QString &title, const QString &artist, const QString &album, int seconds,
                         const QString &type = QStringLiteral("ATV")) {
        Hit hit;
        hit.videoId = id;
        hit.title = title;
        hit.artist = artist;
        hit.album = album;
        hit.seconds = seconds;
        hit.type = type;
        return hit;
    };
    // Job A's songs (a Deezer playlist).
    songs.insert(QStringLiteral("USUG11904206"), { song(QStringLiteral("BLVID000001"), QStringLiteral("Blinding Lights"),
                                                        QStringLiteral("The Weeknd"), QStringLiteral("After Hours"), 200) });
    songs.insert(QStringLiteral("You Needed Me Anne Murray"),
                 { song(QStringLiteral("AMCOVER0001"), QStringLiteral("You Needed Me (Originally By Anne Murray)"),
                        QStringLiteral("Various Musique"), QStringLiteral("Hits Made Famous by Anne Murray"), 222),
                   song(QStringLiteral("AMREAL00001"), QStringLiteral("You Needed Me"), QStringLiteral("Anne Murray"),
                        QStringLiteral("Let's Keep It That Way"), 223) });
    songs.insert(QStringLiteral("Yellow Coldplay"), { song(QStringLiteral("YELLIVE0001"), QStringLiteral("Yellow (Live)"),
                                                           QStringLiteral("Coldplay"), QStringLiteral("Live 2003"), 300) });
    songs.insert(QStringLiteral("Yellow Parachutes"), { song(QStringLiteral("YELSTUDIO01"), QStringLiteral("Yellow"),
                                                             QStringLiteral("Coldplay"), QStringLiteral("Parachutes"), 267) });
    songs.insert(QStringLiteral("Song Twenty Test Band"), { song(QStringLiteral("STWENTY0001"), QStringLiteral("Song Twenty"),
                                                                 QStringLiteral("Test Band"), QStringLiteral("Test Album"), 200) });
    {
        Hit video = song(QStringLiteral("VOVID000001"), QStringLiteral("Test Band - Video Only (Official Video)"),
                         QStringLiteral("Test Band"), QString(), 0, QStringLiteral("OMV"));
        video.video = true;
        videos.insert(QStringLiteral("Video Only Test Band"), { video });
    }
    songs.insert(QStringLiteral("XX0000000001"), { song(QStringLiteral("SOMEELSE001"), QStringLiteral("Something Else"),
                                                        QStringLiteral("Other Artist"), QString(), 190),
                                                   song(QStringLiteral("SBISRC00002"), QStringLiteral("Second Best"),
                                                        QStringLiteral("Test Band"), QString(), 190) });
    songs.insert(QStringLiteral("Second Best Test Band"), { song(QStringLiteral("SBOMV000001"), QStringLiteral("Second Best"),
                                                                 QStringLiteral("Test Band"), QStringLiteral("Test Album"), 191,
                                                                 QStringLiteral("OMV")) });
    // Job B's (Spotify), whose first song is refused twice before an answer.
    songs.insert(QStringLiteral("Rest Song Test Band"), { song(QStringLiteral("RESTSONG001"), QStringLiteral("Rest Song"),
                                                               QStringLiteral("Test Band"), QString(), 200) });
    songs.insert(QStringLiteral("Calm Song Test Band"), { song(QStringLiteral("CALMSONG001"), QStringLiteral("Calm Song"),
                                                               QStringLiteral("Test Band"), QString(), 180) });
    refuse.insert(QStringLiteral("Rest Song Test Band"), 2);
    // Job S's (JioSaavn), job R's (a pasted list, across a restart), job W's.
    const QStringList saavnTitles{ QStringLiteral("Saavn One"), QStringLiteral("Saavn Two"), QStringLiteral("Saavn Three"),
                                   QStringLiteral("Saavn Four"), QStringLiteral("Saavn Five") };
    for (int i = 0; i < saavnTitles.size(); ++i)
        songs.insert(saavnTitles.at(i) + QStringLiteral(" Singer A"),
                     { song(QStringLiteral("SAAVNVID%1").arg(i, 3, 10, QLatin1Char('0')), saavnTitles.at(i), QStringLiteral("Singer A"),
                            QStringLiteral("Saavn Album"), 201 + i) });
    const QStringList resumeTitles{ QStringLiteral("Resume One"), QStringLiteral("Resume Two"), QStringLiteral("Resume Three"),
                                    QStringLiteral("Resume Four"), QStringLiteral("Resume Five"), QStringLiteral("Resume Six") };
    for (int i = 0; i < resumeTitles.size(); ++i)
        songs.insert(resumeTitles.at(i) + QStringLiteral(" Test Band"),
                     { song(QStringLiteral("RESUMEVID%1").arg(i, 2, 10, QLatin1Char('0')), resumeTitles.at(i),
                            QStringLiteral("Test Band"), QString(), 181 + i) });
    songs.insert(QStringLiteral("Cap One Test Band"), { song(QStringLiteral("CAPONE00001"), QStringLiteral("Cap One"), QStringLiteral("Test Band"), QString(), 200) });
    songs.insert(QStringLiteral("Cap Two Test Band"), { song(QStringLiteral("CAPTWO00001"), QStringLiteral("Cap Two"), QStringLiteral("Test Band"), QString(), 201) });

    const QString spotifyBig = QStringLiteral("TESTBIGPLAYLIST0000001");
    const QString spotifyRest = QStringLiteral("TESTRESTPLAYLIST000001");
    const QString spotifyAlbum = QStringLiteral("TESTALBUM0000000000001");

    standIn.respond = [&](const StandIn::Request &request) {
        StandIn::Answer answer;
        const QString path = QString::fromUtf8(request.path);
        const QUrl url(standIn.base() + path);
        const QUrlQuery query(url);
        const QJsonObject body = request.json();
        // — YouTube Music —
        if (path.startsWith(QLatin1String("/youtubei/v1/search"))) {
            const QString q = body.value(QStringLiteral("query")).toString();
            const bool video = body.value(QStringLiteral("params")).toString().startsWith(QLatin1String("EgWKAQIQ"));
            if (refuse.value(q) > 0) {
                refuse[q] -= 1;
                answer.status = 429;
                answer.body = R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED"}})";
                return answer;
            }
            answer.body = searchAnswer(video ? videos.value(q) : songs.value(q));
            return answer;
        }
        if (path.startsWith(QLatin1String("/youtubei/v1/account/account_menu"))) {
            answer.body = request.authed() ? menuAnswer() : QByteArray(R"({"actions":[]})");
            return answer;
        }
        if (path.startsWith(QLatin1String("/youtubei/v1/browse"))) {
            const QString id = body.value(QStringLiteral("browseId")).toString();
            const QString token = body.value(QStringLiteral("continuation")).toString();
            if (id == QLatin1String("VLPLIMPORTTEST"))
                answer.body = playlistPage(QStringLiteral("YouTube Test List"), playlistRows(0, 100), QStringLiteral("IMPTOK2"),
                                           QStringLiteral("130 songs • 8+ hours"));
            else if (token == QLatin1String("IMPTOK2"))
                answer.body = appendedPage(playlistRows(100, 30));
            else if (id == QLatin1String("VLPLIMPORTPRIVATE"))
                answer.body = R"({"responseContext":{"visitorData":"IMPVISITOR01"}})";
            else
                answer.body = trackingAnswer(request.authed() ? "1" : "0");
            return answer;
        }
        // — Spotify —
        if (path.startsWith(QLatin1String("/open.spotify.com/embed/playlist/") + spotifyBig)) {
            QJsonArray tracks;
            for (int i = 0; i < 100; ++i)
                tracks.append(spotifyTrack(i, QStringLiteral("Big Song %1").arg(i), QStringLiteral("Artist One, Artist Two"), 200));
            answer.type = "text/html; charset=utf-8";
            answer.body = spotifyEmbed(QStringLiteral("playlist"), spotifyBig, QStringLiteral("Big Spotify List"), tracks);
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/playlist/") + spotifyBig)) {
            answer.type = "text/html; charset=utf-8";
            answer.body = spotifyPublic(spotifyBig, 150, QStringLiteral("First Album"));
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/embed/playlist/") + spotifyRest)) {
            answer.type = "text/html; charset=utf-8";
            answer.body = spotifyEmbed(QStringLiteral("playlist"), spotifyRest, QStringLiteral("Rest List"),
                                       { spotifyTrack(1, QStringLiteral("Rest Song"), QStringLiteral("Test Band"), 200),
                                         spotifyTrack(2, QStringLiteral("Calm Song"), QStringLiteral("Test Band"), 180) });
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/embed/album/") + spotifyAlbum)) {
            answer.type = "text/html; charset=utf-8";
            answer.body = spotifyEmbed(QStringLiteral("album"), spotifyAlbum, QStringLiteral("Test Album"),
                                       { spotifyTrack(3, QStringLiteral("Album Song"), QStringLiteral("Test Band"), 200) });
            return answer;
        }
        // — JioSaavn —
        if (path.startsWith(QLatin1String("/www.jiosaavn.com/api.php"))) {
            const QString token = query.queryItemValue(QStringLiteral("token"));
            const int p = query.queryItemValue(QStringLiteral("p")).toInt();
            QJsonObject root{ { QStringLiteral("title"), QStringLiteral("Saavn &amp; Test") },
                              { QStringLiteral("image"), QStringLiteral("https://c.saavncdn.com/test-150x150.jpg") },
                              { QStringLiteral("more_info"), QJsonObject{ { QStringLiteral("firstname"), QStringLiteral("Test") } } } };
            QJsonArray list;
            if (token == QLatin1String("IMPSAAVNBIG")) {
                const int count = p == 1 ? 100 : p == 2 ? 37 : 0;
                for (int i = 0; i < count; ++i)
                    list.append(saavnSong(QStringLiteral("SB%1").arg((p - 1) * 100 + i), QStringLiteral("Big Saavn %1").arg((p - 1) * 100 + i),
                                          QStringLiteral("Singer A"), QStringLiteral("Saavn Album"), 200));
            } else if (token == QLatin1String("IMPSAAVNJOB")) {
                const int from = p == 1 ? 0 : p == 2 ? 3 : 5;
                const int to = p == 1 ? 3 : p == 2 ? 5 : 5;
                for (int i = from; i < to; ++i)
                    list.append(saavnSong(QStringLiteral("SJ%1").arg(i), saavnTitles.at(i), QStringLiteral("Singer A"),
                                          QStringLiteral("Saavn Album"), 201 + i));
            }
            if (list.isEmpty())
                root.insert(QStringLiteral("list"), QString());   // the way JioSaavn ends a list
            else
                root.insert(QStringLiteral("list"), list);
            answer.body = json(root);
            return answer;
        }
        // — Deezer —
        if (path.startsWith(QLatin1String("/api.deezer.com/"))) {
            const QString what = url.path().mid(QStringLiteral("/api.deezer.com/").size());
            const int index = query.queryItemValue(QStringLiteral("index")).toInt();
            if (what == QLatin1String("playlist/900001") || what == QLatin1String("playlist/900001/tracks")) {
                QJsonArray rows{
                    deezerTrack(1001, QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), QStringLiteral("After Hours"), 200, QStringLiteral("USUG11904206")),
                    deezerTrack(1002, QStringLiteral("You Needed Me"), QStringLiteral("Anne Murray"), QStringLiteral("Let's Keep It That Way"), 222, QString()),
                    deezerTrack(1003, QStringLiteral("Yellow"), QStringLiteral("Coldplay"), QStringLiteral("Parachutes"), 266, QString()),
                    deezerTrack(1004, QStringLiteral("Song Twenty"), QStringLiteral("Test Band"), QStringLiteral("Test Album"), 180, QString()),
                    deezerTrack(1005, QStringLiteral("Video Only"), QStringLiteral("Test Band"), QStringLiteral("Test Album"), 210, QString()),
                    deezerTrack(1006, QStringLiteral("Second Best"), QStringLiteral("Test Band"), QStringLiteral("Test Album"), 190, QStringLiteral("XX0000000001")),
                    deezerTrack(1007, QStringLiteral("In Library"), QStringLiteral("Test Band"), QString(), 240, QString()) };
                if (what.endsWith(QLatin1String("tracks"))) {
                    answer.body = json({ { QStringLiteral("data"), rows }, { QStringLiteral("total"), 7 } });
                } else {
                    // On the header the songs come without their ISRC: read again from the pages.
                    QJsonArray bare;
                    for (const QJsonValue &row : std::as_const(rows)) {
                        QJsonObject copy = row.toObject();
                        copy.remove(QStringLiteral("isrc"));
                        bare.append(copy);
                    }
                    answer.body = json({ { QStringLiteral("id"), 900001 }, { QStringLiteral("title"), QStringLiteral("Deezer test") },
                                         { QStringLiteral("nb_tracks"), 7 },
                                         { QStringLiteral("picture_xl"), QStringLiteral("https://cdn-images.dzcdn.net/test.jpg") },
                                         { QStringLiteral("creator"), QJsonObject{ { QStringLiteral("name"), QStringLiteral("Test Owner") } } },
                                         { QStringLiteral("tracks"), QJsonObject{ { QStringLiteral("data"), bare } } } });
                }
            } else if (what == QLatin1String("playlist/900002")) {
                QJsonArray bare;
                for (int i = 0; i < 25; ++i)
                    bare.append(deezerTrack(5000 + i, QStringLiteral("Deep %1").arg(i), QStringLiteral("Deep Artist"), QStringLiteral("Deep Album"), 200, QString()));
                answer.body = json({ { QStringLiteral("title"), QStringLiteral("Deep List") }, { QStringLiteral("nb_tracks"), 250 },
                                     { QStringLiteral("creator"), QJsonObject{ { QStringLiteral("name"), QStringLiteral("Deep Owner") } } },
                                     { QStringLiteral("tracks"), QJsonObject{ { QStringLiteral("data"), bare } } } });
            } else if (what == QLatin1String("playlist/900002/tracks")) {
                const int count = index < 200 ? 100 : 50;
                QJsonArray rows;
                for (int i = index; i < index + count; ++i)
                    rows.append(deezerTrack(5000 + i, QStringLiteral("Deep %1").arg(i), QStringLiteral("Deep Artist"), QStringLiteral("Deep Album"), 200,
                                            QStringLiteral("DEEP%1").arg(i, 8, 10, QLatin1Char('0'))));
                QJsonObject page{ { QStringLiteral("data"), rows }, { QStringLiteral("total"), 250 } };
                if (index + count < 250)
                    page.insert(QStringLiteral("next"), QStringLiteral("https://api.deezer.com/playlist/900002/tracks?index=%1&limit=100").arg(index + count));
                answer.body = json(page);
            } else if (what == QLatin1String("playlist/900003")) {
                answer.body = json({ { QStringLiteral("title"), QStringLiteral("Odd List") }, { QStringLiteral("nb_tracks"), 20 },
                                     { QStringLiteral("tracks"), QJsonObject{ { QStringLiteral("data"), QJsonArray() } } } });
            } else if (what == QLatin1String("playlist/900003/tracks")) {
                QJsonArray rows;
                for (int i = 0; i < 10; ++i)
                    rows.append(deezerTrack(7000 + i, QStringLiteral("Odd %1").arg(i), QStringLiteral("Odd Artist"), QString(), 200, QStringLiteral("ODD%1").arg(i)));
                answer.body = json({ { QStringLiteral("data"), rows }, { QStringLiteral("total"), 20 },
                                     { QStringLiteral("next"), QStringLiteral("https://elsewhere.example.com/playlist/900003/tracks?index=10") } });
            } else {
                answer.body = R"({"error":{"type":"DataException","message":"no data","code":800}})";
            }
            return answer;
        }
        // — Apple Music —
        if (path.startsWith(QLatin1String("/music.apple.com/us/playlist/"))) {
            QJsonArray items;
            for (int i = 0; i < 300; ++i)
                items.append(appleTrack(QString::number(9000 + i), QStringLiteral("Apple Song %1").arg(i), QStringLiteral("Apple Artist"),
                                        QStringLiteral("Apple Album"), 200000, false));
            answer.type = "text/html; charset=utf-8";
            answer.body = applePage(QStringLiteral("Apple Test Hits"), QStringLiteral("Apple Music Test"), items, true, false);
            return answer;
        }
        if (path.startsWith(QLatin1String("/music.apple.com/gb/album/"))) {
            QJsonArray items{ appleTrack(QStringLiteral("1499378115"), QStringLiteral("Alone Again"), QStringLiteral("The Weeknd"), QString(), 250057, true),
                              appleTrack(QStringLiteral("1499378116"), QStringLiteral("Too Late"), QStringLiteral("The Weeknd"), QString(), 239000, true) };
            answer.type = "text/html; charset=utf-8";
            answer.body = applePage(QStringLiteral("After Hours"), QStringLiteral("The Weeknd"), items, false, true);
            return answer;
        }
        answer.status = 404;
        answer.body = "not here";
        return answer;
    };

    // ------------------------------------------------------------ 4. the session the import must never carry
    const QString secretName = QStringLiteral("ytmusic.cookies");
    SecretStore::remove(secretName);
    library->setSetting(QStringLiteral("ytmusic.guard"), QString());
    auto session = std::make_unique<YtmSession>(library);
    {
        YtmSession::Timing timing;
        timing.launchCheckMs = 100;
        timing.recheckMs = 300;
        timing.retryMinMs = 10 * 60 * 1000;
        timing.saveDelayMs = 300;
        timing.periodicMs = 6 * 60 * 60 * 1000;
        timing.guard = unpaced();
        session->setTiming(timing);
    }
    QStringList verdicts;
    QObject::connect(session.get(), &YtmSession::checked, session.get(), [&verdicts](const QString &v) { verdicts << v; });
    session->start();
    t.check(session->importText(QString::fromLatin1(kSessionHeader)),
            QStringLiteral("a YouTube Music session imports (invented cookies, every value marked TEST)"), session->importError());
    waitUntil([&verdicts]() { return !verdicts.isEmpty(); }, 15000);
    t.check(session->state() == QLatin1String("active"), QStringLiteral("  and is confirmed: signed in as Import Test"),
            session->state());
    {
        InnerTube consumer(nullptr, /*warmUp=*/false);
        bool done = false;
        consumer.browse(QStringLiteral("FEtest_withaccount"), QString(),
                        [&done](const QJsonObject &, const QString &) { done = true; }, InnerTube::Auth::IfSignedIn);
        waitUntil([&done]() { return done; }, 10000);
        const StandIn::Request &last = standIn.requests.constLast();
        t.check(done && last.authed() && last.has("cookie") && last.has("x-goog-authuser"),
                QStringLiteral("  a call that asks for the account does carry it, so the session is in force"));
    }
    const int importFrom = int(standIn.requests.size());

    // ------------------------------------------------------------ 5. each source's pages
    PlaylistImport::Timing timing;
    timing.searchGapMs = 150;
    timing.searchJitterMs = 50;
    timing.pageGapMs = 120;
    timing.pageJitterMs = 30;
    timing.restMs = 400;
    timing.pauseMs = 800;
    timing.resumeDelayMs = 400;
    timing.perHour = 100000;
    timing.perDay = 100000;

    auto importer = std::make_unique<PlaylistImport>(library);
    importer->setTiming(timing);
    importer->load();
    QList<bool> busyChanges;
    QList<QPair<int, QString>> finishedJobs;
    const auto follow = [&busyChanges, &finishedJobs](PlaylistImport *imp) {
        QObject::connect(imp, &PlaylistImport::busyChanged, imp, [imp, &busyChanges]() { busyChanges << imp->busy(); });
        QObject::connect(imp, &PlaylistImport::finished, imp, [&finishedJobs](int id, const QString &summary) {
            finishedJobs.append({ id, summary });
        });
    };
    follow(importer.get());
    const auto previewNow = [&importer](const QString &link) {
        importer->preview(link);
        waitUntil([&importer]() {
            const QString state = importer->previewed().value(QStringLiteral("state")).toString();
            return state == QLatin1String("ready") || state == QLatin1String("failed");
        }, 15000);
        return importer->previewed();
    };
    const auto finishedFor = [&finishedJobs](int jobId) {
        for (const auto &pair : finishedJobs) {
            if (pair.first == jobId)
                return pair.second;
        }
        return QString();
    };

    // A CSV file as the import's file picker hands it over: a file: URL, here
    // to a folder whose name has a space in it.
    {
        const QString folder = qEnvironmentVariable("MONOLIST_DATA_DIR") + QStringLiteral("/import test files");
        QDir().mkpath(folder);
        const QString path = folder + QStringLiteral("/Road_Trip.csv");
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                             && file.write("\"Track URI\",\"Track Name\",\"Artist Name(s)\",\"Album Name\",\"Track Duration (ms)\",\"ISRC\"\n"
                                           "\"spotify:track:1\",\"Blinding Lights\",\"The Weeknd\",\"After Hours\",\"200040\",\"USUG11904206\"\n"
                                           "\"spotify:track:2\",\"Yellow\",\"Coldplay\",\"Parachutes\",\"266773\",\"\"\n") > 0;
        file.close();
        importer->previewFile(QUrl::fromLocalFile(path).toString());
        const QVariantMap p = importer->previewed();
        t.check(written && p.value(QStringLiteral("state")).toString() == QLatin1String("ready")
                    && p.value(QStringLiteral("platform")).toString() == QLatin1String("csv")
                    && p.value(QStringLiteral("readable")).toInt() == 2
                    && p.value(QStringLiteral("name")).toString() == QLatin1String("Road Trip"),
                QStringLiteral("a CSV file opened by its file: URL, as the file picker gives it: 2 songs, named after the file"),
                QStringLiteral("%1 %2 %3 \"%4\"").arg(p.value(QStringLiteral("state")).toString(), p.value(QStringLiteral("error")).toString())
                    .arg(p.value(QStringLiteral("readable")).toInt()).arg(p.value(QStringLiteral("name")).toString()));
        importer->clearPreview();
    }

    // Spotify: 100 in the embed, 150 on the public page.
    {
        const int from = int(standIn.requests.size());
        const QVariantMap p = previewNow(QStringLiteral("https://open.spotify.com/playlist/") + spotifyBig + QStringLiteral("?si=x"));
        const QStringList warnings = p.value(QStringLiteral("warnings")).toStringList();
        t.check(p.value(QStringLiteral("state")).toString() == QLatin1String("ready")
                    && p.value(QStringLiteral("name")).toString() == QLatin1String("Big Spotify List")
                    && p.value(QStringLiteral("owner")).toString() == QLatin1String("Test Owner")
                    && p.value(QStringLiteral("artwork")).toString() == QLatin1String("https://i.scdn.co/image/large")
                    && p.value(QStringLiteral("count")).toInt() == 150 && p.value(QStringLiteral("readable")).toInt() == 100,
                QStringLiteral("Spotify: the embed's 100 songs, and the public page's real total of 150"),
                QStringLiteral("%1 %2/%3").arg(p.value(QStringLiteral("error")).toString())
                    .arg(p.value(QStringLiteral("readable")).toInt()).arg(p.value(QStringLiteral("count")).toInt()));
        t.check(warnings.size() == 1 && warnings.first().contains(QLatin1String("first 100 of 150"))
                    && warnings.first().contains(QLatin1String("exportify.app")),
                QStringLiteral("  the warning: only the first 100 of 150 without signing in, and the CSV way round"),
                warnings.join(QLatin1Char('|')));
        int embed = -1;
        int open = -1;
        for (int i = from; i < standIn.requests.size(); ++i) {
            const QByteArray path = standIn.requests.at(i).path;
            if (path.startsWith("/open.spotify.com/embed/"))
                embed = i;
            else if (path.startsWith("/open.spotify.com/playlist/"))
                open = i;
        }
        // Measured where they arrive, which the first request on a fresh
        // network manager can reach a little late (Windows looks for a proxy
        // first): a quarter of the gap is allowed for that, no more.
        const qint64 gap = embed >= 0 && open > embed ? standIn.requests.at(open).atMs - standIn.requests.at(embed).atMs : -1;
        t.check(int(standIn.requests.size()) - from == 2 && embed >= 0 && open > embed
                    && gap >= timing.pageGapMs * 3 / 4,
                QStringLiteral("  two requests for it, the public page a page's gap after the embed"),
                QStringLiteral("%1 request(s), %2 ms apart").arg(int(standIn.requests.size()) - from).arg(gap));
        const QVariantList sample = p.value(QStringLiteral("sample")).toList();
        t.check(sample.size() == 5 && sample.first().toMap().value(QStringLiteral("artist")).toString() == QLatin1String("Artist One, Artist Two"),
                QStringLiteral("  the artists of a row split at Spotify's \", \" (a no-break space and all)"));
        const int asked = int(standIn.requests.size());
        previewNow(QStringLiteral("https://open.spotify.com/playlist/") + spotifyBig);
        t.check(int(standIn.requests.size()) == asked, QStringLiteral("  the same list previewed again asks nothing more"));
        const Import::Page album = Import::Reader::parseSpotifyEmbed(
            Import::parseLink(QStringLiteral("https://open.spotify.com/album/") + spotifyAlbum),
            spotifyEmbed(QStringLiteral("album"), spotifyAlbum, QStringLiteral("Test Album"),
                         { spotifyTrack(3, QStringLiteral("Album Song"), QStringLiteral("Test Band"), 200) }));
        t.check(album.rows.size() == 1 && album.rows.first().album == QLatin1String("Test Album"),
                QStringLiteral("  an album's songs are given its name as their album"));
        const Import::Page bare = Import::Reader::parseSpotifyEmbed(
            Import::parseLink(QStringLiteral("https://open.spotify.com/album/") + spotifyAlbum),
            spotifyEmbed(QStringLiteral("album"), spotifyAlbum, QStringLiteral("Test Album"),
                         { spotifyTrack(3, QStringLiteral("Album Song"), QStringLiteral("Test Band"), 200) }, /*coverArt=*/false));
        t.check(bare.artwork == QLatin1String("https://image-cdn-ak.spotifycdn.com/image/large"),
                QStringLiteral("  an album's embed with no coverArt: its cover from its visualIdentity, the widest"), bare.artwork);
    }
    // Apple Music: the first 300, and the warning.
    {
        const QVariantMap p = previewNow(QStringLiteral("https://music.apple.com/us/playlist/test-hits/pl.0123456789abcdef"));
        const QStringList warnings = p.value(QStringLiteral("warnings")).toStringList();
        t.check(p.value(QStringLiteral("state")).toString() == QLatin1String("ready") && p.value(QStringLiteral("readable")).toInt() == 300
                    && p.value(QStringLiteral("name")).toString() == QLatin1String("Apple Test Hits")
                    && p.value(QStringLiteral("artwork")).toString().endsWith(QLatin1String("600x600bb.jpg"))
                    && warnings.size() == 1 && warnings.first().contains(QLatin1String("first 300")),
                QStringLiteral("Apple Music: 300 songs from serialized-server-data, and \"first 300\" where there is more"),
                warnings.join(QLatin1Char('|')) + QLatin1Char(' ') + p.value(QStringLiteral("error")).toString());
        const QVariantMap one = previewNow(QStringLiteral("https://music.apple.com/gb/album/after-hours/1499378108?i=1499378116"));
        const QVariantList sample = one.value(QStringLiteral("sample")).toList();
        t.check(one.value(QStringLiteral("readable")).toInt() == 1 && sample.size() == 1
                    && sample.first().toMap().value(QStringLiteral("title")).toString() == QLatin1String("Too Late")
                    && sample.first().toMap().value(QStringLiteral("artist")).toString() == QLatin1String("The Weeknd"),
                QStringLiteral("  one song of an album (?i=): that song alone, its artist from the row's own field"));
        const QVariantMap whole = previewNow(QStringLiteral("https://music.apple.com/gb/album/after-hours/1499378108"));
        t.check(whole.value(QStringLiteral("count")).toInt() == 2 && whole.value(QStringLiteral("readable")).toInt() == 2,
                QStringLiteral("  an album: its 2 songs counted from \"2 songs, 56 minutes\", not its date's or its ℗ line's year"),
                QStringLiteral("%1 of %2").arg(whole.value(QStringLiteral("readable")).toInt())
                    .arg(whole.value(QStringLiteral("count")).toInt()));
    }
    // JioSaavn: pages until one comes back empty.
    {
        InnerTube tube(nullptr, /*warmUp=*/false);
        tube.setCookieless(true);
        Import::Reader reader(&tube);
        reader.setGap(timing.pageGapMs, timing.pageJitterMs);
        const Import::Link link = Import::parseLink(QStringLiteral("https://www.jiosaavn.com/featured/big-test/IMPSAAVNBIG"));
        QList<Import::Page> pages;
        QString cursor;
        bool more = true;
        for (int i = 0; i < 5 && more; ++i) {
            bool done = false;
            reader.next(link, cursor, [&](const Import::Page &page) {
                pages.append(page);
                cursor = page.cursor;
                more = !page.cursor.isEmpty() && page.error.isEmpty();
                done = true;
            });
            waitUntil([&done]() { return done; }, 10000);
        }
        int rows = 0;
        for (const Import::Page &page : std::as_const(pages))
            rows += int(page.rows.size());
        t.check(pages.size() == 3 && rows == 137 && pages.value(2).rows.isEmpty() && pages.value(2).error.isEmpty()
                    && pages.value(0).name == QLatin1String("Saavn & Test") && pages.value(0).owner == QLatin1String("Test")
                    && pages.value(0).artwork.endsWith(QLatin1String("test-500x500.jpg")),
                QStringLiteral("JioSaavn: 100, then 37, then an empty page ends it (137 songs); entities and the cover's size undone"),
                QStringLiteral("%1 pages, %2 songs").arg(pages.size()).arg(rows));
        const Import::Row first = pages.value(0).rows.value(0);
        t.check(first.title == QLatin1String("Big Saavn 0") && first.artists == QStringList{ QStringLiteral("Singer A") }
                    && first.album == QLatin1String("Saavn Album") && first.durationMs == 200000 && first.sourceId == QLatin1String("SB0"),
                QStringLiteral("  a row: title, artists, album, length in seconds, JioSaavn's id"));
        // Deezer: the header, then its pages by `next`.
        const Import::Link deezer = Import::parseLink(QStringLiteral("https://www.deezer.com/en/playlist/900002"));
        QList<Import::Page> deep;
        cursor.clear();
        more = true;
        for (int i = 0; i < 6 && more; ++i) {
            bool done = false;
            reader.next(deezer, cursor, [&](const Import::Page &page) {
                deep.append(page);
                cursor = page.cursor;
                more = !page.cursor.isEmpty() && page.error.isEmpty();
                done = true;
            });
            waitUntil([&done]() { return done; }, 10000);
        }
        int deepRows = 0;
        bool everyIsrc = true;
        for (const Import::Page &page : std::as_const(deep)) {
            deepRows += int(page.rows.size());
            for (const Import::Row &row : page.rows)
                everyIsrc = everyIsrc && !row.isrc.isEmpty();
        }
        t.check(deep.size() == 4 && deep.first().rows.isEmpty() && deep.first().total == 250 && deepRows == 250 && everyIsrc,
                QStringLiteral("Deezer: the header (its songs lack ISRCs, so they are read from the pages), then 100, 100 and 50 "
                               "by its next links, each with its ISRC"),
                QStringLiteral("%1 pages, %2 songs").arg(deep.size()).arg(deepRows));
        const Import::Link odd = Import::parseLink(QStringLiteral("https://www.deezer.com/playlist/900003"));
        Import::Page oddHeader;
        Import::Page oddPage;
        bool done = false;
        reader.next(odd, QString(), [&](const Import::Page &page) { oddHeader = page; done = true; });
        waitUntil([&done]() { return done; }, 10000);
        done = false;
        reader.next(odd, oddHeader.cursor, [&](const Import::Page &page) { oddPage = page; done = true; });
        waitUntil([&done]() { return done; }, 10000);
        const int asked = int(standIn.requests.size());
        t.check(oddPage.rows.size() == 10 && oddPage.cursor.isEmpty(),
                QStringLiteral("  a next link to another host is not followed"));
        Import::Page refused;
        done = false;
        reader.next(odd, QStringLiteral("https://elsewhere.example.com/playlist/900003/tracks?index=10"),
                    [&](const Import::Page &page) { refused = page; done = true; });
        waitUntil([&done]() { return done; }, 10000);
        t.check(!refused.error.isEmpty() && int(standIn.requests.size()) == asked,
                QStringLiteral("  nor asked when handed to it as a cursor"));
        const QVariantMap missing = previewNow(QStringLiteral("https://www.deezer.com/playlist/900404"));
        t.check(missing.value(QStringLiteral("state")).toString() == QLatin1String("failed")
                    && missing.value(QStringLiteral("error")).toString().contains(QLatin1String("doesn't exist")),
                QStringLiteral("  Deezer's \"no data\": the list doesn't exist, said plainly"),
                missing.value(QStringLiteral("error")).toString());
        {
            // A playlist's header as YouTube Music writes it for a list of
            // someone's: its views first, its owner in a row of avatars.
            QJsonObject root = QJsonDocument::fromJson(playlistPage(QStringLiteral("Views First"), playlistRows(0, 3), QString(),
                                                                    QStringLiteral("2.8K views • 305 tracks • 20+ hours"))).object();
            QJsonObject contents = root.value(QStringLiteral("contents")).toObject();
            QJsonObject two = contents.value(QStringLiteral("twoColumnBrowseResultsRenderer")).toObject();
            QJsonArray tabs = two.value(QStringLiteral("tabs")).toArray();
            QJsonObject tab = tabs.at(0).toObject();
            QJsonObject renderer = tab.value(QStringLiteral("tabRenderer")).toObject();
            QJsonObject content = renderer.value(QStringLiteral("content")).toObject();
            QJsonObject list = content.value(QStringLiteral("sectionListRenderer")).toObject();
            QJsonArray sections = list.value(QStringLiteral("contents")).toArray();
            QJsonObject header = sections.at(0).toObject().value(QStringLiteral("musicResponsiveHeaderRenderer")).toObject();
            header.remove(QStringLiteral("straplineTextOne"));
            header.insert(QStringLiteral("facepile"), QJsonObject{ { QStringLiteral("avatarStackViewModel"), QJsonObject{
                { QStringLiteral("text"), QJsonObject{ { QStringLiteral("content"), QStringLiteral("Someone's Channel") } } } } } });
            sections[0] = QJsonObject{ { QStringLiteral("musicResponsiveHeaderRenderer"), header } };
            list.insert(QStringLiteral("contents"), sections);
            content.insert(QStringLiteral("sectionListRenderer"), list);
            renderer.insert(QStringLiteral("content"), content);
            tab.insert(QStringLiteral("tabRenderer"), renderer);
            tabs[0] = tab;
            two.insert(QStringLiteral("tabs"), tabs);
            contents.insert(QStringLiteral("twoColumnBrowseResultsRenderer"), two);
            root.insert(QStringLiteral("contents"), contents);
            const Import::Page page = Import::Reader::parseYouTube(QStringLiteral("VLPLVIEWS"), root);
            t.check(page.total == 305 && page.owner == QLatin1String("Someone's Channel") && page.rows.size() == 3,
                    QStringLiteral("YouTube: \"2.8K views • 305 tracks\" is 305 songs, not 28; a playlist's owner from its avatars"),
                    QStringLiteral("%1, \"%2\"").arg(page.total).arg(page.owner));
        }
        const QVariantMap hidden = previewNow(QStringLiteral("https://music.youtube.com/playlist?list=PLIMPORTPRIVATE"));
        t.check(hidden.value(QStringLiteral("state")).toString() == QLatin1String("failed")
                    && hidden.value(QStringLiteral("error")).toString().contains(QLatin1String("Public or Unlisted")),
                QStringLiteral("YouTube: a list that answers with nothing is private or gone: make it Public or Unlisted"),
                hidden.value(QStringLiteral("error")).toString());
    }

    // ------------------------------------------------------------ 6. a YouTube list: browse and its continuation
    int jobY = 0;
    {
        const int from = int(standIn.requests.size());
        const QVariantMap p = previewNow(QStringLiteral("https://music.youtube.com/playlist?list=PLIMPORTTEST"));
        t.check(p.value(QStringLiteral("state")).toString() == QLatin1String("ready") && p.value(QStringLiteral("count")).toInt() == 130
                    && p.value(QStringLiteral("name")).toString() == QLatin1String("YouTube Test List")
                    && p.value(QStringLiteral("owner")).toString() == QLatin1String("Test Curator"),
                QStringLiteral("YouTube Music: the list's first page, its name, curator and \"130 songs\""),
                p.value(QStringLiteral("error")).toString());
        jobY = importer->start(QString());
        // The first page's songs are in as the import starts, before the
        // continuation is read: each found, with nothing to say about it
        // (an empty reason, which the NOT NULL column takes; a null one
        // would leave them pending, and a stop would call them not found).
        const int firstFound = countRows(jobY, QStringLiteral("status = 'found' AND reason = ''"));
        const int firstPending = countRows(jobY, QStringLiteral("status = 'pending'"));
        const QVariantMap firstJob = importer->job();
        t.check(firstFound == 100 && firstPending == 0 && firstJob.value(QStringLiteral("found")).toInt() == 100
                    && firstJob.value(QStringLiteral("state")).toString() == QLatin1String("reading"),
                QStringLiteral("  the first page's 100 found at once, while the rest is still to read"),
                QStringLiteral("%1 found, %2 pending, %3").arg(firstFound).arg(firstPending)
                    .arg(firstJob.value(QStringLiteral("state")).toString()));
        waitUntil([&]() { return !finishedFor(jobY).isEmpty(); }, 20000);
        const QStringList ids = playlistVideoIds(importer->job().value(QStringLiteral("playlistId")).toInt());
        bool inOrder = ids.size() == 130;
        for (int i = 0; inOrder && i < 130; ++i)
            inOrder = ids.at(i) == ytId(i);
        t.check(inOrder && statuses(jobY).count(QStringLiteral("found")) == 130
                    && countRows(jobY, QStringLiteral("status = 'found' AND reason = ''")) == 130,
                QStringLiteral("  all 130 in the new playlist, in the list's order, the second page by its continuation"),
                QString::number(ids.size()));
        int searchesAsked = 0;
        bool visitorNamed = false;
        for (int i = from; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            searchesAsked += request.path.startsWith("/youtubei/v1/search") ? 1 : 0;
            if (request.json().value(QStringLiteral("continuation")).toString() == QLatin1String("IMPTOK2"))
                visitorNamed = request.header("x-goog-visitor-id") == "IMPVISITOR01";
        }
        t.check(searchesAsked == 0 && visitorNamed,
                QStringLiteral("  nothing searched for (the songs are the list's own), the rest asked as the first page's visitor"));
        t.check(finishedFor(jobY) == QStringLiteral("Imported ‘YouTube Test List’: all 130 songs"),
                QStringLiteral("  and the toast says so"), finishedFor(jobY));
    }

    // ------------------------------------------------------------ 7. the matching, end to end (a Deezer playlist)
    // A song already in the library: liked, with its own length.
    {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("INSERT INTO tracks (title, artist, duration_ms, source_id, favourite) VALUES (?, ?, ?, ?, 1)"));
        q.addBindValue(QStringLiteral("In Library"));
        q.addBindValue(QStringLiteral("Test Band"));
        q.addBindValue(241000);
        q.addBindValue(QStringLiteral("LIBVID00001"));
        q.exec();
    }
    int jobA = 0;
    int playlistA = 0;
    {
        busyChanges.clear();
        const QVariantMap p = previewNow(QStringLiteral("https://www.deezer.com/en/playlist/900001"));
        t.check(p.value(QStringLiteral("state")).toString() == QLatin1String("ready") && p.value(QStringLiteral("count")).toInt() == 7,
                QStringLiteral("Deezer: the playlist previewed, 7 songs"), p.value(QStringLiteral("error")).toString());
        const int from = int(standIn.requests.size());
        jobA = importer->start(QStringLiteral("Deezer test"));
        playlistA = importer->job().value(QStringLiteral("playlistId")).toInt();
        t.check(jobA > 0 && playlistA > 0 && library->playlistName(playlistA) == QLatin1String("Deezer test")
                    && importer->busy(),
                QStringLiteral("  Import: the playlist is made at once, under the name given, and the import is busy"));
        waitUntil([&]() { return !finishedFor(jobA).isEmpty(); }, 30000);
        const QStringList s = statuses(jobA);
        t.check(s == QStringList({ QStringLiteral("found"), QStringLiteral("found"), QStringLiteral("found"), QStringLiteral("notfound"),
                                   QStringLiteral("check"), QStringLiteral("found"), QStringLiteral("found") }),
                QStringLiteral("  7 songs: 5 found, 1 not found, 1 to check"), s.join(QLatin1Char(' ')));
        const QStringList ids = playlistVideoIds(playlistA);
        t.check(ids == QStringList({ QStringLiteral("BLVID000001"), QStringLiteral("AMREAL00001"), QStringLiteral("YELSTUDIO01"),
                                     QStringLiteral("SBOMV000001"), QStringLiteral("LIBVID00001") }),
                QStringLiteral("  the playlist holds them in the source's order"), ids.join(QLatin1Char(' ')));
        t.check(searchesFor(standIn.requests, QStringLiteral("USUG11904206"), from) == 1
                    && searchesFor(standIn.requests, QStringLiteral("Blinding Lights The Weeknd"), from) == 0,
                QStringLiteral("  the ISRC first: its first result taken, nothing more asked for that song"));
        t.check(rowField(jobA, 1, "video_id") == QLatin1String("AMREAL00001"),
                QStringLiteral("  \"You Needed Me (Originally By Anne Murray)\" refused; Anne Murray's own taken"));
        t.check(searchesFor(standIn.requests, QStringLiteral("Yellow Parachutes"), from) == 1
                    && rowField(jobA, 2, "video_id") == QLatin1String("YELSTUDIO01"),
                QStringLiteral("  \"Yellow (Live)\" refused; the second search, by album, found the studio song"));
        const QString notFound = rowField(jobA, 3, "reason");
        t.check(searchesFor(standIn.requests, QStringLiteral("Song Twenty Test Band"), from) == 2 && notFound.contains(QLatin1String("20 s"))
                    && notFound.contains(QLatin1String("Song Twenty")),
                QStringLiteral("  20 s apart: not found, after the songs and the videos; the row says why"), notFound);
        const QVariantMap check = QJsonDocument::fromJson(rowField(jobA, 4, "candidate").toUtf8()).object().toVariantMap();
        t.check(check.value(QStringLiteral("videoId")).toString() == QLatin1String("VOVID000001")
                    && rowField(jobA, 4, "video_id").isEmpty() && rowField(jobA, 4, "reason").contains(QLatin1String("no length")),
                QStringLiteral("  a video with no length: to check, its candidate kept, nothing added"), rowField(jobA, 4, "reason"));
        t.check(rowField(jobA, 5, "video_id") == QLatin1String("SBOMV000001")
                    && searchesFor(standIn.requests, QStringLiteral("XX0000000001"), from) == 1,
                QStringLiteral("  an ISRC whose first result is another song: its second never taken, the plain search's official video was"));
        t.check(searchesFor(standIn.requests, QStringLiteral("In Library Test Band"), from) == 0
                    && rowField(jobA, 6, "reason") == QLatin1String("In your library already"),
                QStringLiteral("  a song the library holds (within 3 s) is not searched for"));
        t.check(finishedFor(jobA) == QStringLiteral("Imported ‘Deezer test’: 5 of 7 — 1 to check, 1 not found"),
                QStringLiteral("  the toast: \"Imported 'Deezer test': 5 of 7 — 1 to check, 1 not found\""), finishedFor(jobA));
        t.check(!busyChanges.isEmpty() && busyChanges.constLast() == false && !importer->busy(),
                QStringLiteral("  and the import is no longer busy (the recommender's covers go on)"));
        const QVariantMap job = importer->job();
        t.check(job.value(QStringLiteral("state")).toString() == QLatin1String("done") && job.value(QStringLiteral("found")).toInt() == 5
                    && job.value(QStringLiteral("check")).toInt() == 1 && job.value(QStringLiteral("notFound")).toInt() == 1
                    && importer->rows()->count() == 7,
                QStringLiteral("  Importer.job and Importer.rows show it"));
        // The gap between searches, over every search asked so far.
        qint64 smallest = -1;
        qint64 last = -1;
        for (int i = importFrom; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            if (!request.path.startsWith("/youtubei/v1/search"))
                continue;
            if (last >= 0 && (smallest < 0 || request.atMs - last < smallest))
                smallest = request.atMs - last;
            last = request.atMs;
        }
        t.check(smallest >= timing.searchGapMs, QStringLiteral("  one search at a time, never less than the gap apart (%1 ms here)")
                                                    .arg(timing.searchGapMs),
                QStringLiteral("smallest %1 ms").arg(smallest));
    }

    // ------------------------------------------------------------ 8. the review
    int jobC = 0;
    {
        const QVariantMap review = importer->review(playlistA);
        t.check(review.value(QStringLiteral("jobId")).toInt() == jobA && review.value(QStringLiteral("check")).toList().size() == 1
                    && review.value(QStringLiteral("notFound")).toList().size() == 1,
                QStringLiteral("the review on the playlist's page: 1 to check, 1 not found"));
        const int checkRow = rowIdAt(jobA, 4);
        t.check(importer->searchTextFor(checkRow) == QLatin1String("Video Only Test Band"),
                QStringLiteral("  Search prefilled with the title and the artist"));
        importer->notThis(checkRow);
        t.check(rowField(jobA, 4, "status") == QLatin1String("notfound")
                    && importer->review(playlistA).value(QStringLiteral("notFound")).toList().size() == 2,
                QStringLiteral("  Not this: it goes to Not found"));
        // Blinding Lights out of the library again, to show it is matched before rather than found there.
        {
            QSqlQuery q(AppDatabase::connection());
            q.prepare(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'BLVID000001'"));
            q.addBindValue(playlistA);
            if (q.exec() && q.next())
                library->removeFromPlaylist(playlistA, q.value(0).toInt());
        }
        importer->previewList(QStringLiteral("The Weeknd - Blinding Lights (3:20)\nTest Band - Video Only (3:30)\n"));
        t.check(importer->previewed().value(QStringLiteral("readable")).toInt() == 2,
                QStringLiteral("a pasted list previewed: 2 songs"));
        const int from = int(standIn.requests.size());
        jobC = importer->start(QStringLiteral("Pasted test"));
        waitUntil([&]() { return !finishedFor(jobC).isEmpty(); }, 20000);
        t.check(rowField(jobC, 0, "status") == QLatin1String("found") && rowField(jobC, 0, "reason") == QLatin1String("Matched before")
                    && rowField(jobC, 0, "video_id") == QLatin1String("BLVID000001")
                    && searchesFor(standIn.requests, QStringLiteral("Blinding Lights The Weeknd"), from) == 0,
                QStringLiteral("  a song matched before by another import: taken again, not searched for"),
                rowField(jobC, 0, "reason"));
        t.check(searchesFor(standIn.requests, QStringLiteral("Video Only Test Band"), from) == 2
                    && rowField(jobC, 1, "status") == QLatin1String("notfound") && rowField(jobC, 1, "candidate").isEmpty(),
                QStringLiteral("  the video said Not this to is never offered for that song again"),
                rowField(jobC, 1, "status") + QLatin1Char(' ') + rowField(jobC, 1, "candidate"));
        QVariantMap chosen{ { QStringLiteral("sourceId"), QStringLiteral("VOMANUAL001") }, { QStringLiteral("title"), QStringLiteral("Video Only") },
                            { QStringLiteral("artist"), QStringLiteral("Test Band") }, { QStringLiteral("durationMs"), 210000 } };
        t.check(importer->useSong(checkRow, chosen), QStringLiteral("  a song found in Search used for it"));
        const QStringList ids = playlistVideoIds(playlistA);
        t.check(ids == QStringList({ QStringLiteral("AMREAL00001"), QStringLiteral("YELSTUDIO01"), QStringLiteral("VOMANUAL001"),
                                     QStringLiteral("SBOMV000001"), QStringLiteral("LIBVID00001") }),
                QStringLiteral("  and put at its place in the source's order, not at the end"), ids.join(QLatin1Char(' ')));
        const int dismissRow = rowIdAt(jobA, 3);
        importer->dismiss(dismissRow);
        t.check(importer->review(playlistA).value(QStringLiteral("notFound")).toList().isEmpty()
                    && rowField(jobA, 3, "status") == QLatin1String("dismissed"),
                QStringLiteral("  Dismiss takes a song out of the review"));
    }
    // A match made before is not used for another version of the song: the
    // studio "Yellow" was taken for job A; "Yellow (Live)" is searched for.
    {
        importer->previewList(QStringLiteral("Coldplay - Yellow (Live) (4:26)\n"));
        const int from = int(standIn.requests.size());
        const int jobL = importer->start(QStringLiteral("Live test"));
        waitUntil([&]() { return !finishedFor(jobL).isEmpty(); }, 20000);
        t.check(searchesFor(standIn.requests, QStringLiteral("Yellow Coldplay"), from) >= 1
                    && rowField(jobL, 0, "video_id") != QLatin1String("YELSTUDIO01")
                    && rowField(jobL, 0, "reason") != QLatin1String("Matched before"),
                QStringLiteral("\"Yellow (Live)\" after \"Yellow\" was matched: searched for, the studio song not reused"),
                rowField(jobL, 0, "status") + QLatin1Char(' ') + rowField(jobL, 0, "video_id") + QLatin1Char(' ')
                    + rowField(jobL, 0, "reason"));
    }
    // An ISRC search whose first result the user said Not this to finds
    // nothing: the junk after it is not judged in its place, where no artist
    // is asked for.
    {
        songs.insert(QStringLiteral("TESTISRC0001"),
                     { song(QStringLiteral("ISRCFIRST01"), QStringLiteral("Isrc Song"), QStringLiteral("Test Band"), QString(), 0),
                       song(QStringLiteral("SYNTHKID001"), QStringLiteral("Isrc Song"), QStringLiteral("Synthwave Kid"), QString(), 181) });
        const QString path = qEnvironmentVariable("MONOLIST_DATA_DIR") + QStringLiteral("/import test files/isrc.csv");
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                             && file.write("Track Name,Artist Name(s),Track Duration (ms),ISRC\n"
                                           "Isrc Song,Test Band,180000,TESTISRC0001\n") > 0;
        file.close();
        importer->previewFile(QUrl::fromLocalFile(path).toString());
        const int jobI = importer->start(QStringLiteral("ISRC once"));
        waitUntil([&]() { return !finishedFor(jobI).isEmpty(); }, 20000);
        const bool toCheck = rowField(jobI, 0, "status") == QLatin1String("check")
                             && rowField(jobI, 0, "candidate").contains(QLatin1String("ISRCFIRST01"));
        importer->notThis(rowIdAt(jobI, 0));
        importer->previewFile(QUrl::fromLocalFile(path).toString());
        const int from = int(standIn.requests.size());
        const int jobJ = importer->start(QStringLiteral("ISRC again"));
        waitUntil([&]() { return !finishedFor(jobJ).isEmpty(); }, 20000);
        t.check(written && toCheck && searchesFor(standIn.requests, QStringLiteral("TESTISRC0001"), from) == 1
                    && rowField(jobJ, 0, "status") == QLatin1String("notfound") && rowField(jobJ, 0, "video_id").isEmpty()
                    && !rowField(jobJ, 0, "candidate").contains(QLatin1String("SYNTHKID001")),
                QStringLiteral("an ISRC's first result said Not this to: the search found nothing; the one after it, by "
                               "another artist, is not taken"),
                QStringLiteral("%1 %2 %3").arg(rowField(jobJ, 0, "status"), rowField(jobJ, 0, "video_id"),
                                               rowField(jobJ, 0, "candidate").left(80)));
    }

    // ------------------------------------------------------------ 9. a refusal, rested on; a second, paused on
    {
        QStringList states;
        QObject::connect(importer.get(), &PlaylistImport::jobChanged, importer.get(), [&states, &importer]() {
            const QString state = importer->job().value(QStringLiteral("state")).toString();
            if (states.isEmpty() || states.constLast() != state)
                states << state;
        });
        QString restNote;
        QString pauseNote;
        QObject::connect(importer.get(), &PlaylistImport::jobChanged, importer.get(), [&]() {
            const QVariantMap job = importer->job();
            if (job.value(QStringLiteral("state")).toString() == QLatin1String("resting"))
                restNote = job.value(QStringLiteral("note")).toString();
            if (job.value(QStringLiteral("state")).toString() == QLatin1String("paused"))
                pauseNote = job.value(QStringLiteral("note")).toString();
        });
        previewNow(QStringLiteral("https://open.spotify.com/playlist/") + spotifyRest);
        const int from = int(standIn.requests.size());
        const int jobB = importer->start(QString());
        waitUntil([&]() { return !finishedFor(jobB).isEmpty(); }, 20000);
        QObject::disconnect(importer.get(), &PlaylistImport::jobChanged, nullptr, nullptr);
        QList<qint64> at;
        for (int i = from; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            if (request.path.startsWith("/youtubei/v1/search")
                && request.json().value(QStringLiteral("query")).toString() == QLatin1String("Rest Song Test Band"))
                at << request.atMs;
        }
        t.check(at.size() == 3, QStringLiteral("Spotify list: the first search refused twice (429), then answered: asked 3 times"),
                QString::number(at.size()));
        t.check(states.contains(QStringLiteral("resting")) && restNote.contains(QLatin1String("rests for"))
                    && restNote.startsWith(QLatin1String("YouTube Music asked Monolist to slow down"))
                    && !restNote.contains(QLatin1String("http")),
                QStringLiteral("  the first refusal: a rest, saying plainly why and when it tries again"), restNote);
        t.check(at.size() == 3 && at.at(1) - at.at(0) >= timing.restMs && at.at(2) - at.at(1) >= timing.pauseMs,
                QStringLiteral("  asked again only after the rest, and after the pause"),
                at.size() == 3 ? QStringLiteral("%1 ms, %2 ms").arg(at.at(1) - at.at(0)).arg(at.at(2) - at.at(1)) : QString());
        t.check(states.contains(QStringLiteral("paused")) && pauseNote.contains(QLatin1String("goes on by itself at")),
                QStringLiteral("  the second in a row: paused, saying when it goes on by itself"), pauseNote);
        t.check(statuses(jobB) == QStringList({ QStringLiteral("found"), QStringLiteral("found") })
                    && states.constLast() == QLatin1String("done"),
                QStringLiteral("  and then it went on, and finished"), states.join(QLatin1Char(' ')));
        t.check(library->settingValue(QStringLiteral("import.youtube_rest")).isEmpty(),
                QStringLiteral("  YouTube Music answering again forgets its refusals"),
                library->settingValue(QStringLiteral("import.youtube_rest")));
    }

    // ------------------------------------------------------------ 9b. a refusal rests every import, stopped or new
    // The rest is the computer's, not one job's: stopping the import that was
    // refused, and starting another, neither cuts it short nor starts the
    // count afresh. The next refusal, in another import, is the second.
    {
        refuse.insert(QStringLiteral("Held One Test Band"), 1);
        refuse.insert(QStringLiteral("Held Two Test Band"), 1);
        songs.insert(QStringLiteral("Held Two Test Band"), { song(QStringLiteral("HELDTWO0001"), QStringLiteral("Held Two"),
                                                                  QStringLiteral("Test Band"), QString(), 180) });
        importer->previewList(QStringLiteral("Test Band - Held One (3:00)\n"));
        const int from = int(standIn.requests.size());
        const int jobX = importer->start(QStringLiteral("Held X"));
        waitUntil([&]() { return jobState(jobX) == QLatin1String("resting"); }, 10000);
        const QString kept = library->settingValue(QStringLiteral("import.youtube_rest"));
        importer->cancel(jobX);
        pause(timing.restMs / 4);
        const bool restBusy = importer->busy();
        int askedDuringRest = 0;
        for (int i = from; i < standIn.requests.size(); ++i)
            askedDuringRest += standIn.requests.at(i).path.startsWith("/youtubei/v1/search") ? 1 : 0;
        t.check(jobState(jobX) == QLatin1String("cancelled") && kept.section(QLatin1Char(' '), 1, 1) == QLatin1String("1")
                    && restBusy && askedDuringRest == 1,
                QStringLiteral("a refused import stopped: the rest is kept in settings, nothing more is asked, and the import "
                               "stays busy (the recommender's covers wait too)"),
                QStringLiteral("\"%1\", busy %2, %3 search(es)").arg(kept).arg(restBusy ? 1 : 0).arg(askedDuringRest));

        QStringList states;
        QString heldNote;
        QString pauseNote;
        QObject::connect(importer.get(), &PlaylistImport::jobChanged, importer.get(), [&]() {
            const QVariantMap job = importer->job();
            const QString state = job.value(QStringLiteral("state")).toString();
            if (states.isEmpty() || states.constLast() != state)
                states << state;
            if (state == QLatin1String("resting"))
                heldNote = job.value(QStringLiteral("note")).toString();
            if (state == QLatin1String("paused"))
                pauseNote = job.value(QStringLiteral("note")).toString();
        });
        importer->previewList(QStringLiteral("Test Band - Held Two (3:00)\n"));
        const int jobY2 = importer->start(QStringLiteral("Held Y"));
        waitUntil([&]() { return !finishedFor(jobY2).isEmpty(); }, 20000);
        QObject::disconnect(importer.get(), &PlaylistImport::jobChanged, nullptr, nullptr);
        QList<qint64> one;
        QList<qint64> two;
        for (int i = from; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            if (!request.path.startsWith("/youtubei/v1/search"))
                continue;
            const QString query = request.json().value(QStringLiteral("query")).toString();
            if (query == QLatin1String("Held One Test Band"))
                one << request.atMs;
            else if (query == QLatin1String("Held Two Test Band"))
                two << request.atMs;
        }
        t.check(one.size() == 1 && two.size() == 2 && two.at(0) - one.at(0) >= timing.restMs,
                QStringLiteral("  the import started next waits out the rest before it asks anything"),
                one.size() == 1 && !two.isEmpty() ? QStringLiteral("%1 ms after the refusal").arg(two.at(0) - one.at(0))
                                                  : QStringLiteral("%1 and %2 search(es)").arg(one.size()).arg(two.size()));
        t.check(states.contains(QStringLiteral("resting")) && heldNote.contains(QLatin1String("every import rests until")),
                QStringLiteral("  and says why: every import rests until then"), heldNote);
        t.check(two.size() == 2 && two.at(1) - two.at(0) >= timing.pauseMs && states.contains(QStringLiteral("paused"))
                    && pauseNote.contains(QLatin1String("goes on by itself at")),
                QStringLiteral("  its own first refusal is the second in a row: paused, not rested, saying when it goes on"),
                pauseNote);
        t.check(statuses(jobY2) == QStringList{ QStringLiteral("found") }
                    && library->settingValue(QStringLiteral("import.youtube_rest")).isEmpty(),
                QStringLiteral("  then it is answered, the song found, and the refusals forgotten"),
                statuses(jobY2).join(QLatin1Char(' ')));
        waitUntil([&]() { return !importer->busy(); }, 5000);
        t.check(!importer->busy(), QStringLiteral("  and the import is idle again"));
    }

    // ------------------------------------------------------------ 10. JioSaavn's pages in a job, ending empty
    {
        previewNow(QStringLiteral("https://www.jiosaavn.com/featured/job-test/IMPSAAVNJOB"));
        const int from = int(standIn.requests.size());
        const int jobS = importer->start(QString());
        waitUntil([&]() { return !finishedFor(jobS).isEmpty(); }, 20000);
        QList<int> pages;
        QList<qint64> pageAt;
        for (int i = from; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            if (request.path.startsWith("/www.jiosaavn.com/")) {
                pages << QUrlQuery(QUrl(standIn.base() + QString::fromUtf8(request.path))).queryItemValue(QStringLiteral("p")).toInt();
                pageAt << request.atMs;
            }
        }
        t.check(pages == QList<int>({ 2, 3 }) && statuses(jobS).count(QStringLiteral("found")) == 5,
                QStringLiteral("JioSaavn list: page 1 from the preview, then 2, then 3 (empty) ends it; 5 songs found"),
                QStringLiteral("pages asked %1").arg([&pages]() { QStringList l; for (int p : pages) l << QString::number(p); return l.join(QLatin1Char(',')); }()));
        t.check(pageAt.size() == 2 && pageAt.at(1) - pageAt.at(0) >= timing.pageGapMs,
                QStringLiteral("  its pages a page's gap apart"),
                pageAt.size() == 2 ? QStringLiteral("%1 ms apart").arg(pageAt.at(1) - pageAt.at(0))
                                   : QStringLiteral("%1 page(s)").arg(pageAt.size()));
        const QStringList ids = playlistVideoIds(importer->job().value(QStringLiteral("playlistId")).toInt());
        t.check(ids == QStringList({ QStringLiteral("SAAVNVID000"), QStringLiteral("SAAVNVID001"), QStringLiteral("SAAVNVID002"),
                                     QStringLiteral("SAAVNVID003"), QStringLiteral("SAAVNVID004") }),
                QStringLiteral("  in order"), ids.join(QLatin1Char(' ')));
    }

    // ------------------------------------------------------------ 11. the hour's cap
    {
        const QStringList budget = library->settingValue(QStringLiteral("import.search_budget")).split(QLatin1Char(' '));
        t.check(budget.size() == 4 && budget.at(1).toInt() > 0, QStringLiteral("the searches of the hour and the day are kept in settings"),
                budget.join(QLatin1Char(' ')));
        PlaylistImport::Timing capped = timing;
        capped.perHour = budget.value(1).toInt() + 1;
        importer->setTiming(capped);
        importer->previewList(QStringLiteral("Test Band - Cap One (3:20)\nTest Band - Cap Two (3:21)\n"));
        const int jobW = importer->start(QString());
        waitUntil([&]() { return jobState(jobW) == QLatin1String("waiting"); }, 10000);
        const QVariantMap job = importer->job();
        t.check(jobState(jobW) == QLatin1String("waiting") && statuses(jobW).value(0) == QLatin1String("found")
                    && statuses(jobW).value(1) == QLatin1String("pending")
                    && job.value(QStringLiteral("note")).toString().contains(QLatin1String("times an hour"))
                    && job.value(QStringLiteral("resumeAt")).toLongLong() > QDateTime::currentMSecsSinceEpoch() && importer->busy(),
                QStringLiteral("  at the hour's cap the import waits, saying until when, and is still busy"),
                job.value(QStringLiteral("note")).toString());
        importer->setTiming(timing);
        importer->resume(jobW);
        waitUntil([&]() { return !finishedFor(jobW).isEmpty(); }, 10000);
        t.check(statuses(jobW) == QStringList({ QStringLiteral("found"), QStringLiteral("found") }),
                QStringLiteral("  and goes on when it may"));
    }

    // ------------------------------------------------------------ 12. a restart: a second Importer on the same library
    {
        QString text;
        for (int i = 0; i < resumeTitles.size(); ++i)
            text += QStringLiteral("Test Band - %1 (3:%2)\n").arg(resumeTitles.at(i)).arg(1 + i, 2, 10, QLatin1Char('0'));
        importer->previewList(text);
        const int from = int(standIn.requests.size());
        const int jobR = importer->start(QStringLiteral("Restart test"));
        const int playlistR = importer->job().value(QStringLiteral("playlistId")).toInt();
        waitUntil([&]() { return statuses(jobR).count(QStringLiteral("found")) >= 2; }, 10000);
        importer.reset();   // the app closes, mid-import
        const QStringList before = statuses(jobR);
        t.check(jobState(jobR) == QLatin1String("running") && before.count(QStringLiteral("found")) >= 2
                    && before.count(QStringLiteral("pending")) >= 3,
                QStringLiteral("a pasted list, the app closed after 2 songs: the job is kept, still to go on"),
                before.join(QLatin1Char(' ')));
        importer = std::make_unique<PlaylistImport>(library);
        importer->setTiming(timing);
        follow(importer.get());
        importer->load();
        t.check(importer->busy() && importer->job().value(QStringLiteral("id")).toInt() == jobR,
                QStringLiteral("  the next launch shows it, busy, and waits a moment before going on"));
        waitUntil([&]() { return !finishedFor(jobR).isEmpty(); }, 20000);
        const QStringList ids = playlistVideoIds(playlistR);
        QStringList expected;
        for (int i = 0; i < resumeTitles.size(); ++i)
            expected << QStringLiteral("RESUMEVID%1").arg(i, 2, 10, QLatin1Char('0'));
        t.check(ids == expected && statuses(jobR).count(QStringLiteral("found")) == 6,
                QStringLiteral("  it went on where it was, and all 6 are in the playlist in order"), ids.join(QLatin1Char(' ')));
        t.check(searchesFor(standIn.requests, QStringLiteral("Resume One Test Band"), from) == 1
                    && searchesFor(standIn.requests, QStringLiteral("Resume Two Test Band"), from) == 1,
                QStringLiteral("  the songs found before the restart were not searched for again"));
    }

    // ------------------------------------------------------------ 13. Cancel keeps what was found
    {
        importer->previewList(QStringLiteral("Test Band - Cap One (3:20)\nTest Band - Never Asked (3:00)\n"));
        const int jobK = importer->start(QStringLiteral("Cancel test"));
        importer->cancel(jobK);
        const QVariantMap job = importer->job();
        t.check(jobState(jobK) == QLatin1String("cancelled") && statuses(jobK).count(QStringLiteral("notfound")) >= 1
                    && library->playlistName(job.value(QStringLiteral("playlistId")).toInt()) == QLatin1String("Cancel test")
                    && rowField(jobK, 1, "reason").contains(QLatin1String("stopped")),
                QStringLiteral("Cancel: the playlist and what was found stay; the rest says it was not searched for"),
                statuses(jobK).join(QLatin1Char(' ')));
        const QVariantMap review = importer->review(job.value(QStringLiteral("playlistId")).toInt());
        t.check(review.value(QStringLiteral("notSearched")).toList().size() == statuses(jobK).count(QStringLiteral("notfound"))
                    && review.value(QStringLiteral("notFound")).toList().isEmpty() && importer->copyNotFound(jobK) == 0,
                QStringLiteral("  the review lists them apart, as not looked for, not as songs YouTube Music had nothing for"));
    }

    // ------------------------------------------------------------ 13b. songs with no artist
    // A title alone, pasted, and a CSV file with no artist column: kept, and
    // looked for, the user checking what is found for them.
    {
        songs.insert(QStringLiteral("Lonely Title"), { song(QStringLiteral("LONELY00001"), QStringLiteral("Lonely Title"),
                                                            QStringLiteral("Somebody"), QStringLiteral("Some Album"), 180) });
        importer->previewList(QStringLiteral("Lonely Title (3:00)\nTest Band - Cap Two (3:21)\n"));
        const int jobT = importer->start(QStringLiteral("No artist"));
        waitUntil([&]() { return !finishedFor(jobT).isEmpty(); }, 20000);
        t.check(statuses(jobT) == QStringList({ QStringLiteral("check"), QStringLiteral("found") })
                    && rowField(jobT, 0, "artists").isEmpty() && rowField(jobT, 0, "candidate").contains(QLatin1String("LONELY00001")),
                QStringLiteral("a pasted title with no artist: kept in its place, and what was found waits to be checked"),
                statuses(jobT).join(QLatin1Char(' ')));

        const QString path = qEnvironmentVariable("MONOLIST_DATA_DIR") + QStringLiteral("/import test files/titles only.csv");
        QFile file(path);
        const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                             && file.write("Track Name,Album Name\nOrphan Song,Orphan Album\nOther Orphan,Orphan Album\n") > 0;
        file.close();
        importer->previewFile(QUrl::fromLocalFile(path).toString());
        const int readable = importer->previewed().value(QStringLiteral("readable")).toInt();
        const int jobO = importer->start(QStringLiteral("Orphans"));
        waitUntil([&]() { return !finishedFor(jobO).isEmpty(); }, 20000);
        t.check(written && readable == 2 && statuses(jobO) == QStringList({ QStringLiteral("notfound"), QStringLiteral("notfound") })
                    && finishedFor(jobO) == QStringLiteral("Imported ‘Orphans’: 0 of 2 — 2 not found")
                    && importer->review(importer->job().value(QStringLiteral("playlistId")).toInt())
                               .value(QStringLiteral("notFound")).toList().size() == 2,
                QStringLiteral("  a CSV file with no artist column: its 2 songs kept, looked for, and listed as not found"),
                statuses(jobO).join(QLatin1Char(' ')) + QStringLiteral(" / ") + finishedFor(jobO));
    }

    // ------------------------------------------------------------ 13c. Spotify's first 100 of 150, imported
    // Counted against the 100 read, not the 150 the public page says.
    {
        previewNow(QStringLiteral("https://open.spotify.com/playlist/") + spotifyBig);
        const int jobP = importer->start(QString());
        const QVariantMap job = importer->job();
        importer->cancel(jobP);
        t.check(job.value(QStringLiteral("total")).toInt() == 100 && job.value(QStringLiteral("read")).toInt() == 100
                    && finishedFor(jobP) == QStringLiteral("Stopped ‘Big Spotify List’: 0 of 100 — 100 not looked for"),
                QStringLiteral("Spotify's 100 of 150 imported: counted as 100 from the start, and stopped as 0 of 100"),
                QStringLiteral("total %1; %2").arg(job.value(QStringLiteral("total")).toInt()).arg(finishedFor(jobP)));
    }

    // ------------------------------------------------------------ 13d. how long a long list is said to take
    {
        PlaylistImport estimator(library);   // the real pacing: 600 an hour, 2,000 a day
        const int hundred = estimator.estimateSeconds(100);
        const int thousand = estimator.estimateSeconds(1000);
        const int fiveThousand = estimator.estimateSeconds(5000);
        t.check(hundred >= 200 && hundred < 600 && thousand >= 3600 + 400 * 2 && fiveThousand >= 2 * 86400,
                QStringLiteral("the time a list is said to take: 100 songs minutes, 1,000 over an hour (600 an hour), "
                               "5,000 over two days (2,000 a day)"),
                QStringLiteral("%1 s, %2 s, %3 s").arg(hundred).arg(thousand).arg(fiveThousand));
    }

    // ------------------------------------------------------------ 14. never the account
    {
        int checked = 0;
        QStringList carried;
        bool sessionCalls = false;
        for (int i = importFrom; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            const QString browseId = request.json().value(QStringLiteral("browseId")).toString();
            const bool sessionCall = request.path.contains("account_menu") || browseId == QLatin1String("FEmusic_home");
            if (sessionCall) {
                sessionCalls = sessionCalls || request.authed();
                continue;
            }
            ++checked;
            for (const char *name : { "cookie", "authorization", "x-goog-authuser" }) {
                if (request.has(name))
                    carried << QString::fromUtf8(request.path.left(60)) + QStringLiteral(" (") + QLatin1String(name) + QLatin1Char(')');
            }
        }
        t.check(checked > 50 && carried.isEmpty(),
                QStringLiteral("none of the import's %1 requests carried a Cookie, an Authorization or an X-Goog-AuthUser header, "
                               "with the session imported, and every answer setting a cookie").arg(checked),
                carried.join(QStringLiteral("; ")));
        t.check(session->state() == QLatin1String("active"), QStringLiteral("  the session still active throughout"),
                session->state());
        Q_UNUSED(sessionCalls);
        bool honest = true;
        for (int i = importFrom; i < standIn.requests.size(); ++i) {
            const StandIn::Request &request = standIn.requests.at(i);
            if (!request.path.startsWith("/youtubei/"))
                honest = honest && request.header("user-agent").startsWith("Monolist/");
        }
        t.check(honest, QStringLiteral("  Spotify, JioSaavn, Deezer and Apple Music were asked as Monolist, by name"));
    }

    importer.reset();
    session.reset();
    SecretStore::remove(secretName);
    InnerTube::setTestServer(QString());
    PlaylistImport::setTestServer(QString());
    return t.finish();
}

// ------------------------------------------------------------------ live checks

int runImportPreview(const QString &text)
{
    const Import::Link link = Import::parseLink(text);
    qWarning("import-preview: %s -> %s %s, id %s%s%s", qPrintable(text), link.ok ? "ok" : "refused",
             qPrintable(link.label), qPrintable(link.id), link.reason.isEmpty() ? "" : ": ", qPrintable(link.reason));
    if (!link.ok)
        return 1;
    InnerTube tube(nullptr, /*warmUp=*/false);
    tube.setCookieless(true);
    Import::Reader reader(&tube);
    reader.setGap(2000, 1000);
    const auto show = [](const char *which, const Import::Page &page) {
        qWarning("import-preview: %s: name \"%s\", owner \"%s\", total %d, %d song(s), %s; artwork %s", which,
                 qPrintable(page.name), qPrintable(page.owner), page.total, int(page.rows.size()),
                 page.cursor.isEmpty() ? "no more pages" : "more to read", qPrintable(page.artwork.left(80)));
        if (!page.error.isEmpty())
            qWarning("import-preview:   error: %s%s", qPrintable(page.error), page.retryable ? " (worth trying later)" : "");
        for (const QString &warning : page.warnings)
            qWarning("import-preview:   warning: %s", qPrintable(warning));
        for (int i = 0; i < page.rows.size(); ++i) {
            if (i >= 3 && i < page.rows.size() - 1)
                continue;
            const Import::Row &row = page.rows.at(i);
            qWarning("import-preview:   #%d \"%s\" / %s / \"%s\" / %lld s%s%s%s%s", i, qPrintable(row.title),
                     qPrintable(row.artists.join(QStringLiteral(" + "))), qPrintable(row.album),
                     (long long)(row.durationMs / 1000), row.isrc.isEmpty() ? "" : " / ISRC ", qPrintable(row.isrc),
                     row.videoId.isEmpty() ? "" : " / video ", qPrintable(row.videoId));
        }
    };
    Import::Page first;
    bool done = false;
    reader.first(link, [&](const Import::Page &page) { first = page; done = true; });
    waitUntil([&done]() { return done; }, 60000);
    show("first page", first);
    if (!done || !first.error.isEmpty())
        return 1;
    if (!first.cursor.isEmpty()) {
        Import::Page second;
        done = false;
        reader.next(link, first.cursor, [&](const Import::Page &page) { second = page; done = true; });
        waitUntil([&done]() { return done; }, 60000);
        show("next page", second);
        if (!done || !second.error.isEmpty())
            return 1;
    }
    return 0;
}

int runImportMatch(const QString &title, const QString &artist, int seconds, const QString &album, const QString &isrc)
{
    ImportMatch::Wanted wanted;
    wanted.title = title;
    wanted.artists = Saavn::splitArtists(artist);
    wanted.album = album;
    wanted.durationSec = seconds;
    wanted.isrc = isrc;
    InnerTube tube(nullptr, /*warmUp=*/false);
    tube.setCookieless(true);
    const QList<ImportMatch::Search> searches = ImportMatch::searches(wanted);
    bool haveBest = false;
    ImportMatch::Candidate best;
    ImportMatch::Verdict bestVerdict;
    int bestOrder = 0;
    for (int i = 0; i < searches.size(); ++i) {
        const ImportMatch::Search &search = searches.at(i);
        if (i > 0)
            pause(2000 + int(QRandomGenerator::global()->bounded(1001)));
        bool done = false;
        QJsonObject root;
        QString error;
        tube.searchAnswer(search.query, search.videos ? InnerTube::Filter::Videos : InnerTube::Filter::Songs,
                          [&](const QJsonObject &answer, const QString &why) { root = answer; error = why; done = true; },
                          /*retries=*/0);
        waitUntil([&done]() { return done; }, 20000);
        if (!error.isEmpty() || !root.contains(QLatin1String("contents"))) {
            qWarning("import-match: \"%s\": YouTube Music did not answer as it should (%s); stopping",
                     qPrintable(search.query), qPrintable(error.isEmpty() ? QStringLiteral("empty answer") : error));
            return 1;
        }
        QList<ImportMatch::Candidate> candidates;
        for (const InnerTube::Track &track : InnerTube::parseSearch(root))
            candidates.append(ImportMatch::fromTrack(track, search.videos));
        const ImportMatch::Pick pick = ImportMatch::pick(wanted, candidates, search);
        qWarning("import-match: search %d \"%s\"%s%s: %d result(s)", i + 1, qPrintable(search.query),
                 search.isrc ? " (ISRC)" : "", search.videos ? " (videos)" : "", int(candidates.size()));
        for (const QString &note : pick.notes)
            qWarning("import-match:   %s", qPrintable(note));
        for (int c = 0; c < candidates.size() && c < 5; ++c)
            qWarning("import-match:   #%d type %s, credits [%s]", c + 1, qPrintable(candidates.at(c).videoType),
                     qPrintable(candidates.at(c).artists.join(QStringLiteral(" + "))));
        if (pick.index >= 0) {
            const int order = i * 100 + pick.index;
            if (!haveBest || ImportMatch::better(pick.verdict.confidence, candidates.at(pick.index).videoType, order,
                                                 bestVerdict.confidence, best.videoType, bestOrder)) {
                haveBest = true;
                best = candidates.at(pick.index);
                bestVerdict = pick.verdict;
                bestOrder = order;
            }
        }
        const ImportMatch::Confidence c = haveBest ? bestVerdict.confidence : ImportMatch::Confidence::None;
        if (c == ImportMatch::Confidence::Sure || c == ImportMatch::Confidence::Likely
            || (c == ImportMatch::Confidence::Check && seconds <= 0))
            break;
    }
    if (!haveBest) {
        qWarning("import-match: not found");
        return 1;
    }
    qWarning("import-match: %s: %s \"%s\" by %s, %d s (%s)", qPrintable(ImportMatch::confidenceName(bestVerdict.confidence)),
             qPrintable(best.videoId), qPrintable(best.title), qPrintable(best.artistLine), best.durationSec,
             qPrintable(best.videoType));
    return 0;
}

// ------------------------------------------------------------------ the demo

namespace {

// A flat picture for a stand-in cover: a field of one colour with a disc
// and a band of two others, different for each `n`, so the covers differ
// from one another in black and white as well as in colour.
QByteArray demoPicture(int n)
{
    static const char *const colours[] = { "#1f3a93", "#e8b23a", "#2e7d5b", "#c0392b", "#6c3483",
                                           "#f0e6d2", "#17202a", "#d35400", "#5dade2", "#a9cce3" };
    constexpr int count = int(std::size(colours));
    const QColor field(QString::fromLatin1(colours[n % count]));
    const QColor disc(QString::fromLatin1(colours[(n * 3 + 1) % count]));
    const QColor band(QString::fromLatin1(colours[(n * 7 + 4) % count]));
    QImage image(320, 320, QImage::Format_RGB32);
    image.fill(field);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(disc);
    const qreal radius = 70 + (n % 4) * 18;
    painter.drawEllipse(QPointF(110 + (n % 3) * 50, 120 + (n % 2) * 60), radius, radius);
    painter.setBrush(band);
    painter.drawRect(QRectF(0, 236 - (n % 3) * 30, 320, 34));
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

// One song of the invented playlist, and what YouTube Music has of it:
//   's' the song, the same length          'l' the song, 9 s longer (likely)
//   'c' the song, with no length (check)   'v' only its video, no length (check)
//   'L' only a live take (not found)       'f' a version 4 minutes shorter (not found)
//   'n' nothing at all (not found)
struct DemoSong {
    const char *title;
    const char *artists;      // as Spotify lists them
    const char *album;        // YouTube Music's
    int seconds;              // the source's length
    char fate;
};

const DemoSong kDemoSongs[] = {
    { "Take On Me", "a-ha", "Hunting High and Low", 225, 's' },
    { "Billie Jean", "Michael Jackson", "Thriller", 294, 's' },
    { "Sweet Dreams (Are Made of This)", "Eurythmics", "Sweet Dreams (Are Made of This)", 216, 's' },
    { "Don't You (Forget About Me)", "Simple Minds", "Once Upon a Time", 263, 'l' },
    { "Girls Just Want to Have Fun", "Cyndi Lauper", "She's So Unusual", 238, 's' },
    { "Under Pressure", "Queen, David Bowie", "Hot Space", 248, 'c' },
    { "Every Breath You Take", "The Police", "Synchronicity", 253, 's' },
    { "Africa", "Toto", "Toto IV", 295, 's' },
    { "Smalltown Boy", "Bronski Beat", "The Age of Consent", 540, 'f' },
    { "Tainted Love", "Soft Cell", "Non-Stop Erotic Cabaret", 163, 's' },
    { "Running Up That Hill (A Deal with God)", "Kate Bush", "Hounds of Love", 298, 'v' },
    { "Wake Me Up Before You Go-Go", "Wham!", "Make It Big", 231, 's' },
    { "Here Comes the Rain Again", "Eurythmics", "Touch", 306, 'L' },
    { "Never Gonna Give You Up", "Rick Astley", "Whenever You Need Somebody", 213, 's' },
};

// The demo's two Spotify lists: the fourteen above, and a long one that
// Spotify shows 100 of.
const QString kDemoList = QStringLiteral("MONOLISTDEMOEIGHTIES01");
const QString kDemoLongList = QStringLiteral("MONOLISTDEMOLONGLIST01");

QString demoVideoId(int n)
{
    return QStringLiteral("DEMOSONG%1").arg(n, 3, 10, QLatin1Char('0'));
}

// A search's rows with their covers, as YouTube Music sends a row's.
QByteArray demoSearchAnswer(const QList<Hit> &hits, const QStringList &covers)
{
    QJsonArray rows;
    for (int i = 0; i < hits.size(); ++i) {
        QJsonObject row = searchRow(hits.at(i));
        QJsonObject renderer = row.value(QStringLiteral("musicResponsiveListItemRenderer")).toObject();
        renderer.insert(QStringLiteral("thumbnail"), QJsonObject{ { QStringLiteral("musicThumbnailRenderer"), QJsonObject{
            { QStringLiteral("thumbnail"), QJsonObject{ { QStringLiteral("thumbnails"), QJsonArray{ QJsonObject{
                { QStringLiteral("url"), covers.value(i) }, { QStringLiteral("width"), 226 },
                { QStringLiteral("height"), 226 } } } } } } } } });
        row.insert(QStringLiteral("musicResponsiveListItemRenderer"), renderer);
        rows.append(row);
    }
    QJsonArray sections;
    if (!rows.isEmpty())
        sections.append(QJsonObject{ { QStringLiteral("musicShelfRenderer"), QJsonObject{ { QStringLiteral("contents"), rows } } } });
    const QJsonObject list{ { QStringLiteral("sectionListRenderer"), QJsonObject{ { QStringLiteral("contents"), sections } } } };
    const QJsonObject tab{ { QStringLiteral("tabRenderer"), QJsonObject{ { QStringLiteral("content"), list } } } };
    return json({ { QStringLiteral("contents"), QJsonObject{ { QStringLiteral("tabbedSearchResultsRenderer"),
        QJsonObject{ { QStringLiteral("tabs"), QJsonArray{ tab } } } } } } });
}

QByteArray demoSpotifyEmbed(const QString &id, const QString &name, const QString &cover, const QJsonArray &tracks)
{
    const QJsonObject entity{
        { QStringLiteral("type"), QStringLiteral("playlist") }, { QStringLiteral("name"), name },
        { QStringLiteral("title"), name }, { QStringLiteral("id"), id },
        { QStringLiteral("uri"), QStringLiteral("spotify:playlist:") + id },
        { QStringLiteral("subtitle"), QStringLiteral("Monolist demo") },
        { QStringLiteral("coverArt"), QJsonObject{ { QStringLiteral("sources"), QJsonArray{
            QJsonObject{ { QStringLiteral("url"), cover }, { QStringLiteral("width"), 640 } } } } } },
        { QStringLiteral("trackList"), tracks },
    };
    const QJsonObject data{ { QStringLiteral("props"), QJsonObject{ { QStringLiteral("pageProps"), QJsonObject{
        { QStringLiteral("state"), QJsonObject{ { QStringLiteral("data"), QJsonObject{ { QStringLiteral("entity"), entity } } } } } } } } } };
    return "<!DOCTYPE html><html><head><title>Spotify</title></head><body><div id=\"__next\"></div>"
           "<script id=\"__NEXT_DATA__\" type=\"application/json\">" + json(data) + "</script></body></html>";
}

} // namespace

QObject *startImportDemo(const QString &stage)
{
    const bool refuseSearches = stage == QLatin1String("resting") || stage == QLatin1String("held");
    auto *standIn = new StandIn;
    if (!standIn->listen()) {
        qWarning("import-demo: the stand-ins could not listen on this computer");
        delete standIn;
        return nullptr;
    }
    const QString base = standIn->base();
    InnerTube::setTestServer(base);
    PlaylistImport::setTestServer(base);
    qWarning("import-demo: YouTube Music and Spotify are stand-ins at %s; nothing is asked of the real ones",
             qPrintable(base));

    standIn->respond = [base, refuseSearches](const StandIn::Request &request) {
        StandIn::Answer answer;
        const QString path = QString::fromUtf8(request.path);
        const QJsonObject body = request.json();
        const int songCount = int(std::size(kDemoSongs));
        if (refuseSearches && path.startsWith(QLatin1String("/youtubei/v1/search"))) {
            answer.status = 429;
            answer.body = R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED"}})";
            return answer;
        }
        if (path.startsWith(QLatin1String("/art/"))) {
            const QString name = path.mid(5).section(QLatin1Char('.'), 0, 0);
            answer.type = "image/png";
            answer.body = demoPicture(name == QLatin1String("cover") ? 0
                                      : name == QLatin1String("long") ? 6 : name.toInt() + 1);
            return answer;
        }
        if (path.startsWith(QLatin1String("/youtubei/v1/search"))) {
            // The song the query begins with, by its title without asides.
            const QString query = body.value(QStringLiteral("query")).toString().toLower();
            const bool videos = body.value(QStringLiteral("params")).toString().startsWith(QLatin1String("EgWKAQIQ"));
            QList<Hit> hits;
            QStringList covers;
            for (int i = 0; i < songCount; ++i) {
                const DemoSong &song = kDemoSongs[i];
                const QString title = QString::fromUtf8(song.title);
                if (!query.startsWith(title.section(QStringLiteral(" ("), 0, 0).toLower()))
                    continue;
                Hit hit;
                hit.videoId = demoVideoId(i);
                hit.title = title;
                hit.artist = QString::fromUtf8(song.artists).section(QStringLiteral(", "), 0, 0);
                hit.album = QString::fromUtf8(song.album);
                hit.seconds = song.seconds;
                bool none = false;
                switch (song.fate) {
                case 'l':
                    hit.seconds += 9;
                    break;
                case 'c':
                    hit.seconds = 0;
                    break;
                case 'f':
                    hit.seconds -= 238;
                    break;
                case 'L':
                    hit.title += QStringLiteral(" (Live)");
                    hit.seconds += 41;
                    break;
                case 'v':
                    none = !videos;
                    hit.title = hit.artist + QStringLiteral(" - ") + title + QStringLiteral(" (Official Video)");
                    hit.album.clear();
                    hit.seconds = 0;
                    hit.type = QStringLiteral("OMV");
                    hit.video = true;
                    break;
                case 'n':
                    none = true;
                    break;
                default:
                    break;
                }
                if (!none) {
                    hits.append(hit);
                    covers.append(base + QStringLiteral("/art/%1.png").arg(i));
                }
                break;
            }
            answer.body = demoSearchAnswer(hits, covers);
            return answer;
        }
        if (path.startsWith(QLatin1String("/youtubei/"))) {
            // Home, suggestions, the visitor: nothing, but an answer.
            answer.body = R"({"responseContext":{}})";
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/embed/playlist/") + kDemoList)) {
            QJsonArray tracks;
            for (int i = 0; i < songCount; ++i)
                tracks.append(spotifyTrack(i, QString::fromUtf8(kDemoSongs[i].title),
                                           QString::fromUtf8(kDemoSongs[i].artists), kDemoSongs[i].seconds));
            answer.type = "text/html; charset=utf-8";
            answer.body = demoSpotifyEmbed(kDemoList, QStringLiteral("Eighties, mostly"),
                                           base + QStringLiteral("/art/cover.png"), tracks);
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/embed/playlist/") + kDemoLongList)) {
            QJsonArray tracks;
            for (int i = 0; i < 100; ++i) {
                const DemoSong &song = kDemoSongs[i % songCount];
                tracks.append(spotifyTrack(i, i < songCount ? QString::fromUtf8(song.title)
                                                            : QStringLiteral("Side B, song %1").arg(i + 1),
                                           QString::fromUtf8(song.artists), song.seconds));
            }
            answer.type = "text/html; charset=utf-8";
            answer.body = demoSpotifyEmbed(kDemoLongList, QStringLiteral("Every song from the eighties"),
                                           base + QStringLiteral("/art/long.png"), tracks);
            return answer;
        }
        if (path.startsWith(QLatin1String("/open.spotify.com/playlist/") + kDemoLongList)) {
            answer.type = "text/html; charset=utf-8";
            answer.body = spotifyPublic(kDemoLongList, 434, QStringLiteral("Hunting High and Low"));
            return answer;
        }
        answer.status = 404;
        answer.body = "not here";
        return answer;
    };
    return standIn;
}
