#include "justplayedselftest.h"

#include "appdatabase.h"
#include "artistlinks.h"
#include "catalog.h"
#include "innertube.h"
#include "justplayed.h"
#include "library.h"
#include "listentracker.h"
#include "rec/matchkey.h"
#include "rec/sessionrows.h"
#include "rec/shelves.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>

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
        qWarning("just-played-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("just-played-test: %d checks, %d failed", m_count, m_failed);
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

// The clocks JustPlayed and ListenTracker read: this computer's, moved on
// by however much the test has let pass (an hour, a day) in an instant.
qint64 g_offsetMs = 0;

qint64 fakeNow()
{
    return QDateTime::currentMSecsSinceEpoch() + g_offsetMs;
}

void passMs(qint64 ms)
{
    g_offsetMs += ms;
}

// What the program logs, kept to look for anything secret in it.
QStringList *g_log = nullptr;
QtMessageHandler g_previousHandler = nullptr;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_log)
        g_log->append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    else
        std::fprintf(stderr, "%s\n", qPrintable(message));
}

// The invented session the fake account hook hands out. Never sent: the
// test fails if any request, or any log line, carries it.
const QByteArray kFakeCookie = "SAPISID=TESTJUSTPLAYEDSAPISID; LOGIN_INFO=TESTJUSTPLAYEDLOGIN";

// ------------------------------------------------------------- the stand-in

// YouTube Music on this computer: every request kept with its headers, and
// answered as `respond` says. One request a connection.
class StandIn : public QObject
{
public:
    struct Request {
        QByteArray path;
        QList<QPair<QByteArray, QByteArray>> headers;   // names in lower case
        QJsonObject body;

        bool has(const char *name) const
        {
            for (const auto &header : headers) {
                if (header.first == name)
                    return true;
            }
            return false;
        }
        QString videoId() const { return body.value(QLatin1String("videoId")).toString(); }
    };
    struct Answer {
        int status = 200;
        QByteArray body = "{}";
        QByteArray type = "application/json; charset=UTF-8";
        int delayMs = 0;
    };

    std::function<Answer(const Request &request)> respond;
    QList<Request> requests;
    int open = 0;
    int maxOpen = 0;

    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &StandIn::accept);
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    // The /next requests, in the order they came, by the song asked about.
    QStringList nexts() const
    {
        QStringList ids;
        for (const Request &request : requests) {
            if (request.path.startsWith("/youtubei/v1/next"))
                ids << request.videoId();
        }
        return ids;
    }
    int count(const char *pathStart) const
    {
        int n = 0;
        for (const Request &request : requests)
            n += request.path.startsWith(pathStart) ? 1 : 0;
        return n;
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
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        Request request;
        request.path = lines.value(0).trimmed().split(' ').value(1);
        int length = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = int(line.indexOf(':'));
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            request.headers.append({ name, value });
            if (name == "content-length")
                length = value.toInt();
        }
        if (buffer.size() < end + 4 + length)
            return;
        request.body = QJsonDocument::fromJson(buffer.mid(end + 4, length)).object();
        buffer.clear();
        requests.append(request);
        ++open;
        maxOpen = qMax(maxOpen, open);

        const Answer answer = respond ? respond(request) : Answer();
        // An anonymous cookie on every answer, as YouTube sets one: a jar
        // that kept it would send it back.
        const QByteArray out = "HTTP/1.1 " + QByteArray::number(answer.status)
                               + (answer.status == 200 ? " OK" : " Error") + "\r\n"
                               "Content-Type: " + answer.type + "\r\n"
                               "Set-Cookie: JPTEST=anon; Path=/\r\n"
                               "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + answer.body;
        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(answer.delayMs, this, [this, guarded, out]() {
            --open;
            if (!guarded)
                return;
            guarded->write(out);
            guarded->disconnectFromHost();
        });
    }

    QTcpServer m_server;
};

// ------------------------------------------------------- songs and radios

// An 11-character video id, as YouTube's are: a tag and a number.
QString makeId(const QString &tag, int n)
{
    return tag + QString::number(n).rightJustified(11 - int(tag.size()), QLatin1Char('0'));
}

Rec::RadioTrack song(const QString &id, const QString &title, const QString &artist)
{
    Rec::RadioTrack track;
    track.videoId = id;
    track.title = title;
    track.artist = artist;
    track.primaryArtist = artist;
    track.durationMs = 180000;
    return track;
}

// `count` songs tagged `tag`, by `artists` artists named after `stem`, in
// turn: none of them more than three times while count <= 3 * artists.
QVector<Rec::RadioTrack> family(const QString &tag, const QString &stem, int artists, int count)
{
    QVector<Rec::RadioTrack> tracks;
    for (int i = 0; i < count; ++i)
        tracks.append(song(makeId(tag, i + 1), QStringLiteral("%1 song %2").arg(tag).arg(i + 1),
                           QStringLiteral("%1 %2").arg(stem).arg(i % artists)));
    return tracks;
}

// A seed's name and artist, from its id: "sd000000007" is "Seed sd0007".
QString seedTitleOf(const QString &id)
{
    return QStringLiteral("Seed ") + id.left(2) + id.right(4);
}

QString seedArtistOf(const QString &id)
{
    return QStringLiteral("Seed Artist ") + id.left(2) + id.right(4);
}

// What the stand-in answers for a seed: the seed first, as YouTube Music's
// radio starts with it, then 30 songs by 12 artists of its own.
QVector<Rec::RadioTrack> radioOf(const QString &seed)
{
    QVector<Rec::RadioTrack> tracks{ song(seed, seedTitleOf(seed), seedArtistOf(seed)) };
    tracks += family(QStringLiteral("r") + seed.left(2) + seed.right(2),
                     QStringLiteral("Artist ") + seed.left(2) + seed.right(4), 12, 30);
    return tracks;
}

QList<InnerTube::Track> asTracks(const QVector<Rec::RadioTrack> &radio)
{
    QList<InnerTube::Track> tracks;
    for (const Rec::RadioTrack &from : radio) {
        InnerTube::Track track;
        track.videoId = from.videoId;
        track.title = from.title;
        track.artist = from.artist;
        track.primaryArtist = from.primaryArtist;
        track.durationMs = from.durationMs;
        tracks.append(track);
    }
    return tracks;
}

QJsonObject object(std::initializer_list<std::pair<const char *, QJsonValue>> fields)
{
    QJsonObject result;
    for (const auto &field : fields)
        result.insert(QLatin1String(field.first), field.second);
    return result;
}

QJsonObject runsOf(const QString &text)
{
    return object({ { "runs", QJsonArray{ object({ { "text", text } }) } } });
}

// /next's answer, shaped as the real one is (October 2026): the queue's
// rows, each with its title, the artist's linked name and its length.
QByteArray radioAnswer(const QVector<Rec::RadioTrack> &tracks)
{
    QJsonArray rows;
    for (const Rec::RadioTrack &track : tracks) {
        const QJsonObject page = object({ { "browseEndpoint", object({
            { "browseId", QStringLiteral("UCjp_") + QString(track.artist).replace(QLatin1Char(' '), QLatin1Char('_')) },
            { "browseEndpointContextSupportedConfigs", object({ { "browseEndpointContextMusicConfig",
                object({ { "pageType", QStringLiteral("MUSIC_PAGE_TYPE_ARTIST") } }) } }) } }) } });
        const QJsonObject byline = object({ { "runs", QJsonArray{
            object({ { "text", track.artist }, { "navigationEndpoint", page } }) } } });
        rows.append(object({ { "playlistPanelVideoRenderer", object({
            { "videoId", track.videoId },
            { "title", runsOf(track.title) },
            { "longBylineText", byline },
            { "lengthText", runsOf(QStringLiteral("3:00")) },
            { "thumbnail", object({ { "thumbnails", QJsonArray{ object({
                { "url", QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(track.videoId) } }) } } }) },
            { "navigationEndpoint", object({ { "watchEndpoint", object({ { "videoId", track.videoId } }) } }) } }) } }));
    }
    const QJsonObject queue = object({ { "musicQueueRenderer", object({ { "content", object({
        { "playlistPanelRenderer", object({ { "contents", rows } }) } }) } }) } });
    const QJsonObject tab = object({ { "tabRenderer", object({ { "content", queue } }) } });
    const QJsonObject root = object({ { "contents", object({ { "singleColumnMusicWatchNextResultsRenderer",
        object({ { "tabbedRenderer", object({ { "watchNextTabbedResultsRenderer",
            object({ { "tabs", QJsonArray{ tab } } }) } }) } }) } }) } });
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// A song as playback hands it to ListenTracker (QueueModel's map).
QVariantMap trackOf(const QString &id, qint64 durationMs = 180000)
{
    return {
        { QStringLiteral("sourceId"), id },
        { QStringLiteral("title"), seedTitleOf(id) },
        { QStringLiteral("artist"), seedArtistOf(id) },
        { QStringLiteral("durationMs"), durationMs }
    };
}

// Heard from `fromSecs` to `toSecs`, a second at a time.
void hear(ListenTracker &tracker, int fromSecs, int toSecs)
{
    for (int s = fromSecs; s <= toSecs; ++s)
        tracker.positionChanged(qint64(s) * 1000);
}

// A new listen of `id`, heard for `seconds`.
void listen(ListenTracker &tracker, const QString &id, bool chosen, int seconds, qint64 durationMs = 180000)
{
    tracker.begin(trackOf(id, durationMs), chosen);
    tracker.setPlaying(true);
    hear(tracker, 1, seconds);
}

void wire(ListenTracker &tracker, JustPlayed *justPlayed)
{
    QObject::connect(&tracker, &ListenTracker::listenSettled, justPlayed, &JustPlayed::noteSettled);
    QObject::connect(&tracker, &ListenTracker::listenQualified, justPlayed, &JustPlayed::noteQualified);
}

// ------------------------------------------------------------ the library

void exec(const QString &sql)
{
    QSqlQuery query(AppDatabase::connection());
    query.exec(sql);
}

qint64 scalar(const QString &sql, const QVariantList &binds = {})
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(sql);
    for (const QVariant &value : binds)
        query.addBindValue(value);
    if (query.exec() && query.next())
        return query.value(0).toLongLong();
    return -1;
}

// A play in play_events, `when` as SQLite's modifier ("-60 seconds"), and the
// song in Recently played.
void addPlay(const QString &id, const QString &when, const QVariant &label, qint64 listenedMs = 180000,
             const QString &source = QStringLiteral("search"))
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms, listened_ms, label, source)"
        " VALUES ('play', ?, ?, ?, datetime('now', ?), 180000, ?, ?, ?)"));
    query.addBindValue(id);
    query.addBindValue(seedTitleOf(id));
    query.addBindValue(seedArtistOf(id));
    query.addBindValue(when);
    query.addBindValue(listenedMs);
    query.addBindValue(label);
    query.addBindValue(source);
    query.exec();
    QSqlQuery recent(AppDatabase::connection());
    recent.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO recent (video_id, title, artist, played_at) VALUES (?, ?, ?, datetime('now', ?))"));
    recent.addBindValue(id);
    recent.addBindValue(seedTitleOf(id));
    recent.addBindValue(seedArtistOf(id));
    recent.addBindValue(when);
    recent.exec();
}

