#include "lyricsselftest.h"

#include "appdatabase.h"
#include "innertube.h"
#include "lyrics.h"
#include "lyricsquery.h"
#include "mediaextractor.h"
#include "playbackcontroller.h"
#include "lyrics/lyricsrace.h"

#include <QCoreApplication>
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

    // Rows a run before this one left, in either table: an answer kept per
    // provider would stand in for the requests this test counts.
    {
        QSqlQuery clean(AppDatabase::connection());
        clean.exec(QStringLiteral("DELETE FROM lyrics WHERE video_id LIKE 'lyrtest-%'"));
        clean.exec(QStringLiteral("DELETE FROM lyrics_results WHERE video_id LIKE 'lyrtest-%'"));
    }

    // How each source answers, set per case.
    enum class Exact { Synced, Slow, Missing, Hold, Drop, Error };
    enum class Search { Synced, OtherWords, Empty, Hold, WrongSong };
    enum class Tube { Text, None, Error, Slow };
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
                answer.body = tube == Tube::None ? kNextWithout : kNextWithLyrics;
            if (tube == Tube::Slow)
                answer.delayMs = 800;
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
    // Until the answer: past "loading", a kept answer being checked, and the
    // best in hand shown while a better provider is still out.
    const auto settled = [&probe]() {
        return probe.state() != QLatin1String("loading") && !probe.checking() && !probe.interim();
    };
    const auto lookup = [&](const QVariantMap &song, int timeoutMs = 15000) {
        standIn.paths.clear();
        clock.start();
        probe.lookup(song);
        waitUntil(settled, timeoutMs);
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
    tube = Tube::Slow;
    lookup(track(QStringLiteral("lyrtest-exact")));
    t.check(probe.state() == QLatin1String("synced") && probe.source() == QLatin1String("LRCLIB")
                && standIn.count("/api/get?") == 1 && standIn.count("/api/search") == 0,
            QStringLiteral("the exact lookup answers: synced, and no search asked"), shown() + QStringLiteral("; ") + requests());
    // Both were asked at once; LRCLIB's timed lines won, so YouTube Music's
    // lookup was called off before its second request.
    waitUntil([]() { return false; }, 1200);
    t.check(standIn.count("/youtubei/v1/browse") == 0,
            QStringLiteral("YouTube Music, asked at the same time, was called off when LRCLIB won: no browse"),
            requests() + QStringLiteral(", browse %1").arg(standIn.count("/youtubei/v1/browse")));
    tube = Tube::Text;
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

    // One deadline for LRCLIB, whatever its requests are doing; YouTube
    // Music's text on show long before it, at the race's patience window.
    exact = Exact::Hold;
    search = Search::Hold;
    tube = Tube::Text;
    standIn.paths.clear();
    clock.start();
    probe.lookup(track(QStringLiteral("lyrtest-deadline")));
    waitUntil([&probe]() { return probe.state() == QLatin1String("plain"); }, 5000);
    const qint64 shownAt = clock.elapsed();
    t.check(probe.state() == QLatin1String("plain") && probe.interim() && shownAt >= 1150 && shownAt < 2000,
            QStringLiteral("LRCLIB silent: YouTube Music's lyrics on show at the patience window (%1 ms), "
                           "LRCLIB still asked").arg(shownAt),
            shown());
    waitUntil(settled, 20000);
    ms = clock.elapsed();
    row = rowFor(QStringLiteral("lyrtest-deadline"));
    t.check(probe.state() == QLatin1String("plain") && row.provisional && ms >= 5900 && ms < 9000,
            QStringLiteral("... and LRCLIB given up after 6 s, YouTube Music's lyrics provisional (%1 ms)").arg(ms),
            shown() + QStringLiteral("; ") + requests());

    // The switch back: one provider after the other, YouTube Music only once
    // LRCLIB has come up empty, and nothing on show before.
    {
        LyricsRace::Options serial = probe.raceOptions();
        const LyricsRace::Options raced = serial;
        serial.serial = true;
        serial.patienceMs = -1;
        probe.setRaceOptions(serial);
        exact = Exact::Synced;
        tube = Tube::Text;
        lookup(track(QStringLiteral("lyrtest-serial")));
        t.check(probe.state() == QLatin1String("synced") && standIn.count("/youtubei/v1/next") == 0,
                QStringLiteral("lyrics.race=serial: LRCLIB's lines, and YouTube Music never asked"), requests());
        exact = Exact::Drop;
        lookup(track(QStringLiteral("lyrtest-serial2")));
        t.check(probe.state() == QLatin1String("plain") && standIn.count("/youtubei/v1/next") == 1,
                QStringLiteral("... and asked once LRCLIB failed"), shown() + QStringLiteral("; ") + requests());
        probe.setRaceOptions(raced);
    }

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
    cleanup.exec(QStringLiteral("DELETE FROM lyrics_results WHERE video_id LIKE 'lyrtest-%'"));
    InnerTube::setTestServer(QString());
    return t.finish();
}

// ------------------------------------------------------------- the race

