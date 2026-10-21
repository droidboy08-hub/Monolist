#include "driveselftest.h"

#include "appdatabase.h"
#include "driveauth.h"
#include "drivesync.h"
#include "innertube.h"
#include "library.h"
#include "librarybackup.h"
#include "secretstore.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QStringList>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>
#include <utility>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
// Descriptions are fixed text: nothing secret is ever put in one, since
// every line logged is scanned for secrets at the end.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("drive-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }
    bool same(qint64 got, qint64 want, const QString &what)
    {
        return check(got == want, what, QStringLiteral("got %1, want %2").arg(got).arg(want));
    }
    void note(const QString &text) { qWarning("drive-test: %s", qPrintable(text)); }
    int finish()
    {
        qWarning("drive-test: %d checks, %d failed", m_count, m_failed);
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
    poll.setInterval(5);
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

// Every message logged while the test runs, whatever its level, to show no
// secret is ever among them.
QStringList *g_captured = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_captured)
        g_captured->append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// Invented, and shaped like the real thing.
const QByteArray kClientId = QByteArrayLiteral("123456789012-selftestclient0abcdefghijklmnopq.apps.googleusercontent.com");
const QByteArray kClientSecret = QByteArrayLiteral("GOCSPX-SELFTEST_client-secret0000");
const QString kEmail = QStringLiteral("selftest.listener@example.com");
const QString kDriveScope = QStringLiteral("https://www.googleapis.com/auth/drive.appdata");
const QString kGrantedScopes = kDriveScope + QStringLiteral(" openid https://www.googleapis.com/auth/userinfo.email");
const QByteArray kYtmJar = QByteArrayLiteral("SELFTEST-YTM-JAR-0000");
const QByteArray kLastFmSession = QByteArrayLiteral("SELFTEST-LASTFM-SESSION-0000");

