#include "lyricsselftest.h"

#include "appdatabase.h"
#include "innertube.h"
#include "lyrics.h"
#include "lyricsquery.h"
#include "playbackcontroller.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
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

// A piece of an artist line as YouTube Music's credits (and ArtistLinks)
// give it: a name with its page, or the joiner between two names.
QVariantMap piece(const QString &text, bool name)
{
    return {
        { QStringLiteral("text"), text },
        { QStringLiteral("id"), name ? QStringLiteral("UCselftest") + QString::number(qHash(text)) : QString() },
        { QStringLiteral("link"), name }
    };
}

QVariantList oneName(const QString &name)
{
    return { piece(name, true) };
}

LyricsQuery::Query queryFor(const QString &title, const QString &artist, double seconds,
                            const QVariantList &credits = {})
{
    return LyricsQuery::fromTrack({ { QStringLiteral("title"), title },
                                    { QStringLiteral("artist"), artist },
                                    { QStringLiteral("durationMs"), qint64(seconds * 1000) },
                                    { QStringLiteral("credits"), credits } });
}

QString u(const char *utf8)
{
    return QString::fromUtf8(utf8);
}

} // namespace

// ------------------------------------------------------------- the query

int runLyricsQuerySelfTest()
{
    Checks t("lyrics-query-test");

    // — the 18 fixed cases (bench lyrics_query_cases.json, checklist C40) —
    struct TitleCase {
        const char *title;
        const char *artist;
        const char *want;
    };
    const TitleCase titles[] = {
        { "Song (Remix)", "Artist", "Song (Remix)" },
        { "Song (Live)", "Artist", "Song (Live)" },
        { "Song (Sped Up)", "Artist", "Song (Sped Up)" },
        { "Song (Acoustic Version) [Official Audio]", "Artist", "Song (Acoustic Version)" },
        { "Song (Official Live Video)", "Artist", "Song (Live)" },
        { "Song (Live Video)", "Artist", "Song (Live)" },
        { "Song (Official Remix)", "Artist", "Song (Remix)" },
        { "Song (Sped Up Audio)", "Artist", "Song (Sped Up)" },
        { "To Sir (With Love)", "Lulu", "To Sir (With Love)" },
        { "Song feat. Guest (Remix)", "Artist", "Song (Remix)" },
        { "Song (feat. Guest)", "Artist", "Song" },
        { "Dancing with a Stranger", "Sam Smith", "Dancing with a Stranger" },
        { "Artist - Song (Official Video)", "Artist", "Song" },
        { "Song (Remastered 2011)", "Artist", "Song" },
    };
    int wrong = 0;
    for (const TitleCase &c : titles) {
        const LyricsQuery::Query q = queryFor(QString::fromUtf8(c.title), QString::fromUtf8(c.artist), 200,
                                              oneName(QString::fromUtf8(c.artist)));
        const bool ok = q.title == QString::fromUtf8(c.want);
        wrong += ok ? 0 : 1;
        t.check(ok, QStringLiteral("title \"%1\" -> \"%2\"").arg(QString::fromUtf8(c.title), QString::fromUtf8(c.want)),
                QStringLiteral("got \"%1\"").arg(q.title));
    }
    // The artist cases, as YouTube Music credits them: a band's name is one
    // link however many commas and ampersands it holds.
    struct ArtistCase {
        const char *line;
        QVariantList credits;
        const char *want;
    };
    const ArtistCase artists[] = {
        { "Simon & Garfunkel", oneName(QStringLiteral("Simon & Garfunkel")), "Simon & Garfunkel" },
        { "Earth, Wind & Fire", oneName(QStringLiteral("Earth, Wind & Fire")), "Earth, Wind & Fire" },
        { "Feathers", oneName(QStringLiteral("Feathers")), "Feathers" },
        { "Daft Punk, Pharrell Williams & Nile Rodgers",
          { piece(QStringLiteral("Daft Punk"), true), piece(QStringLiteral(", "), false),
            piece(QStringLiteral("Pharrell Williams"), true), piece(QStringLiteral(" & "), false),
            piece(QStringLiteral("Nile Rodgers"), true) },
          "Daft Punk" },
    };
    for (const ArtistCase &c : artists) {
        const LyricsQuery::Query q = queryFor(QStringLiteral("Song"), QString::fromUtf8(c.line), 200, c.credits);
        const bool ok = q.leadArtist() == QString::fromUtf8(c.want);
        wrong += ok ? 0 : 1;
        t.check(ok, QStringLiteral("artist \"%1\" (credited) -> \"%2\"").arg(QString::fromUtf8(c.line), QString::fromUtf8(c.want)),
                QStringLiteral("got \"%1\"").arg(q.leadArtist()));
    }
    t.check(wrong == 0, QStringLiteral("C40 fixed cases: %1 of 18 not as wanted").arg(wrong));

    // — lines without credits (Liked songs, the history): never cut on a guess —
    const auto namesOf = [](const QString &line, const QString &primary = QString(), const QVariantList &pieces = {}) {
        return LyricsQuery::artistNames(line, primary, pieces);
    };
    t.check(namesOf(QStringLiteral("Simon & Garfunkel")) == QStringList{ QStringLiteral("Simon & Garfunkel") },
            QStringLiteral("no credits: \"Simon & Garfunkel\" stays whole (never split on '&' alone)"));
    t.check(namesOf(QStringLiteral("Earth, Wind & Fire")) == QStringList{ QStringLiteral("Earth, Wind & Fire") },
            QStringLiteral("no credits: \"Earth, Wind & Fire\" stays whole"));
    t.check(namesOf(QStringLiteral("Feathers")) == QStringList{ QStringLiteral("Feathers") },
            QStringLiteral("no credits: \"Feathers\" is not cut at \"feat\""));
    t.check(namesOf(QStringLiteral("Daft Punk, Pharrell Williams & Nile Rodgers"))
                == QStringList{ QStringLiteral("Daft Punk, Pharrell Williams & Nile Rodgers") },
            QStringLiteral("no credits, names nobody linked: the line stays whole rather than cut at a comma"));
    t.check(namesOf(QStringLiteral("Dua Lipa feat. DaBaby")) == QStringList{ QStringLiteral("Dua Lipa"), QStringLiteral("DaBaby") },
            QStringLiteral("no credits: an unambiguous \"feat.\" still splits"));
    t.check(namesOf(QStringLiteral("Daft Punk, Pharrell Williams & Nile Rodgers"), QString(),
                    { piece(QStringLiteral("Daft Punk"), true), piece(QStringLiteral(", "), false),
                      piece(QStringLiteral("Pharrell Williams"), true), piece(QStringLiteral(" & "), false),
                      piece(QStringLiteral("Nile Rodgers"), true) })
                .value(0) == QLatin1String("Daft Punk"),
            QStringLiteral("no credits, names ArtistLinks knows: split into them, lead first"));
    t.check(namesOf(QStringLiteral("Bruno Mars & Lady Gaga"), QStringLiteral("Bruno Mars")).value(0) == QLatin1String("Bruno Mars"),
            QStringLiteral("primaryArtist alone leads"));
    t.check(namesOf(QStringLiteral("The Weeknd - Topic")) == QStringList{ QStringLiteral("The Weeknd") },
            QStringLiteral("a \" - Topic\" channel is the artist"));

    // — more titles —
    const auto title = [](const QString &raw, const QString &artist, const QVariantList &credits = {}) {
        return queryFor(raw, artist, 200, credits).title;
    };
    t.check(title(QStringLiteral("Song (with Guest)"), QStringLiteral("Artist & Guest"),
                  { piece(QStringLiteral("Artist"), true), piece(QStringLiteral(" & "), false), piece(QStringLiteral("Guest"), true) })
                == QLatin1String("Song"),
            QStringLiteral("\"(with Guest)\" goes when Guest is credited"));
    t.check(title(QStringLiteral("Song (with Guest)"), QStringLiteral("Artist & Guest")) == QLatin1String("Song"),
            QStringLiteral("... or named in an artist line that has no credits"));
    t.check(title(QStringLiteral("Song (Remix feat. Guest)"), QStringLiteral("Artist")) == QLatin1String("Song (Remix)"),
            QStringLiteral("\"(Remix feat. Guest)\" keeps its marker"));
    t.check(title(QStringLiteral("Hey Jude - Remastered 2015"), QStringLiteral("The Beatles")) == QLatin1String("Hey Jude"),
            QStringLiteral("\" - Remastered 2015\" goes"));
    t.check(title(QStringLiteral("Sunday Bloody Sunday - Live"), QStringLiteral("U2")) == QLatin1String("Sunday Bloody Sunday - Live"),
            QStringLiteral("\" - Live\" stays"));
    t.check(title(QStringLiteral("Mama - Remix"), QStringLiteral("M")) == QLatin1String("Mama - Remix"),
            QStringLiteral("an artist called \"M\" does not own \"Mama - \""));
    // The 30-track list's titles are asked for as before.
    t.check(title(QStringLiteral("Dreams (2004 Remaster)"), QStringLiteral("Fleetwood Mac")) == QLatin1String("Dreams"),
            QStringLiteral("list #3: \"Dreams (2004 Remaster)\" -> \"Dreams\""));
    t.check(title(QStringLiteral("Juicy (2005 Remaster)"), QStringLiteral("Notorious B.I.G.")) == QLatin1String("Juicy"),
            QStringLiteral("list #6: \"Juicy (2005 Remaster)\" -> \"Juicy\""));
    t.check(title(QStringLiteral("Get Lucky (feat. Pharrell Williams and Nile Rodgers)"),
                  QStringLiteral("Daft Punk, Pharrell Williams & Nile Rodgers")) == QLatin1String("Get Lucky"),
            QStringLiteral("list #8: \"Get Lucky (feat. ...)\" -> \"Get Lucky\""));
    t.check(title(QStringLiteral("La Bamba (Single Version)"), QStringLiteral("Ritchie Valens")) == QLatin1String("La Bamba (Single Version)"),
            QStringLiteral("list #15: \"(Single Version)\" stays, as before"));
    t.check(title(u("上を向いて歩こう - Sukiyaki"), QStringLiteral("Kyu Sakamoto")) == u("上を向いて歩こう - Sukiyaki"),
            QStringLiteral("list #25: a \" - \" that is not the artist stays"));
    t.check(LyricsQuery::bareTitle(QStringLiteral("Song (Live)")) == QLatin1String("Song")
                && LyricsQuery::bareTitle(QStringLiteral("To Sir (With Love)")) == QLatin1String("To Sir")
                && LyricsQuery::bareTitle(QStringLiteral("Sunday Bloody Sunday - Live")) == QLatin1String("Sunday Bloody Sunday"),
            QStringLiteral("the bare title: markers and brackets off"));

    // — the scorer —
    const auto accepted = [](const LyricsQuery::Query &q, const QString &entryTitle, const QString &entryArtist, double seconds) {
        return LyricsQuery::match(q, entryTitle, entryArtist, seconds).accepted;
    };
    t.check(!accepted(queryFor(QStringLiteral("Hero"), QStringLiteral("Enrique Iglesias"), 264),
                      QStringLiteral("Anti-Hero"), QStringLiteral("Enrique Iglesias"), 264),
            QStringLiteral("\"Anti-Hero\" does not match \"Hero\", even by the same artist at the same length"));
    t.check(!accepted(queryFor(QStringLiteral("Anti-Hero"), QStringLiteral("Taylor Swift"), 200),
                      QStringLiteral("Hero"), QStringLiteral("Taylor Swift"), 200),
            QStringLiteral("... nor \"Hero\" \"Anti-Hero\""));
    t.check(!accepted(queryFor(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"), 200),
                      QStringLiteral("Save Your Tears"), QStringLiteral("The Weeknd"), 215),
            QStringLiteral("an entry matching the artist alone is refused"));
    t.check(!accepted(queryFor(QStringLiteral("Hero"), QStringLiteral("Mariah Carey"), 259),
                      QStringLiteral("Hero"), QStringLiteral("Enrique Iglesias"), 264),
            QStringLiteral("the same title by another artist, 5 s apart, is refused"));
    t.check(accepted(queryFor(QStringLiteral("Get Lucky (feat. Pharrell Williams and Nile Rodgers)"),
                              QStringLiteral("Daft Punk, Pharrell Williams & Nile Rodgers"), 369,
                              { piece(QStringLiteral("Daft Punk"), true), piece(QStringLiteral(", "), false),
                                piece(QStringLiteral("Pharrell Williams"), true), piece(QStringLiteral(" & "), false),
                                piece(QStringLiteral("Nile Rodgers"), true) }),
                     QStringLiteral("Get Lucky"), QStringLiteral("Daft Punk"), 369),
            QStringLiteral("\"Get Lucky (feat. ...)\" matches \"Get Lucky\" by Daft Punk"));
    t.check(accepted(queryFor(u("Tití Me Preguntó"), QStringLiteral("Bad Bunny"), 244),
                     QStringLiteral("Titi Me Pregunto"), QStringLiteral("Bad Bunny"), 244),
            QStringLiteral("accents do not count"));
    t.check(accepted(queryFor(QStringLiteral("Don't Stop Me Now"), QStringLiteral("Queen"), 209),
                     QStringLiteral("Dont Stop Me Now"), QStringLiteral("Queen"), 210),
            QStringLiteral("nor apostrophes"));
    t.check(accepted(queryFor(QStringLiteral("Hey Jude"), QStringLiteral("The Beatles"), 426),
                     QStringLiteral("Hey Jude - Remastered 2015"), QStringLiteral("The Beatles"), 425),
            QStringLiteral("an entry's \" - Remastered 2015\" does not count against it"));
    t.check(accepted(queryFor(QStringLiteral("Spring Day"), QStringLiteral("BTS"), 275),
                     u("봄날 (Spring Day)"), u("BTS (방탄소년단)"), 274),
            QStringLiteral("a title filed with its translation in brackets"));
    t.check(accepted(queryFor(QStringLiteral("Juicy (2005 Remaster)"), QStringLiteral("Notorious B.I.G."), 303),
                     QStringLiteral("Juicy"), QStringLiteral("The Notorious B.I.G."), 302),
            QStringLiteral("\"The Notorious B.I.G.\" is \"Notorious B.I.G.\""));
    t.check(accepted(queryFor(QStringLiteral("Despacito"), QStringLiteral("Luis Fonsi & Daddy Yankee"), 229),
                     QStringLiteral("Despacito"), QStringLiteral("Luis Fonsi"), 229),
            QStringLiteral("an unsplit line holds the entry's artist"));
    t.check(accepted(queryFor(QStringLiteral("Lemon"), QStringLiteral("Kenshi Yonezu"), 257),
                     QStringLiteral("Lemon"), u("米津玄師"), 256),
            QStringLiteral("an artist in another script: the length decides (1 s)"));
    t.check(!accepted(queryFor(QStringLiteral("Lemon"), QStringLiteral("Kenshi Yonezu"), 257),
                      QStringLiteral("Lemon"), u("米津玄師"), 240),
            QStringLiteral("... and refuses at 17 s"));
    t.check(accepted(queryFor(QStringLiteral("Song (Live)"), QStringLiteral("Artist"), 200),
                     QStringLiteral("Song"), QStringLiteral("Artist"), 230),
            QStringLiteral("a live take and the studio one are the same song (the length picks the timing)"));

    return t.finish();
}

// ------------------------------------------------------------- the flow

namespace {

// A stand-in for LRCLIB and YouTube Music on this computer: it keeps each
// request's path, and answers as `respond` says. One request a connection,
// closed after the answer.
class StandIn : public QObject
{
public:
    struct Answer {
        int status = 200;
        QByteArray body = "{}";
        bool drop = false;   // close the connection without a word
        bool hold = false;   // keep it open and never answer
        int delayMs = 0;     // answer this much later
    };

    std::function<Answer(const QByteArray &path)> respond;
    QList<QByteArray> paths;

    bool listen()
    {
        connect(&m_server, &QTcpServer::newConnection, this, &StandIn::accept);
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    int count(const char *prefix) const
    {
        int n = 0;
        for (const QByteArray &path : paths)
            n += path.startsWith(prefix) ? 1 : 0;
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
        const QByteArray path = lines.value(0).trimmed().split(' ').value(1);
        int length = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        }
        if (buffer.size() < end + 4 + length)
            return;
        buffer.clear();
        paths.append(path);

        const Answer answer = respond ? respond(path) : Answer();
        if (answer.drop) {
            socket->abort();
            return;
        }
        if (answer.hold)
            return;
        const QByteArray reason = answer.status == 200 ? "OK" : answer.status == 404 ? "Not Found" : "Error";
        const QByteArray out = "HTTP/1.1 " + QByteArray::number(answer.status) + ' ' + reason + "\r\n"
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

struct Row {
    bool exists = false;
    QString synced;
    QString plain;
    QString source;
    bool provisional = false;
    bool recent = false;   // fetched within the last minute
};

Row rowFor(const QString &videoId)
{
    Row row;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT synced, plain, source, provisional, fetched_at > datetime('now', '-1 minute')"
                             " FROM lyrics WHERE video_id = ?"));
    q.addBindValue(videoId);
    if (q.exec() && q.next()) {
        row.exists = true;
        row.synced = q.value(0).toString();
        row.plain = q.value(1).toString();
        row.source = q.value(2).toString();
        row.provisional = q.value(3).toBool();
        row.recent = q.value(4).toBool();
    }
    return row;
}

void putRow(const QString &videoId, const QString &synced, const QString &plain, const QString &source,
            const QString &age)
{
    QSqlQuery q(AppDatabase::connection());
    // No provisional column named: a row as the app wrote it before there was one.
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO lyrics (video_id, synced, plain, source, fetched_at)"
                             " VALUES (?, ?, ?, ?, datetime('now', ?))"));
    q.addBindValue(videoId);
    q.addBindValue(synced);
    q.addBindValue(plain);
    q.addBindValue(source);
    q.addBindValue(age);
    q.exec();
}