namespace {

const QString kTimed = QStringLiteral("[00:01.00] Timed line\n[00:05.00] Second timed line");
const QString kPlainWords = QStringLiteral("Plain line\nSecond plain line");

LyricsOutcome foundTimed(const QString &source, double durationS = -1, const QString &lrc = kTimed)
{
    LyricsOutcome outcome;
    outcome.kind = LyricsOutcome::Found;
    outcome.answer.synced = lrc;
    outcome.answer.source = source;
    outcome.answer.durationS = durationS;
    return outcome;
}

LyricsOutcome foundPlain(const QString &source)
{
    LyricsOutcome outcome;
    outcome.kind = LyricsOutcome::Found;
    outcome.answer.plain = kPlainWords;
    outcome.answer.source = source;
    return outcome;
}

LyricsOutcome missedOutcome()
{
    return LyricsOutcome();
}

LyricsOutcome failedOutcome(const QString &why)
{
    LyricsOutcome outcome;
    outcome.kind = LyricsOutcome::Failed;
    outcome.error = why;
    return outcome;
}

// A provider that answers as scripted, after a delay, and counts how often
// it was started and called off.
class FakeProvider : public LyricsProvider
{
public:
    struct Script {
        int delayMs = 10;               // < 0: at once, from start() itself
        bool hold = false;              // never answers
        bool strayAfterCancel = false;  // answers anyway once called off
        LyricsOutcome outcome;
    };

    FakeProvider(const QString &id, Timing timing, bool lazy = false)
        : m_id(id), m_timing(timing), m_lazy(lazy)
    {
        reset();
    }

    QString id() const override { return m_id; }
    QString name() const override { return m_id; }
    Timing bestTiming() const override { return m_timing; }
    bool lazy() const override { return m_lazy; }
    bool background() const override { return true; }
    LyricsLookup *lookUp(const LyricsRequest &request, QObject *parent) override;

    void reset()
    {
        script = Script();
        scripts.clear();
        started = 0;
        cancelled = 0;
        startedAtMs = -1;
        dropStrays();
    }
    // Answers still to come from lookups already called off, dropped.
    void dropStrays() { context = std::make_unique<QObject>(); }

    Script script;                    // for every song
    QHash<QString, Script> scripts;   // by video id, over `script`
    int started = 0;
    int cancelled = 0;
    qint64 startedAtMs = -1;          // on `clock`, when last started
    QElapsedTimer *clock = nullptr;
    std::unique_ptr<QObject> context;

private:
    QString m_id;
    Timing m_timing;
    bool m_lazy;
};

class FakeLookup : public LyricsLookup
{
public:
    FakeLookup(FakeProvider *provider, const FakeProvider::Script &script, QObject *parent)
        : LyricsLookup(parent), m_provider(provider), m_script(script) {}

    void start() override
    {
        ++m_provider->started;
        if (m_provider->clock)
            m_provider->startedAtMs = m_provider->clock->elapsed();
        if (m_script.hold)
            return;
        if (m_script.delayMs < 0) {
            finish(m_script.outcome);
            return;
        }
        if (m_script.strayAfterCancel) {
            // Kept apart from the lookup, so it arrives after cancel() too.
            const auto done = onFinished;
            const LyricsOutcome outcome = m_script.outcome;
            QTimer::singleShot(m_script.delayMs, m_provider->context.get(), [done, outcome]() {
                if (done)
                    done(outcome);
            });
            return;
        }
        QTimer::singleShot(m_script.delayMs, this, [this]() { finish(m_script.outcome); });
    }

    void cancel() override
    {
        ++m_provider->cancelled;
        onFinished = nullptr;
    }

private:
    FakeProvider *m_provider;
    FakeProvider::Script m_script;
};

LyricsLookup *FakeProvider::lookUp(const LyricsRequest &request, QObject *parent)
{
    return new FakeLookup(this, scripts.value(request.videoId, script), parent);
}

FakeProvider::Script after(int delayMs, const LyricsOutcome &outcome)
{
    FakeProvider::Script script;
    script.delayMs = delayMs;
    script.outcome = outcome;
    return script;
}

FakeProvider::Script never()
{
    FakeProvider::Script script;
    script.hold = true;
    return script;
}

// One race to its end, and what it did.
struct RaceRun {
    LyricsRace::Verdict verdict = LyricsRace::Verdict::None;
    LyricsAnswer answer;
    QString error;
    qint64 finishedMs = -1;
    qint64 offeredMs = -1;
    QString offeredFrom;
    int finishedCalls = 0;
    bool decidedInStart = false;
};

QString verdictName(LyricsRace::Verdict verdict)
{
    switch (verdict) {
    case LyricsRace::Verdict::Found: return QStringLiteral("found");
    case LyricsRace::Verdict::None:  return QStringLiteral("none");
    case LyricsRace::Verdict::Error: return QStringLiteral("error");
    }
    return QString();
}

QString describe(const RaceRun &run)
{
    return QStringLiteral("%1 %2%3 at %4 ms; offered %5 at %6 ms; decided %7 time(s)%8")
        .arg(verdictName(run.verdict), run.answer.source,
             run.answer.provisional ? QStringLiteral(" (provisional)") : QString())
        .arg(run.finishedMs)
        .arg(run.offeredFrom.isEmpty() ? QStringLiteral("nothing") : run.offeredFrom)
        .arg(run.offeredMs)
        .arg(run.finishedCalls)
        .arg(run.error.isEmpty() ? QString() : QStringLiteral(" (") + run.error + QLatin1Char(')'));
}

} // namespace