QByteArray challengeOf(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString databaseTime(const QDateTime &when)
{
    return when.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QVariantMap track(const QString &id)
{
    return { { QStringLiteral("sourceId"), id },
             { QStringLiteral("title"), QStringLiteral("Selftest ") + id },
             { QStringLiteral("artist"), QStringLiteral("Selftest") },
             { QStringLiteral("durationMs"), 180000 } };
}

QVariant one(const QString &sql, const QVariantList &binds = {})
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(sql);
    for (const QVariant &bind : binds)
        query.addBindValue(bind);
    if (query.exec() && query.next())
        return query.value(0);
    return QVariant();
}

void run(const QString &sql)
{
    QSqlQuery query(AppDatabase::connection());
    query.exec(sql);
}

QString settingOf(const QString &key)
{
    return one(QStringLiteral("SELECT value FROM settings WHERE key = ?"), { key }).toString();
}

bool hasSetting(const QString &key)
{
    return one(QStringLiteral("SELECT COUNT(*) FROM settings WHERE key = ?"), { key }).toInt() > 0;
}

void emptyLibrary(Library *library)
{
    for (const QString &table : { QStringLiteral("playlist_tracks"), QStringLiteral("playlists"),
                                  QStringLiteral("tracks"), QStringLiteral("albums"), QStringLiteral("play_events"),
                                  QStringLiteral("recent"), QStringLiteral("history"), QStringLiteral("sync_removed") })
        run(QStringLiteral("DELETE FROM %1").arg(table));
    run(QStringLiteral("DELETE FROM settings WHERE key LIKE 'backup.%'"));
    library->reloadAll();
}

// — the stand-ins —

// A service on this computer: each request kept as it arrived (its headers
// byte for byte, and when), and answered as `respond` says. One request a
// connection. Every answer also sets a cookie, so that one sent back would
// show.
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
    };
    struct Answer {
        int status = 200;
        QByteArray body = "{}";
        QByteArray type = "application/json; charset=UTF-8";
        QList<QByteArray> extra;   // more header lines
        // Bytes promised (Content-Length) and never sent: the connection
        // ends part way through the answer.
        int cutShort = 0;
    };

    std::function<Answer(const Request &)> respond;
    QList<Request> requests;
    QElapsedTimer clock;
    // Every answer says what time it is (Date), as Google's do, this far
    // from this computer's clock: a clock check's stand-in.
    bool sendsDate = false;
    qint64 dateOffsetSecs = 0;

    bool listen(const QHostAddress &address)
    {
        clock.start();
        connect(&m_server, &QTcpServer::newConnection, this, &StandIn::accept);
        return m_server.listen(address, 0);
    }
    QString base() const
    {
        return QStringLiteral("http://%1:%2").arg(m_server.serverAddress().toString()).arg(m_server.serverPort());
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
        const QByteArray body = answer.status == 204 ? QByteArray() : answer.body;
        QByteArray out = "HTTP/1.1 " + QByteArray::number(answer.status) + " Selftest\r\n"
                         "Content-Type: " + answer.type + "\r\n"
                         "Content-Length: " + QByteArray::number(body.size() + answer.cutShort) + "\r\n"
                         "Set-Cookie: DRIVETEST=anon-" + QByteArray::number(requests.size()) + "; Path=/\r\n"
                         "Connection: close\r\n";
        if (sendsDate) {
            out += "Date: "
                   + QLocale::c()
                         .toString(QDateTime::currentDateTimeUtc().addSecs(dateOffsetSecs),
                                   QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"))
                         .toLatin1()
                   + " GMT\r\n";
        }
        for (const QByteArray &line : answer.extra)
            out += line + "\r\n";
        out += "\r\n" + body;
        socket->write(out);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// Google's OAuth endpoints and Drive, as far as Monolist uses them.
class World
{
public:
    // — Google: /token and /revoke —
    StandIn google;
    QString scope = kGrantedScopes;
    int expiresIn = 3600;
    QByteArray refreshError;           // answered to every refresh while set
    QByteArray expectedCode;
    QByteArray expectedChallenge;
    QByteArray expectedRedirect;
    QByteArray currentAccess;
    QByteArray currentRefresh;
    int accessIssued = 0;
    int refreshIssued = 0;
    int exchanges = 0;
    int refreshes = 0;
    bool lastExchangeOk = false;
    bool lastVerifierOk = false;
    QByteArray lastVerifier;
    QUrlQuery lastExchange;
    QUrlQuery lastRefresh;
    QList<QByteArray> revoked;
    // Everything secret Monolist was given or sent, for the log's scan.
    QList<QByteArray> secrets;

    // — Drive: files, and their upload —
    struct File {
        QString id;
        QString name;
        QStringList parents;
        QString mimeType;
        QByteArray content;
        QString modified;
    };
    StandIn drive;
    QList<File> files;
    int fileCounter = 0;
    int modifiedCounter = 0;
    QSet<QByteArray> invalidTokens;
    bool rejectAll = false;
    QList<StandIn::Answer> failNext;
    // Answered to the next uploads (a create or an update) only.
    QList<StandIn::Answer> failNextUpload;
    QByteArray redirectTo;
    bool vanishOnRead = false;
    // The next read of the file sends part of it, then hangs up.
    bool cutNextRead = false;
    int finds = 0;
    int downloads = 0;
    int creates = 0;
    int updates = 0;
    int deletes = 0;
    int unauthorized = 0;
    bool findQueryOk = true;
    QJsonObject lastMetadata;
    QByteArray lastUploadType;

    // Somewhere a redirect might point: nothing may ever reach it.
    StandIn elsewhere;

    bool listen()
    {
        google.respond = [this](const StandIn::Request &request) { return googleAnswer(request); };
        drive.respond = [this](const StandIn::Request &request) { return driveAnswer(request); };
        drive.sendsDate = true;
        // Another loopback address is another host; Windows answers on all
        // of 127/8.
        if (!elsewhere.listen(QHostAddress(QStringLiteral("127.0.0.2"))))
            elsewhere.listen(QHostAddress::LocalHost);
        return google.listen(QHostAddress::LocalHost) && drive.listen(QHostAddress::LocalHost);
    }

    QString newModified()
    {
        ++modifiedCounter;
        return QStringLiteral("2026-10-20T10:%1:%2.000Z")
            .arg(modifiedCounter / 60, 2, 10, QLatin1Char('0'))
            .arg(modifiedCounter % 60, 2, 10, QLatin1Char('0'));
    }

    File *file(const QString &id)
    {
        for (File &file : files) {
            if (file.id == id)
                return &file;
        }
        return nullptr;
    }

    File &addFile(const QByteArray &content)
    {
        File made;
        made.id = QStringLiteral("selftestfile%1").arg(++fileCounter, 3, 10, QLatin1Char('0'));
        made.name = DriveSync::kFileName;
        made.parents = { QStringLiteral("appDataFolder") };
        made.mimeType = QStringLiteral("application/json");
        made.content = content;
        made.modified = newModified();
        files.append(made);
        return files.last();
    }

    static QJsonObject meta(const File &file)
    {
        return { { QStringLiteral("id"), file.id },
                 { QStringLiteral("modifiedTime"), file.modified },
                 { QStringLiteral("size"), QString::number(file.content.size()) } };
    }

    QByteArray issueAccess()
    {
        currentAccess = "SELFTEST-ACCESS-" + QByteArray::number(++accessIssued) + "-x9";
        secrets.append(currentAccess);
        return currentAccess;
    }

    QByteArray idToken()
    {
        const QByteArray header = QJsonDocument(QJsonObject{ { QStringLiteral("alg"), QStringLiteral("RS256") } })
                                      .toJson(QJsonDocument::Compact)
                                      .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const QByteArray payload = QJsonDocument(QJsonObject{ { QStringLiteral("email"), kEmail },
                                                              { QStringLiteral("email_verified"), true },
                                                              { QStringLiteral("sub"), QStringLiteral("1234567890") } })
                                       .toJson(QJsonDocument::Compact)
                                       .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const QByteArray token = header + '.' + payload + ".SELFTESTSIGNATURE0";
        secrets.append(token);
        return token;
    }

    static StandIn::Answer json(int status, const QJsonObject &object)
    {
        StandIn::Answer answer;
        answer.status = status;
        answer.body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return answer;
    }

    StandIn::Answer googleAnswer(const StandIn::Request &request)
    {
        const QUrlQuery form(QString::fromUtf8(request.body));
        const auto value = [&form](const char *name) {
            return form.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded).toUtf8();
        };
        const QByteArray path = request.path;
        if (request.method != "POST")
            return json(405, { { QStringLiteral("error"), QStringLiteral("invalid_request") } });
        if (path == "/revoke") {
            revoked.append(value("token"));
            return json(200, {});
        }
        if (path != "/token")
            return json(404, {});
        const QByteArray grant = value("grant_type");
        const bool client = value("client_id") == kClientId && value("client_secret") == kClientSecret;
        if (grant == "authorization_code") {
            ++exchanges;
            lastExchange = form;
            lastVerifier = value("code_verifier");
            secrets.append(lastVerifier);
            lastVerifierOk = !lastVerifier.isEmpty() && challengeOf(lastVerifier) == expectedChallenge;
            lastExchangeOk = client && lastVerifierOk && value("code") == expectedCode
                             && value("redirect_uri") == expectedRedirect;
            if (!lastExchangeOk)
                return json(400, { { QStringLiteral("error"), QStringLiteral("invalid_grant") } });
            currentRefresh = "SELFTEST-REFRESH-" + QByteArray::number(++refreshIssued) + "-1//x9";
            secrets.append(currentRefresh);
            return json(200, { { QStringLiteral("access_token"), QString::fromLatin1(issueAccess()) },
                               { QStringLiteral("expires_in"), expiresIn },
                               { QStringLiteral("refresh_token"), QString::fromLatin1(currentRefresh) },
                               { QStringLiteral("scope"), scope },
                               { QStringLiteral("token_type"), QStringLiteral("Bearer") },
                               { QStringLiteral("id_token"), QString::fromLatin1(idToken()) } });
        }
        if (grant == "refresh_token") {
            ++refreshes;
            lastRefresh = form;
            if (!refreshError.isEmpty()) {
                return json(400, { { QStringLiteral("error"), QString::fromLatin1(refreshError) },
                                   { QStringLiteral("error_description"), QStringLiteral("Token has been expired or revoked.") } });
            }
            if (!client || value("refresh_token") != currentRefresh)
                return json(400, { { QStringLiteral("error"), QStringLiteral("invalid_grant") } });
            return json(200, { { QStringLiteral("access_token"), QString::fromLatin1(issueAccess()) },
                               { QStringLiteral("expires_in"), expiresIn },
                               { QStringLiteral("scope"), scope },
                               { QStringLiteral("token_type"), QStringLiteral("Bearer") } });
        }
        return json(400, { { QStringLiteral("error"), QStringLiteral("unsupported_grant_type") } });
    }

    StandIn::Answer driveAnswer(const StandIn::Request &request)
    {
        const QByteArray authorization = request.header("authorization");
        const QByteArray token = authorization.startsWith("Bearer ") ? authorization.mid(7) : QByteArray();
        if (rejectAll || token.isEmpty() || token != currentAccess || invalidTokens.contains(token)) {
            ++unauthorized;
            return json(401, { { QStringLiteral("error"),
                                 QJsonObject{ { QStringLiteral("code"), 401 },
                                              { QStringLiteral("message"), QStringLiteral("Invalid Credentials") },
                                              { QStringLiteral("errors"),
                                                QJsonArray{ QJsonObject{ { QStringLiteral("reason"),
                                                                           QStringLiteral("authError") } } } } } } });
        }
        if (!failNext.isEmpty())
            return failNext.takeFirst();
        if (!redirectTo.isEmpty()) {
            StandIn::Answer answer;
            answer.status = 302;
            answer.extra << "Location: " + redirectTo;
            redirectTo.clear();
            return answer;
        }
        const QUrl url(QStringLiteral("http://drive") + QString::fromLatin1(request.path));
        const QString path = url.path();
        const QUrlQuery query(url);
        const auto item = [&query](const char *name) {
            return query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded);
        };
        const QString filesPath = QStringLiteral("/drive/v3/files/");
        const QString uploadPath = QStringLiteral("/upload/drive/v3/files/");
        if ((request.method == "POST" || request.method == "PATCH") && path.startsWith(QLatin1String("/upload/"))
            && !failNextUpload.isEmpty())
            return failNextUpload.takeFirst();

        if (request.method == "GET" && path == QLatin1String("/drive/v3/files")) {
            ++finds;
            if (item("spaces") != QLatin1String("appDataFolder")
                || item("q") != QStringLiteral("name='%1' and trashed=false").arg(DriveSync::kFileName)
                || item("fields") != QLatin1String("files(id,modifiedTime,size)"))
                findQueryOk = false;
            QJsonArray list;
            for (const File &file : files) {
                if (file.name == DriveSync::kFileName && file.parents.contains(QLatin1String("appDataFolder")))
                    list.append(meta(file));
            }
            return json(200, { { QStringLiteral("files"), list } });
        }
        if (request.method == "GET" && path.startsWith(filesPath) && item("alt") == QLatin1String("media")) {
            ++downloads;
            const QString id = path.mid(filesPath.size());
            if (vanishOnRead) {
                vanishOnRead = false;
                files.removeIf([&id](const File &file) { return file.id == id; });
            }
            if (File *found = file(id)) {
                StandIn::Answer answer;
                if (std::exchange(cutNextRead, false)) {
                    // Half the file, and a promise of all of it.
                    answer.body = found->content.left(found->content.size() / 2);
                    answer.cutShort = int(found->content.size() - answer.body.size()) + 100;
                } else {
                    answer.body = found->content;
                }
                return answer;
            }
            return json(404, { { QStringLiteral("error"), QJsonObject{ { QStringLiteral("code"), 404 } } } });
        }
        if (request.method == "POST" && path == QLatin1String("/upload/drive/v3/files")
            && item("uploadType") == QLatin1String("multipart")) {
            ++creates;
            lastUploadType = request.header("content-type");
            const qsizetype at = lastUploadType.indexOf("boundary=");
            const QByteArray delimiter = "--" + (at >= 0 ? lastUploadType.mid(at + 9) : QByteArray("?"));
            QList<QByteArray> parts;
            qsizetype from = request.body.indexOf(delimiter);
            while (from >= 0) {
                const qsizetype start = from + delimiter.size();
                if (request.body.mid(start, 2) == "--")
                    break;
                const qsizetype next = request.body.indexOf(delimiter, start);
                if (next < 0)
                    break;
                QByteArray part = request.body.mid(start, next - start);
                if (part.startsWith("\r\n"))
                    part.remove(0, 2);
                if (part.endsWith("\r\n"))
                    part.chop(2);
                const qsizetype split = part.indexOf("\r\n\r\n");
                parts.append(split >= 0 ? part.mid(split + 4) : QByteArray());
                from = next;
            }
            if (parts.size() != 2)
                return json(400, {});
            lastMetadata = QJsonDocument::fromJson(parts.at(0)).object();
            File &made = addFile(parts.at(1));
            made.name = lastMetadata.value(QStringLiteral("name")).toString();
            made.parents.clear();
            for (const QJsonValue &parent : lastMetadata.value(QStringLiteral("parents")).toArray())
                made.parents.append(parent.toString());
            made.mimeType = lastMetadata.value(QStringLiteral("mimeType")).toString();
            return json(200, meta(made));
        }
        if (request.method == "PATCH" && path.startsWith(uploadPath) && item("uploadType") == QLatin1String("media")) {
            ++updates;
            File *found = file(path.mid(uploadPath.size()));
            if (!found)
                return json(404, {});
            found->content = request.body;
            found->modified = newModified();
            return json(200, meta(*found));
        }
        if (request.method == "DELETE" && path.startsWith(filesPath)) {
            ++deletes;
            const QString id = path.mid(filesPath.size());
            files.removeIf([&id](const File &file) { return file.id == id; });
            StandIn::Answer answer;
            answer.status = 204;
            return answer;
        }
        return json(404, {});
    }

    StandIn::Answer failure(int status, const QString &reason = QString())
    {
        QJsonObject error{ { QStringLiteral("code"), status } };
        if (!reason.isEmpty())
            error.insert(QStringLiteral("errors"),
                         QJsonArray{ QJsonObject{ { QStringLiteral("reason"), reason } } });
        return json(status, { { QStringLiteral("error"), error } });
    }
};

// The browser, as far as the sign-in needs one: it visits the address Google
// would send it back to, and shows what the listener answered.
struct Page {
    int status = 0;
    QByteArray body;
};

Page visit(QNetworkAccessManager &browser, const QUrl &url, const QByteArray &verb = "GET")
{
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    if (verb == "POST")
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = verb == "POST" ? browser.post(request, QByteArray("x=1")) : browser.get(request);
    waitUntil([reply]() { return reply->isFinished(); }, 12000);
    Page page;
    page.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    page.body = reply->readAll();
    reply->deleteLater();
    return page;
}

QUrl landing(const QString &redirect, const QList<QPair<QString, QString>> &items)
{
    QString query;
    for (const auto &item : items) {
        if (!query.isEmpty())
            query += QLatin1Char('&');
        query += item.first + QLatin1Char('=') + QString::fromLatin1(QUrl::toPercentEncoding(item.second));
    }
    QUrl url(redirect + QStringLiteral("/"));
    url.setQuery(query);
    return url;
}

// The timings the tests run with: the launch's and the half-hourly syncs far
// off, everything else in fractions of a second.
DriveSync::Timing quickTiming()
{
    DriveSync::Timing timing;
    timing.launchMs = 60 * 60 * 1000;
    timing.editMs = 200;
    timing.editMaxMs = 2000;
    timing.periodMs = 60 * 60 * 1000;
    timing.gapMs = 100;
    timing.backoffMs = { 400, 800, 1200 };
    timing.timeoutMs = 5000;
    timing.tickMs = 60 * 60 * 1000;
    timing.clockRetryMs = 500;
    return timing;
}