void addLike(const QString &id)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT INTO play_events (kind, video_id, title, artist, started_at) VALUES ('like', ?, ?, ?, datetime('now'))"));
    query.addBindValue(id);
    query.addBindValue(seedTitleOf(id));
    query.addBindValue(seedArtistOf(id));
    query.exec();
}

// Each group starts from nothing: no plays, no radios, no pace, the switch
// at its default, and the clocks at this computer's.
void freshState()
{
    exec(QStringLiteral("DELETE FROM rec_radio"));
    exec(QStringLiteral("DELETE FROM play_events"));
    exec(QStringLiteral("DELETE FROM recent"));
    exec(QStringLiteral("DELETE FROM home_cache"));
    exec(QStringLiteral("DELETE FROM settings WHERE key LIKE 'home.justPlayed%'"));
    g_offsetMs = 0;
}

int radioRows(const QString &id)
{
    return int(scalar(QStringLiteral("SELECT COUNT(*) FROM rec_radio WHERE video_id = ?"), { id }));
}

QJsonObject paceOf(Library *library)
{
    return QJsonDocument::fromJson(library->settingValue(QStringLiteral("home.justPlayed.pace")).toUtf8()).object();
}

// The look-ups the stored pace counts (those of the last day).
int sentCount(Library *library)
{
    return int(paceOf(library).value(QLatin1String("sent")).toArray().size());
}

// How long the stored rest has to go, in seconds of the fake clock.
qint64 restLeft(Library *library)
{
    return qint64(paceOf(library).value(QLatin1String("restUntil")).toDouble()) - fakeNow() / 1000;
}

// ---------------------------------------------------------------- rows

// What the rows show, as one string: keys, titles and songs.
QString signatureOf(const QVariantList &rows)
{
    QStringList parts;
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        parts << row.value(QStringLiteral("key")).toString() << row.value(QStringLiteral("title")).toString();
        for (const QVariant &card : row.value(QStringLiteral("items")).toList())
            parts << card.toMap().value(QStringLiteral("videoId")).toString();
        parts << QStringLiteral("|");
    }
    return parts.join(QLatin1Char(' '));
}

QString signatureOf(const QVector<Rec::SessionRow> &rows)
{
    QStringList parts;
    for (const Rec::SessionRow &row : rows) {
        parts << row.key << row.title;
        for (const Rec::RadioTrack &track : row.items)
            parts << track.videoId;
        parts << QStringLiteral("|");
    }
    return parts.join(QLatin1Char(' '));
}

const Rec::SessionRow *rowOfKind(const QVector<Rec::SessionRow> &rows, const QString &kind)
{
    for (const Rec::SessionRow &row : rows) {
        if (row.kind == kind)
            return &row;
    }
    return nullptr;
}

bool hasSeed(const QVector<Rec::SessionSeed> &seeds, const QString &id)
{
    for (const Rec::SessionSeed &seed : seeds) {
        if (seed.videoId == id)
            return true;
    }
    return false;
}

Rec::SessionSeed seed(const QString &id, const QString &title, const QString &artist, qint64 lastAt,
                      bool chosen = true, bool liked = false, int listens = 1)
{
    Rec::SessionSeed made;
    made.videoId = id;
    made.title = title;
    made.artist = artist;
    made.artistName = artist;
    made.leadKey = Rec::suggestionArtistKey(artist);
    made.lastAt = lastAt;
    made.chosen = chosen;
    made.liked = liked;
    made.listens = listens;
    return made;
}

quint64 songKeyOf(const Rec::RadioTrack &track)
{
    return Rec::strictKey(Rec::cleanSeedTitle(track.title, track.artist), Rec::radioLeadKey(track));
}

JustPlayed::Timing quick()
{
    JustPlayed::Timing timing;
    timing.launchDelayMs = 0;
    timing.debounceMs = 100;
    timing.gapMs = 300;
    timing.jitterMs = 0;
    timing.recheckMs = 50;
    return timing;
}

std::unique_ptr<JustPlayed> started(const JustPlayed::Timing &timing)
{
    auto justPlayed = std::make_unique<JustPlayed>();
    justPlayed->setTiming(timing);
    justPlayed->load();
    return justPlayed;
}

} // namespace