const QByteArray kEntry =
    R"({"id":1,"trackName":"Selftest Song","artistName":"Selftest Artist","albumName":"Selftest Album",)"
    R"("duration":200,"instrumental":false,"plainLyrics":"First line\nSecond line",)"
    R"("syncedLyrics":"[00:01.00] First line\n[00:05.00] Second line"})";
// The same song under the same title, with other words: what a search can
// hold where the exact lookup has the right entry.
const QByteArray kOtherWords =
    R"([{"id":3,"trackName":"Selftest Song","artistName":"Selftest Artist","albumName":"Singles",)"
    R"("duration":200,"instrumental":false,"plainLyrics":"Search words",)"
    R"("syncedLyrics":"[00:01.00] Search words"}])";
const QByteArray kWrongSong =
    R"([{"id":2,"trackName":"Anti-Hero","artistName":"Selftest Artist","albumName":"Selftest Album",)"
    R"("duration":200,"instrumental":false,"plainLyrics":"Wrong song",)"
    R"("syncedLyrics":"[00:01.00] Wrong song"}])";
const QByteArray kNextWithLyrics =
    R"({"contents":{"singleColumnMusicWatchNextResultsRenderer":{"tabbedRenderer":{"watchNextTabbedResultsRenderer":)"
    R"({"tabs":[{"tabRenderer":{}},{"tabRenderer":{"endpoint":{"browseEndpoint":{"browseId":"MPLYtselftest"}}}}]}}}}})";