int runLyricsRaceSelfTest()
{
    Checks t("lyrics-race-test");
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes lyrics rows"));
        return t.finish();
    }

    QElapsedTimer clock;
    FakeProvider a(QStringLiteral("A"), LyricsProvider::Timing::Line);
    FakeProvider b(QStringLiteral("B"), LyricsProvider::Timing::Line);
    FakeProvider p(QStringLiteral("P"), LyricsProvider::Timing::Plain);
    FakeProvider lazy(QStringLiteral("L"), LyricsProvider::Timing::Plain, /*lazy=*/true);
    const QList<FakeProvider *> fakes{ &a, &b, &p, &lazy };
    for (FakeProvider *fake : fakes)
        fake->clock = &clock;

    // Quick windows, so the whole test takes seconds.
    LyricsRace::Options fast;
    fast.patienceMs = 400;
    fast.deadlineMs = 2000;

    const auto run = [&](const QList<LyricsProvider *> &order, const LyricsRace::Options &options,
                         const QHash<QString, LyricsOutcome> &known = {}, int lingerMs = 0,
                         const std::function<void(LyricsRace *)> &afterStart = {}) {
        LyricsRequest request;
        request.videoId = QStringLiteral("race-song");
        request.query.title = QStringLiteral("Song");
        request.query.artists = QStringList{ QStringLiteral("Artist") };
        request.query.durationS = 200;
        LyricsRace race(request, order, options);
        RaceRun result;
        race.onOffer = [&]() {
            if (result.offeredMs < 0) {
                result.offeredMs = clock.elapsed();
                result.offeredFrom = race.offer().source;
            }
        };
        race.onFinished = [&]() {
            ++result.finishedCalls;
            result.finishedMs = clock.elapsed();
        };
        clock.start();
        race.start(known);
        result.decidedInStart = result.finishedCalls > 0;
        if (afterStart)
            afterStart(&race);
        waitUntil([&result]() { return result.finishedCalls > 0; }, 15000);
        // Late and stray answers, while the race is still there to ignore them.
        if (lingerMs > 0)
            waitUntil([]() { return false; }, lingerMs);
        for (FakeProvider *fake : fakes)
            fake->dropStrays();
        result.verdict = race.verdict();
        result.answer = race.answer();
        result.error = race.error();
        return result;
    };
    const auto resetAll = [&]() {
        for (FakeProvider *fake : fakes)
            fake->reset();
    };

    // — the order —
    resetAll();
    a.script = after(300, foundTimed(QStringLiteral("A")));
    b.script = after(50, foundTimed(QStringLiteral("B")));
    RaceRun r = run({ &a, &b }, fast);
    t.check(r.answer.source == QLatin1String("A") && r.finishedMs >= 280 && r.finishedMs < 500 && r.offeredMs < 0,
            QStringLiteral("answers are taken in order: A's at 300 ms wins over B's, which came at 50"), describe(r));

    resetAll();
    a.script = after(50, foundTimed(QStringLiteral("A")));
    b.script = never();
    r = run({ &a, &b }, fast);
    t.check(r.answer.source == QLatin1String("A") && r.finishedMs < 150 && b.cancelled == 1,
            QStringLiteral("the winner is taken at once (%1 ms after its 50 ms answer), and the loser called off")
                .arg(r.finishedMs - 50),
            describe(r) + QStringLiteral("; B called off %1 time(s)").arg(b.cancelled));

    // — patience —
    resetAll();
    a.script = never();
    p.script = after(50, foundPlain(QStringLiteral("P")));
    r = run({ &a, &p }, fast);
    t.check(r.offeredFrom == QLatin1String("P") && r.offeredMs >= 380 && r.offeredMs < 650,
            QStringLiteral("A silent: P's plain text put forward at the patience window (%1 ms)").arg(r.offeredMs),
            describe(r));
    t.check(r.verdict == LyricsRace::Verdict::Found && r.answer.source == QLatin1String("P") && r.answer.provisional
                && r.finishedMs >= 1950 && r.finishedMs < 2700 && a.cancelled == 1,
            QStringLiteral("... and A given up at its deadline: P's answer, provisional"), describe(r));

    resetAll();
    a.script = after(800, foundTimed(QStringLiteral("A")));
    p.script = after(50, foundPlain(QStringLiteral("P")));
    r = run({ &a, &p }, fast);
    t.check(r.offeredFrom == QLatin1String("P") && r.answer.source == QLatin1String("A") && !r.answer.provisional
                && r.finishedMs >= 780 && r.finishedMs < 1100,
            QStringLiteral("A's timed lines after the patience window replace P's plain text"), describe(r));

    // — timed and plain kept apart (L3) —
    resetAll();
    a.script = after(50, foundPlain(QStringLiteral("A")));
    b.script = after(150, foundTimed(QStringLiteral("B")));
    r = run({ &a, &b }, fast);
    t.check(r.answer.source == QLatin1String("B") && !r.answer.synced.isEmpty(),
            QStringLiteral("a plain answer from higher up waits for a timed one from below, and loses to it"), describe(r));

    resetAll();
    a.script = after(50, foundPlain(QStringLiteral("A")));
    p.script = never();
    r = run({ &a, &p }, fast);
    t.check(r.answer.source == QLatin1String("A") && r.finishedMs < 150 && p.cancelled == 1,
            QStringLiteral("a provider that only has plain text cannot beat plain text from above it: called off"),
            describe(r));

    // — a failure is not "none" (L14) —
    const auto verdictOf = [&](const LyricsOutcome &first, const LyricsOutcome &second) {
        resetAll();
        a.script = after(10, first);
        p.script = after(20, second);
        return run({ &a, &p }, fast);
    };
    r = verdictOf(failedOutcome(QStringLiteral("A down")), missedOutcome());
    t.check(r.verdict == LyricsRace::Verdict::Error, QStringLiteral("A fails, P has none: an error, not \"none\""), describe(r));
    r = verdictOf(missedOutcome(), missedOutcome());
    t.check(r.verdict == LyricsRace::Verdict::None, QStringLiteral("both have none: \"none\""), describe(r));
    r = verdictOf(failedOutcome(QStringLiteral("A down")), foundPlain(QStringLiteral("P")));
    t.check(r.verdict == LyricsRace::Verdict::Found && r.answer.provisional,
            QStringLiteral("A fails, P has plain text: P's, provisional"), describe(r));
    r = verdictOf(missedOutcome(), foundPlain(QStringLiteral("P")));
    t.check(r.verdict == LyricsRace::Verdict::Found && !r.answer.provisional,
            QStringLiteral("A has none, P has plain text: P's, for good"), describe(r));

    // — lazy providers —
    resetAll();
    a.script = after(50, missedOutcome());
    lazy.script = after(20, foundPlain(QStringLiteral("L")));
    r = run({ &a, &lazy }, fast);
    t.check(r.answer.source == QLatin1String("L") && lazy.started == 1 && lazy.startedAtMs >= 45,
            QStringLiteral("a lazy provider starts only once everything above it came up empty (at %1 ms)")
                .arg(lazy.startedAtMs),
            describe(r));
    resetAll();
    a.script = after(50, foundPlain(QStringLiteral("A")));
    r = run({ &a, &lazy }, fast);
    t.check(r.answer.source == QLatin1String("A") && lazy.started == 0,
            QStringLiteral("... and not at all when anything was found"), describe(r));

    // — the switch back: one after the other —
    LyricsRace::Options serial = fast;
    serial.serial = true;
    serial.patienceMs = -1;
    resetAll();
    a.script = after(100, foundTimed(QStringLiteral("A")));
    p.script = after(20, foundPlain(QStringLiteral("P")));
    r = run({ &a, &p }, serial);
    t.check(r.answer.source == QLatin1String("A") && p.started == 0,
            QStringLiteral("serial: A's lines, and P never asked"), describe(r));
    resetAll();
    a.script = after(100, missedOutcome());
    p.script = after(20, foundPlain(QStringLiteral("P")));
    r = run({ &a, &p }, serial);
    t.check(r.answer.source == QLatin1String("P") && p.started == 1 && p.startedAtMs >= 95,
            QStringLiteral("serial: P asked only once A had none (at %1 ms)").arg(p.startedAtMs), describe(r));

    // — answers kept from before —
    resetAll();
    r = run({ &a, &p }, fast, { { QStringLiteral("A"), foundTimed(QStringLiteral("A")) } });
    t.check(r.decidedInStart && a.started == 0 && p.started == 0 && r.answer.source == QLatin1String("A"),
            QStringLiteral("A's timed answer kept from before: decided at once, nobody asked"), describe(r));
    resetAll();
    a.script = after(100, foundTimed(QStringLiteral("A")));
    r = run({ &a, &p }, fast, { { QStringLiteral("P"), foundPlain(QStringLiteral("P")) } });
    t.check(r.answer.source == QLatin1String("A") && a.started == 1 && p.started == 0,
            QStringLiteral("P's answer kept from before: only A asked, and A's lines win"), describe(r));

    // — the shared gate —
    resetAll();
    a.script = after(20, foundTimed(QStringLiteral("A"), 210));
    p.script = after(50, foundPlain(QStringLiteral("P")));
    r = run({ &a, &p }, fast);
    t.check(r.answer.source == QLatin1String("P"),
            QStringLiteral("timed lines 10 s off the recording's length count as another edit's words: P's text wins"),
            describe(r));
    resetAll();
    a.script = after(20, foundTimed(QStringLiteral("A"), 240));
    p.script = after(30, missedOutcome());
    r = run({ &a, &p }, fast);
    t.check(r.verdict == LyricsRace::Verdict::None,
            QStringLiteral("an answer 40 s off is not this song"), describe(r));
    resetAll();
    a.script = after(20, foundTimed(QStringLiteral("A"), 202));
    r = run({ &a, &p }, fast);
    t.check(r.answer.source == QLatin1String("A") && !r.answer.synced.isEmpty(),
            QStringLiteral("2 s off: timed lines, as they came"), describe(r));

    // — stray, late and immediate answers —
    resetAll();
    a.script = after(50, foundTimed(QStringLiteral("A")));
    b.script = after(300, foundTimed(QStringLiteral("B")));
    b.script.strayAfterCancel = true;
    r = run({ &a, &b }, fast, {}, 500);
    t.check(r.answer.source == QLatin1String("A") && r.finishedCalls == 1 && b.cancelled == 1,
            QStringLiteral("a loser that answers after being called off changes nothing"), describe(r));
    resetAll();
    a.script = after(-1, foundTimed(QStringLiteral("A")));
    r = run({ &a, &b }, fast);
    t.check(r.decidedInStart && r.answer.source == QLatin1String("A") && r.finishedCalls == 1,
            QStringLiteral("an answer given the moment it was asked for is taken"), describe(r));
    resetAll();
    a.script = never();
    LyricsRace::Options shortDeadline = fast;
    shortDeadline.deadlineMs = 600;
    r = run({ &a }, shortDeadline);
    t.check(r.verdict == LyricsRace::Verdict::Error && r.error.contains(QLatin1String("did not answer"))
                && r.finishedMs >= 580 && r.finishedMs < 900,
            QStringLiteral("a provider that never answers fails at its deadline, on the wall clock"), describe(r));

    // — joining: the view's providers added to the queue's lookup —
    resetAll();
    a.script = after(150, foundTimed(QStringLiteral("A")));
    p.script = after(20, foundPlain(QStringLiteral("P")));
    r = run({ &p }, fast, {}, 0, [&](LyricsRace *race) { race->include({ &a, &p }); });
    t.check(r.answer.source == QLatin1String("A") && a.started == 1,
            QStringLiteral("a provider joining a lookup takes its place in the order"), describe(r));

    // — the view (Lyrics): what is shown of it —
    {
        QSqlQuery clean(AppDatabase::connection());
        clean.exec(QStringLiteral("DELETE FROM lyrics WHERE video_id LIKE 'lyrrace-%'"));
        clean.exec(QStringLiteral("DELETE FROM lyrics_results WHERE video_id LIKE 'lyrrace-%'"));
    }
    // Nothing leaves this computer: every InnerTube made from here on asks a
    // closed port, and none warms up.
    InnerTube::setTestServer(QStringLiteral("http://127.0.0.1:9"));
    {
        PlaybackController player(nullptr, nullptr, nullptr);
        player.setAutoplay(false);
        Lyrics view(&player);
        view.setProviders({ &a, &b, &p });
        LyricsRace::Options options = view.raceOptions();
        options.patienceMs = 300;
        options.deadlineMs = 2000;
        view.setRaceOptions(options);
        const auto song = [](const QString &id) {
            return QVariantMap{ { QStringLiteral("sourceId"), id },
                                { QStringLiteral("title"), QStringLiteral("Race Song") },
                                { QStringLiteral("artist"), QStringLiteral("Race Artist") },
                                { QStringLiteral("durationMs"), qint64(200000) } };
        };
        const auto firstLine = [&view]() {
            const QList<LyricsModel::Line> &lines = view.lines()->lines();
            return lines.isEmpty() ? QString() : lines.first().text;
        };
        const auto settled = [&view]() {
            return view.state() != QLatin1String("loading") && !view.checking() && !view.interim();
        };

        resetAll();
        a.scripts.insert(QStringLiteral("lyrrace-x"),
                         after(400, foundTimed(QStringLiteral("A"), -1, QStringLiteral("[00:01.00] X line"))));
        a.scripts.insert(QStringLiteral("lyrrace-y"),
                         after(100, foundTimed(QStringLiteral("A"), -1, QStringLiteral("[00:01.00] Y line"))));
        view.lookup(song(QStringLiteral("lyrrace-x")));
        view.lookup(song(QStringLiteral("lyrrace-y")));
        waitUntil([]() { return false; }, 700);
        t.check(firstLine() == QLatin1String("Y line") && a.cancelled >= 1 && !rowFor(QStringLiteral("lyrrace-x")).exists,
                QStringLiteral("an earlier song's lookup is called off, and its answer never shown"),
                QStringLiteral("on show: %1; A called off %2 time(s)").arg(firstLine()).arg(a.cancelled));

        // One of the same kind from higher in the order, after the patience
        // window: it waits while someone reads along.
        const auto sameKind = [&](const QString &id, qint64 position) {
            resetAll();
            a.scripts.insert(id, after(700, foundTimed(QStringLiteral("A"), -1, QStringLiteral("[00:01.00] A words"))));
            b.scripts.insert(id, after(50, foundTimed(QStringLiteral("B"), -1, QStringLiteral("[00:01.00] B words"))));
            view.setActive(false);
            player.playTracks({ song(id) }, 0);
            view.setActive(true);
            waitUntil([&view]() { return view.interim(); }, 2000);
            const bool interimB = view.interim() && view.source() == QLatin1String("B");
            player.setPosition(position);
            waitUntil(settled, 5000);
            return interimB;
        };
        bool interimB = sameKind(QStringLiteral("lyrrace-z1"), 5000);
        t.check(interimB && view.source() == QLatin1String("B") && view.currentLine() >= 0
                    && rowFor(QStringLiteral("lyrrace-z1")).source == QLatin1String("A"),
                QStringLiteral("A's timed lines after B's are on show and being sung: B's stay, A's are kept for next time"),
                QStringLiteral("on show from %1, line %2; kept: %3").arg(view.source()).arg(view.currentLine())
                    .arg(rowFor(QStringLiteral("lyrrace-z1")).source));
        interimB = sameKind(QStringLiteral("lyrrace-z2"), 0);
        t.check(interimB && view.source() == QLatin1String("A"),
                QStringLiteral("... but before the first line is sung, A's replace them"),
                QStringLiteral("on show from %1").arg(view.source()));

        resetAll();
        a.scripts.insert(QStringLiteral("lyrrace-z3"), after(700, foundTimed(QStringLiteral("A"))));
        p.scripts.insert(QStringLiteral("lyrrace-z3"), after(50, foundPlain(QStringLiteral("P"))));
        view.setActive(false);
        player.playTracks({ song(QStringLiteral("lyrrace-z3")) }, 0);
        view.setActive(true);
        waitUntil([&view]() { return view.interim(); }, 2000);
        const bool interimP = view.state() == QLatin1String("plain");
        waitUntil(settled, 5000);
        t.check(interimP && view.state() == QLatin1String("synced") && view.source() == QLatin1String("A"),
                QStringLiteral("timed lines replace plain text on show, read along or not"),
                QStringLiteral("%1 from %2").arg(view.state(), view.source()));
        view.setActive(false);

        // A provider that answered is not asked again while its answer is
        // fresh: only the one that failed.
        resetAll();
        a.script = after(10, failedOutcome(QStringLiteral("A down")));
        b.script = after(10, missedOutcome());
        p.script = after(20, foundPlain(QStringLiteral("P")));
        view.lookup(song(QStringLiteral("lyrrace-kept")));
        waitUntil(settled, 5000);
        const Row provisional = rowFor(QStringLiteral("lyrrace-kept"));
        view.lookup(song(QStringLiteral("lyrrace-kept")));
        waitUntil(settled, 5000);
        t.check(provisional.provisional && a.started == 2 && b.started == 1 && p.started == 1,
                QStringLiteral("looked up again after A failed: only A is asked, B's and P's answers are kept"),
                QStringLiteral("asked: A %1, B %2, P %3").arg(a.started).arg(b.started).arg(p.started));

        resetAll();
        a.script = after(10, failedOutcome(QStringLiteral("A down")));
        b.script = after(10, missedOutcome());
        p.script = after(10, missedOutcome());
        view.lookup(song(QStringLiteral("lyrrace-error")));
        waitUntil(settled, 5000);
        t.check(view.state() == QLatin1String("error") && !rowFor(QStringLiteral("lyrrace-error")).exists,
                QStringLiteral("A fails, the rest have none: an error on show, nothing kept"), view.state());
        resetAll();
        a.script = after(10, missedOutcome());
        b.script = after(10, missedOutcome());
        p.script = after(10, missedOutcome());
        view.lookup(song(QStringLiteral("lyrrace-none")));
        waitUntil(settled, 5000);
        t.check(view.state() == QLatin1String("none") && rowFor(QStringLiteral("lyrrace-none")).exists,
                QStringLiteral("every provider has none: \"none\", kept"), view.state());
    }
    InnerTube::setTestServer(QString());
    QSqlQuery cleanup(AppDatabase::connection());
    cleanup.exec(QStringLiteral("DELETE FROM lyrics WHERE video_id LIKE 'lyrrace-%'"));
    cleanup.exec(QStringLiteral("DELETE FROM lyrics_results WHERE video_id LIKE 'lyrrace-%'"));
    return t.finish();
}