// A sync as the app makes one, given the stand-ins and an invented client,
// and quick timings.
std::unique_ptr<DriveSync> makeSync(Library *library, World &world, QList<QUrl> *opened)
{
    auto sync = std::make_unique<DriveSync>(library);
    DriveAuth *auth = sync->auth();
    auth->setTestClient(kClientId, kClientSecret);
    auth->setTestEndpoints(QUrl(world.google.base() + QStringLiteral("/o/oauth2/v2/auth")),
                           QUrl(world.google.base() + QStringLiteral("/token")),
                           QUrl(world.google.base() + QStringLiteral("/revoke")));
    auth->setUrlOpener([opened](const QUrl &url) {
        opened->append(url);
        return true;
    });
    DriveAuth::Timing authTiming;
    authTiming.timeoutMs = 5000;
    auth->setTiming(authTiming);
    sync->setTestDrive(QUrl(world.drive.base()));
    sync->setTiming(quickTiming());
    return sync;
}

bool idle(DriveSync &sync, int timeoutMs = 8000)
{
    return waitUntil([&sync]() { return !sync.busy(); }, timeoutMs);
}

// Every request to list the files, answered or refused.
int findRequests(const World &world)
{
    int count = 0;
    for (const StandIn::Request &request : world.drive.requests) {
        if (request.method == "GET" && request.path.startsWith("/drive/v3/files?"))
            ++count;
    }
    return count;
}

// The library a file holds, read back.
LibraryBackup::Data contentOf(const World::File *file)
{
    LibraryBackup::Data data;
    if (file)
        LibraryBackup::parse(file->content, &data, nullptr);
    return data;
}

bool likes(const LibraryBackup::Data &data, const QString &videoId)
{
    for (const LibraryBackup::Song &song : data.likes) {
        if (song.videoId == videoId)
            return true;
    }
    return false;
}

bool played(const LibraryBackup::Data &data, const QString &videoId)
{
    for (const LibraryBackup::Play &play : data.plays) {
        if (play.kind == QLatin1String("play") && play.videoId == videoId)
            return true;
    }
    return false;
}

// A like as another computer's file holds it, made a minute ago.
LibraryBackup::Song likedElsewhere(const QString &videoId)
{
    LibraryBackup::Song song;
    song.videoId = videoId;
    song.title = QStringLiteral("Selftest ") + videoId;
    song.artist = QStringLiteral("Selftest");
    song.durationMs = 200000;
    song.at = databaseTime(QDateTime::currentDateTimeUtc().addSecs(-60));
    return song;
}

// The whole sign-in, as the browser makes it: the page opened, the browser
// sent back with a code. Returns whether it ended signed in.
bool signInThroughBrowser(Checks &checks, World &world, DriveSync &sync, QNetworkAccessManager &browser,
                          QList<QUrl> &opened, const QByteArray &code, bool checkAddress)
{
    const int before = int(opened.size());
    sync.signIn();
    if (!checks.check(opened.size() == before + 1 && sync.state() == QLatin1String("waiting"),
                      QStringLiteral("SIGN IN WITH GOOGLE opens Google's page and waits")))
        return false;
    const QUrl page = opened.last();
    const QUrlQuery query(page);
    const auto item = [&query](const char *name) {
        return query.queryItemValue(QString::fromLatin1(name), QUrl::FullyDecoded);
    };
    const QString redirect = item("redirect_uri");
    const QByteArray state = item("state").toUtf8();
    const QByteArray challenge = item("code_challenge").toUtf8();
    world.secrets.append(state);
    world.expectedChallenge = challenge;
    world.expectedRedirect = redirect.toUtf8();
    world.expectedCode = code;
    world.secrets.append(code);
    if (checkAddress) {
        checks.check(page.toString(QUrl::RemoveQuery) == world.google.base() + QStringLiteral("/o/oauth2/v2/auth"),
                     QStringLiteral("  the page is the authorization endpoint (the stand-in's, for a test client)"));
        checks.check(item("client_id").toUtf8() == kClientId, QStringLiteral("  with this build's client"));
        checks.check(QRegularExpression(QStringLiteral("^http://127\\.0\\.0\\.1:[0-9]{2,5}$")).match(redirect).hasMatch(),
                     QStringLiteral("  coming back to 127.0.0.1 at a port of its own"), redirect);
        checks.check(item("response_type") == QLatin1String("code"), QStringLiteral("  asking for a code"));
        checks.check(item("scope") == QStringLiteral("openid email ") + kDriveScope,
                     QStringLiteral("  for openid, email and the app's Drive folder, and nothing else"), item("scope"));
        checks.check(item("code_challenge_method") == QLatin1String("S256")
                         && QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{43}$")).match(QString::fromLatin1(challenge)).hasMatch(),
                     QStringLiteral("  with a PKCE challenge, S256"));
        checks.check(state.size() >= 32, QStringLiteral("  and a random state"));
        checks.check(item("access_type") == QLatin1String("offline") && item("prompt") == QLatin1String("consent")
                         && item("include_granted_scopes") == QLatin1String("false"),
                     QStringLiteral("  offline, with consent asked every time, no other grants carried"));
        checks.check(!query.hasQueryItem(QStringLiteral("login_hint")) && query.queryItems().size() == 10,
                     QStringLiteral("  and no other parameter"), QString::number(query.queryItems().size()));
        checks.check(sync.status() == QLatin1String("Waiting for you in the browser"),
                     QStringLiteral("  the row says it is waiting in the browser"), sync.status());

        // What is not the browser's return is ignored: a wrong state, a
        // wrong path, a POST.
        const int exchanges = world.exchanges;
        Page wrong = visit(browser, landing(redirect, { { QStringLiteral("state"), QStringLiteral("WRONGSTATE") },
                                                        { QStringLiteral("code"), QString::fromLatin1(code) } }));
        checks.check(wrong.status == 404, QStringLiteral("a return with the wrong state gets a 404"),
                     QString::number(wrong.status));
        Page icon = visit(browser, QUrl(redirect + QStringLiteral("/favicon.ico")));
        checks.check(icon.status == 404, QStringLiteral("  as does another path"), QString::number(icon.status));
        Page posted = visit(browser, landing(redirect, { { QStringLiteral("state"), QString::fromLatin1(state) },
                                                         { QStringLiteral("code"), QString::fromLatin1(code) } }),
                            "POST");
        checks.check(posted.status == 404, QStringLiteral("  and a POST"), QString::number(posted.status));
        checks.check(world.exchanges == exchanges && sync.state() == QLatin1String("waiting"),
                     QStringLiteral("  and none of them ends the wait or is exchanged"));
    }

    Page back = visit(browser, landing(redirect, { { QStringLiteral("state"), QString::fromLatin1(state) },
                                                   { QStringLiteral("code"), QString::fromLatin1(code) },
                                                   { QStringLiteral("scope"), kGrantedScopes } }));
    if (checkAddress) {
        checks.check(back.status == 200 && back.body.contains("Monolist is connected to Google Drive"),
                     QStringLiteral("the browser's return is answered with the connected page"),
                     QString::number(back.status));
        checks.check(back.body.contains("<style>") && !back.body.contains("<script") && !back.body.contains("http"),
                     QStringLiteral("  a page of its own, with nothing to load or run"));
        checks.check(world.lastExchangeOk, QStringLiteral("the code is exchanged with the client, the redirect and the code"));
        checks.check(world.lastExchange.queryItemValue(QStringLiteral("grant_type")) == QLatin1String("authorization_code"),
                     QStringLiteral("  as an authorization_code grant"));
        checks.check(world.lastVerifierOk, QStringLiteral("  and a verifier whose S256 is the challenge"));
        checks.check(QRegularExpression(QStringLiteral("^[A-Za-z0-9._~-]{64}$")).match(QString::fromLatin1(world.lastVerifier)).hasMatch(),
                     QStringLiteral("  the verifier 64 characters of RFC 7636's"));
        Page again = visit(browser, landing(redirect, { { QStringLiteral("state"), QString::fromLatin1(state) },
                                                        { QStringLiteral("code"), QString::fromLatin1(code) } }));
        checks.check(again.status == 0, QStringLiteral("the listener is closed once the browser is back"),
                     QString::number(again.status));
    }
    waitUntil([&sync]() { return sync.signedIn() || sync.state() == QLatin1String("error"); }, 5000);
    return sync.signedIn();
}

} // namespace