const QByteArray kNextWithout =
    R"({"contents":{"singleColumnMusicWatchNextResultsRenderer":{"tabbedRenderer":{"watchNextTabbedResultsRenderer":)"
    R"({"tabs":[{"tabRenderer":{}},{"tabRenderer":{}}]}}}}})";
const QByteArray kLyricsPage =
    R"({"contents":{"sectionListRenderer":{"contents":[{"musicDescriptionShelfRenderer":)"
    R"({"description":{"runs":[{"text":"Selftest line one\nSelftest line two"}]},)"
    R"("footer":{"runs":[{"text":"Source: Selftest"}]}}}]}}})";

} // namespace

int runLyricsFlowSelfTest(PlaybackController *player)
{
    Checks t("lyrics-flow-test");
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes lyrics rows"));
        return t.finish();
    }

    // How each source answers, set per case.
    enum class Exact { Synced, Slow, Missing, Hold, Drop, Error };
    enum class Search { Synced, OtherWords, Empty, Hold, WrongSong };
    enum class Tube { Text, None, Error };
    Exact exact = Exact::Synced;
    Search search = Search::Synced;
    Search words = Search::Empty;
    Tube tube = Tube::Text;

    StandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in server on this computer")))
        return t.finish();
    standIn.respond = [&](const QByteArray &path) {
        StandIn::Answer answer;
        const auto searchAnswer = [&answer](Search kind) {
            switch (kind) {
            case Search::Synced:     answer.body = "[" + kEntry + "]"; break;
            case Search::OtherWords: answer.body = kOtherWords; break;
            case Search::Empty:      answer.body = "[]"; break;
            case Search::Hold:      answer.hold = true; break;
            case Search::WrongSong: answer.body = kWrongSong; break;
            }
        };
        if (path.startsWith("/api/get?")) {
            switch (exact) {
            case Exact::Synced:  answer.body = kEntry; break;
            case Exact::Slow:    answer.body = kEntry; answer.delayMs = 600; break;
            case Exact::Missing: answer.status = 404; answer.body = R"({"code":404,"name":"TrackNotFound"})"; break;
            case Exact::Hold:    answer.hold = true; break;
            case Exact::Drop:    answer.drop = true; break;
            case Exact::Error:   answer.status = 500; break;
            }
        } else if (path.startsWith("/api/search?q=")) {
            searchAnswer(words);
        } else if (path.startsWith("/api/search")) {
            searchAnswer(search);
        } else if (path.startsWith("/youtubei/v1/next")) {
            if (tube == Tube::Error)
                answer.status = 500;
            else
                answer.body = tube == Tube::Text ? kNextWithLyrics : kNextWithout;
        } else if (path.startsWith("/youtubei/v1/browse")) {
            answer.body = kLyricsPage;
        }
        return answer;
    };
    // Before the Lyrics below exists, so its InnerTube opens no connection to
    // YouTube; every request goes to the stand-in.
    InnerTube::setTestServer(standIn.base());
    t.note(QStringLiteral("LRCLIB and YouTube Music are ") + standIn.base() + QStringLiteral("; nothing leaves this computer"));

    Lyrics probe(player);
    // Only the lookups this test makes: the player changing song must not
    // clear one halfway.
    QObject::disconnect(player, nullptr, &probe, nullptr);
    probe.setLrclibUrl(standIn.base());

    const auto track = [](const QString &id, const QString &title = QStringLiteral("Selftest Song")) {
        return QVariantMap{ { QStringLiteral("sourceId"), id },
                            { QStringLiteral("title"), title },
                            { QStringLiteral("artist"), QStringLiteral("Selftest Artist") },
                            { QStringLiteral("album"), QStringLiteral("Selftest Album") },
                            { QStringLiteral("durationMs"), qint64(200000) },
                            { QStringLiteral("credits"), oneName(QStringLiteral("Selftest Artist")) } };
    };
    QElapsedTimer clock;
    const auto lookup = [&](const QVariantMap &song, int timeoutMs = 15000) {
        standIn.paths.clear();
        clock.start();
        probe.lookup(song);
        waitUntil([&probe]() { return probe.state() != QLatin1String("loading") && !probe.checking(); }, timeoutMs);
        return clock.elapsed();
    };
    const auto requests = [&standIn]() {
        return QStringLiteral("get %1, search %2, words %3, YouTube Music %4")
            .arg(standIn.count("/api/get?")).arg(standIn.count("/api/search?track_name"))
            .arg(standIn.count("/api/search?q=")).arg(standIn.count("/youtubei/v1/next"));
    };
    const auto shown = [&probe]() {
        return QStringLiteral("%1 from %2").arg(probe.state(), probe.source().isEmpty() ? QStringLiteral("-") : probe.source());
    };

    // — LRCLIB: the exact lookup, then the search —
    exact = Exact::Synced;
    lookup(track(QStringLiteral("lyrtest-exact")));
    t.check(probe.state() == QLatin1String("synced") && probe.source() == QLatin1String("LRCLIB")
                && standIn.count("/api/get?") == 1 && standIn.count("/api/search") == 0,
            QStringLiteral("the exact lookup answers: synced, and no search asked"), shown() + QStringLiteral("; ") + requests());
    t.check(standIn.paths.value(0).contains("album_name=Selftest%20Album") && standIn.paths.value(0).contains("duration=200"),
            QStringLiteral("it names the album and the length"), QString::fromUtf8(standIn.paths.value(0)));
    t.check(!rowFor(QStringLiteral("lyrtest-exact")).provisional, QStringLiteral("... and is kept as final"));

    exact = Exact::Missing;
    search = Search::Synced;
    lookup(track(QStringLiteral("lyrtest-search")));
    t.check(probe.state() == QLatin1String("synced") && standIn.count("/api/get?") == 1
                && standIn.count("/api/search?track_name") == 1,
            QStringLiteral("a 404 from the exact lookup: the search answers"), shown() + QStringLiteral("; ") + requests());

    // Slow, the exact lookup has the search asked as well; its answer still
    // outranks the search's, which is held for it for a second.
    exact = Exact::Slow;
    search = Search::OtherWords;
    qint64 ms = lookup(track(QStringLiteral("lyrtest-slow")));
    Row row = rowFor(QStringLiteral("lyrtest-slow"));
    t.check(probe.state() == QLatin1String("synced") && standIn.count("/api/search?track_name") == 1
                && row.synced.contains(QLatin1String("First line")) && ms >= 550 && ms < 1000,
            QStringLiteral("the exact lookup slow (600 ms): the search is asked too, but the exact answer wins (%1 ms)").arg(ms),
            shown() + QStringLiteral("; ") + requests() + QStringLiteral("; kept: ") + row.synced.left(30));

    exact = Exact::Hold;
    search = Search::OtherWords;
    ms = lookup(track(QStringLiteral("lyrtest-silent")));
    row = rowFor(QStringLiteral("lyrtest-silent"));
    t.check(probe.state() == QLatin1String("synced") && row.synced.contains(QLatin1String("Search words"))
                && ms >= 950 && ms < 2000,
            QStringLiteral("the exact lookup silent: the search's answer is taken a second after it was sent (%1 ms)").arg(ms),
            shown() + QStringLiteral("; ") + requests());

    exact = Exact::Error;
    search = Search::Synced;
    lookup(track(QStringLiteral("lyrtest-http500")));
    t.check(probe.state() == QLatin1String("synced") && standIn.count("/api/search?track_name") == 1,
            QStringLiteral("the exact lookup answering HTTP 500: the search is still asked"), shown() + QStringLiteral("; ") + requests());

    exact = Exact::Missing;
    search = Search::WrongSong;
    words = Search::Empty;
    tube = Tube::None;
    lookup(track(QStringLiteral("lyrtest-hero"), QStringLiteral("Hero")));
    t.check(probe.state() == QLatin1String("none"),
            QStringLiteral("a search entry for \"Anti-Hero\" is not taken for \"Hero\""), shown() + QStringLiteral("; ") + requests());
    probe.setLrclibExact(false);
    lookup(track(QStringLiteral("lyrtest-hero-old"), QStringLiteral("Hero")));
    t.check(probe.state() == QLatin1String("synced") && standIn.count("/api/get?") == 0,
            QStringLiteral("lyrics.lrclib=search: the search alone, taken by length, as before"),
            shown() + QStringLiteral("; ") + requests());
    probe.setLrclibExact(true);

    // — an answer kept only because a better source failed —
    exact = Exact::Drop;
    tube = Tube::Text;
    lookup(track(QStringLiteral("lyrtest-down")));
    row = rowFor(QStringLiteral("lyrtest-down"));
    t.check(probe.state() == QLatin1String("plain") && row.provisional && standIn.count("/api/search") == 0,
            QStringLiteral("LRCLIB out of reach: YouTube Music's plain lyrics shown, kept as provisional, no search tried"),
            shown() + QStringLiteral("; ") + requests());

    exact = Exact::Drop;
    lookup(track(QStringLiteral("lyrtest-down")));
    row = rowFor(QStringLiteral("lyrtest-down"));
    t.check(probe.state() == QLatin1String("plain") && row.provisional && standIn.count("/api/get?") == 1,
            QStringLiteral("viewed again with LRCLIB still down: asked again, still provisional"),
            shown() + QStringLiteral("; ") + requests());

    exact = Exact::Synced;
    standIn.paths.clear();
    clock.start();
    probe.lookup(track(QStringLiteral("lyrtest-down")));
    const bool shownAtOnce = probe.state() == QLatin1String("plain") && probe.checking();
    waitUntil([&probe]() { return !probe.checking(); }, 15000);
    row = rowFor(QStringLiteral("lyrtest-down"));
    t.check(shownAtOnce, QStringLiteral("viewed with LRCLIB back: the kept lyrics show at once, while it is asked again"));
    t.check(probe.state() == QLatin1String("synced") && probe.source() == QLatin1String("LRCLIB") && !row.provisional
                && row.synced.contains(QLatin1String("First line")),
            QStringLiteral("... and LRCLIB's synced lyrics replace them, kept as final (%1 ms)").arg(clock.elapsed()),
            shown());

    // Both directions: YouTube Music failing leaves LRCLIB's other-edit words
    // provisional too, and a failure is never kept as "none".
    exact = Exact::Missing;
    search = Search::Empty;
    words = Search::Empty;
    tube = Tube::Error;
    lookup(track(QStringLiteral("lyrtest-reverse")));
    t.check(probe.state() == QLatin1String("error") && !rowFor(QStringLiteral("lyrtest-reverse")).exists
                && standIn.count("/api/search?q=") == 1,
            QStringLiteral("LRCLIB has none, YouTube Music fails: an error to retry, no \"none\" kept"),
            shown() + QStringLiteral("; ") + requests());

    exact = Exact::Drop;
    tube = Tube::None;
    lookup(track(QStringLiteral("lyrtest-reverse2")));
    t.check(probe.state() == QLatin1String("error") && !rowFor(QStringLiteral("lyrtest-reverse2")).exists,
            QStringLiteral("LRCLIB fails, YouTube Music has none: an error, no \"none\" kept"),
            shown() + QStringLiteral("; ") + requests());

    exact = Exact::Missing;
    search = Search::Empty;
    words = Search::Empty;
    tube = Tube::None;
    lookup(track(QStringLiteral("lyrtest-none")));
    row = rowFor(QStringLiteral("lyrtest-none"));
    t.check(probe.state() == QLatin1String("none") && row.exists && row.synced.isEmpty() && row.plain.isEmpty()
                && !row.provisional,
            QStringLiteral("every source answers it has none: \"none\" kept"), shown() + QStringLiteral("; ") + requests());

    // One deadline for LRCLIB, whatever its requests are doing.
    exact = Exact::Hold;
    search = Search::Hold;
    tube = Tube::Text;
    ms = lookup(track(QStringLiteral("lyrtest-deadline")), 20000);
    row = rowFor(QStringLiteral("lyrtest-deadline"));
    t.check(probe.state() == QLatin1String("plain") && row.provisional && ms >= 5900 && ms < 9000,
            QStringLiteral("LRCLIB silent: given up after 6 s, YouTube Music's lyrics provisional (%1 ms)").arg(ms),
            shown() + QStringLiteral("; ") + requests());

    // — rows kept before, and the refresh age —
    putRow(QStringLiteral("lyrtest-old"), QStringLiteral("[00:01.00] Old words"), QString(), QStringLiteral("LRCLIB"),
           QStringLiteral("-10 days"));
    lookup(track(QStringLiteral("lyrtest-old")));
    t.check(probe.state() == QLatin1String("synced") && standIn.paths.isEmpty(),
            QStringLiteral("a row from before the provisional column reads as final: shown, nothing asked"), requests());

    exact = Exact::Synced;
    putRow(QStringLiteral("lyrtest-aged"), QStringLiteral("[00:01.00] Old words"), QString(), QStringLiteral("LRCLIB"),
           QStringLiteral("-61 days"));
    standIn.paths.clear();
    probe.lookup(track(QStringLiteral("lyrtest-aged")));
    const bool agedShown = probe.state() == QLatin1String("synced") && probe.checking();
    waitUntil([&probe]() { return !probe.checking(); }, 15000);
    row = rowFor(QStringLiteral("lyrtest-aged"));
    t.check(agedShown && row.synced.contains(QLatin1String("First line")) && row.recent,
            QStringLiteral("found lyrics 61 days old: shown at once, checked behind them, and replaced"), requests());

    exact = Exact::Drop;
    tube = Tube::Text;
    putRow(QStringLiteral("lyrtest-keep"), QStringLiteral("[00:01.00] Old words"), QString(), QStringLiteral("LRCLIB"),
           QStringLiteral("-61 days"));
    lookup(track(QStringLiteral("lyrtest-keep")));
    row = rowFor(QStringLiteral("lyrtest-keep"));
    t.check(probe.state() == QLatin1String("synced") && row.synced.contains(QLatin1String("Old words")) && !row.provisional,
            QStringLiteral("... but a stand-in (LRCLIB down, YouTube Music's text) never replaces them"),
            shown() + QStringLiteral("; ") + requests());

    QSqlQuery cleanup(AppDatabase::connection());
    cleanup.exec(QStringLiteral("DELETE FROM lyrics WHERE video_id LIKE 'lyrtest-%'"));
    InnerTube::setTestServer(QString());
    return t.finish();
}