// ---------------------------------------------------- the queue's lookups

int runLyricsPrefetchSelfTest()
{
    Checks t("lyrics-prefetch-test");
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes lyrics rows"));
        return t.finish();
    }
    const auto clean = []() {
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("DELETE FROM lyrics WHERE video_id LIKE 'lyrpre-%'"));
        q.exec(QStringLiteral("DELETE FROM lyrics_results WHERE video_id LIKE 'lyrpre-%'"));
    };
    clean();

    enum class Exact { Synced, Slow, Drop };
    Exact exact = Exact::Synced;
    bool tubeFails = false;
    StandIn standIn;
    if (!t.check(standIn.listen(), QStringLiteral("a stand-in server on this computer")))
        return t.finish();
    standIn.respond = [&](const QByteArray &path) {
        StandIn::Answer answer;
        if (path.startsWith("/api/get?")) {
            answer.body = kEntry;
            if (exact == Exact::Slow)
                answer.delayMs = 600;
            else if (exact == Exact::Drop)
                answer.drop = true;
        } else if (path.startsWith("/api/search")) {
            answer.body = "[]";
        } else if (path.startsWith("/youtubei/v1/next")) {
            if (tubeFails)
                answer.status = 500;
            else
                answer.body = kNextWithout;
        } else if (path.startsWith("/youtubei/v1/browse")) {
            answer.body = kLyricsPage;
        }
        return answer;
    };
    InnerTube::setTestServer(standIn.base());
    t.note(QStringLiteral("LRCLIB and YouTube Music are ") + standIn.base() + QStringLiteral("; nothing leaves this computer"));

    {
        // A player with no engine: songs become current and nothing plays,
        // so the lookups follow the queue without waiting for sound.
        PlaybackController player(nullptr, nullptr, nullptr);
        player.setAutoplay(false);
        Lyrics lyrics(&player);
        lyrics.setLrclibUrl(standIn.base());
        lyrics.setFollowWithoutSound(true);
        lyrics.setMeteredForTest(0);
        lyrics.followQueue();

        // Each song its own title, so its LRCLIB requests can be told apart.
        const auto song = [](const QString &id, qint64 durationMs = 200000) {
            return QVariant(QVariantMap{ { QStringLiteral("sourceId"), QStringLiteral("lyrpre-") + id },
                                         { QStringLiteral("title"), QStringLiteral("Prefetch ") + id },
                                         { QStringLiteral("artist"), QStringLiteral("Selftest Artist") },
                                         { QStringLiteral("album"), QStringLiteral("Selftest Album") },
                                         { QStringLiteral("durationMs"), durationMs },
                                         { QStringLiteral("credits"), oneName(QStringLiteral("Selftest Artist")) } });
        };
        const auto asked = [&standIn](const QString &id) {
            const QByteArray title = "track_name=Prefetch%20" + id.toUtf8() + '&';
            int n = 0;
            for (const QByteArray &path : std::as_const(standIn.paths))
                n += path.startsWith("/api/get?") && path.contains(title) ? 1 : 0;
            return n;
        };
        const auto firstAsked = [&standIn](const QString &id) {
            const QByteArray title = "track_name=Prefetch%20" + id.toUtf8() + '&';
            for (int i = 0; i < standIn.paths.size(); ++i) {
                if (standIn.paths.at(i).contains(title))
                    return i;
            }
            return -1;
        };
        const auto kept = [](const QString &id) { return rowFor(QStringLiteral("lyrpre-") + id); };

        // — two rows, the lyrics closed —
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("A")), song(QStringLiteral("B")) }, 0);
        waitUntil([&]() { return kept(QStringLiteral("A")).exists && kept(QStringLiteral("B")).exists; }, 10000);
        t.check(kept(QStringLiteral("A")).exists && kept(QStringLiteral("B")).exists
                    && !kept(QStringLiteral("A")).synced.isEmpty() && !kept(QStringLiteral("B")).synced.isEmpty(),
                QStringLiteral("two queue rows, the lyrics closed: both looked up and kept"));
        t.check(!lyrics.active() && lyrics.state() == QLatin1String("idle"),
                QStringLiteral("... and nothing put on show while nobody looks"), lyrics.state());
        t.check(asked(QStringLiteral("A")) == 1 && asked(QStringLiteral("B")) == 1
                    && firstAsked(QStringLiteral("A")) < firstAsked(QStringLiteral("B")),
                QStringLiteral("one LRCLIB lookup each, the song playing first"),
                QStringLiteral("A %1, B %2").arg(asked(QStringLiteral("A"))).arg(asked(QStringLiteral("B"))));

        // — the lyrics opened: a read of the database —
        standIn.paths.clear();
        QElapsedTimer clock;
        clock.start();
        lyrics.setActive(true);
        const qint64 openMs = clock.elapsed();
        t.check(lyrics.state() == QLatin1String("synced") && openMs <= 50 && standIn.paths.isEmpty(),
                QStringLiteral("opened: lines on show in %1 ms, nothing asked").arg(openMs), lyrics.state());
        clock.start();
        player.next();
        const qint64 nextMs = clock.elapsed();
        t.check(lyrics.state() == QLatin1String("synced") && nextMs <= 50 && asked(QStringLiteral("B")) == 0,
                QStringLiteral("Next, with the lyrics open: the next song's lines in %1 ms, nothing asked").arg(nextMs),
                lyrics.state());
        lyrics.setActive(false);

        // — never with a length of 0 (L8) —
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("C"), 0), song(QStringLiteral("D"), 0) }, 0);
        waitUntil([]() { return false; }, 1500);
        t.check(asked(QStringLiteral("C")) == 0 && asked(QStringLiteral("D")) == 0 && !kept(QStringLiteral("C")).exists,
                QStringLiteral("songs whose length is not known are not looked up"));

        // — one lookup per song: the view waits on the queue's —
        exact = Exact::Slow;
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("E")), song(QStringLiteral("F")) }, 0);
        const bool out = lyrics.lookingUp(QStringLiteral("lyrpre-E"));
        lyrics.setActive(true);
        waitUntil([&lyrics]() { return lyrics.state() != QLatin1String("loading") && !lyrics.interim(); }, 5000);
        t.check(out && lyrics.state() == QLatin1String("synced") && asked(QStringLiteral("E")) == 1,
                QStringLiteral("opened while the queue's lookup is out: that one answers, nothing asked twice"),
                QStringLiteral("%1; E asked %2 time(s)").arg(lyrics.state()).arg(asked(QStringLiteral("E"))));
        lyrics.setActive(false);
        waitUntil([&]() { return kept(QStringLiteral("F")).exists; }, 5000);
        exact = Exact::Synced;

        // — the setting off —
        lyrics.setBackground(false);
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("G")), song(QStringLiteral("H")) }, 0);
        waitUntil([]() { return false; }, 1000);
        t.check(standIn.paths.isEmpty() && !kept(QStringLiteral("G")).exists,
                QStringLiteral("\"Look up lyrics in the background\" off: nothing asked"),
                QStringLiteral("%1 request(s)").arg(standIn.paths.size()));
        lyrics.setBackground(true);
        waitUntil([&]() { return kept(QStringLiteral("G")).exists && kept(QStringLiteral("H")).exists; }, 5000);
        t.check(kept(QStringLiteral("G")).exists, QStringLiteral("... and turned on again: the song playing is looked up"));

        // — metered —
        lyrics.setMeteredForTest(1);
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("I")), song(QStringLiteral("J")) }, 0);
        waitUntil([&]() { return kept(QStringLiteral("I")).exists; }, 5000);
        waitUntil([]() { return false; }, 800);
        t.check(kept(QStringLiteral("I")).exists && !kept(QStringLiteral("J")).exists && asked(QStringLiteral("J")) == 0,
                QStringLiteral("a metered connection: the song playing, not the next"));
        lyrics.setMeteredForTest(0);

        // — a failure is not asked again at every change of the queue —
        // (A dropped connection is sent once more by Qt itself, so requests
        // are counted only once both lookups are over.)
        exact = Exact::Drop;
        tubeFails = true;
        standIn.paths.clear();
        player.playTracks({ song(QStringLiteral("K")), song(QStringLiteral("M")) }, 0);
        // K's lookup, then M's once K's is over.
        waitUntil([&]() { return asked(QStringLiteral("M")) >= 1; }, 8000);
        waitUntil([&lyrics]() { return !lyrics.lookingUp(QStringLiteral("lyrpre-M")); }, 8000);
        waitUntil([]() { return false; }, 300);
        standIn.paths.clear();
        for (int i = 0; i < 3; ++i)
            player.cycleRepeat();   // off, all, one, off: the queue's next song asked about each time
        player.addToQueue(song(QStringLiteral("N")).toMap());
        waitUntil([]() { return false; }, 1500);
        t.check(asked(QStringLiteral("K")) == 0 && asked(QStringLiteral("M")) == 0 && !kept(QStringLiteral("K")).exists,
                QStringLiteral("LRCLIB and YouTube Music down: nothing kept, and not asked again at every change of the queue"),
                QStringLiteral("%1 request(s) since").arg(standIn.paths.size()));
    }
    InnerTube::setTestServer(QString());
    clean();
    return t.finish();
}