int runDriveSelfTest(Library *library)
{
    Checks checks;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        checks.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                     QStringLiteral("this test empties the library and its secrets, and runs only in a scratch data folder"));
        return checks.finish();
    }
    // Nothing here may reach the internet: should anything ask InnerTube,
    // it asks a port on this computer that nothing listens on.
    InnerTube::setTestServer(QStringLiteral("http://127.0.0.1:9"));
    QStringList captured;
    g_captured = &captured;
    g_previousHandler = qInstallMessageHandler(captureMessage);

    emptyLibrary(library);
    SecretStore::remove(DriveAuth::kSecretName);
    // Another sign-in's secrets and settings, which signing out of Drive must
    // leave exactly as they are.
    SecretStore::write(QStringLiteral("ytmusic.cookies"), kYtmJar);
    SecretStore::write(QStringLiteral("lastfm.session"), kLastFmSession);
    library->setSetting(QStringLiteral("ytmusic.guard"), QStringLiteral("selftest-guard"));
    library->setSetting(QStringLiteral("signin.refused_at"), QStringLiteral("selftest-refused"));
    library->setSetting(QStringLiteral("lastfm.user"), QStringLiteral("selftest-user"));

    World world;
    if (!checks.check(world.listen(), QStringLiteral("the stand-ins listen on this computer"))) {
        qInstallMessageHandler(g_previousHandler);
        g_captured = nullptr;
        return checks.finish();
    }
    QNetworkAccessManager browser;

    // ------------------------------------------------- endpoints and client
    checks.note(QStringLiteral("- where the sign-in may go"));
    {
        DriveAuth plain(library);
        plain.setTestEndpoints(QUrl(world.google.base() + QStringLiteral("/auth")),
                               QUrl(world.google.base() + QStringLiteral("/token")),
                               QUrl(world.google.base() + QStringLiteral("/revoke")));
        checks.check(plain.authorizeEndpoint() == QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"))
                         && plain.tokenEndpoint() == QUrl(QStringLiteral("https://oauth2.googleapis.com/token"))
                         && plain.revokeEndpoint() == QUrl(QStringLiteral("https://oauth2.googleapis.com/revoke")),
                     QStringLiteral("the build's client goes to Google's endpoints, whatever stand-in is named"));
        DriveAuth lan(library);
        lan.setTestClient(kClientId, kClientSecret);
        lan.setTestEndpoints(QUrl(QStringLiteral("http://192.168.1.20/auth")), QUrl(QStringLiteral("http://192.168.1.20/token")),
                             QUrl(QStringLiteral("https://example.com/revoke")));
        checks.check(lan.authorizeEndpoint().host() == QLatin1String("accounts.google.com")
                         && lan.tokenEndpoint().host() == QLatin1String("oauth2.googleapis.com")
                         && lan.revokeEndpoint().host() == QLatin1String("oauth2.googleapis.com"),
                     QStringLiteral("a test client's stand-in elsewhere than this computer is refused"));
        DriveAuth local(library);
        local.setTestClient(kClientId, kClientSecret);
        local.setTestEndpoints(QUrl(QStringLiteral("http://[::1]:5/auth")), QUrl(QStringLiteral("http://localhost:5/token")),
                               QUrl(QStringLiteral("http://127.0.0.1:5/revoke")));
        checks.check(local.authorizeEndpoint().port() == 5 && local.tokenEndpoint().port() == 5
                         && local.revokeEndpoint().port() == 5,
                     QStringLiteral("a test client's stand-in on [::1], localhost or 127.0.0.1 is taken"));

        DriveSync real(library);
        real.setTestDrive(QUrl(world.drive.base()));
        checks.check(real.driveBase() == QUrl(QStringLiteral("https://www.googleapis.com")),
                     QStringLiteral("Drive's stand-in is ignored without a test client"));
        DriveSync none(library);
        int opens = 0;
        none.auth()->setTestClient(QByteArray(), QByteArray());
        none.auth()->setUrlOpener([&opens](const QUrl &) {
            ++opens;
            return true;
        });
        none.start();
        checks.check(!none.available() && !none.hasClient() && none.state() == QLatin1String("unavailable")
                         && none.status().contains(QLatin1String("built without Google sign-in")),
                     QStringLiteral("a copy built without a client says so"), none.status());
        none.signIn();
        checks.check(opens == 0 && none.state() == QLatin1String("unavailable"),
                     QStringLiteral("  and SIGN IN opens nothing"));
        checks.check(!none.storedSignIn(), QStringLiteral("  with no sign-in kept, it offers no SIGN OUT"));
    }

    // ------------------------------- a sign-in kept by an earlier copy
    checks.note(QStringLiteral("- a copy without Google sign-in, after one that had it"));
    {
        // What a build with the client left: its refresh token and email.
        const QByteArray orphan = QByteArrayLiteral("SELFTEST-REFRESH-ORPHAN-1//x9");
        world.secrets.append(orphan);
        SecretStore::write(DriveAuth::kSecretName, orphan);
        library->setSetting(DriveAuth::kEmailKey, kEmail);
        DriveSync none(library);
        none.auth()->setTestClient(QByteArray(), QByteArray());
        none.auth()->setTestEndpoints(QUrl(world.google.base() + QStringLiteral("/o/oauth2/v2/auth")),
                                      QUrl(world.google.base() + QStringLiteral("/token")),
                                      QUrl(world.google.base() + QStringLiteral("/revoke")));
        none.start();
        checks.check(none.state() == QLatin1String("unavailable") && none.storedSignIn() && none.email() == kEmail,
                     QStringLiteral("the row knows a sign-in is still kept, and whose"), none.state());
        checks.check(none.status().contains(QLatin1String("built without Google sign-in"))
                         && none.status().contains(QLatin1String("SIGN OUT withdraws it")),
                     QStringLiteral("  and says SIGN OUT can withdraw it"), none.status());
        QByteArray kept;
        checks.check(SecretStore::read(DriveAuth::kSecretName, &kept) == SecretStore::Status::Ok && kept == orphan,
                     QStringLiteral("  it is kept until then, for a copy that can sync"));
        const qsizetype revoked = world.revoked.size();
        none.signOut();
        checks.check(waitUntil([&]() { return world.revoked.size() == revoked + 1; }, 3000)
                         && world.revoked.last() == orphan,
                     QStringLiteral("SIGN OUT withdraws it at Google, with no client"));
        checks.check(SecretStore::read(DriveAuth::kSecretName, &kept) == SecretStore::Status::NotFound
                         && !hasSetting(DriveAuth::kEmailKey),
                     QStringLiteral("  and deletes it and its email here"));
        checks.check(none.state() == QLatin1String("unavailable") && !none.storedSignIn()
                         && none.status().startsWith(QLatin1String("Signed out.")),
                     QStringLiteral("  the row is the build's again, saying it signed out"), none.status());
    }

    // --------------------------------------------------------- the sign-in
    checks.note(QStringLiteral("- signing in"));
    library->setLiked(track(QStringLiteral("a1")), true);
    library->setLiked(track(QStringLiteral("a2")), true);
    {
        const int id = library->createPlaylist(QStringLiteral("Selftest A"));
        library->addAllToPlaylist(id, { track(QStringLiteral("a1")), track(QStringLiteral("p1")) });
    }
    QList<QUrl> opened;
    std::unique_ptr<DriveSync> owned = makeSync(library, world, &opened);
    DriveSync &sync = *owned;
    QStringList notices;
    QList<bool> sticky;   // beside each notice: whether it stays until put away
    QObject::connect(&sync, &DriveSync::notice, [&notices, &sticky](const QString &text, bool stays) {
        notices << text;
        sticky << stays;
    });
    int returns = 0;
    QObject::connect(sync.auth(), &DriveAuth::returned, [&returns]() { ++returns; });
    sync.start();
    checks.check(sync.available() && sync.state() == QLatin1String("off") && !sync.signedIn(),
                 QStringLiteral("before signing in the row is off"), sync.state());

    const bool signedIn = signInThroughBrowser(checks, world, sync, browser, opened,
                                               QByteArrayLiteral("SELFTEST-CODE-1/0Aa_b-c"), true);
    checks.check(signedIn && sync.state() == QLatin1String("connected"), QStringLiteral("signed in"), sync.state());
    checks.check(sync.email() == kEmail, QStringLiteral("  as the email in the id_token"));
    checks.check(returns == 1, QStringLiteral("  and Monolist's window is brought forward"), QString::number(returns));
    {
        QByteArray stored;
        SecretStore::read(DriveAuth::kSecretName, &stored);
        checks.check(!stored.isEmpty() && stored == world.currentRefresh,
                     QStringLiteral("  the refresh token is in the secret store, as backup.google"));
        checks.check(settingOf(DriveAuth::kEmailKey) == kEmail, QStringLiteral("  and the email in backup.google_email"));
    }
    checks.check(idle(sync), QStringLiteral("the first sync runs at once and ends"));
    checks.same(world.finds, 1, QStringLiteral("  it looks for the file"));
    checks.same(world.creates, 1, QStringLiteral("  finds none, and makes it"));
    checks.check(world.findQueryOk, QStringLiteral("  looking in appDataFolder by name, not trashed, for id, time and size"));
    checks.check(world.lastUploadType.startsWith("multipart/related; boundary="),
                 QStringLiteral("  as one multipart upload"));
    checks.check(world.lastMetadata.value(QStringLiteral("name")).toString() == DriveSync::kFileName
                     && world.lastMetadata.value(QStringLiteral("parents")).toArray() == QJsonArray{ QStringLiteral("appDataFolder") }
                     && world.lastMetadata.value(QStringLiteral("mimeType")).toString() == QLatin1String("application/json"),
                 QStringLiteral("  named monolist-library.json, in appDataFolder, as JSON"));
    {
        const LibraryBackup::Data made = contentOf(world.files.isEmpty() ? nullptr : &world.files.first());
        checks.check(likes(made, QStringLiteral("a1")) && likes(made, QStringLiteral("a2")) && made.playlists.size() == 1,
                     QStringLiteral("  holding this library"));
    }
    checks.check(notices.value(0) == QLatin1String("Your library is backed up in your Google Drive"),
                 QStringLiteral("  and the toast says the library is backed up"), notices.value(0));
    checks.check(sticky.value(0), QStringLiteral("  and stays until put away (the window may be behind the browser)"));
    checks.check(sync.status() == QLatin1String("Synced just now") && sync.lastSyncedAt().isValid() && sync.error().isEmpty(),
                 QStringLiteral("  the row says it synced"), sync.status());

    // ---------------------------------------------------- nothing changed
    checks.note(QStringLiteral("- a sync with nothing new"));
    {
        const int finds = world.finds, downloads = world.downloads, uploads = world.creates + world.updates;
        sync.syncNow();
        checks.check(idle(sync), QStringLiteral("SYNC NOW runs"));
        checks.check(world.finds == finds + 1 && world.downloads == downloads + 1,
                     QStringLiteral("  reading the file even though it has not changed"));
        checks.same(world.creates + world.updates, uploads, QStringLiteral("  and uploads nothing, as it holds the library"));
        sync.requestSync();
        checks.check(waitUntil([&]() { return world.finds == finds + 2 && !sync.busy(); }, 3000),
                     QStringLiteral("a sync of its own runs after the gap"));
        checks.check(world.downloads == downloads + 1 && world.creates + world.updates == uploads,
                     QStringLiteral("  and, the file unchanged since, neither reads nor uploads"));
    }

    // ---------------------------------------------------------- an edit
    checks.note(QStringLiteral("- an edit here"));
    {
        const QMetaObject::Connection edits =
            QObject::connect(library, &Library::edited, &sync, &DriveSync::noteLibraryEdited);
        const int finds = world.finds, downloads = world.downloads, updates = world.updates;
        library->setLiked(track(QStringLiteral("a3")), true);
        checks.check(sync.editedSinceSync(), QStringLiteral("a like is an edit the backup waits on"));
        checks.check(waitUntil([&]() { return world.updates == updates + 1 && !sync.busy(); }, 3000),
                     QStringLiteral("  and is uploaded soon after"));
        checks.check(world.finds == finds + 1 && world.downloads == downloads,
                     QStringLiteral("  without reading the file, unchanged since this computer wrote it"));
        checks.check(likes(contentOf(world.files.isEmpty() ? nullptr : &world.files.first()), QStringLiteral("a3")),
                     QStringLiteral("  the file holds the new like"));
        checks.check(!sync.editedSinceSync(), QStringLiteral("  and the library counts as synced"));
        QObject::disconnect(edits);

        // Never two at once: SYNC NOW pressed during a sync waits for it.
        const qsizetype before = world.drive.requests.size();
        sync.syncNow();
        sync.syncNow();
        // Waits for the second sync itself, not for the gap between the two:
        // on a busy computer the queued one can start seconds later, and a
        // shorter wait let it run into the next check (a 0.1.151 smoke test).
        checks.check(waitUntil([&]() { return world.finds == finds + 3 && !sync.busy(); }, 20000),
                     QStringLiteral("SYNC NOW twice runs two syncs"));
        bool apart = true;
        int findsSeen = 0;
        for (qsizetype i = before; i < world.drive.requests.size(); ++i) {
            const StandIn::Request &request = world.drive.requests.at(i);
            if (request.method == "GET" && request.path.startsWith("/drive/v3/files?")) {
                ++findsSeen;
                // The second sync's find comes after the first sync's read.
                if (findsSeen == 2 && (i == before || !world.drive.requests.at(i - 1).path.contains("alt=media")))
                    apart = false;
            }
        }
        checks.check(apart && findsSeen == 2, QStringLiteral("  one after the other, never side by side"));
    }

    // ------------------------------------------------------------ plays
    checks.note(QStringLiteral("- plays wait for the half-hourly sync"));
    {
        const int finds = findRequests(world), updates = world.updates, downloads = world.downloads;
        // A listen as the player writes one, then tells of it.
        run(QStringLiteral("INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms) VALUES"
                           " ('play', 'pl1', 'Selftest pl1', 'Selftest', datetime('now', '-1 minute'), 180000)"));
        sync.notePlayed();
        pause(700);   // three times an edit's wait here
        checks.check(findRequests(world) == finds && world.updates == updates,
                     QStringLiteral("a play starts no sync of its own"));
        sync.requestSync();
        checks.check(waitUntil([&]() { return findRequests(world) == finds + 1 && !sync.busy(); }, 3000)
                         && world.updates == updates && world.downloads == downloads,
                     QStringLiteral("  a sync with nothing else to send looks, and reads and sends nothing"));
        DriveSync::Timing halfHour = quickTiming();
        halfHour.periodMs = 300;
        sync.setTiming(halfHour);
        checks.check(waitUntil([&]() { return world.updates == updates + 1 && !sync.busy(); }, 4000),
                     QStringLiteral("  the half-hourly sync sends it"));
        sync.setTiming(quickTiming());
        idle(sync);
        checks.check(played(contentOf(world.files.isEmpty() ? nullptr : &world.files.first()), QStringLiteral("pl1")),
                     QStringLiteral("  and the file holds it"));
        const int after = world.updates;
        pause(400);
        checks.check(world.updates == after, QStringLiteral("  once"));
    }

    // -------------------------------------------------- another computer
    checks.note(QStringLiteral("- another computer's backup"));
    {
        World::File *kept = world.files.isEmpty() ? nullptr : &world.files.first();
        LibraryBackup::Data other = contentOf(kept);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        other.writtenAt = databaseTime(now);
        LibraryBackup::Song liked;
        liked.videoId = QStringLiteral("b1");
        liked.title = QStringLiteral("Selftest b1");
        liked.artist = QStringLiteral("Selftest");
        liked.durationMs = 200000;
        liked.at = databaseTime(now.addSecs(-600));
        other.likes.append(liked);
        LibraryBackup::Playlist playlist;
        playlist.uid = QStringLiteral("selftest-b-playlist");
        playlist.name = QStringLiteral("Selftest B");
        playlist.createdAt = databaseTime(now.addSecs(-900));
        playlist.updatedAt = databaseTime(now.addSecs(-300));
        LibraryBackup::Song entry = liked;
        entry.at = databaseTime(now.addSecs(-800));
        playlist.entries.append(entry);
        entry.videoId = QStringLiteral("b5");
        entry.title = QStringLiteral("Selftest b5");
        playlist.entries.append(entry);
        other.playlists.append(playlist);
        for (int i = 0; i < 2; ++i) {
            LibraryBackup::Play play;
            play.kind = QStringLiteral("play");
            play.videoId = QStringLiteral("b1");
            play.title = QStringLiteral("Selftest b1");
            play.artist = QStringLiteral("Selftest");
            play.startedAt = databaseTime(now.addSecs(-1200 - i * 300));
            play.trackMs = 200000;
            play.listenedMs = 190000;
            play.completed = true;
            play.label = 1.0;
            play.source = QStringLiteral("search");
            other.plays.append(play);
        }
        // a1 unliked there, after it was liked here (both in the past: a time
        // later than now would be left out, as from a clock running ahead).
        run(QStringLiteral("UPDATE tracks SET liked_at = datetime('now', '-10 minutes') WHERE source_id = 'a1'"));
        for (LibraryBackup::Song &song : other.likes) {
            if (song.videoId == QLatin1String("a1"))
                song.at = databaseTime(now.addSecs(-600));
        }
        other.removed.append({ LibraryBackup::kRemovedLike, QStringLiteral("a1"), databaseTime(now.addSecs(-60)) });
        if (kept) {
            kept->content = LibraryBackup::toJson(other);
            kept->modified = world.newModified();
        }
        // And a like here that the file has not had yet.
        library->setLiked(track(QStringLiteral("a4")), true);

        const int downloads = world.downloads, updates = world.updates, creates = world.creates;
        sync.requestSync();
        checks.check(waitUntil([&]() { return world.downloads == downloads + 1 && !sync.busy(); }, 3000),
                     QStringLiteral("a file changed elsewhere is read"));
        checks.check(library->isLiked(QStringLiteral("b1")), QStringLiteral("  its like is here now"));
        checks.check(!library->isLiked(QStringLiteral("a1")), QStringLiteral("  its later unlike too"));
        checks.same(one(QStringLiteral("SELECT COUNT(*) FROM playlist_tracks t JOIN playlists p ON p.id = t.playlist_id"
                                       " WHERE p.uid = 'selftest-b-playlist'")).toInt(), 2,
                    QStringLiteral("  its playlist, with both songs"));
        checks.same(one(QStringLiteral("SELECT COUNT(*) FROM play_events WHERE kind = 'play' AND video_id = 'b1'")).toInt(), 2,
                    QStringLiteral("  and its plays"));
        checks.same(world.updates - updates, 1, QStringLiteral("  the merged library is uploaded once"));
        checks.same(world.creates, creates, QStringLiteral("  into the same file"));
        const LibraryBackup::Data merged = contentOf(world.files.isEmpty() ? nullptr : &world.files.first());
        checks.check(likes(merged, QStringLiteral("a4")) && likes(merged, QStringLiteral("b1")) && !likes(merged, QStringLiteral("a1"))
                         && merged.playlists.size() == 2,
                     QStringLiteral("  holding both computers' changes"));
        checks.check(notices.size() == 1, QStringLiteral("  with no toast for a sync of its own"),
                     QString::number(notices.size()));

        const int after = world.creates + world.updates;
        sync.syncNow();
        checks.check(idle(sync) && world.creates + world.updates == after,
                     QStringLiteral("a second sync uploads nothing"));
        sync.requestSync();
        const int finds = world.finds;
        checks.check(waitUntil([&]() { return world.finds == finds + 1 && !sync.busy(); }, 3000)
                         && world.creates + world.updates == after,
                     QStringLiteral("  nor does a third"));
    }

    // ------------------------------------------- what SYNC NOW says
    checks.note(QStringLiteral("- what SYNC NOW says it brought"));
    {
        World::File *kept = world.files.isEmpty() ? nullptr : &world.files.first();
        LibraryBackup::Data other = contentOf(kept);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        // Another computer put a song in a playlist both have, and cleared
        // History, after what this one played.
        for (LibraryBackup::Playlist &playlist : other.playlists) {
            if (playlist.uid != QLatin1String("selftest-b-playlist"))
                continue;
            LibraryBackup::Song entry = likedElsewhere(QStringLiteral("b6"));
            entry.at = databaseTime(now.addSecs(-30));
            playlist.entries.append(entry);
            playlist.updatedAt = entry.at;
        }
        other.removed.append({ LibraryBackup::kRemovedHistoryCleared, QString(), databaseTime(now.addSecs(-20)) });
        if (kept) {
            kept->content = LibraryBackup::toJson(other);
            kept->modified = world.newModified();
        }
        run(QStringLiteral("INSERT OR REPLACE INTO recent (video_id, title, played_at, play_count)"
                           " VALUES ('hx1', 'Selftest hx1', datetime('now', '-10 minutes'), 1)"));
        const qsizetype told = notices.size();
        sync.syncNow();
        checks.check(idle(sync) && notices.size() == told + 1, QStringLiteral("SYNC NOW that brings something says so"),
                     QString::number(notices.size() - told));
        const QString said = notices.value(told);
        checks.check(said.contains(QLatin1String("1 playlist changed")) && !said.contains(QLatin1String("added")),
                     QStringLiteral("  a playlist that only took a song is called changed, not new"), said);
        checks.check(said.contains(QLatin1String("from History removed, as they were deleted on another computer")),
                     QStringLiteral("  and the History a clearing there took out is named"), said);
        checks.check(!sticky.value(told), QStringLiteral("  (it leaves by itself, the window being in front)"));
        checks.same(one(QStringLiteral("SELECT COUNT(*) FROM recent WHERE video_id = 'hx1'")).toInt(), 0,
                    QStringLiteral("  History here from before the clearing is gone"));
    }

    // ---------------------------------------------------- a wrong clock
    checks.note(QStringLiteral("- a clock that is wrong"));
    {
        World::File *kept = world.files.isEmpty() ? nullptr : &world.files.first();
        // Something new in the file, which a wrong clock must not take in.
        LibraryBackup::Data other = contentOf(kept);
        other.likes.append(likedElsewhere(QStringLiteral("k1")));
        if (kept) {
            kept->content = LibraryBackup::toJson(other);
            kept->modified = world.newModified();
        }
        const int downloads = world.downloads, uploads = world.creates + world.updates;
        // Google's clock five hours behind this computer's: this one is fast,
        // as one keeping UTC as local time can be.
        world.drive.dateOffsetSecs = -5 * 3600;
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::Clock,
                     QStringLiteral("a clock five hours fast stops the sync"), sync.status());
        checks.check(world.downloads == downloads && world.creates + world.updates == uploads
                         && !library->isLiked(QStringLiteral("k1")),
                     QStringLiteral("  before anything is read or written"));
        checks.check(sync.status().startsWith(QLatin1String("This computer's clock is 5 hours fast, so syncing has "
                                                            "stopped until it is set right. Trying again at "))
                         && sync.attention(),
                     QStringLiteral("  the row says so, in red"), sync.status());
        world.drive.dateOffsetSecs = 2 * 3600;
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::Clock
                         && sync.status().contains(QLatin1String("2 hours slow")),
                     QStringLiteral("  as does one two hours slow"), sync.status());
        // Within the five minutes allowed: set right.
        world.drive.dateOffsetSecs = 90;
        checks.check(waitUntil([&]() { return sync.problem() == DriveSync::Problem::None && !sync.busy(); }, 3000)
                         && library->isLiked(QStringLiteral("k1")),
                     QStringLiteral("  set right, it syncs again by itself soon after"), sync.status());
        world.drive.dateOffsetSecs = 0;
    }

    // ------------------------------------------------- the access token
    checks.note(QStringLiteral("- the access token"));
    {
        world.expiresIn = 61;   // one second left, after the minute kept in hand
        const int refreshes = world.refreshes;
        sync.auth()->forgetAccessToken();
        sync.syncNow();
        checks.check(idle(sync) && world.refreshes == refreshes + 1, QStringLiteral("a token forgotten is renewed"));
        checks.check(world.lastRefresh.queryItemValue(QStringLiteral("grant_type")) == QLatin1String("refresh_token")
                         && world.lastRefresh.queryItemValue(QStringLiteral("refresh_token"), QUrl::FullyDecoded).toUtf8()
                                == world.currentRefresh
                         && world.lastRefresh.queryItemValue(QStringLiteral("client_id"), QUrl::FullyDecoded).toUtf8() == kClientId,
                     QStringLiteral("  with the stored refresh token and the client"));
        pause(1300);
        sync.syncNow();
        checks.check(idle(sync) && world.refreshes == refreshes + 2,
                     QStringLiteral("a token within a minute of its end is renewed before it is used"));
        world.expiresIn = 3600;
        sync.auth()->forgetAccessToken();
        sync.syncNow();
        idle(sync);
        const int renewed = world.refreshes;
        sync.syncNow();
        checks.check(idle(sync) && world.refreshes == renewed, QStringLiteral("  and one with an hour left is not"));

        // Drive refuses a token before its time: renewed once, sent again once.
        const int unauthorized = world.unauthorized;
        world.invalidTokens.insert(world.currentAccess);
        sync.syncNow();
        checks.check(idle(sync) && world.unauthorized == unauthorized + 1 && world.refreshes == renewed + 1
                         && sync.problem() == DriveSync::Problem::None,
                     QStringLiteral("a 401 renews the token once, and the sync goes on"));
        world.rejectAll = true;
        const int finds = findRequests(world);
        sync.syncNow();
        checks.check(idle(sync) && world.refreshes == renewed + 2 && findRequests(world) == finds + 2,
                     QStringLiteral("  a second 401 is not asked again"),
                     QStringLiteral("%1 renewals, %2 requests").arg(world.refreshes - renewed).arg(findRequests(world) - finds));
        checks.check(sync.problem() != DriveSync::Problem::None && sync.signedIn()
                         && sync.status().contains(QLatin1String("Trying again at")),
                     QStringLiteral("  the sync stops and waits"), sync.status());
        world.rejectAll = false;
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::None, QStringLiteral("  until Drive takes it again"));
    }

    // ------------------------------------------------------- backing off
    checks.note(QStringLiteral("- backing off"));
    {
        world.failNext = { world.failure(503), world.failure(429),
                           world.failure(403, QStringLiteral("rateLimitExceeded")) };
        const int finds = findRequests(world);
        const qsizetype first = world.drive.requests.size();
        sync.syncNow();
        checks.check(waitUntil([&]() { return sync.problem() == DriveSync::Problem::Server; }, 3000),
                     QStringLiteral("a 503 stops the sync"));
        checks.check(sync.status().startsWith(QLatin1String("Google Drive had a problem. Trying again at "))
                         && !sync.error().isEmpty(),
                     QStringLiteral("  the row says when it tries again"), sync.status());
        checks.check(waitUntil([&]() { return findRequests(world) == finds + 4 && !sync.busy(); }, 8000)
                         && sync.problem() == DriveSync::Problem::None,
                     QStringLiteral("  it tries again by itself, through a 429 and Drive's rate limit, and syncs"));
        QList<qint64> times;
        for (qsizetype i = first; i < world.drive.requests.size(); ++i) {
            const StandIn::Request &request = world.drive.requests.at(i);
            if (request.method == "GET" && request.path.startsWith("/drive/v3/files?"))
                times << request.atMs;
        }
        const bool spaced = times.size() >= 4 && times.at(1) - times.at(0) >= 360 && times.at(2) - times.at(1) >= 720
                            && times.at(3) - times.at(2) >= 1080;
        checks.check(spaced, QStringLiteral("  waiting longer each time (here 0.4, 0.8, 1.2 s for 1, 5, 30 minutes)"),
                     times.size() >= 4 ? QStringLiteral("%1, %2, %3 ms").arg(times.at(1) - times.at(0))
                                                .arg(times.at(2) - times.at(1)).arg(times.at(3) - times.at(2))
                                       : QString::number(times.size()));
        world.failNext = { world.failure(500) };
        const qsizetype again = world.drive.requests.size();
        const int before = findRequests(world);
        sync.syncNow();
        checks.check(waitUntil([&]() { return findRequests(world) == before + 2 && !sync.busy(); }, 4000),
                     QStringLiteral("after a sync, the next failure waits the shortest time again"));
        qint64 firstAt = -1, secondAt = -1;
        for (qsizetype i = again; i < world.drive.requests.size(); ++i) {
            const StandIn::Request &request = world.drive.requests.at(i);
            if (request.method == "GET" && request.path.startsWith("/drive/v3/files?"))
                (firstAt < 0 ? firstAt : secondAt) = request.atMs;
        }
        checks.check(secondAt - firstAt >= 360 && secondAt - firstAt < 780, QStringLiteral("  0.4 s here"),
                     QString::number(secondAt - firstAt));
    }

    // ------------------------------------------------ a file that vanished
    checks.note(QStringLiteral("- a file gone while it was read"));
    {
        world.vanishOnRead = true;
        const int finds = world.finds, creates = world.creates;
        sync.syncNow();
        checks.check(idle(sync) && world.finds == finds + 2 && world.creates == creates + 1
                         && sync.problem() == DriveSync::Problem::None && world.files.size() == 1,
                     QStringLiteral("a 404 for the file looks again, and makes it afresh"));
        checks.check(settingOf(QStringLiteral("backup.file_id")) == world.files.value(0).id,
                     QStringLiteral("  and the new file is the one kept"));
    }

    // ----------------------------------------- a read cut off part way
    checks.note(QStringLiteral("- a read cut off part way"));
    {
        World::File &kept = world.files.first();
        kept.modified = world.newModified();   // changed, so it is read
        world.cutNextRead = true;
        const int finds = findRequests(world);
        sync.syncNow();
        checks.check(waitUntil([&]() { return sync.problem() != DriveSync::Problem::None && !sync.busy(); }, 3000)
                         && sync.problem() == DriveSync::Problem::Offline && !sync.held()
                         && settingOf(QStringLiteral("backup.held")).isEmpty(),
                     QStringLiteral("a read cut off part way is no answer, never a damaged file held"), sync.status());
        checks.check(waitUntil([&]() {
                         return sync.problem() == DriveSync::Problem::None && !sync.busy()
                                && findRequests(world) == finds + 2;
                     }, 3000),
                     QStringLiteral("  and the next try reads it whole, by itself"), sync.status());
    }

    // -------------------------------------------- a redirect to elsewhere
    checks.note(QStringLiteral("- a redirect elsewhere"));
    {
        world.redirectTo = (world.elsewhere.base() + QStringLiteral("/drive/v3/files")).toUtf8();
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::Odd,
                     QStringLiteral("a redirect to another host stops the sync"));
        pause(200);
        checks.same(world.elsewhere.requests.size(), 0, QStringLiteral("  and is never followed"));
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::None, QStringLiteral("  the next sync goes on"));
    }

    // -------------------------------------------------- two copies at once
    checks.note(QStringLiteral("- two copies of the file"));
    {
        LibraryBackup::Data second;
        LibraryBackup::Song liked;
        liked.videoId = QStringLiteral("c1");
        liked.title = QStringLiteral("Selftest c1");
        liked.artist = QStringLiteral("Selftest");
        liked.at = databaseTime(QDateTime::currentDateTimeUtc().addSecs(-60));
        second.likes.append(liked);
        world.addFile(LibraryBackup::toJson(second));
        const QString keptId = world.files.first().id;
        const int downloads = world.downloads, updates = world.updates, deletes = world.deletes;
        sync.requestSync();
        checks.check(waitUntil([&]() { return world.deletes == deletes + 1 && !sync.busy(); }, 3000),
                     QStringLiteral("both copies are read and the second deleted"));
        checks.check(world.downloads == downloads + 2 && world.updates == updates + 1 && world.files.size() == 1
                         && world.files.first().id == keptId,
                     QStringLiteral("  the first kept, holding both"));
        checks.check(library->isLiked(QStringLiteral("c1")) && likes(contentOf(&world.files.first()), QStringLiteral("c1")),
                     QStringLiteral("  the second's like here and in the one kept"));
    }

    // ------------------------------------------- a file that cannot be read
    checks.note(QStringLiteral("- a file that cannot be read"));
    {
        World::File &kept = world.files.first();
        const QByteArray damaged = QByteArrayLiteral("{\"format\":\"monolist-library\",\"version\":1,\"likes\":[{\"vid");
        kept.content = damaged;
        kept.modified = world.newModified();
        const int uploads = world.creates + world.updates + world.deletes;
        sync.requestSync();
        checks.check(waitUntil([&]() { return sync.held() && !sync.busy(); }, 3000)
                         && sync.problem() == DriveSync::Problem::Unreadable,
                     QStringLiteral("a damaged file stops the sync"));
        checks.check(sync.status().startsWith(QLatin1String("The backup in your Google Drive could not be read; it was "
                                                            "left as it is")),
                     QStringLiteral("  the row says so"), sync.status());
        checks.check(settingOf(QStringLiteral("backup.held")) == QLatin1String("unreadable"),
                     QStringLiteral("  and remembers it across a restart"));
        {
            DriveSync later(library);
            later.auth()->setTestClient(kClientId, kClientSecret);
            DriveSync::Timing timing;
            timing.launchMs = 60 * 60 * 1000;
            later.setTiming(timing);
            later.start();
            checks.check(later.held() && later.signedIn(), QStringLiteral("  (a new launch starts held)"));
        }
        const int finds = world.finds;
        sync.requestSync();
        sync.noteLibraryEdited();
        pause(600);
        checks.check(world.finds == finds, QStringLiteral("  edits and timers sync nothing while held"));
        sync.syncNow();
        checks.check(idle(sync) && world.finds == finds + 1 && sync.problem() == DriveSync::Problem::Unreadable,
                     QStringLiteral("  SYNC NOW reads it again, and it is still held"));
        checks.check(world.creates + world.updates + world.deletes == uploads && kept.content == damaged,
                     QStringLiteral("  the file is never overwritten"));

        kept.content = QByteArrayLiteral("{\"format\":\"monolist-library\",\"version\":2,\"likes\":[]}");
        kept.modified = world.newModified();
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::Newer
                         && sync.status().contains(QLatin1String("saved by a newer Monolist")),
                     QStringLiteral("a newer Monolist's file is held too, and says so"), sync.status());
        checks.check(world.creates + world.updates + world.deletes == uploads, QStringLiteral("  and is left as it is"));

        kept.content = LibraryBackup::toJson(sync.backup()->snapshot());
        kept.modified = world.newModified();
        sync.syncNow();
        checks.check(idle(sync) && sync.problem() == DriveSync::Problem::None && !sync.held()
                         && settingOf(QStringLiteral("backup.held")).isEmpty(),
                     QStringLiteral("a readable file again: SYNC NOW syncs, and the hold is gone"));
    }

    // ----------------------------------------------- the sign-in ending
    checks.note(QStringLiteral("- a sign-in Google ended"));
    {
        world.refreshError = QByteArrayLiteral("invalid_grant");
        sync.auth()->forgetAccessToken();
        const int finds = world.finds;
        sync.syncNow();
        checks.check(waitUntil([&]() { return sync.state() == QLatin1String("expired"); }, 3000) && idle(sync),
                     QStringLiteral("invalid_grant ends the sign-in"), sync.state());
        checks.check(sync.status().startsWith(QLatin1String("Your Google sign-in has ended")) && !sync.signedIn()
                         && !sync.canSyncNow(),
                     QStringLiteral("  the row says so, and offers signing in again"), sync.status());
        QByteArray stored;
        checks.check(SecretStore::read(DriveAuth::kSecretName, &stored) == SecretStore::Status::NotFound,
                     QStringLiteral("  the refresh token is deleted"));
        checks.check(settingOf(DriveAuth::kEmailKey) == kEmail, QStringLiteral("  the account's email is kept for the row"));
        checks.same(world.finds, finds, QStringLiteral("  nothing went to Drive"));
        {
            DriveSync later(library);
            later.auth()->setTestClient(kClientId, kClientSecret);
            later.start();
            checks.check(later.state() == QLatin1String("expired"), QStringLiteral("  (a new launch shows it ended)"));
        }
        // SIGN IN AGAIN, then CANCEL: the row is as it was, saying why.
        sync.signIn();
        world.secrets.append(QUrlQuery(opened.last()).queryItemValue(QStringLiteral("state")).toUtf8());
        const bool waited = sync.state() == QLatin1String("waiting");
        sync.cancelSignIn();
        checks.check(waited && sync.state() == QLatin1String("expired")
                         && sync.status().startsWith(QLatin1String("Your Google sign-in has ended")),
                     QStringLiteral("SIGN IN AGAIN cancelled: the row still says the sign-in has ended"), sync.status());
        world.refreshError.clear();
    }

    // ------------------------------------------------ again, then out
    checks.note(QStringLiteral("- signing in again, then out"));
    {
        // Something new in Drive meanwhile, and the first sync's upload
        // refused once: it merges, fails, and finishes on its own a moment
        // later, when the toast must still say what it brought.
        World::File &kept = world.files.first();
        LibraryBackup::Data other = contentOf(&kept);
        other.likes.append(likedElsewhere(QStringLiteral("g1")));
        kept.content = LibraryBackup::toJson(other);
        kept.modified = world.newModified();
        // And a like here the file has not had, so there is an upload to refuse.
        library->setLiked(track(QStringLiteral("g2")), true);
        world.failNextUpload = { world.failure(503) };
        const qsizetype told = notices.size();
        const bool again = signInThroughBrowser(checks, world, sync, browser, opened,
                                                QByteArrayLiteral("SELFTEST-CODE-2"), false);
        checks.check(again && waitUntil([&]() { return notices.size() > told && !sync.busy(); }, 5000)
                         && sync.problem() == DriveSync::Problem::None && world.failNextUpload.isEmpty(),
                     QStringLiteral("SIGN IN AGAIN signs in and syncs, through an upload refused once"), sync.status());
        checks.check(notices.value(told) == QLatin1String("From your Google Drive: 1 liked song added") && sticky.value(told),
                     QStringLiteral("  and the first sync's toast, after the retry, says what it brought in"),
                     notices.value(told));
        const QByteArray refresh = world.currentRefresh;
        const qsizetype revoked = world.revoked.size();
        const int deletes = world.deletes;
        sync.signOut();
        checks.check(waitUntil([&]() { return world.revoked.size() == revoked + 1; }, 3000)
                         && world.revoked.last() == refresh,
                     QStringLiteral("SIGN OUT withdraws the sign-in at Google"));
        QByteArray stored;
        checks.check(SecretStore::read(DriveAuth::kSecretName, &stored) == SecretStore::Status::NotFound,
                     QStringLiteral("  deletes the refresh token"));
        bool gone = true;
        for (const char *key : { "backup.google_email", "backup.last_hash", "backup.file_id", "backup.remote_modified",
                                 "backup.last_synced_at", "backup.held" }) {
            if (hasSetting(QString::fromLatin1(key)))
                gone = false;
        }
        checks.check(gone, QStringLiteral("  and the backup's own settings"));
        {
            // The file in the user's Drive names no computer: no device, no
            // host name (which is often the person's own).
            const QByteArray host = QSysInfo::machineHostName().toUtf8();
            const QByteArray content = world.files.isEmpty() ? QByteArray() : world.files.first().content;
            checks.check(!content.isEmpty() && !content.contains("\"device\"")
                             && (host.size() < 3 || !content.contains(host)),
                         QStringLiteral("the file in Drive names no computer"));
        }
        QByteArray jar, session;
        checks.check(SecretStore::read(QStringLiteral("ytmusic.cookies"), &jar) == SecretStore::Status::Ok && jar == kYtmJar
                         && SecretStore::read(QStringLiteral("lastfm.session"), &session) == SecretStore::Status::Ok
                         && session == kLastFmSession,
                     QStringLiteral("  the YouTube Music and Last.fm secrets are untouched"));
        checks.check(settingOf(QStringLiteral("ytmusic.guard")) == QLatin1String("selftest-guard")
                         && settingOf(QStringLiteral("signin.refused_at")) == QLatin1String("selftest-refused")
                         && settingOf(QStringLiteral("lastfm.user")) == QLatin1String("selftest-user"),
                     QStringLiteral("  as are their settings"));
        checks.check(world.files.size() == 1 && world.deletes == deletes,
                     QStringLiteral("  the file in Drive stays"));
        checks.check(library->isLiked(QStringLiteral("b1")) && library->isLiked(QStringLiteral("a4")),
                     QStringLiteral("  and so does the library here"));
        checks.check(sync.state() == QLatin1String("off") && sync.status().contains(QLatin1String("Delete hidden app data")),
                     QStringLiteral("  the row says how to delete the backup"), sync.status());
    }

    // ------------------------------------------------- refusals
    checks.note(QStringLiteral("- a sign-in that is refused"));
    {
        sync.signIn();
        const QUrlQuery query(opened.last());
        const QString redirect = query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded);
        const QString state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
        world.secrets.append(state.toUtf8());
        const int exchanges = world.exchanges;
        Page denied = visit(browser, landing(redirect, { { QStringLiteral("error"), QStringLiteral("access_denied") },
                                                         { QStringLiteral("state"), state } }));
        checks.check(denied.status == 200 && denied.body.contains("You didn't allow Monolist"),
                     QStringLiteral("access_denied gets a page saying so"));
        checks.check(sync.state() == QLatin1String("error")
                         && sync.status() == QLatin1String("You didn't allow it, so Monolist isn't signed in to Google Drive"),
                     QStringLiteral("  and the row: \"You didn't allow it\""), sync.status());
        checks.check(world.exchanges == exchanges && visit(browser, QUrl(redirect + QStringLiteral("/"))).status == 0,
                     QStringLiteral("  nothing is exchanged, and the listener is closed"));

        world.scope = QStringLiteral("openid https://www.googleapis.com/auth/userinfo.email");
        const qsizetype revoked = world.revoked.size();
        const int finds = world.finds;
        const bool signedInWithout = signInThroughBrowser(checks, world, sync, browser, opened,
                                                          QByteArrayLiteral("SELFTEST-CODE-3"), false);
        checks.check(!signedInWithout && sync.state() == QLatin1String("error")
                         && sync.status() == QLatin1String("Monolist needs its folder in your Google Drive. Press TRY "
                                                           "AGAIN and, on Google's page, tick the box for Monolist's "
                                                           "folder before Continue"),
                     QStringLiteral("the Drive box unticked: refused, and the row says to TRY AGAIN and tick it"),
                     sync.status());
        checks.check(waitUntil([&]() { return world.revoked.size() == revoked + 1; }, 3000)
                         && world.revoked.last() == world.currentRefresh,
                     QStringLiteral("  the sign-in Google gave is withdrawn"));
        QByteArray stored;
        checks.check(SecretStore::read(DriveAuth::kSecretName, &stored) == SecretStore::Status::NotFound
                         && world.finds == finds,
                     QStringLiteral("  nothing is kept, and nothing goes to Drive"));
        world.scope = kGrantedScopes;

        sync.signIn();
        const QString cancelled = QUrlQuery(opened.last()).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded);
        world.secrets.append(QUrlQuery(opened.last()).queryItemValue(QStringLiteral("state")).toUtf8());
        sync.cancelSignIn();
        checks.check(sync.state() == QLatin1String("off") && visit(browser, QUrl(cancelled + QStringLiteral("/"))).status == 0,
                     QStringLiteral("CANCEL stops waiting and closes the listener"));

        // A browser that would not open: Google's page to paste by hand,
        // never a link that would open the same way.
        sync.auth()->setUrlOpener([&opened](const QUrl &url) {
            opened.append(url);
            return false;
        });
        sync.signIn();
        const QUrl page = opened.last();
        world.secrets.append(QUrlQuery(page).queryItemValue(QStringLiteral("state")).toUtf8());
        checks.check(sync.browserFailed() && sync.status().contains(QLatin1String("COPY LINK"))
                         && !sync.status().contains(QLatin1String("href")) && !sync.status().contains(QLatin1String("&amp;")),
                     QStringLiteral("a browser that would not open: the row offers COPY LINK"), sync.status());
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            // The user's clipboard is put back after.
            const QString before = clipboard->text();
            const qsizetype told = notices.size();
            sync.copySignInLink();
            const QString copied = clipboard->text();
            checks.check(copied == page.toString(QUrl::FullyEncoded) && QUrl(copied) == page
                             && !copied.contains(QLatin1String("&amp;")),
                         QStringLiteral("  COPY LINK puts Google's whole page on the clipboard"));
            checks.check(notices.value(told).startsWith(QLatin1String("Copied. Paste it into your browser")),
                         QStringLiteral("  and says to paste it"), notices.value(told));
            clipboard->setText(before);
        }
        sync.cancelSignIn();
        sync.auth()->setUrlOpener([&opened](const QUrl &url) {
            opened.append(url);
            return true;
        });

        DriveAuth::Timing quick;
        quick.signInWaitMs = 300;
        quick.timeoutMs = 5000;
        sync.auth()->setTiming(quick);
        sync.signIn();
        world.secrets.append(QUrlQuery(opened.last()).queryItemValue(QStringLiteral("state")).toUtf8());
        const QString waited = QUrlQuery(opened.last()).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded);
        checks.check(waitUntil([&]() { return sync.state() == QLatin1String("error"); }, 2000)
                         && sync.status().startsWith(QLatin1String("Monolist stopped waiting for Google"))
                         && visit(browser, QUrl(waited + QStringLiteral("/"))).status == 0,
                     QStringLiteral("a sign-in left in the browser stops waiting at its limit (ten minutes)"), sync.status());
    }

    // --------------------------------------------- what was ever sent
    checks.note(QStringLiteral("- what went where"));
    {
        bool googleClean = true;
        bool agents = true;
        for (const StandIn::Request &request : world.google.requests) {
            if (request.has("cookie") || request.has("authorization"))
                googleClean = false;
            if (!request.header("user-agent").startsWith("Monolist/"))
                agents = false;
        }
        bool driveClean = true;
        bool bearer = true;
        for (const StandIn::Request &request : world.drive.requests) {
            if (request.has("cookie"))
                driveClean = false;
            if (!request.header("authorization").startsWith("Bearer SELFTEST-ACCESS-"))
                bearer = false;
            if (!request.header("user-agent").startsWith("Monolist/"))
                agents = false;
        }
        checks.check(googleClean && driveClean, QStringLiteral("no request ever carried a cookie"),
                     QStringLiteral("%1 to Google, %2 to Drive").arg(world.google.requests.size()).arg(world.drive.requests.size()));
        checks.check(googleClean && bearer, QStringLiteral("the access token went to Drive alone, with every request to it"));
        checks.check(world.elsewhere.requests.isEmpty(), QStringLiteral("nothing reached any other host"));
        checks.check(agents, QStringLiteral("every request said it was Monolist"));

        // The secrets Monolist holds, nowhere in the settings table.
        QStringList values;
        QSqlQuery settings(AppDatabase::connection());
        settings.exec(QStringLiteral("SELECT key, value FROM settings"));
        while (settings.next())
            values << settings.value(0).toString() + QLatin1Char('=') + settings.value(1).toString();
        const QByteArray table = values.join(QLatin1Char('\n')).toUtf8();
        bool tableClean = !table.contains(kClientSecret) && !table.contains(kClientId) && !table.contains("SELFTEST-ACCESS-")
                          && !table.contains("SELFTEST-REFRESH-") && !table.contains("SELFTEST-CODE-");
        for (const QByteArray &secret : std::as_const(world.secrets)) {
            if (!secret.isEmpty() && table.contains(secret))
                tableClean = false;
        }
        checks.check(tableClean, QStringLiteral("no token, code, verifier, state or secret is in the settings table"));
    }

    // Done with the stand-ins and the planted secrets.
    owned.reset();
    SecretStore::remove(DriveAuth::kSecretName);
    SecretStore::remove(QStringLiteral("ytmusic.cookies"));
    SecretStore::remove(QStringLiteral("lastfm.session"));
    for (const char *key : { "ytmusic.guard", "signin.refused_at", "lastfm.user" })
        run(QStringLiteral("DELETE FROM settings WHERE key = '%1'").arg(QString::fromLatin1(key)));
    emptyLibrary(library);

    // ------------------------------------------------------- the log
    {
        QList<QPair<QByteArray, QString>> watched{
            { kClientId, QStringLiteral("the client's id") },
            { kClientSecret, QStringLiteral("the client's secret") },
            { QByteArrayLiteral("SELFTEST-ACCESS-"), QStringLiteral("an access token") },
            { QByteArrayLiteral("SELFTEST-REFRESH-"), QStringLiteral("a refresh token") },
            { QByteArrayLiteral("SELFTEST-CODE-"), QStringLiteral("a code") },
            { QByteArrayLiteral("SELFTESTSIGNATURE"), QStringLiteral("an id_token") },
            { QByteArrayLiteral("code_challenge"), QStringLiteral("the sign-in page's address") },
            { QByteArrayLiteral("Bearer"), QStringLiteral("an Authorization header") },
            { kEmail.toUtf8(), QStringLiteral("the email") },
            { kYtmJar, QStringLiteral("the YouTube Music secret") },
            { kLastFmSession, QStringLiteral("the Last.fm secret") },
        };
        for (const QByteArray &secret : std::as_const(world.secrets)) {
            if (secret.size() >= 8)
                watched.append({ secret, QStringLiteral("a value Google or Monolist sent") });
        }
        QStringList found;
        const QStringList lines = captured;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).toUtf8();
            for (const auto &item : std::as_const(watched)) {
                if (line.contains(item.first)) {
                    found << QStringLiteral("line %1: %2").arg(i).arg(item.second);
                    break;
                }
            }
        }
        checks.check(found.isEmpty(),
                     QStringLiteral("no token, code, verifier, state, secret, id_token or email reached the log (%1 lines)")
                         .arg(lines.size()),
                     found.join(QStringLiteral("; ")));
    }

    qInstallMessageHandler(g_previousHandler);
    g_captured = nullptr;
    return checks.finish();
}