int runJustPlayedSelfTest(Library *library)
{
    Checks t;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes plays, history and settings"));
        return t.finish();
    }
    StandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in YouTube Music listens on this computer")))
        return t.finish();

    // How the stand-in answers /next, by the song asked about: a radio of
    // its own unless a test says otherwise.
    QHash<QString, QString> modes;
    QHash<QString, int> delays;
    standIn.respond = [&modes, &delays](const StandIn::Request &request) {
        StandIn::Answer answer;
        if (!request.path.startsWith("/youtubei/v1/next"))
            return answer;   // "{}": a browse with nothing on it
        const QString id = request.videoId();
        answer.delayMs = delays.value(id);
        const QString mode = modes.value(id);
        if (mode == QLatin1String("429")) {
            answer.status = 429;
            answer.body = R"({"error":{"code":429}})";
        } else if (mode == QLatin1String("500")) {
            answer.status = 500;
            answer.body = R"({"error":{"code":500}})";
        } else if (mode == QLatin1String("400")) {
            answer.status = 400;
            answer.body = R"({"error":{"code":400,"status":"FAILED_PRECONDITION"}})";
        } else if (mode == QLatin1String("html")) {
            answer.type = "text/html; charset=utf-8";
            answer.body = "<html><body>Our systems have detected unusual traffic from your computer network."
                          "</body></html>";
        } else if (mode == QLatin1String("empty")) {
            answer.body = radioAnswer({});
        } else {
            answer.body = radioAnswer(radioOf(id));
        }
        return answer;
    };
    InnerTube::setTestServer(standIn.base());
    qWarning("just-played-test: YouTube Music is %s; nothing leaves this computer", qPrintable(standIn.base()));

    // A session that is signed in, as far as InnerTube can tell, counting
    // every time it is asked. Home's rows must never ask it.
    int admitted = 0;
    int headed = 0;
    {
        InnerTube::AccountHook hook;
        hook.admit = [&admitted](const InnerTube::AccountRequest &, QString *) -> qint64 {
            ++admitted;
            return 0;
        };
        hook.headers = [&headed](const InnerTube::AccountRequest &, InnerTube::AccountHeaders *headers) -> quint64 {
            ++headed;
            headers->cookie = kFakeCookie;
            headers->authorization = "SAPISIDHASH TESTJUSTPLAYEDHASH";
            return 7;
        };
        InnerTube::setAccountHook(hook);
    }

    QStringList log;
    g_log = &log;
    g_previousHandler = qInstallMessageHandler(captureMessage);
    JustPlayed::setClock(fakeNow);
    ListenTracker::setClock(fakeNow);

    // ------------------------------------------------ 1-6. what is sent, and when
    {
        freshState();
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        int rowsChanged = 0;
        QObject::connect(justPlayed.get(), &JustPlayed::rowsChanged, justPlayed.get(), [&rowsChanged]() { ++rowsChanged; });
        pause(100);

        // 1. a song chosen, heard 45 s
        const QString x = makeId(QStringLiteral("sd"), 1);
        listen(tracker, x, true, 44);
        pause(200);
        t.check(standIn.nexts().isEmpty() && justPlayed->seeds().isEmpty(),
                QStringLiteral("1  a song chosen, heard 44 s: no seed yet, nothing sent"));
        hear(tracker, 45, 45);
        t.check(justPlayed->seeds().size() == 1 && justPlayed->seeds().first().videoId == x
                    && justPlayed->seeds().first().chosen,
                QStringLiteral("1  at 45 s it is a seed, chosen"));
        pause(30);
        t.check(standIn.nexts().isEmpty(), QStringLiteral("1  and nothing is sent within the debounce"));
        waitUntil([&]() { return !standIn.nexts().isEmpty() && !justPlayed->inFlight(); }, 3000);
        pause(400);
        t.check(standIn.nexts() == QStringList{ x }, QStringLiteral("1  then exactly one POST /youtubei/v1/next"),
                standIn.nexts().join(QLatin1Char(',')));
        const StandIn::Request first = standIn.requests.isEmpty() ? StandIn::Request() : standIn.requests.constLast();
        t.check(first.path.startsWith("/youtubei/v1/next?prettyPrint=false") && first.videoId() == x
                    && first.body.value(QLatin1String("playlistId")).toString() == QLatin1String("RDAMVM") + x
                    && first.body.value(QLatin1String("params")).toString() == QLatin1String("wAEB"),
                QStringLiteral("1  for videoId X, playlistId RDAMVMX, params wAEB"), QString::fromLatin1(first.path));
        const QVariantList rows = justPlayed->rows();
        const QVariantMap s1 = rows.value(0).toMap();
        t.check(rows.size() == 1 && s1.value(QStringLiteral("kind")).toString() == QLatin1String("s1")
                    && s1.value(QStringLiteral("title")).toString() == QStringLiteral("More like ") + seedTitleOf(x)
                    && s1.value(QStringLiteral("items")).toList().size() == 12
                    && s1.value(QStringLiteral("items")).toList().value(0).toMap().value(QStringLiteral("suggestion")).toBool(),
                QStringLiteral("1  and the answer is row 01, More like <it>, 12 cards"), signatureOf(rows).left(120));
        t.check(radioRows(x) == 1, QStringLiteral("1  its radio is kept in rec_radio"));

        // 2. nothing of the account, nor any cookie
        t.check(!first.has("cookie") && !first.has("authorization") && !first.has("x-goog-authuser")
                    && !first.has("x-goog-visitor-id") && !first.has("x-origin"),
                QStringLiteral("2  no Cookie, Authorization, X-Goog-AuthUser or X-Goog-Visitor-Id on it"));
        t.check(admitted == 0 && headed == 0,
                QStringLiteral("2  the account's hook was asked 0 times (admit, headers), with a session signed in"),
                QStringLiteral("admit %1, headers %2").arg(admitted).arg(headed));
        bool logged = false;
        for (const QString &line : std::as_const(log))
            logged = logged || (line.contains(QLatin1String("home: songs like")) && line.contains(x)
                                && line.contains(QLatin1String("asked signed out")));
        t.check(logged, QStringLiteral("2  the log says it was asked signed out"));

        // 3. a skip, and a seek to the end
        const int before = int(standIn.nexts().size());
        const QString skipped = makeId(QStringLiteral("sd"), 2);
        listen(tracker, skipped, true, 10);
        const QString sought = makeId(QStringLiteral("sd"), 3);
        listen(tracker, sought, true, 3);
        tracker.seeked(176000);
        tracker.positionChanged(177000);
        tracker.positionChanged(178000);
        tracker.positionChanged(179000);
        tracker.begin(trackOf(makeId(QStringLiteral("sd"), 4)), true);   // Next
        pause(500);
        t.check(int(standIn.nexts().size()) == before && !hasSeed(justPlayed->seeds(), skipped)
                    && !hasSeed(justPlayed->seeds(), sought) && justPlayed->seeds().size() == 1,
                QStringLiteral("3  a skip at 10 s, and a seek to the end then Next: 0 requests, 0 seeds"));

        // 4. a song autoplay added
        const QString autoplay = makeId(QStringLiteral("sd"), 5);
        listen(tracker, autoplay, false, 46);
        pause(300);
        t.check(!hasSeed(justPlayed->seeds(), autoplay) && int(standIn.nexts().size()) == before,
                QStringLiteral("4  an autoplay song heard 46 s: not a seed"));
        hear(tracker, 47, 89);
        t.check(!hasSeed(justPlayed->seeds(), autoplay), QStringLiteral("4  nor at 89 s"));
        hear(tracker, 90, 90);
        const bool seeded = hasSeed(justPlayed->seeds(), autoplay) && !justPlayed->seeds().first().chosen;
        t.check(seeded, QStringLiteral("4  at half the track (90 s) it is one, as autoplay's"));
        waitUntil([&]() { return standIn.nexts().contains(autoplay) && !justPlayed->inFlight(); }, 3000);
        t.check(standIn.nexts().count(autoplay) == 1, QStringLiteral("4  and its radio is asked for once"));

        // 5. repeat-one
        const QString loop = makeId(QStringLiteral("sd"), 6);
        listen(tracker, loop, true, 46);
        waitUntil([&]() { return standIn.nexts().contains(loop) && !justPlayed->inFlight(); }, 3000);
        pause(100);
        const int changedAfterFirst = rowsChanged;
        for (int i = 1; i < 30; ++i) {
            tracker.restart();
            hear(tracker, 1, 46);
        }
        pause(600);
        const QVector<Rec::SessionSeed> looped = justPlayed->seeds();
        t.check(standIn.nexts().count(loop) == 1, QStringLiteral("5  one song settled 30 times (repeat-one): 1 request"),
                QString::number(standIn.nexts().count(loop)));
        t.check(looped.size() == 1 && looped.first().videoId == loop && looped.first().listens == 30,
                QStringLiteral("5  one seed, with listens = 30"),
                looped.isEmpty() ? QStringLiteral("none") : QString::number(looped.first().listens));
        t.check(rowsChanged == changedAfterFirst, QStringLiteral("5  and no rowsChanged after the first"),
                QStringLiteral("%1 after %2").arg(rowsChanged).arg(changedAfterFirst));

        // 6. two seeds a second apart
        const int from = int(standIn.requests.size());
        standIn.maxOpen = 0;
        const QString older = makeId(QStringLiteral("sd"), 7);
        const QString newer = makeId(QStringLiteral("sd"), 8);
        delays.insert(newer, 150);
        listen(tracker, older, true, 46);
        passMs(1000);
        listen(tracker, newer, true, 46);
        qint64 firstAnswered = 0;
        qint64 secondAsked = 0;
        QElapsedTimer clock;
        clock.start();
        waitUntil([&]() {
            const int since = int(standIn.requests.size()) - from;
            if (since >= 1 && firstAnswered == 0 && !justPlayed->inFlight())
                firstAnswered = clock.elapsed();
            if (since >= 2 && secondAsked == 0)
                secondAsked = clock.elapsed();
            return since >= 2 && !justPlayed->inFlight();
        }, 4000);
        const QList<StandIn::Request> sent = standIn.requests.mid(from);
        t.check(sent.size() == 2 && sent.at(0).videoId() == newer && sent.at(1).videoId() == older,
                QStringLiteral("6  two seeds 1 s apart: the newer one's asked first, then the older"),
                sent.isEmpty() ? QStringLiteral("none") : sent.at(0).videoId());
        t.check(firstAnswered > 0 && secondAsked - firstAnswered >= 280,
                QStringLiteral("6  the second only after the gap (300 ms here)"),
                QStringLiteral("%1 ms").arg(secondAsked - firstAnswered));
        t.check(standIn.maxOpen == 1, QStringLiteral("6  and never two in flight"), QString::number(standIn.maxOpen));
        bool anyCookie = false;
        for (const StandIn::Request &request : std::as_const(standIn.requests))
            anyCookie = anyCookie || request.has("cookie");
        t.check(!anyCookie, QStringLiteral("6  the stand-in's Set-Cookie is never sent back (cookieless)"));
    }

    // ------------------------------------------------ 7. the hour's and the day's caps
    {
        freshState();
        JustPlayed::Timing timing = quick();
        timing.hourCap = 3;
        ListenTracker tracker;
        auto justPlayed = started(timing);
        wire(tracker, justPlayed.get());
        const int from = int(standIn.nexts().size());
        const auto askedSince = [&]() { return int(standIn.nexts().size()) - from; };
        for (int k = 1; k <= 3; ++k) {
            listen(tracker, makeId(QStringLiteral("cp"), k), true, 46);
            waitUntil([&]() { return askedSince() == k && !justPlayed->inFlight(); }, 3000);
        }
        const QString fourth = makeId(QStringLiteral("cp"), 4);
        listen(tracker, fourth, true, 46);
        pause(700);
        t.check(askedSince() == 3 && justPlayed->queued().contains(fourth),
                QStringLiteral("7  hourCap 3: a 4th seed waits"), QString::number(askedSince()));
        passMs(3600 * 1000);
        waitUntil([&]() { return askedSince() == 4 && !justPlayed->inFlight(); }, 3000);
        t.check(askedSince() == 4 && standIn.nexts().constLast() == fourth,
                QStringLiteral("7  and goes once the hour has passed on the clock"));
        for (int k = 5; k <= 6; ++k) {
            listen(tracker, makeId(QStringLiteral("cp"), k), true, 46);
            waitUntil([&]() { return askedSince() == k && !justPlayed->inFlight(); }, 3000);
        }
        justPlayed.reset();
        auto again = started(timing);
        wire(tracker, again.get());
        listen(tracker, makeId(QStringLiteral("cp"), 7), true, 46);
        pause(700);
        t.check(askedSince() == 6, QStringLiteral("7  the hour's count survives a new JustPlayed"),
                QString::number(askedSince()));
        passMs(3600 * 1000);
        waitUntil([&]() { return askedSince() == 7; }, 3000);
        t.check(askedSince() == 7, QStringLiteral("7  which asks once its hour is over"));
        waitUntil([&]() { return !again->inFlight(); }, 3000);
        again.reset();

        // the day's
        freshState();
        timing = quick();
        timing.dayCap = 3;
        auto daily = started(timing);
        wire(tracker, daily.get());
        const int dayFrom = int(standIn.nexts().size());
        const auto askedToday = [&]() { return int(standIn.nexts().size()) - dayFrom; };
        for (int k = 1; k <= 3; ++k) {
            listen(tracker, makeId(QStringLiteral("dc"), k), true, 46);
            waitUntil([&]() { return askedToday() == k && !daily->inFlight(); }, 3000);
        }
        listen(tracker, makeId(QStringLiteral("dc"), 4), true, 46);
        pause(500);
        passMs(2 * 3600 * 1000);
        pause(300);
        t.check(askedToday() == 3, QStringLiteral("7  dayCap 3: a 4th waits, past the hour too"),
                QString::number(askedToday()));
        daily.reset();
        auto nextDay = started(timing);
        wire(tracker, nextDay.get());
        listen(tracker, makeId(QStringLiteral("dc"), 5), true, 46);
        pause(500);
        t.check(askedToday() == 3, QStringLiteral("7  the day's count survives a new JustPlayed"));
        passMs((22 * 3600 + 60) * 1000);
        waitUntil([&]() { return askedToday() == 4; }, 3000);
        t.check(askedToday() == 4, QStringLiteral("7  and it asks once the day is over"));
        waitUntil([&]() { return !nextDay->inFlight(); }, 3000);
        nextDay.reset();

        // counted back from now, not by the clock's hours: three asked
        // across the end of one hour leave room for one, not three, at the
        // start of the next
        freshState();
        timing = quick();
        timing.hourCap = 3;
        auto rolling = started(timing);
        wire(tracker, rolling.get());
        const int rollFrom = int(standIn.nexts().size());
        const auto askedRolling = [&]() { return int(standIn.nexts().size()) - rollFrom; };
        listen(tracker, makeId(QStringLiteral("rl"), 1), true, 46);
        waitUntil([&]() { return askedRolling() == 1 && !rolling->inFlight(); }, 3000);
        passMs(50 * 60 * 1000);
        for (int k = 2; k <= 3; ++k) {
            listen(tracker, makeId(QStringLiteral("rl"), k), true, 46);
            waitUntil([&]() { return askedRolling() == k && !rolling->inFlight(); }, 3000);
        }
        passMs(15 * 60 * 1000);
        listen(tracker, makeId(QStringLiteral("rl"), 4), true, 46);
        waitUntil([&]() { return askedRolling() == 4 && !rolling->inFlight(); }, 3000);
        listen(tracker, makeId(QStringLiteral("rl"), 5), true, 46);
        pause(700);
        t.check(askedRolling() == 4, QStringLiteral("7  any hour: at 65 min the first is an hour old, one more goes, then a wait"),
                QString::number(askedRolling()));
        passMs(50 * 60 * 1000);
        waitUntil([&]() { return askedRolling() == 5 && !rolling->inFlight(); }, 3000);
        t.check(askedRolling() == 5, QStringLiteral("7  until the two of 50 min are an hour old"));
        waitUntil([&]() { return !rolling->inFlight(); }, 3000);
        rolling.reset();

        // the clock ran two days ahead while the app ran, and was put back
        // without a restart: what was asked meanwhile counts as asked now,
        // so the hour's cap waits an hour, not two days and an hour
        freshState();
        timing = quick();
        timing.hourCap = 3;
        auto ahead = started(timing);
        wire(tracker, ahead.get());
        const int aheadFrom = int(standIn.nexts().size());
        const auto askedAhead = [&]() { return int(standIn.nexts().size()) - aheadFrom; };
        passMs(2 * 24 * 3600 * 1000LL);
        for (int k = 1; k <= 3; ++k) {
            listen(tracker, makeId(QStringLiteral("ck"), k), true, 46);
            waitUntil([&]() { return askedAhead() == k && !ahead->inFlight(); }, 3000);
        }
        passMs(-2 * 24 * 3600 * 1000LL);
        listen(tracker, makeId(QStringLiteral("ck"), 4), true, 46);
        pause(700);
        t.check(askedAhead() == 3, QStringLiteral("7  the clock put back mid-session: the hour's cap still holds a 4th"),
                QString::number(askedAhead()));
        passMs((3600 + 5) * 1000);
        waitUntil([&]() { return askedAhead() == 4 && !ahead->inFlight(); }, 3000);
        t.check(askedAhead() == 4, QStringLiteral("7  and lets it go an hour later, not two days and an hour"),
                QString::number(askedAhead()));
        t.check(sentCount(library) == 4, QStringLiteral("7  with the 3 dated ahead kept as now, not dropped"),
                QString::number(sentCount(library)));
    }

    // ------------------------------------------------ 8. failures: one try, then a rest
    {
        freshState();
        const QString kept = makeId(QStringLiteral("gk"), 1);
        addPlay(kept, QStringLiteral("-120 seconds"), 1.0);
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        justPlayed->noteRadio(kept, asTracks(radioOf(kept)));
        pause(150);
        const QString onScreen = signatureOf(justPlayed->rows());
        t.check(!justPlayed->rows().isEmpty(), QStringLiteral("8  a kept radio is on screen before the failures"));

        const struct {
            const char *mode;
            qint64 restSecs;
            const char *what;
        } failures[] = {
            { "429", 15 * 60, "a 429" },
            { "500", 60 * 60, "a 500" },
            { "html", 180 * 60, "an HTML 'unusual traffic' page" },
        };
        bool rowsHeld = true;
        int n = 0;
        for (const auto &failure : failures) {
            ++n;
            const QString id = makeId(QStringLiteral("fl"), n);
            modes.insert(id, QString::fromLatin1(failure.mode));
            listen(tracker, id, true, 46);
            waitUntil([&]() { return standIn.nexts().contains(id) && !justPlayed->inFlight(); }, 4000);
            pause(1600);   // past InnerTube's second attempt, were there one
            const qint64 left = restLeft(library);
            t.check(standIn.nexts().count(id) == 1, QStringLiteral("8  %1: exactly one attempt").arg(QLatin1String(failure.what)),
                    QString::number(standIn.nexts().count(id)));
            t.check(left > failure.restSecs - 30 && left <= failure.restSecs + 1,
                    QStringLiteral("8  then a rest of %1 min").arg(failure.restSecs / 60), QStringLiteral("%1 s").arg(left));
            rowsHeld = rowsHeld && signatureOf(justPlayed->rows()) == onScreen;
            // The rest over, for the next.
            passMs((failure.restSecs + 2) * 1000);
        }
        // A 400 with a country set (YouTube wanting more of a signed-out
        // /next): a failure like any other, not a reason to drop the country
        // and ask again.
        {
            int regionDrops = 0;
            InnerTube::setRegionRejectedHandler([&regionDrops](const QString &) { ++regionDrops; });
            InnerTube::setRegion(QStringLiteral("IN"));
            const QString id = makeId(QStringLiteral("fl"), 10);
            modes.insert(id, QStringLiteral("400"));
            listen(tracker, id, true, 46);
            waitUntil([&]() { return standIn.nexts().contains(id) && !justPlayed->inFlight(); }, 4000);
            pause(1600);
            const qint64 left = restLeft(library);
            t.check(standIn.nexts().count(id) == 1 && regionDrops == 0 && InnerTube::region() == QLatin1String("IN"),
                    QStringLiteral("8  a 400 with a country set: exactly one attempt, and the country kept"),
                    QStringLiteral("%1 attempts, %2 drops, region %3").arg(standIn.nexts().count(id)).arg(regionDrops)
                        .arg(InnerTube::region()));
            t.check(left > 180 * 60 - 30 && left <= 180 * 60 + 1, QStringLiteral("8  then a rest of 180 min"),
                    QStringLiteral("%1 s").arg(left));
            InnerTube::setRegion(QString());
            InnerTube::setRegionRejectedHandler(nullptr);
            rowsHeld = rowsHeld && signatureOf(justPlayed->rows()) == onScreen;
            passMs((180 * 60 + 2) * 1000);
        }
        // A refused connection: nobody listening on that port.
        {
            QTcpServer gone;
            gone.listen(QHostAddress::LocalHost, 0);
            const QString deadBase = QStringLiteral("http://127.0.0.1:%1").arg(gone.serverPort());
            gone.close();
            InnerTube::setTestServer(deadBase);
            const int asked = int(standIn.requests.size());
            const int sent = sentCount(library);
            listen(tracker, makeId(QStringLiteral("fl"), 4), true, 46);
            waitUntil([&]() { return paceOf(library).value(QLatin1String("failStreak")).toInt() == 5; }, 8000);
            pause(1600);
            const qint64 left = restLeft(library);
            t.check(sentCount(library) == sent + 1 && int(standIn.requests.size()) == asked,
                    QStringLiteral("8  a refused connection: one attempt"));
            t.check(left > 180 * 60 - 30 && left <= 180 * 60 + 1, QStringLiteral("8  and a rest of 3 h again"),
                    QStringLiteral("%1 s").arg(left));
            InnerTube::setTestServer(standIn.base());
            rowsHeld = rowsHeld && signatureOf(justPlayed->rows()) == onScreen;
        }
        t.check(rowsHeld, QStringLiteral("8  the rows on screen unchanged through every failure and rest"));

        // a success resets
        passMs((180 * 60 + 2) * 1000);
        const QString fine = makeId(QStringLiteral("fl"), 5);
        listen(tracker, fine, true, 46);
        waitUntil([&]() { return standIn.nexts().contains(fine) && !justPlayed->inFlight(); }, 3000);
        t.check(paceOf(library).value(QLatin1String("failStreak")).toInt() == 0 && radioRows(fine) == 1,
                QStringLiteral("8  a success resets the run of failures"));
        const QString again = makeId(QStringLiteral("fl"), 6);
        modes.insert(again, QStringLiteral("500"));
        listen(tracker, again, true, 46);
        waitUntil([&]() { return standIn.nexts().contains(again) && !justPlayed->inFlight(); }, 3000);
        pause(100);
        const qint64 left = restLeft(library);
        t.check(left > 15 * 60 - 30 && left <= 15 * 60 + 1, QStringLiteral("8  so the next failure rests 15 min again"),
                QStringLiteral("%1 s").arg(left));

        // the rest across a restart
        justPlayed.reset();
        auto restarted = started(quick());
        wire(tracker, restarted.get());
        const QString waiting = makeId(QStringLiteral("fl"), 7);
        listen(tracker, waiting, true, 46);
        pause(700);
        t.check(!standIn.nexts().contains(waiting), QStringLiteral("8  the rest survives a new JustPlayed"));
        restarted.reset();
        library->setSetting(QStringLiteral("home.justPlayed.pace"),
                            QString::fromUtf8(QJsonDocument(QJsonObject{
                                { QStringLiteral("restUntil"), fakeNow() / 1000 + 3 * 24 * 3600 } }).toJson(QJsonDocument::Compact)));
        auto clamped = started(quick());
        wire(tracker, clamped.get());
        const QString later = makeId(QStringLiteral("fl"), 8);
        listen(tracker, later, true, 46);
        pause(500);
        passMs(23 * 3600 * 1000);
        pause(400);
        t.check(!standIn.nexts().contains(later), QStringLiteral("8  a stored rest 3 days ahead holds for 23 h"));
        passMs((3600 + 60) * 1000);
        waitUntil([&]() { return standIn.nexts().contains(later); }, 3000);
        t.check(standIn.nexts().contains(later), QStringLiteral("8  and is clamped to now + 24 h"));
        waitUntil([&]() { return !clamped->inFlight(); }, 3000);
    }

    // ------------------------------------------------ 8. a change of country mid-fetch
    {
        freshState();
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        const QString id = makeId(QStringLiteral("rg"), 1);
        delays.insert(id, 400);
        listen(tracker, id, true, 46);
        waitUntil([&]() { return justPlayed->inFlight(); }, 3000);
        justPlayed->onRegionChanged();
        QElapsedTimer clock;
        clock.start();
        qint64 droppedAt = 0;
        qint64 againAt = 0;
        waitUntil([&]() {
            if (droppedAt == 0 && !justPlayed->inFlight())
                droppedAt = clock.elapsed();
            if (againAt == 0 && standIn.nexts().count(id) >= 2)
                againAt = clock.elapsed();
            return againAt > 0 && !justPlayed->inFlight();
        }, 4000);
        pause(100);
        t.check(standIn.nexts().count(id) == 2 && radioRows(id) == 1,
                QStringLiteral("8  the country changed mid-fetch: that answer dropped, the song asked again, once"),
                QString::number(standIn.nexts().count(id)));
        t.check(droppedAt > 0 && againAt - droppedAt >= 280,
                QStringLiteral("8  and only the gap (300 ms here) after the answer dropped"),
                QStringLiteral("%1 ms").arg(againAt - droppedAt));
    }

    // ------------------------------------------------ 8. a change of country with no listen open
    {
        freshState();
        const QString newest = makeId(QStringLiteral("rn"), 1);
        const QString older = makeId(QStringLiteral("rn"), 2);
        addPlay(older, QStringLiteral("-600 seconds"), 1.0);
        addPlay(newest, QStringLiteral("-120 seconds"), 1.0);
        auto justPlayed = started(quick());
        justPlayed->noteRadio(newest, asTracks(radioOf(newest)));
        justPlayed->noteRadio(older, asTracks(radioOf(older)));
        pause(500);
        const int newestBefore = int(standIn.nexts().count(newest));
        const int olderBefore = int(standIn.nexts().count(older));
        t.check(newestBefore == 0 && olderBefore == 0 && !justPlayed->seeds().isEmpty()
                    && justPlayed->seeds().first().videoId == newest,
                QStringLiteral("8  the last song's listen closed, both radios kept: nothing asked"));
        InnerTube::setRegion(QStringLiteral("GB"));
        justPlayed->onRegionChanged();
        waitUntil([&]() { return standIn.nexts().count(newest) == 1 && !justPlayed->inFlight(); }, 3000);
        pause(500);
        t.check(standIn.nexts().count(newest) == 1 && standIn.nexts().count(older) == 0,
                QStringLiteral("8  the country changed with nothing playing: the newest seed is asked again, once,"
                               " the older one not"),
                QStringLiteral("newest %1, older %2").arg(standIn.nexts().count(newest)).arg(standIn.nexts().count(older)));
        InnerTube::setRegion(QString());
    }

    // ------------------------------------------------ 9. empty answers
    {
        freshState();
        const QString kept = makeId(QStringLiteral("gk"), 2);
        addPlay(kept, QStringLiteral("-120 seconds"), 1.0);
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        justPlayed->noteRadio(kept, asTracks(radioOf(kept)));
        const QString e1 = makeId(QStringLiteral("em"), 1);
        modes.insert(e1, QStringLiteral("empty"));
        listen(tracker, e1, true, 46);
        waitUntil([&]() { return standIn.nexts().contains(e1) && !justPlayed->inFlight(); }, 3000);
        t.check(radioRows(e1) == 1 && scalar(QStringLiteral("SELECT ok FROM rec_radio WHERE video_id = ?"), { e1 }) == 0,
                QStringLiteral("9  an empty radio is kept, ok = 0"));
        const QVariantList rows = justPlayed->rows();
        t.check(!rows.isEmpty() && rows.first().toMap().value(QStringLiteral("seedVideoId")).toString() == kept,
                QStringLiteral("9  it gives no row; the next seed takes row 01"));
        t.check(restLeft(library) <= 0, QStringLiteral("9  one empty answer is no reason to rest"));
        listen(tracker, e1, true, 46);
        pause(600);
        t.check(standIn.nexts().count(e1) == 1, QStringLiteral("9  and it is not asked again (for 3 days)"));
        for (int k = 2; k <= 3; ++k) {
            const QString id = makeId(QStringLiteral("em"), k);
            modes.insert(id, QStringLiteral("empty"));
            listen(tracker, id, true, 46);
            waitUntil([&]() { return standIn.nexts().contains(id) && !justPlayed->inFlight(); }, 3000);
        }
        const qint64 left = restLeft(library);
        t.check(left > 3600 - 30 && left <= 3601, QStringLiteral("9  3 empties in a row: a rest of 1 h"),
                QStringLiteral("%1 s").arg(left));
        const QString after = makeId(QStringLiteral("em"), 4);
        listen(tracker, after, true, 46);
        pause(600);
        t.check(!standIn.nexts().contains(after), QStringLiteral("9  during which nothing is asked"));
    }

    // ------------------------------------------------ 10-11. autoplay's radio
    {
        freshState();
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());

        // 10. kept for free
        const QString heard = makeId(QStringLiteral("ap"), 1);
        justPlayed->noteRadio(heard, asTracks(radioOf(heard)));
        t.check(radioRows(heard) == 1, QStringLiteral("10 radioFetched from autoplay fills the cache"));
        listen(tracker, heard, true, 46);
        pause(600);
        t.check(!standIn.nexts().contains(heard) && !justPlayed->rows().isEmpty(),
                QStringLiteral("10 a later settle of it sends 0 requests, and its row is there"));

        // 11. never cancelled
        const QString ours = makeId(QStringLiteral("ap"), 2);
        const QString autoplays = makeId(QStringLiteral("ap"), 3);
        delays.insert(ours, 500);
        InnerTube player(nullptr, /*warmUp=*/false);
        bool ready = false;
        bool failed = false;
        QObject::connect(&player, &InnerTube::radioReady, &player,
                         [&](const QString &seedId, const QList<InnerTube::Track> &) { ready = ready || seedId == autoplays; });
        QObject::connect(&player, &InnerTube::radioFailed, &player, [&](const QString &, const QString &) { failed = true; });
        listen(tracker, ours, true, 46);
        waitUntil([&]() { return justPlayed->inFlight(); }, 3000);
        player.radio(autoplays);
        waitUntil([&]() { return ready && radioRows(ours) == 1; }, 4000);
        t.check(ready && !failed, QStringLiteral("11 autoplay's radio on another InnerTube, asked mid-fetch: radioReady still comes"));
        t.check(radioRows(ours) == 1, QStringLiteral("11 and JustPlayed's own answer still lands"));
    }

    // ------------------------------------------------ 12. holds
    {
        freshState();
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        const struct {
            const char *what;
            std::function<void(bool)> hold;
        } holds[] = {
            { "setPaused (an import)", [&](bool on) { justPlayed->setPaused(on); } },
            { "setAccountResting", [&](bool on) { justPlayed->setAccountResting(on); } },
            { "watching = false (hidden, minimised, the tray)", [&](bool on) { justPlayed->setWatching(!on); } },
        };
        int k = 0;
        for (const auto &hold : holds) {
            ++k;
            const QString id = makeId(QStringLiteral("hd"), k);
            hold.hold(true);
            listen(tracker, id, true, 46);
            pause(500);
            t.check(hasSeed(justPlayed->seeds(), id) && !standIn.nexts().contains(id),
                    QStringLiteral("12 %1: the seed recorded, nothing sent").arg(QLatin1String(hold.what)));
            hold.hold(false);
            waitUntil([&]() { return standIn.nexts().contains(id) && !justPlayed->inFlight(); }, 3000);
            t.check(standIn.nexts().count(id) == 1, QStringLiteral("12   released: it is sent"));
        }
        justPlayed->setWatching(false);
        const int from = int(standIn.nexts().size());
        for (int m = 10; m <= 13; ++m)
            listen(tracker, makeId(QStringLiteral("hd"), m), true, 46);
        pause(400);
        t.check(int(standIn.nexts().size()) == from && justPlayed->queued().size() == 3,
                QStringLiteral("12 four seeds while held: 0 sent, the newest 3 waiting"));
        justPlayed->setWatching(true);
        waitUntil([&]() { return int(standIn.nexts().size()) > from; }, 3000);
        t.check(standIn.nexts().value(from) == makeId(QStringLiteral("hd"), 13),
                QStringLiteral("12 released: the newest seed goes first"), standIn.nexts().value(from));
        waitUntil([&]() { return justPlayed->queued().isEmpty() && !justPlayed->inFlight(); }, 4000);
        t.check(!standIn.nexts().contains(makeId(QStringLiteral("hd"), 10)),
                QStringLiteral("12 and the oldest, dropped, is never sent"));
    }

    // ------------------------------------------------ 13. switched off
    {
        freshState();
        const QString kept = makeId(QStringLiteral("gk"), 3);
        addPlay(kept, QStringLiteral("-120 seconds"), 1.0);
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        justPlayed->noteRadio(kept, asTracks(radioOf(kept)));
        const QString inFlight = makeId(QStringLiteral("of"), 1);
        delays.insert(inFlight, 600);
        listen(tracker, inFlight, true, 46);
        waitUntil([&]() { return justPlayed->inFlight(); }, 3000);
        justPlayed->setEnabled(false);
        t.check(justPlayed->seeds().isEmpty() && justPlayed->rows().isEmpty() && justPlayed->queued().isEmpty()
                    && !justPlayed->timersActive(),
                QStringLiteral("13 enabled = false: no seeds, empty rows, nothing queued, no timers"));
        t.check(library->settingValue(QStringLiteral("home.justPlayed")) == QLatin1String("0"),
                QStringLiteral("13 kept as home.justPlayed = 0"));
        pause(900);
        t.check(radioRows(inFlight) == 0, QStringLiteral("13 the answer that was on its way is dropped"));
        const int from = int(standIn.nexts().size());
        listen(tracker, makeId(QStringLiteral("of"), 2), true, 100);
        pause(500);
        t.check(justPlayed->seeds().isEmpty() && int(standIn.nexts().size()) == from && !justPlayed->timersActive(),
                QStringLiteral("13 a song heard while off: no seed, no request"));
        justPlayed->setEnabled(true);
        pause(500);
        const QVariantList rows = justPlayed->rows();
        t.check(!rows.isEmpty() && rows.first().toMap().value(QStringLiteral("seedVideoId")).toString() == kept
                    && int(standIn.nexts().size()) == from,
                QStringLiteral("13 on again: the rows come from the cache, with 0 requests"));
    }

    // ------------------------------------------------ 14. quitting mid-fetch
    {
        freshState();
        ListenTracker tracker;
        auto justPlayed = started(quick());
        wire(tracker, justPlayed.get());
        const QString id = makeId(QStringLiteral("qt"), 1);
        delays.insert(id, 2000);
        listen(tracker, id, true, 46);
        waitUntil([&]() { return justPlayed->inFlight(); }, 3000);
        const qint64 radios = scalar(QStringLiteral("SELECT COUNT(*) FROM rec_radio"));
        const QString pace = library->settingValue(QStringLiteral("home.justPlayed.pace"));
        justPlayed.reset();
        pause(2500);
        t.check(scalar(QStringLiteral("SELECT COUNT(*) FROM rec_radio")) == radios && radioRows(id) == 0
                    && library->settingValue(QStringLiteral("home.justPlayed.pace")) == pace,
                QStringLiteral("14 deleted mid-fetch: no crash, the late answer has no effect, nothing written"));
    }

    // ------------------------------------------------ 15. a second launch
    {
        freshState();
        for (int k = 1; k <= 5; ++k)
            addPlay(makeId(QStringLiteral("ln"), k), QStringLiteral("-%1 seconds").arg(60 * k), 1.0);
        const QString older = makeId(QStringLiteral("ln"), 6);
        addPlay(older, QStringLiteral("-3600 seconds"), 1.0);
        JustPlayed::Timing timing = quick();
        timing.launchDelayMs = 60000;
        auto first = started(timing);
        first->noteRadio(older, asTracks(radioOf(older)));
        const QString shown = signatureOf(first->rows());
        first.reset();
        const int from = int(standIn.nexts().size());
        timing.launchDelayMs = 600;
        auto second = started(timing);
        t.check(!shown.isEmpty() && signatureOf(second->rows()) == shown,
                QStringLiteral("15 a new JustPlayed on the same library shows the same rows"));
        pause(300);
        t.check(int(standIn.nexts().size()) == from, QStringLiteral("15 with 0 requests before the launch delay"));
        waitUntil([&]() { return int(standIn.nexts().size()) - from >= 3 && !second->inFlight(); }, 5000);
        pause(800);
        const QStringList backfilled = standIn.nexts().mid(from);
        t.check(backfilled.size() == 3 && !backfilled.contains(older),
                QStringLiteral("15 and at most 3 after it, for seeds with nothing kept"), backfilled.join(QLatin1Char(',')));
    }

    // 2, for every request so far
    {
        bool clean = true;
        for (const StandIn::Request &request : std::as_const(standIn.requests))
            clean = clean && !request.has("cookie") && !request.has("authorization") && !request.has("x-goog-authuser")
                    && !request.has("x-goog-visitor-id");
        t.check(clean && admitted == 0 && headed == 0,
                QStringLiteral("2  none of the %1 requests carried a cookie, the account or a visitor id; the hook was never asked")
                    .arg(standIn.requests.size()));
    }

    const qint64 now = 1800000000;

    // ------------------------------------------------ 16. what the rows leave out
    {
        const QString seedId = makeId(QStringLiteral("lo"), 0);
        const QVector<Rec::SessionSeed> seeds{
            seed(seedId, QStringLiteral("Seed Zero"), QStringLiteral("Seed Maker"), now - 60) };
        const QString heard = makeId(QStringLiteral("lh"), 1);
        const QString turned = makeId(QStringLiteral("lt"), 1);
        QVector<Rec::RadioTrack> radio{
            song(seedId, QStringLiteral("Seed Zero"), QStringLiteral("Seed Maker")),
            song(makeId(QStringLiteral("lc"), 1), QStringLiteral("Seed Zero (Lyrics)"), QStringLiteral("Seed Maker")),
            song(heard, QStringLiteral("Heard Lately"), QStringLiteral("Plain One")),
            song(turned, QStringLiteral("Turned Down"), QStringLiteral("Plain Two")),
            song(makeId(QStringLiteral("ls"), 1), QStringLiteral("Not This Song"), QStringLiteral("Plain Three")),
            song(makeId(QStringLiteral("la"), 1), QStringLiteral("Anything"), QStringLiteral("Blocked Artist")),
            song(makeId(QStringLiteral("lw"), 1), QStringLiteral("White Power Anthem"), QStringLiteral("Plain Four")),
            song(makeId(QStringLiteral("lx"), 1), QStringLiteral("Rough Song (Explicit)"), QStringLiteral("Plain Five")),
        };
        radio += family(QStringLiteral("lf"), QStringLiteral("Fine"), 8, 20);
        const QHash<QString, QVector<Rec::RadioTrack>> radios{ { seedId, radio } };
        const Rec::SessionFilter unwanted = [&turned](const QString &id, const QString &title, const QString &artist) {
            return id == turned || (title == QLatin1String("Not This Song") && artist == QLatin1String("Plain Three"))
                   || Rec::suggestionArtistKey(artist) == Rec::suggestionArtistKey(QStringLiteral("Blocked Artist"));
        };
        Rec::SessionOptions options;
        options.recentHeard = { heard };
        options.hideExplicit = true;
        const QVector<Rec::SessionRow> rows = Rec::buildSessionRows(seeds, radios, now, options, unwanted);
        QSet<QString> shown;
        for (const Rec::SessionRow &row : rows) {
            for (const Rec::RadioTrack &track : row.items)
                shown.insert(track.title);
        }
        t.check(!rows.isEmpty() && rowOfKind(rows, QStringLiteral("s1")), QStringLiteral("16 a row is made"));
        t.check(!shown.contains(QStringLiteral("Seed Zero")) && !shown.contains(QStringLiteral("Seed Zero (Lyrics)")),
                QStringLiteral("16 left out: the seed itself, and another upload of it"));
        t.check(!shown.contains(QStringLiteral("Heard Lately")), QStringLiteral("16 left out: a song played in the last 48 h"));
        t.check(!shown.contains(QStringLiteral("Turned Down")) && !shown.contains(QStringLiteral("Not This Song"))
                    && !shown.contains(QStringLiteral("Anything")),
                QStringLiteral("16 left out: a turned-down id, song and artist"));
        t.check(!shown.contains(QStringLiteral("White Power Anthem")), QStringLiteral("16 left out: a hate-list title"));
        t.check(!shown.contains(QStringLiteral("Rough Song (Explicit)")),
                QStringLiteral("16 left out: an (Explicit) title, with hideExplicit on"));
        options.hideExplicit = false;
        QSet<QString> shownExplicit;
        for (const Rec::SessionRow &row : Rec::buildSessionRows(seeds, radios, now, options, unwanted)) {
            for (const Rec::RadioTrack &track : row.items)
                shownExplicit.insert(track.title);
        }
        t.check(shownExplicit.contains(QStringLiteral("Rough Song (Explicit)")),
                QStringLiteral("16 which comes back with it off"));
    }

    // ------------------------------------------------ 17. no song twice, and the minimums
    {
        // Four seeds of the last hours whose radios share most of their
        // songs, a heavy artist, and one song uploaded twice.
        const QString s1 = makeId(QStringLiteral("dd"), 1);
        const QString s2 = makeId(QStringLiteral("dd"), 2);
        const QString s3 = makeId(QStringLiteral("dd"), 3);
        const QString thin = makeId(QStringLiteral("dd"), 4);
        const QVector<Rec::SessionSeed> seeds{
            seed(thin, QStringLiteral("Thin One"), QStringLiteral("Thin Maker"), now - 30),
            seed(s1, QStringLiteral("First One"), QStringLiteral("Maker One"), now - 60),
            seed(s2, QStringLiteral("Second One"), QStringLiteral("Maker Two"), now - 120),
            seed(s3, QStringLiteral("Third One"), QStringLiteral("Maker Three"), now - 180),
        };
        const QVector<Rec::RadioTrack> shared = family(QStringLiteral("ds"), QStringLiteral("Shared"), 10, 30);
        QVector<Rec::RadioTrack> radio1 = family(QStringLiteral("dh"), QStringLiteral("Heavy"), 1, 10);
        radio1.append(song(makeId(QStringLiteral("dt"), 1), QStringLiteral("Twin Song"), QStringLiteral("Twin Artist")));
        radio1 += shared;
        QVector<Rec::RadioTrack> radio2{ song(makeId(QStringLiteral("dt"), 2), QStringLiteral("Twin Song (Official Video)"),
                                              QStringLiteral("Twin Artist")) };
        radio2 += shared;
        radio2 += family(QStringLiteral("dv"), QStringLiteral("Own Two"), 6, 12);
        QVector<Rec::RadioTrack> radio3 = shared.mid(10);
        radio3 += family(QStringLiteral("dw"), QStringLiteral("Own Three"), 6, 12);
        const QHash<QString, QVector<Rec::RadioTrack>> radios{
            { thin, family(QStringLiteral("dn"), QStringLiteral("Thin"), 5, 5) },
            { s1, radio1 }, { s2, radio2 }, { s3, radio3 } };
        const QVector<Rec::SessionRow> rows = Rec::buildSessionRows(seeds, radios, now, Rec::SessionOptions(), nullptr);
        QSet<QString> ids;
        QSet<quint64> keys;
        bool twice = false;
        bool overArtist = false;
        for (const Rec::SessionRow &row : rows) {
            QHash<QString, int> perArtist;
            for (const Rec::RadioTrack &track : row.items) {
                twice = twice || ids.contains(track.videoId) || keys.contains(songKeyOf(track));
                ids.insert(track.videoId);
                keys.insert(songKeyOf(track));
                overArtist = overArtist || ++perArtist[Rec::radioLeadKey(track)] > 3;
            }
        }
        QStringList kinds;
        for (const Rec::SessionRow &row : rows)
            kinds << row.kind + QLatin1Char(':') + row.seedVideoId.right(1);
        t.check(rows.size() >= 3, QStringLiteral("17 rows are made from shared radios"), kinds.join(QLatin1Char(' ')));
        t.check(!twice, QStringLiteral("17 no videoId or song (strict key) appears in two rows, or twice in one"));
        t.check(!overArtist, QStringLiteral("17 at most 3 cards of one lead artist in a row"));
        bool thinRow = false;
        for (const Rec::SessionRow &row : rows)
            thinRow = thinRow || (row.kind != QLatin1String("mix") && row.seedVideoId == thin);
        t.check(!thinRow && rows.first().seedVideoId == s1,
                QStringLiteral("17 the newest seed, with 5 usable songs, has no row; the next is row 01"));

        // The mix's minimums.
        const QVector<Rec::SessionSeed> alone{ seed(s1, QStringLiteral("First One"), QStringLiteral("Maker One"), now - 60) };
        const QVector<Rec::SessionRow> one = Rec::buildSessionRows(
            alone, { { s1, family(QStringLiteral("ma"), QStringLiteral("Alone"), 14, 40) } }, now, Rec::SessionOptions(), nullptr);
        t.check(one.size() == 1 && !rowOfKind(one, QStringLiteral("mix")), QStringLiteral("17 one seed: no mix"));
        const QVector<Rec::SessionSeed> pair{ seed(s1, QStringLiteral("First One"), QStringLiteral("Maker One"), now - 60),
                                              seed(s2, QStringLiteral("Second One"), QStringLiteral("Maker Two"), now - 120) };
        const QVector<Rec::SessionRow> few = Rec::buildSessionRows(
            pair, { { s1, family(QStringLiteral("mb"), QStringLiteral("Few A"), 5, 14) },
                    { s2, family(QStringLiteral("mc"), QStringLiteral("Few B"), 5, 14) } },
            now, Rec::SessionOptions(), nullptr);
        t.check(rowOfKind(few, QStringLiteral("s2")) && !rowOfKind(few, QStringLiteral("mix")),
                QStringLiteral("17 two seeds with 4 songs left between them: no mix (it needs 8)"));
        const QVector<Rec::SessionRow> enough = Rec::buildSessionRows(
            pair, { { s1, family(QStringLiteral("md"), QStringLiteral("Many A"), 14, 40) },
                    { s2, family(QStringLiteral("me"), QStringLiteral("Many B"), 14, 40) } },
            now, Rec::SessionOptions(), nullptr);
        const Rec::SessionRow *mix = rowOfKind(enough, QStringLiteral("mix"));
        t.check(mix && mix->items.size() == 15 && mix->title == QLatin1String("Based on your recent listening"),
                QStringLiteral("17 two seeds with plenty left: the mix, 15 cards"));
    }

    // ------------------------------------------------ 18. the rotation
    {
        const QString a = makeId(QStringLiteral("ro"), 1);
        const QString b = makeId(QStringLiteral("ro"), 2);
        const QString c = makeId(QStringLiteral("ro"), 3);
        const QVector<Rec::SessionSeed> seeds{
            seed(a, QStringLiteral("Rot A"), QStringLiteral("Rot Maker A"), now - 60),
            seed(b, QStringLiteral("Rot B"), QStringLiteral("Rot Maker B"), now - 3600),
            seed(c, QStringLiteral("Rot C"), QStringLiteral("Rot Maker C"), now - 5 * 24 * 3600),
        };
        const QHash<QString, QVector<Rec::RadioTrack>> radios{
            { a, family(QStringLiteral("rpa"), QStringLiteral("Rot Pa"), 14, 40) },
            { b, family(QStringLiteral("rpb"), QStringLiteral("Rot Pb"), 14, 40) },
            { c, family(QStringLiteral("rpc"), QStringLiteral("Rot Pc"), 14, 40) } };
        Rec::SessionOptions options;
        options.rotation = Rec::rotationFor(1, 0);
        const QVector<Rec::SessionRow> first = Rec::buildSessionRows(seeds, radios, now, options, nullptr);
        const QVector<Rec::SessionRow> same = Rec::buildSessionRows(seeds, radios, now, options, nullptr);
        options.rotation = Rec::rotationFor(2, 0);
        const QVector<Rec::SessionRow> other = Rec::buildSessionRows(seeds, radios, now, options, nullptr);
        t.check(first.size() == 4 && signatureOf(first) == signatureOf(same),
                QStringLiteral("18 the same inputs and rotation give identical rows"));
        bool firstFour = true;
        for (const QString &kind : { QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3") }) {
            const Rec::SessionRow *was = rowOfKind(first, kind);
            const Rec::SessionRow *now2 = rowOfKind(other, kind);
            firstFour = firstFour && was && now2 && was->key == now2->key && was->items.size() >= 4
                        && now2->items.size() >= 4;
            for (int i = 0; firstFour && i < 4; ++i)
                firstFour = was->items.at(i).videoId == now2->items.at(i).videoId;
        }
        t.check(firstFour, QStringLiteral("18 another rotation keeps each seed row's first 4 cards"));
        t.check(signatureOf(first) != signatureOf(other), QStringLiteral("18 and does change the rest"));

        // Two seeds of one cluster whose radios share songs: what S1 draws
        // from its tail never reaches S2's first four.
        const QString p = makeId(QStringLiteral("ov"), 1);
        const QString q = makeId(QStringLiteral("ov"), 2);
        const QVector<Rec::SessionSeed> overlapping{
            seed(p, QStringLiteral("Over P"), QStringLiteral("Over Maker P"), now - 60),
            seed(q, QStringLiteral("Over Q"), QStringLiteral("Over Maker Q"), now - 3600),
        };
        const QVector<Rec::RadioTrack> radioP = family(QStringLiteral("ovs"), QStringLiteral("Over Shared"), 14, 40);
        const QHash<QString, QVector<Rec::RadioTrack>> shared{
            { p, radioP }, { q, radioP.mid(4, 20) + family(QStringLiteral("ovq"), QStringLiteral("Over Own"), 14, 30) } };
        QString headP;
        QString headQ;
        bool steady = true;
        bool apart = true;
        for (int visit = 1; visit <= 8; ++visit) {
            Rec::SessionOptions drawn;
            drawn.rotation = Rec::rotationFor(visit, 0);
            const QVector<Rec::SessionRow> rows = Rec::buildSessionRows(overlapping, shared, now, drawn, nullptr);
            const Rec::SessionRow *rowP = rowOfKind(rows, QStringLiteral("s1"));
            const Rec::SessionRow *rowQ = rowOfKind(rows, QStringLiteral("s2"));
            if (!rowP || !rowQ || rowQ->seedVideoId != q || rowP->items.size() < 4 || rowQ->items.size() < 4) {
                steady = false;
                break;
            }
            QStringList fourP;
            QStringList fourQ;
            for (int i = 0; i < 4; ++i) {
                fourP << rowP->items.at(i).videoId;
                fourQ << rowQ->items.at(i).videoId;
            }
            if (visit == 1) {
                headP = fourP.join(QLatin1Char(','));
                headQ = fourQ.join(QLatin1Char(','));
            }
            steady = steady && fourP.join(QLatin1Char(',')) == headP && fourQ.join(QLatin1Char(',')) == headQ;
            QSet<QString> ids;
            for (const Rec::SessionRow &row : rows) {
                for (const Rec::RadioTrack &track : row.items) {
                    apart = apart && !ids.contains(track.videoId);
                    ids.insert(track.videoId);
                }
            }
        }
        t.check(steady, QStringLiteral("18 radios sharing songs: S1's and S2's first 4 the same on 8 visits"));
        t.check(apart, QStringLiteral("18 and still no song in two rows"));

        // The time it takes, with a couple of days' listening: 60 seeds of
        // their own artists, 50 songs a radio, in six families that share
        // artists (so clusters form).
        QVector<Rec::SessionSeed> many;
        QHash<QString, QVector<Rec::RadioTrack>> manyRadios;
        for (int k = 0; k < 60; ++k) {
            const QString id = makeId(QStringLiteral("tm"), k);
            many.append(seed(id, QStringLiteral("Timed %1").arg(k), QStringLiteral("Timed Maker %1").arg(k), now - 600 * (k + 1),
                             k % 3 != 0, k % 7 == 0, 1 + k % 4));
            QVector<Rec::RadioTrack> radio = family(QStringLiteral("tf%1").arg(k % 6), QStringLiteral("Family %1").arg(k % 6), 10, 20);
            radio += family(QStringLiteral("to") + QString::number(k).rightJustified(2, QLatin1Char('0')),
                            QStringLiteral("Own %1").arg(k), 10, 30);
            manyRadios.insert(id, radio);
        }
        Rec::SessionOptions timed;
        timed.rotation = Rec::rotationFor(3, 0);
        QElapsedTimer took;
        took.start();
        int builtRows = 0;
        for (int run = 0; run < 5; ++run)
            builtRows += int(Rec::buildSessionRows(many, manyRadios, now, timed, nullptr).size());
        const double ms = double(took.nsecsElapsed()) / 1e6 / 5.0;
        t.check(builtRows == 5 * 4 && ms < 40.0,
                QStringLiteral("18 60 seeds with 50-song radios: the rows in %1 ms a build").arg(ms, 0, 'f', 1),
                QStringLiteral("%1 rows").arg(builtRows));
    }

    // ------------------------------------------------ 19. clusters
    {
        const QString a1 = makeId(QStringLiteral("ca"), 1);
        const QString a2 = makeId(QStringLiteral("ca"), 2);
        const QString b = makeId(QStringLiteral("cb"), 1);
        QVector<Rec::SessionSeed> seeds{
            seed(a2, QStringLiteral("Phonk Two"), QStringLiteral("Phonk Maker Two"), now - 60),
            seed(a1, QStringLiteral("Phonk One"), QStringLiteral("Phonk Maker One"), now - 600),
            seed(b, QStringLiteral("Punjabi One"), QStringLiteral("Punjabi Maker"), now - 47 * 3600),
        };
        QHash<QString, QVector<Rec::RadioTrack>> radios{
            { a1, family(QStringLiteral("cpa"), QStringLiteral("Phonk"), 15, 45) },
            { a2, family(QStringLiteral("cpb"), QStringLiteral("Phonk"), 15, 45) },
            { b, family(QStringLiteral("cpj"), QStringLiteral("Punjabi"), 15, 45) } };
        const QVector<int> clusters = Rec::clusterSeeds(seeds, radios);
        t.check(clusters.at(0) == clusters.at(1) && clusters.at(0) != clusters.at(2),
                QStringLiteral("19 two phonk artists with overlapping radios cluster; Punjabi apart"));
        const QVector<Rec::SessionRow> rows = Rec::buildSessionRows(seeds, radios, now, Rec::SessionOptions(), nullptr);
        const Rec::SessionRow *s1 = rowOfKind(rows, QStringLiteral("s1"));
        const Rec::SessionRow *s2 = rowOfKind(rows, QStringLiteral("s2"));
        t.check(s1 && s1->seedVideoId == a2, QStringLiteral("19 S1 = A2, the newest"));
        t.check(s2 && s2->seedVideoId == b, QStringLiteral("19 S2 = B (2 days ago, not A1 of S1's cluster)"));
        t.check(!rowOfKind(rows, QStringLiteral("s3")), QStringLiteral("19 S3: none, with no third cluster"));
        const Rec::SessionRow *mix = rowOfKind(rows, QStringLiteral("mix"));
        int firstB = -1;
        bool anyA = false;
        for (int i = 0; mix && i < mix->items.size(); ++i) {
            const bool isB = mix->items.at(i).videoId.startsWith(QLatin1String("cpj"));
            if (isB && firstB < 0)
                firstB = i;
            anyA = anyA || !isB;
        }
        t.check(mix && anyA && firstB >= 0 && firstB < 4, QStringLiteral("19 the mix interleaves A and B"),
                QStringLiteral("first B at %1").arg(firstB));

        const QString c1 = makeId(QStringLiteral("cc"), 1);
        const QString c2 = makeId(QStringLiteral("cc"), 2);
        const QString c3 = makeId(QStringLiteral("cc"), 3);
        const QString d = makeId(QStringLiteral("cd"), 1);
        seeds.append(seed(d, QStringLiteral("Drill One"), QStringLiteral("Drill Maker"), now - 3 * 24 * 3600, false));
        seeds.append(seed(c1, QStringLiteral("Lofi One"), QStringLiteral("Lofi Maker"), now - 5 * 24 * 3600, true, true, 10));
        seeds.append(seed(c2, QStringLiteral("Lofi Two"), QStringLiteral("Lofi Maker"), now - 5 * 24 * 3600 - 60, true, true, 10));
        seeds.append(seed(c3, QStringLiteral("Lofi Three"), QStringLiteral("Lofi Maker"), now - 5 * 24 * 3600 - 120, true, true, 10));
        radios.insert(d, family(QStringLiteral("cpd"), QStringLiteral("Drill"), 15, 45));
        radios.insert(c1, family(QStringLiteral("cpl"), QStringLiteral("Lofi"), 15, 45));
        radios.insert(c2, family(QStringLiteral("cpm"), QStringLiteral("Lofi"), 15, 45));
        radios.insert(c3, family(QStringLiteral("cpn"), QStringLiteral("Lofi"), 15, 45));
        const QVector<Rec::SessionRow> anchored = Rec::buildSessionRows(seeds, radios, now, Rec::SessionOptions(), nullptr);
        const Rec::SessionRow *s3 = rowOfKind(anchored, QStringLiteral("s3"));
        t.check(s3 && s3->seedVideoId == c1,
                QStringLiteral("19 with a third cluster C, older and heavier than a light D: S3 = C's newest"),
                s3 ? s3->seedVideoId : QStringLiteral("none"));
    }

    // ------------------------------------------------ 20. the live seed, and which plays seed
    {
        freshState();
        const QString newer = makeId(QStringLiteral("lv"), 1);   // a DB play dated after the live song
        const QString live = makeId(QStringLiteral("lv"), 2);
        const QString likedSkip = makeId(QStringLiteral("lv"), 3);
        const QString skipped = makeId(QStringLiteral("lv"), 4);
        addPlay(newer, QStringLiteral("+60 seconds"), 1.0);
        addPlay(likedSkip, QStringLiteral("-300 seconds"), 0.2, 12000);
        addLike(likedSkip);
        addPlay(skipped, QStringLiteral("-400 seconds"), 0.2, 12000);
        JustPlayed::Timing timing = quick();
        timing.launchDelayMs = 60000;
        ListenTracker tracker;
        auto justPlayed = started(timing);
        wire(tracker, justPlayed.get());
        justPlayed->noteRadio(newer, asTracks(radioOf(newer)));
        justPlayed->noteRadio(live, asTracks(radioOf(live)));
        listen(tracker, live, true, 46);
        const QVariantList rows = justPlayed->rows();
        t.check(!rows.isEmpty() && rows.first().toMap().value(QStringLiteral("seedVideoId")).toString() == live,
                QStringLiteral("20 a live settled seed is S1, above a newer play in the library"));
        t.check(hasSeed(justPlayed->seeds(), likedSkip), QStringLiteral("20 a liked skip seeds"));
        t.check(!hasSeed(justPlayed->seeds(), skipped), QStringLiteral("20 label 0.2 without a like does not"));
    }

    // ------------------------------------------------ 21. cleanSeedTitle
    t.check(Rec::cleanSeedTitle(QStringLiteral("BbY WOW - KAROL G (Lyrics)"), QStringLiteral("KAROL G")) == QLatin1String("BbY WOW"),
            QStringLiteral("21 'BbY WOW - KAROL G (Lyrics)' by KAROL G is 'BbY WOW'"));
    t.check(Rec::cleanSeedTitle(QStringLiteral("Song [Official Video]"), QStringLiteral("Someone")) == QLatin1String("Song"),
            QStringLiteral("21 'Song [Official Video]' is 'Song'"));
    t.check(Rec::cleanSeedTitle(QStringLiteral("DEATH RATTLE (Super Slowed)"), QStringLiteral("Someone"))
                == QLatin1String("DEATH RATTLE (Super Slowed)"),
            QStringLiteral("21 'DEATH RATTLE (Super Slowed)' is kept"));
    t.check(Rec::cleanSeedTitle(QStringLiteral("(Lyrics)"), QStringLiteral("Someone")) == QLatin1String("(Lyrics)"),
            QStringLiteral("21 '(Lyrics)' alone falls back to the raw title"));

    // ------------------------------------------------ 22. Clear history, Remove from history
    {
        freshState();
        const QString h1 = makeId(QStringLiteral("hs"), 1);
        const QString h2 = makeId(QStringLiteral("hs"), 2);
        addPlay(h1, QStringLiteral("-60 seconds"), 1.0);
        addPlay(h2, QStringLiteral("-120 seconds"), 1.0);
        JustPlayed::Timing timing = quick();
        timing.launchDelayMs = 60000;
        auto justPlayed = started(timing);
        QObject::connect(library, &Library::historyChanged, justPlayed.get(), &JustPlayed::reloadSeeds);
        justPlayed->noteRadio(h1, asTracks(radioOf(h1)));
        justPlayed->noteRadio(h2, asTracks(radioOf(h2)));
        {
            QSqlQuery part(AppDatabase::connection());
            part.prepare(QStringLiteral("INSERT OR REPLACE INTO home_cache (part, region, saved_at, body) VALUES (?, ?, ?, ?)"));
            part.addBindValue(QStringLiteral("moreLike:") + ArtistLinks::looseKey(seedArtistOf(h1)));
            part.addBindValue(InnerTube::region());
            part.addBindValue(QDateTime::currentSecsSinceEpoch());
            part.addBindValue(qCompress(QByteArray("{}")));
            part.exec();
        }
        t.check(justPlayed->rows().size() >= 2, QStringLiteral("22 two seeds' rows are shown"));
        library->removeFromHistory(h1);
        t.check(!hasSeed(justPlayed->seeds(), h1) && hasSeed(justPlayed->seeds(), h2) && radioRows(h1) == 0
                    && radioRows(h2) == 1,
                QStringLiteral("22 Remove from history drops only that seed and its cached radio"));
        library->clearHistory();
        t.check(justPlayed->rows().isEmpty() && justPlayed->seeds().isEmpty()
                    && scalar(QStringLiteral("SELECT COUNT(*) FROM rec_radio")) == 0
                    && scalar(QStringLiteral("SELECT COUNT(*) FROM home_cache WHERE part LIKE 'moreLike:%'")) == 0,
                QStringLiteral("22 Clear history empties the rows, rec_radio and the moreLike:% parts"));
    }

    // ------------------------------------------------ 23. Library::topArtist(true)
    {
        freshState();
        const auto play = [](const QString &id, const QString &artist, const QString &startedAt) {
            QSqlQuery query(AppDatabase::connection());
            query.prepare(QStringLiteral(
                "INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms, listened_ms, label)"
                " VALUES ('play', ?, ?, ?, ") + startedAt + QStringLiteral(", 180000, 180000, 1.0)"));
            query.addBindValue(id);
            query.addBindValue(QStringLiteral("Song ") + id);
            query.addBindValue(artist);
            query.exec();
        };
        // A's five songs looped 116 times, all today; B's one song on 23 days.
        for (int i = 0; i < 116; ++i)
            play(makeId(QStringLiteral("ta"), i % 5), QStringLiteral("Loop Artist"),
                 QStringLiteral("date('now') || ' 00:00:01'"));
        for (int d = 1; d <= 23; ++d)
            play(makeId(QStringLiteral("tb"), 1), QStringLiteral("Daily Artist"),
                 QStringLiteral("datetime('now', '-%1 days')").arg(d));
        t.check(library->topArtist(true) == QLatin1String("Daily Artist"),
                QStringLiteral("23 damped: 116 same-day loops of 5 songs lose to 23 days of one"), library->topArtist(true));
        t.check(library->topArtist(false) == QLatin1String("Loop Artist"),
                QStringLiteral("23 undamped, as before: the loops win"), library->topArtist(false));
        exec(QStringLiteral("DELETE FROM play_events"));
        for (int i = 0; i < 50; ++i)
            play(makeId(QStringLiteral("tc"), 1), QStringLiteral("Few Days"), QStringLiteral("date('now') || ' 00:00:01'"));
        play(makeId(QStringLiteral("tc"), 1), QStringLiteral("Few Days"), QStringLiteral("datetime('now', '-2 days')"));
        t.check(library->topArtist(true).isEmpty() && library->topArtist(false) == QLatin1String("Few Days"),
                QStringLiteral("23 fewer than 3 song-days: damped is empty"));
        exec(QStringLiteral("DELETE FROM play_events"));
    }

    // ------------------------------------------------ 24. Catalog::loadMoreLikeFrom
    {
        freshState();
        Catalog catalog;
        {
            const QVariantMap kept{
                { QStringLiteral("artist"), QStringLiteral("Cached Artist") },
                { QStringLiteral("name"), QStringLiteral("Cached Artist") },
                { QStringLiteral("browseId"), QStringLiteral("UCcachedartist") },
                { QStringLiteral("items"), QVariantList{ QVariantMap{ { QStringLiteral("type"), QStringLiteral("artist") },
                                                                      { QStringLiteral("title"), QStringLiteral("Someone Alike") } } } } };
            QSqlQuery part(AppDatabase::connection());
            part.prepare(QStringLiteral("INSERT OR REPLACE INTO home_cache (part, region, saved_at, body) VALUES (?, ?, ?, ?)"));
            part.addBindValue(QStringLiteral("moreLike:") + ArtistLinks::looseKey(QStringLiteral("Cached Artist")));
            part.addBindValue(InnerTube::region());
            part.addBindValue(QDateTime::currentSecsSinceEpoch());
            part.addBindValue(qCompress(QJsonDocument::fromVariant(kept).toJson(QJsonDocument::Compact)));
            part.exec();
        }
        const int from = int(standIn.requests.size());
        catalog.loadMoreLikeFrom({ QVariantMap{ { QStringLiteral("name"), QStringLiteral("Cached Artist") },
                                                { QStringLiteral("browseId"), QString() } } });
        pause(300);
        t.check(catalog.moreLike().value(QStringLiteral("artist")).toString() == QLatin1String("Cached Artist")
                    && int(standIn.requests.size()) == from,
                QStringLiteral("24 a cached per-artist part: shown, 0 requests"));
        const QVariantList candidates{
            QVariantMap{ { QStringLiteral("name"), QStringLiteral("Known Artist") },
                         { QStringLiteral("browseId"), QStringLiteral("UCknownartist01") } },
            QVariantMap{ { QStringLiteral("name"), QStringLiteral("Next Artist") },
                         { QStringLiteral("browseId"), QStringLiteral("UCnextartist001") } } };
        catalog.loadMoreLikeFrom(candidates);
        waitUntil([&]() { return int(standIn.requests.size()) > from; }, 3000);
        pause(300);
        const QList<StandIn::Request> asked = standIn.requests.mid(from);
        t.check(asked.size() == 1 && asked.first().path.startsWith("/youtubei/v1/browse")
                    && asked.first().body.value(QLatin1String("browseId")).toString() == QLatin1String("UCknownartist01"),
                QStringLiteral("24 a known browseId: one browse of that page, no search"),
                asked.isEmpty() ? QStringLiteral("none") : QString::fromLatin1(asked.first().path));
        catalog.loadMoreLikeFrom(candidates);
        waitUntil([&]() { return int(standIn.requests.size()) > from + 1; }, 3000);
        pause(300);
        const QList<StandIn::Request> next = standIn.requests.mid(from + 1);
        t.check(next.size() == 1 && next.first().body.value(QLatin1String("browseId")).toString() == QLatin1String("UCnextartist001")
                    && standIn.count("/youtubei/v1/search") == 0,
                QStringLiteral("24 its page had no artists' shelf: remembered, and the next call tries the next one"));
    }

    // ------------------------------------------------ 25. ListenTracker::listenSettled
    {
        ListenTracker tracker;
        QStringList order;
        qint64 settledAt = -1;
        qint64 qualifiedAt = -1;
        int settledCount = 0;
        QObject::connect(&tracker, &ListenTracker::listenSettled, &tracker, [&](const QVariantMap &, qint64, bool) {
            ++settledCount;
            settledAt = tracker.heardMs();
            order << QStringLiteral("settled");
        });
        QObject::connect(&tracker, &ListenTracker::listenQualified, &tracker, [&](const QVariantMap &, qint64, bool) {
            qualifiedAt = tracker.heardMs();
            order << QStringLiteral("qualified");
        });
        const auto reset = [&]() {
            order.clear();
            settledAt = qualifiedAt = -1;
            settledCount = 0;
        };
        listen(tracker, makeId(QStringLiteral("lt"), 1), true, 200);
        t.check(settledCount == 1 && settledAt == 45000 && qualifiedAt == 90000,
                QStringLiteral("25 a 180 s song: settled once at 45 s, qualified at 90 s as before"),
                QStringLiteral("%1 / %2").arg(settledAt).arg(qualifiedAt));
        reset();
        listen(tracker, makeId(QStringLiteral("lt"), 2), true, 70, 60000);
        t.check(settledAt == 30000 && qualifiedAt == 30000 && order == QStringList({ QStringLiteral("settled"), QStringLiteral("qualified") }),
                QStringLiteral("25 a 60 s song: both at its threshold (30 s), settled first"));
        reset();
        listen(tracker, makeId(QStringLiteral("lt"), 3), true, 400, 600000);
        t.check(settledAt == 45000 && qualifiedAt == 240000, QStringLiteral("25 a 10 min song: 45 s, then 4 min"));
        reset();
        listen(tracker, makeId(QStringLiteral("lt"), 4), true, 25, 25000);
        t.check(settledCount == 0 && qualifiedAt < 0, QStringLiteral("25 a 25 s song: neither"));
        reset();
        listen(tracker, makeId(QStringLiteral("lt"), 5), false, 50, 0);
        t.check(settledCount == 0, QStringLiteral("25 no length known: not settled"));
        tracker.setDuration(180000);
        t.check(settledCount == 1 && settledAt == 50000 && qualifiedAt < 0,
                QStringLiteral("25 the length learned at 50 s: settled then, not qualified"));
        hear(tracker, 51, 90);
        t.check(qualifiedAt == 90000, QStringLiteral("25 and qualified at 90 s"));
    }

    // ------------------------------------------------ 26. watchPlaylist's retries
    {
        InnerTube tube(nullptr, /*warmUp=*/false);
        const QString zero = makeId(QStringLiteral("wr"), 1);
        const QString one = makeId(QStringLiteral("wr"), 2);
        modes.insert(zero, QStringLiteral("500"));
        modes.insert(one, QStringLiteral("500"));
        bool done = false;
        QString error;
        tube.watchPlaylist({ zero, QStringLiteral("RDAMVM") + zero, QStringLiteral("wAEB") },
                           [&](const QList<InnerTube::Track> &, const QString &why) {
                               done = true;
                               error = why;
                           },
                           /*retries=*/0);
        waitUntil([&]() { return done; }, 4000);
        pause(1800);
        t.check(done && !error.isEmpty() && standIn.nexts().count(zero) == 1,
                QStringLiteral("26 retries = 0 against a 500: one attempt"), QString::number(standIn.nexts().count(zero)));
        done = false;
        tube.watchPlaylist({ one, QString(), QString() }, [&](const QList<InnerTube::Track> &, const QString &) { done = true; });
        waitUntil([&]() { return done; }, 6000);
        t.check(done && standIn.nexts().count(one) == 2,
                QStringLiteral("26 the default (Catalog's) still makes 2"), QString::number(standIn.nexts().count(one)));
    }

    // Nothing secret in what was logged.
    bool secret = false;
    for (const QString &line : std::as_const(log))
        secret = secret || line.contains(QLatin1String("TESTJUSTPLAYED")) || line.contains(QLatin1String("SAPISID"));
    t.check(!secret, QStringLiteral("2  no cookie or SAPISID in the log (%1 lines)").arg(log.size()));

    JustPlayed::setClock({});
    ListenTracker::setClock({});
    InnerTube::setAccountHook({});
    InnerTube::setTestServer(QString());
    qInstallMessageHandler(g_previousHandler);
    g_log = nullptr;
    freshState();
    return t.finish();
}