// ----------------------------------------------------- opening the lyrics

void startLyricsPaneTest(MediaExtractor *extractor, PlaybackController *player, Lyrics *lyrics,
                         const QString &query, int dwellMs)
{
    struct Pane {
        int step = 0;   // 0 searching, 1 first song queued, 2 its sound started, 3 Next, 4 its sound started
        QString label;
        QString title;
        QElapsedTimer clock;
        qint64 first = -1;
        bool timing = false;
        bool out = false;
    };
    auto pane = std::make_shared<Pane>();

    const auto open = [lyrics, player, pane](const QString &label) {
        pane->label = label;
        pane->title = player->currentTrack().value(QStringLiteral("title")).toString();
        pane->out = lyrics->lookingUp(player->currentSourceId());
        pane->first = -1;
        pane->timing = true;
        pane->clock.start();
        lyrics->setActive(true);
    };
    QObject::connect(lyrics, &Lyrics::stateChanged, lyrics, [lyrics, player, pane, dwellMs]() {
        if (!pane->timing || lyrics->state() == QLatin1String("loading"))
            return;
        if (pane->first < 0)
            pane->first = pane->clock.elapsed();
        if (lyrics->checking() || lyrics->interim())
            return;
        pane->timing = false;
        qWarning("selftest: pane-open step=%s dwell=%d first=%lld final=%lld state=%s lines=%lld out=%s source=%s "
                 "title=\"%s\"",
                 qPrintable(pane->label), pane->label == QLatin1String("current") ? dwellMs : 1000,
                 (long long)pane->first, (long long)pane->clock.elapsed(), qPrintable(lyrics->state()),
                 (long long)lyrics->lines()->lines().size(), pane->out ? "yes" : "no",
                 qPrintable(lyrics->source().isEmpty() ? QStringLiteral("-") : lyrics->source()),
                 qPrintable(pane->title));
        lyrics->setActive(false);
        if (pane->label == QLatin1String("current")) {
            pane->step = 3;
            QTimer::singleShot(0, player, [player]() { player->next(); });
        } else {
            qWarning("selftest: done");
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
    });
    QObject::connect(player, &PlaybackController::listenStarted, lyrics, [open, pane, dwellMs](const QVariantMap &track) {
        if (pane->step != 1 && pane->step != 3)
            return;
        qWarning("selftest: the sound of \"%s\" started", qPrintable(track.value(QStringLiteral("title")).toString()));
        const bool first = pane->step == 1;
        pane->step += 1;
        QTimer::singleShot(first ? dwellMs : 1000, qApp, [open, first]() {
            open(first ? QStringLiteral("current") : QStringLiteral("next"));
        });
    });
    QObject::connect(extractor, &MediaExtractor::searchFinished, lyrics, [player, pane](const QVariantList &results) {
        if (pane->step != 0)
            return;
        QVariantList songs;
        for (const QVariant &result : results) {
            if (!result.toMap().value(QStringLiteral("sourceId")).toString().isEmpty())
                songs << result;
            if (songs.size() == 2)
                break;
        }
        if (songs.size() < 2) {
            qWarning("selftest: the search found fewer than two songs");
            QCoreApplication::exit(2);
            return;
        }
        pane->step = 1;
        qWarning("selftest: playing \"%s\", then \"%s\"",
                 qPrintable(songs.at(0).toMap().value(QStringLiteral("title")).toString()),
                 qPrintable(songs.at(1).toMap().value(QStringLiteral("title")).toString()));
        player->playTracks(songs, 0, QStringLiteral("search"));
    });
    QTimer::singleShot(300, extractor, [extractor, query]() { extractor->search(query); });
    QTimer::singleShot(90000, qApp, []() {
        qWarning("selftest: timed out");
        QCoreApplication::exit(2);
    });
}
