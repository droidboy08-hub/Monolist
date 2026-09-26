#include "scrobbleselftest.h"

#include "appdatabase.h"
#include "innertube.h"
#include "lastfm.h"
#include "library.h"
#include "listentracker.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "scrobbler.h"
#include "secretstore.h"
#include "streamresolver.h"
#include "ytdlp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <tuple>

namespace {

// One line per check, and a count at the end, as in connectionselftest.cpp.
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

using Params = LastFmApi::Params;

// Invented, and shaped like the real thing.
const QByteArray kKey = QByteArrayLiteral("TESTAPIKEY0123456789abcdef012345");
const QByteArray kSecret = QByteArrayLiteral("TESTSHAREDSECRET0123456789abcdef");
const QByteArray kSession = QByteArrayLiteral("TESTSESSIONKEY0000");
const QString kUser = QStringLiteral("monolist-test");
const QString kToken = QStringLiteral("TESTTOKEN6789");

bool scratchDataDir(Checks &t)
{
    if (!qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR"))
        return true;
    t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
            QStringLiteral("refusing to run: this test empties the scrobble queue and the Last.fm settings"));
    return false;
}

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

void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QString param(const Params &params, const QString &name)
{
    for (const auto &pair : params) {
        if (pair.first == name)
            return pair.second;
    }
    return QString();
}

bool hasParam(const Params &params, const QString &name)
{
    for (const auto &pair : params) {
        if (pair.first == name)
            return true;
    }
    return false;
}

int itemCount(const Params &params)
{
    int count = 0;
    for (const auto &pair : params)
        count += pair.first.startsWith(QLatin1String("artist["));
    return count;
}

// The server's side of a signed request: sign what came without api_sig
// with the same secret, and compare.
bool signedWith(const Params &received, const QByteArray &secret)
{
    QString sent;
    Params rest;
    for (const auto &pair : received) {
        if (pair.first == QLatin1String("api_sig"))
            sent = pair.second;
        else
            rest.append(pair);
    }
    return !sent.isEmpty() && LastFmApi::signature(rest, secret) == sent.toLatin1();
}

// track.scrobble's answer with these ignoredMessage codes, one item as an
// object and several as an array, as Last.fm writes them.
QByteArray scrobbleAnswer(const QList<int> &codes)
{
    QJsonArray items;
    int accepted = 0;
    for (int code : codes) {
        accepted += code == 0;
        items.append(QJsonObject{ { QStringLiteral("ignoredMessage"),
                                    QJsonObject{ { QStringLiteral("code"), QString::number(code) },
                                                 { QStringLiteral("#text"), QString() } } } });
    }
    const QJsonObject scrobbles{
        { QStringLiteral("scrobble"), codes.size() == 1 ? QJsonValue(items.first()) : QJsonValue(items) },
        { QStringLiteral("@attr"), QJsonObject{ { QStringLiteral("accepted"), accepted },
                                                { QStringLiteral("ignored"), int(codes.size()) - accepted } } } };
    return QJsonDocument(QJsonObject{ { QStringLiteral("scrobbles"), scrobbles } }).toJson(QJsonDocument::Compact);
}

QByteArray acceptAll(int count)
{
    return scrobbleAnswer(QList<int>(count, 0));
}

QByteArray errorAnswer(int code)
{
    return QJsonDocument(QJsonObject{ { QStringLiteral("error"), code },
                                      { QStringLiteral("message"), QStringLiteral("selftest error %1").arg(code) } })
        .toJson(QJsonDocument::Compact);
}

// Stands in for Last.fm: records every request, answers with `answer`.
struct Server {
    QList<Params> requests;
    std::function<QPair<int, QByteArray>(const Params &sent)> answer;

    QList<Params> of(const QString &method) const
    {
        QList<Params> found;
        for (const Params &request : requests) {
            if (param(request, QStringLiteral("method")) == method)
                found << request;
        }
        return found;
    }
    int count(const QString &method) const { return int(of(method).size()); }
};

void attach(LastFmApi &api, Server &server)
{
    api.setTestAccount(kKey, kSecret);
    api.setTestResponder([&server](const Params &sent) {
        server.requests << sent;
        return server.answer ? server.answer(sent) : qMakePair(0, QByteArray());
    });
}

int queued(const QString &account = QString())
{
    QSqlQuery q(AppDatabase::connection());
    if (account.isEmpty()) {
        q.exec(QStringLiteral("SELECT COUNT(*) FROM scrobble_queue"));
    } else {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM scrobble_queue WHERE account = ?"));
        q.addBindValue(account);
        q.exec();
    }
    return q.next() ? q.value(0).toInt() : -1;
}

void clearQueue()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("DELETE FROM scrobble_queue"));
}

// `count` rows for `account`, oldest first, three minutes apart; every
// seventh chosen by autoplay. Returns their ids.
QList<qint64> seed(const QString &account, int count, qint64 firstStartedAt = 0)
{
    static const QStringList artists = { QStringLiteral("Simon & Garfunkel"), QStringLiteral("Sigur Rós"),
                                         QStringLiteral("AC/DC"), QStringLiteral("Tyler, The Creator") };
    if (firstStartedAt == 0)
        firstStartedAt = QDateTime::currentSecsSinceEpoch() - qint64(count) * 180 - 600;
    QList<qint64> ids;
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO scrobble_queue (account, artist, track, album, duration_s, started_at, chosen_by_user,"
        " video_id, queued_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (int i = 0; i < count; ++i) {
        q.addBindValue(account);
        q.addBindValue(artists.at(i % artists.size()));
        q.addBindValue(i % 5 == 0 ? QStringLiteral("1 + 1 = 2 (%1)").arg(i) : QStringLiteral("Selftest song %1").arg(i));
        q.addBindValue(i % 3 == 0 ? QStringLiteral("Takk…") : QString(QLatin1String("")));
        q.addBindValue(200 + i);
        q.addBindValue(firstStartedAt + qint64(i) * 180);
        q.addBindValue(i % 7 == 6 ? 0 : 1);
        q.addBindValue(QStringLiteral("selftest%1").arg(i));
        q.addBindValue(QDateTime::currentSecsSinceEpoch());
        if (q.exec())
            ids << q.lastInsertId().toLongLong();
    }
    db.commit();
    return ids;
}

qint64 firstStartedAt(const QString &account)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT MIN(started_at) FROM scrobble_queue WHERE account = ?"));
    q.addBindValue(account);
    return q.exec() && q.next() ? q.value(0).toLongLong() : -1;
}

std::unique_ptr<Scrobbler> connectedScrobbler(LastFmApi &api, Library *library, const Scrobbler::Timing &timing)
{
    auto scrobbler = std::make_unique<Scrobbler>(&api, library);
    scrobbler->setTiming(timing);
    scrobbler->setUrlOpener([](const QUrl &) { return true; });
    scrobbler->useTestSession(kUser, kSession);
    return scrobbler;
}

QVariantMap sigurRos()
{
    return {
        { QStringLiteral("title"), QStringLiteral("Hoppípolla") },
        { QStringLiteral("artist"), QStringLiteral("Sigur Rós - Topic") },
        { QStringLiteral("album"), QStringLiteral("Takk…") },
        { QStringLiteral("durationMs"), qint64(268000) },
        { QStringLiteral("sourceId"), QStringLiteral("selftest01") },
    };
}

// — the engine, played by the test —
//
// PlaybackController is driven by the real MpvEngine object, but the test
// sends what mpv would: its clock every quarter second, its pause and
// buffering flags, the length, the end of a file. mpv's own answers never
// arrive, because this test never returns to the event loop: whatever the
// controller asks of the real engine (loading a file that does not exist,
// pausing) is queued and dropped at exit.
struct FakeEngine {
    MpvEngine &engine;
    qint64 position = 0;
    qint64 wall = 0;   // the clock ListenTracker reads, ms since the epoch

    void playing(bool on) { Q_EMIT engine.pausedChanged(!on); }
    void buffering(bool on) { Q_EMIT engine.bufferingChanged(on); }
    void duration(qint64 ms) { Q_EMIT engine.durationChanged(ms); }
    void newFile(qint64 at = 0) { position = at; }
    void play(qint64 ms)
    {
        while (ms > 0) {
            const qint64 step = std::min<qint64>(250, ms);
            position += step;
            wall += step;
            ms -= step;
            Q_EMIT engine.positionChanged(position);
        }
    }
    // Time passing with the playhead still. mpv reports nothing then; the
    // same position is sent once anyway, and must count for nothing.
    void idle(qint64 ms)
    {
        wall += ms;
        Q_EMIT engine.positionChanged(position);
    }
    // A jump the engine reports without being asked: never heard.
    void jump(qint64 to)
    {
        position = to;
        Q_EMIT engine.positionChanged(position);
    }
    void end() { Q_EMIT engine.endOfFile(); }
};

struct Heard {
    QString kind;   // started | qualified | resumed
    QString title;
    qint64 startedAt = 0;
    bool chosen = true;
    qint64 position = 0;   // the engine's clock when it was said
    QVariantMap track;
};

QVariantMap song(const QString &title, qint64 durationMs, const QVariantMap &extra = QVariantMap())
{
    QVariantMap map{
        { QStringLiteral("title"), title },
        { QStringLiteral("artist"), QStringLiteral("Selftest Artist - Topic") },
        { QStringLiteral("durationMs"), durationMs },
        // Not a file that exists: the controller hands it to mpv, whose
        // failure to open it is never delivered.
        { QStringLiteral("sourceUrl"), QStringLiteral("selftest-missing/%1.m4a").arg(title) },
    };
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        map.insert(it.key(), it.value());
    return map;
}

// — canned InnerTube answers, for the primary artist —

QJsonObject obj(const char *key, const QJsonValue &value)
{
    return { { QString::fromLatin1(key), value } };
}

QJsonObject runs(const QJsonArray &list)
{
    return obj("runs", list);
}

QJsonObject run(const QString &text, const char *pageType = nullptr)
{
    QJsonObject object = obj("text", text);
    if (pageType) {
        QJsonObject browse = obj("browseEndpointContextSupportedConfigs",
                                 obj("browseEndpointContextMusicConfig", obj("pageType", QString::fromLatin1(pageType))));
        browse.insert(QStringLiteral("browseId"), QStringLiteral("UCselftest"));
        object.insert(QStringLiteral("navigationEndpoint"), obj("browseEndpoint", browse));
    }
    return object;
}

QJsonObject artistRun(const QString &name) { return run(name, "MUSIC_PAGE_TYPE_ARTIST"); }

QJsonObject flexColumn(const QJsonArray &list)
{
    return obj("musicResponsiveListItemFlexColumnRenderer", obj("text", runs(list)));
}

QJsonObject listItem(const QString &videoId, const QString &title, const QJsonArray &subtitle)
{
    QJsonArray columns{ flexColumn({ run(title) }) };
    if (!subtitle.isEmpty())
        columns.append(flexColumn(subtitle));
    QJsonObject item = obj("playlistItemData", obj("videoId", videoId));
    item.insert(QStringLiteral("flexColumns"), columns);
    return obj("musicResponsiveListItemRenderer", item);
}

void checkPrimaryArtist(Checks &t)
{
    const QJsonObject bullet = run(QStringLiteral(" • "));
    const QJsonArray rows{
        listItem(QStringLiteral("selftestA1"), QStringLiteral("Die With A Smile"),
                 { artistRun(QStringLiteral("Lady Gaga")), run(QStringLiteral(" & ")),
                   artistRun(QStringLiteral("Bruno Mars")), bullet,
                   run(QStringLiteral("Die With A Smile"), "MUSIC_PAGE_TYPE_ALBUM"), bullet, run(QStringLiteral("4:12")) }),
        listItem(QStringLiteral("selftestA2"), QStringLiteral("EARFQUAKE"),
                 { artistRun(QStringLiteral("Tyler, The Creator")), bullet,
                   run(QStringLiteral("IGOR"), "MUSIC_PAGE_TYPE_ALBUM"), bullet, run(QStringLiteral("3:10")) }),
        listItem(QStringLiteral("selftestA3"), QStringLiteral("Unlinked"),
                 { run(QStringLiteral("Song")), bullet, run(QStringLiteral("Some Band")), bullet, run(QStringLiteral("2:01")) }),
    };
    QJsonObject carousel = obj("header", obj("musicCarouselShelfBasicHeaderRenderer",
                                             obj("title", runs({ run(QStringLiteral("Quick picks")) }))));
    carousel.insert(QStringLiteral("contents"), rows);
    const QJsonObject homeTab = obj("tabRenderer", obj("content", obj("sectionListRenderer",
        obj("contents", QJsonArray{ obj("musicCarouselShelfRenderer", carousel) }))));
    const QJsonObject home = obj("contents", obj("singleColumnBrowseResultsRenderer", obj("tabs", QJsonArray{ homeTab })));
    const QList<InnerTube::Shelf> shelves = InnerTube::parseShelves(home);
    const QList<InnerTube::Track> songs = shelves.isEmpty() ? QList<InnerTube::Track>() : shelves.first().songs;
    const auto describe = [&songs](int i) {
        return i < songs.size() ? QStringLiteral("artist \"%1\", primary \"%2\"").arg(songs.at(i).artist, songs.at(i).primaryArtist)
                                : QStringLiteral("no such row");
    };
    t.check(songs.size() == 3 && songs.at(0).artist == QLatin1String("Lady Gaga & Bruno Mars")
                && songs.at(0).primaryArtist == QLatin1String("Lady Gaga"),
            QStringLiteral("two linked credits: the first is the primary artist"), describe(0));
    t.check(songs.size() == 3 && songs.at(1).primaryArtist == QLatin1String("Tyler, The Creator"),
            QStringLiteral("a name with a comma in it stays whole"), describe(1));
    t.check(songs.size() == 3 && songs.at(2).primaryArtist == QLatin1String("Some Band"),
            QStringLiteral("an artist with no page is credited as written"), describe(2));

    // An album page: the rows leave the artist to the header.
    QJsonObject header = obj("title", runs({ run(QStringLiteral("Selftest Album")) }));
    header.insert(QStringLiteral("straplineTextOne"),
                  runs({ artistRun(QStringLiteral("Bruno Mars")), run(QStringLiteral(" & ")),
                         artistRun(QStringLiteral("Lady Gaga")) }));
    const QJsonObject albumTab = obj("tabRenderer", obj("content", obj("sectionListRenderer",
        obj("contents", QJsonArray{ obj("musicResponsiveHeaderRenderer", header) }))));
    const QJsonObject albumRows = obj("sectionListRenderer", obj("contents", QJsonArray{
        obj("musicShelfRenderer", obj("contents", QJsonArray{
            listItem(QStringLiteral("selftestB1"), QStringLiteral("Track one"), QJsonArray()) })) }));
    QJsonObject twoColumns = obj("tabs", QJsonArray{ albumTab });
    twoColumns.insert(QStringLiteral("secondaryContents"), albumRows);
    const QJsonObject album = obj("contents", obj("twoColumnBrowseResultsRenderer", twoColumns));
    const InnerTube::Collection collection = InnerTube::parseCollection(QStringLiteral("MPREselftest"), album);
    const InnerTube::Track first = collection.tracks.value(0);
    t.check(first.artist == QLatin1String("Bruno Mars & Lady Gaga") && first.primaryArtist == QLatin1String("Bruno Mars"),
            QStringLiteral("an album row takes the header's first credit"),
            QStringLiteral("artist \"%1\", primary \"%2\"").arg(first.artist, first.primaryArtist));

    // Home's cards: the credit, not the line under the card.
    {
        const auto card = [](const QString &videoId, const QString &title, const char *videoType,
                             const QJsonArray &subtitle) {
            QJsonObject watch = obj("videoId", videoId);
            watch.insert(QStringLiteral("watchEndpointMusicSupportedConfigs"),
                         obj("watchEndpointMusicConfig", obj("musicVideoType", QString::fromLatin1(videoType))));
            QJsonObject item = obj("title", runs({ run(title) }));
            item.insert(QStringLiteral("subtitle"), runs(subtitle));
            item.insert(QStringLiteral("navigationEndpoint"), obj("watchEndpoint", watch));
            return obj("musicTwoRowItemRenderer", item);
        };
        const QJsonArray cards{
            card(QStringLiteral("selftestC1"), QStringLiteral("Never Gonna Give You Up"), "MUSIC_VIDEO_TYPE_OMV",
                 { run(QStringLiteral("Rick Astley"), "MUSIC_PAGE_TYPE_USER_CHANNEL"), bullet,
                   run(QStringLiteral("1.6B views")) }),
            card(QStringLiteral("selftestC2"), QStringLiteral("Die With A Smile"), "MUSIC_VIDEO_TYPE_ATV",
                 { run(QStringLiteral("Song")), bullet, artistRun(QStringLiteral("Lady Gaga")),
                   run(QStringLiteral(" & ")), artistRun(QStringLiteral("Bruno Mars")) }),
        };
        QJsonObject shelf = obj("header", obj("musicCarouselShelfBasicHeaderRenderer",
                                              obj("title", runs({ run(QStringLiteral("Music videos for you")) }))));
        shelf.insert(QStringLiteral("contents"), cards);
        const QJsonObject tab = obj("tabRenderer", obj("content", obj("sectionListRenderer",
            obj("contents", QJsonArray{ obj("musicCarouselShelfRenderer", shelf) }))));
        const QList<InnerTube::Shelf> parsed = InnerTube::parseShelves(
            obj("contents", obj("singleColumnBrowseResultsRenderer", obj("tabs", QJsonArray{ tab }))));
        const QList<InnerTube::Card> found = parsed.isEmpty() ? QList<InnerTube::Card>() : parsed.first().cards;
        const auto show = [&found](int i) {
            return i < found.size() ? QStringLiteral("subtitle \"%1\", artist \"%2\", primary \"%3\"")
                                          .arg(found.at(i).subtitle, found.at(i).artist, found.at(i).primaryArtist)
                                    : QStringLiteral("no such card");
        };
        t.check(found.size() == 2 && found.at(0).artist == QLatin1String("Rick Astley")
                    && found.at(0).primaryArtist == QLatin1String("Rick Astley"),
                QStringLiteral("a video card: the channel, not \"Rick Astley * 1.6B views\""), show(0));
        t.check(found.size() == 2 && found.at(1).artist == QLatin1String("Lady Gaga & Bruno Mars")
                    && found.at(1).primaryArtist == QLatin1String("Lady Gaga"),
                QStringLiteral("a song card: \"Song\" left out, the first credit primary"), show(1));
    }

    // What Last.fm is sent.
    struct Case { QVariantMap track; const char *expected; };
    const Case cases[] = {
        { { { QStringLiteral("artist"), QStringLiteral("Rick Astley • 1.6B views") } }, "" },
        { { { QStringLiteral("artist"), QStringLiteral("Song • Some Band") } }, "" },
        { { { QStringLiteral("artist"), QStringLiteral("Daft Punk - Topic") } }, "Daft Punk" },
        { { { QStringLiteral("artist"), QStringLiteral("Bruno Mars, Lady Gaga") },
            { QStringLiteral("primaryArtist"), QStringLiteral("Bruno Mars") } }, "Bruno Mars" },
        { { { QStringLiteral("artist"), QStringLiteral("Tyler, The Creator") } }, "Tyler, The Creator" },
        { { { QStringLiteral("artist"), QStringLiteral("Topic") } }, "Topic" },
        { { { QStringLiteral("artist"), QStringLiteral("  Sigur Rós - Topic ") } }, "Sigur Rós" },
    };
    int wrong = 0;
    for (const Case &c : cases) {
        const QString got = Scrobbler::scrobbleArtist(c.track);
        if (got != QString::fromUtf8(c.expected)) {
            ++wrong;
            t.note(QStringLiteral("  scrobbleArtist gave \"%1\", expected \"%2\"").arg(got, QString::fromUtf8(c.expected)));
        }
    }
    t.check(wrong == 0, QStringLiteral("the artist sent: the primary credit, \" - Topic\" stripped, "
                                       "never a line joined with bullets (%1 cases)")
                            .arg(int(std::size(cases))));
}

}

// ============================================================ --listen-test

int runListenSelfTest()
{
    Checks t("listen");
    // The player it drives records its plays, like any other.
    if (!scratchDataDir(t))
        return t.finish();

    // The rule itself.
    t.check(ListenTracker::thresholdMs(25000) < 0 && ListenTracker::thresholdMs(30000) < 0
                && ListenTracker::thresholdMs(0) < 0,
            QStringLiteral("25 s, 30 s and an unknown length never count"));
    t.check(ListenTracker::thresholdMs(31000) == 15500 && ListenTracker::thresholdMs(180000) == 90000
                && ListenTracker::thresholdMs(480000) == 240000 && ListenTracker::thresholdMs(600000) == 240000,
            QStringLiteral("threshold: half, or four minutes, whichever comes first"));

    checkPrimaryArtist(t);

    MpvEngine engine;
    if (!engine.isValid()) {
        t.check(false, QStringLiteral("an audio engine to drive"), engine.lastError());
        return t.finish();
    }
    StreamResolver resolver;
    PlaybackController player(&engine, &resolver, nullptr);
    // Only the songs each scenario plays: no radio asked for.
    player.setAutoplay(false);

    FakeEngine fake{ engine };
    fake.wall = qint64(1790000000) * 1000;
    ListenTracker::setClock([&fake]() { return fake.wall; });

    QList<Heard> heard;
    const auto record = [&heard, &fake](const char *kind) {
        return [&heard, &fake, kind](const QVariantMap &track, qint64 startedAt, bool chosen) {
            heard.append({ QString::fromLatin1(kind), track.value(QStringLiteral("title")).toString(),
                           startedAt, chosen, fake.position, track });
        };
    };
    QObject::connect(&player, &PlaybackController::listenStarted, &player, record("started"));
    QObject::connect(&player, &PlaybackController::listenQualified, &player, record("qualified"));
    QObject::connect(&player, &PlaybackController::listenResumed, &player, record("resumed"));

    const auto count = [&heard](const char *kind, const QString &title) {
        int n = 0;
        for (const Heard &h : heard)
            n += h.kind == QLatin1String(kind) && h.title == title;
        return n;
    };
    const auto lastOf = [&heard](const char *kind, const QString &title) {
        for (auto it = heard.crbegin(); it != heard.crend(); ++it) {
            if (it->kind == QLatin1String(kind) && it->title == title)
                return *it;
        }
        return Heard();
    };
    const auto tally = [&count](const QString &title) {
        return QStringLiteral("%1 started, %2 qualified, %3 resumed")
            .arg(count("started", title)).arg(count("qualified", title)).arg(count("resumed", title));
    };
    // Plays a list from its first song, as a click in a list does.
    const auto start = [&player, &fake](const QVariantList &songs) {
        player.playTracks(songs, 0, QStringLiteral("selftest"));
        fake.newFile();
        fake.playing(true);
    };

    // 1 — too short to count, however much of it is heard.
    for (const auto &[title, length] : { qMakePair(QStringLiteral("Short 25"), qint64(25000)),
                                         qMakePair(QStringLiteral("Exactly 30"), qint64(30000)) }) {
        start({ song(title, length) });
        fake.duration(length);
        fake.play(length);
        fake.end();
        t.check(count("started", title) == 1 && count("qualified", title) == 0,
                QStringLiteral("%1 s heard whole: a listen, never a scrobble").arg(length / 1000), tally(title));
    }
    {
        const QString title = QStringLiteral("Just over 30");
        start({ song(title, 31000) });
        fake.duration(31000);
        fake.play(31000);
        t.check(count("qualified", title) == 1 && lastOf("qualified", title).position == 15500,
                QStringLiteral("31 s: counts at 15.5 s heard"),
                QStringLiteral("%1, at %2 ms").arg(tally(title)).arg(lastOf("qualified", title).position));
    }

    // 2 — three minutes: at half.
    {
        const QString title = QStringLiteral("Three minutes");
        const qint64 began = fake.wall;
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(89750);
        const bool notYet = count("qualified", title) == 0;
        fake.play(250);
        const Heard q = lastOf("qualified", title);
        t.check(notYet && count("qualified", title) == 1 && q.position == 90000,
                QStringLiteral("3:00 counts at 1:30 heard, not a moment before"),
                QStringLiteral("%1, at %2 ms").arg(tally(title)).arg(q.position));
        t.check(q.startedAt == began / 1000 && lastOf("started", title).startedAt == began / 1000 && q.chosen,
                QStringLiteral("started_at is when it began to be heard; chosen by the listener"),
                QStringLiteral("started %1, expected %2").arg(q.startedAt).arg(began / 1000));
        t.check(q.track.value(QStringLiteral("durationMs")).toLongLong() == 180000,
                QStringLiteral("the listen carries the track and its length"));
        fake.play(90000);
        fake.end();
        t.check(count("qualified", title) == 1, QStringLiteral("heard to the end, it still counts once"), tally(title));
    }

    // 3 — ten minutes: at four.
    {
        const QString title = QStringLiteral("Ten minutes");
        start({ song(title, 600000) });
        fake.duration(600000);
        fake.play(300000);
        t.check(count("qualified", title) == 1 && lastOf("qualified", title).position == 240000,
                QStringLiteral("10:00 counts at 4:00 heard"),
                QStringLiteral("%1, at %2 ms").arg(tally(title)).arg(lastOf("qualified", title).position));
    }

    // 4 — seeking to the end is not hearing it.
    {
        const QString title = QStringLiteral("Seeked to the end");
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(10000);
        player.setPosition(175000);
        fake.newFile(175000);   // mpv carries on from where it was sent
        fake.play(5000);
        fake.end();
        t.check(count("started", title) == 1 && count("qualified", title) == 0,
                QStringLiteral("10 s, a seek to 2:55, 5 s to the end: 15 s heard, no scrobble"), tally(title));
    }
    {
        const QString title = QStringLiteral("Seeked back");
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(60000);
        player.setPosition(0);
        fake.newFile(0);
        fake.play(30000);
        t.check(count("qualified", title) == 1 && lastOf("qualified", title).position == 30000,
                QStringLiteral("a minute, a seek back to 0:00, 30 s more: 90 s heard, counts"), tally(title));
    }

    // 5 — pauses: not heard, and a long one says "now playing" again.
    {
        const QString title = QStringLiteral("Paused twice");
        const qint64 began = fake.wall;
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(30000);
        fake.playing(false);
        fake.idle(60000);
        fake.playing(true);
        const int resumedAfterShort = count("resumed", title);
        fake.play(30000);
        fake.playing(false);
        fake.idle(10 * 60000);
        fake.playing(true);
        const int resumedAfterLong = count("resumed", title);
        fake.play(29750);
        const bool notYet = count("qualified", title) == 0;
        fake.play(250);
        const Heard q = lastOf("qualified", title);
        t.check(resumedAfterShort == 0 && resumedAfterLong == 1,
                QStringLiteral("a minute's pause says nothing; ten minutes' says \"now playing\" again"),
                QStringLiteral("after 1 min: %1, after 10 min: %2").arg(resumedAfterShort).arg(resumedAfterLong));
        t.check(notYet && q.position == 90000 && q.startedAt == began / 1000,
                QStringLiteral("11 minutes paused count for nothing: it counts at 1:30 heard, started when it started"),
                QStringLiteral("%1, at %2 ms, started %3").arg(tally(title)).arg(q.position).arg(q.startedAt));
    }

    // 6 — buffering, and jumps the engine reports by itself.
    {
        const QString title = QStringLiteral("Buffering");
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(30000);
        fake.buffering(true);
        fake.play(20000);   // the clock moving anyway: the worst case
        fake.buffering(false);
        fake.jump(fake.position + 30000);
        fake.play(59750);
        const bool notYet = count("qualified", title) == 0;
        fake.play(250);
        t.check(notYet && count("qualified", title) == 1 && lastOf("qualified", title).position == 140000,
                QStringLiteral("20 s buffering and a 30 s jump are not heard: counts at 2:20 on the clock, 1:30 heard"),
                QStringLiteral("%1, at %2 ms").arg(tally(title)).arg(lastOf("qualified", title).position));
    }

    // 7 — repeat-one: each time round is a listen.
    {
        const QString title = QStringLiteral("On repeat");
        while (player.repeatMode() != PlaybackController::RepeatOne)
            player.cycleRepeat();
        start({ song(title, 60000) });
        fake.duration(60000);
        fake.play(60000);
        fake.end();
        fake.newFile();
        fake.play(60000);
        fake.end();          // and round again, not heard this time
        fake.newFile();
        while (player.repeatMode() != PlaybackController::RepeatOff)
            player.cycleRepeat();
        t.check(count("started", title) == 2 && count("qualified", title) == 2,
                QStringLiteral("repeat-one, heard twice: two scrobbles"), tally(title));
    }

    // 8 — the picture switched on and off: the same listen.
    {
        const QString title = QStringLiteral("With a video");
        const QString id = QStringLiteral("selftestVid01");
        if (!YtDlp::isAvailable()) {
            t.note(QStringLiteral("video toggle skipped: the picture is resolved by yt-dlp, and it is not installed"));
        } else {
            QVariantMap video = song(title, 180000, { { QStringLiteral("sourceId"), id },
                                                      { QStringLiteral("isVideo"), true } });
            video.remove(QStringLiteral("sourceUrl"));
            player.playTracks({ video }, 0, QStringLiteral("selftest"));
            Q_EMIT resolver.resolved(id, QStringLiteral("selftest-missing/audio.m4a"), StreamResolver::TierYtDlp, false);
            fake.newFile();
            fake.playing(true);
            fake.duration(180000);
            fake.play(60000);
            player.setVideoWanted(true);
            Q_EMIT resolver.videoResolved(id, QStringLiteral("selftest-missing/video.mp4"),
                                          QStringLiteral("selftest-missing/audio.m4a"), QVariantMap());
            // The picture's stream opens a little behind where the sound was.
            fake.newFile(59600);
            fake.play(30400);
            const int countedWithPicture = count("qualified", title);
            player.setVideoWanted(false);
            Q_EMIT resolver.resolved(id, QStringLiteral("selftest-missing/audio.m4a"), StreamResolver::TierYtDlp, false);
            fake.newFile(fake.position);
            fake.play(90000);
            fake.end();
            t.check(player.videoPlaying() == false && countedWithPicture == 1 && count("started", title) == 1
                        && count("qualified", title) == 1,
                    QStringLiteral("sound, then the picture, then the sound again: one listen, one scrobble"),
                    tally(title));
        }
    }

    // 9 — the song a launch opens on, paused, then played.
    {
        const QString title = QStringLiteral("Opened at launch");
        QStandardItemModel library;
        library.setItemRoleNames({ { Qt::UserRole + 1, "title" }, { Qt::UserRole + 2, "artist" },
                                   { Qt::UserRole + 3, "durationMs" }, { Qt::UserRole + 4, "sourceUrl" } });
        auto *row = new QStandardItem;
        row->setData(title, Qt::UserRole + 1);
        row->setData(QStringLiteral("Selftest Artist"), Qt::UserRole + 2);
        row->setData(qint64(180000), Qt::UserRole + 3);
        row->setData(QStringLiteral("selftest-missing/launch.m4a"), Qt::UserRole + 4);
        library.appendRow(row);

        fake.playing(false);
        player.loadModel(&library, 0);   // as main.cpp does at launch
        fake.newFile();
        fake.duration(180000);
        fake.idle(60 * 60000);           // an hour before anyone presses Play
        const int beforePlay = count("started", title);
        const qint64 pressed = fake.wall;
        player.play();
        fake.playing(true);
        fake.play(90000);
        const Heard q = lastOf("qualified", title);
        t.check(beforePlay == 0 && count("started", title) == 1 && count("qualified", title) == 1
                    && count("resumed", title) == 0 && q.startedAt == pressed / 1000,
                QStringLiteral("loaded paused at launch: nothing until Play, then counted from Play"),
                QStringLiteral("%1; started %2, Play pressed at %3").arg(tally(title)).arg(q.startedAt).arg(pressed / 1000));
    }

    // 10 — a song that never resolves was never heard.
    {
        const QString bad = QStringLiteral("Will not resolve");
        const QString next = QStringLiteral("After the failure");
        QVariantMap broken = song(bad, 180000, { { QStringLiteral("sourceId"), QStringLiteral("selftestBad01") } });
        broken.remove(QStringLiteral("sourceUrl"));
        fake.playing(true);
        player.playTracks({ broken, song(next, 60000) }, 0, QStringLiteral("selftest"));
        Q_EMIT resolver.failed(QStringLiteral("selftestBad01"), QStringLiteral("selftest: no source"));
        fake.newFile();
        fake.duration(60000);
        fake.play(30000);
        t.check(count("started", bad) == 0 && count("qualified", bad) == 0 && count("qualified", next) == 1,
                QStringLiteral("a failed resolve: nothing for it; the song after counts"),
                QStringLiteral("%1 / %2").arg(tally(bad), tally(next)));

        const QString alone = QStringLiteral("Fails alone");
        QVariantMap lone = song(alone, 180000, { { QStringLiteral("sourceId"), QStringLiteral("selftestBad02") } });
        lone.remove(QStringLiteral("sourceUrl"));
        player.playTracks({ lone }, 0, QStringLiteral("selftest"));
        Q_EMIT resolver.failed(QStringLiteral("selftestBad02"), QStringLiteral("selftest: no source"));
        t.check(count("started", alone) == 0 && !player.playing(),
                QStringLiteral("a failed resolve with nothing after it: stopped, nothing heard"), tally(alone));
    }

    // 11 — Previous restarting a song: heard again, a listen again.
    {
        const QString title = QStringLiteral("Heard twice");
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(100000);
        player.previous();
        fake.newFile(0);
        fake.play(90000);
        t.check(count("started", title) == 2 && count("qualified", title) == 2,
                QStringLiteral("played past half, restarted with Previous, played past half again: two"), tally(title));
    }
    {
        // A song that came with no length (a Home card): the engine says
        // 3:30 once, when the file opens, and never again after a seek.
        const QString title = QStringLiteral("Restarted, no listed length");
        start({ song(title, 0) });
        fake.duration(210000);
        fake.play(20000);
        player.previous();
        fake.newFile(0);
        fake.play(104750);
        const bool notYet = count("qualified", title) == 0;
        fake.play(250);
        t.check(notYet && count("started", title) == 2 && count("qualified", title) == 1
                    && lastOf("qualified", title).track.value(QStringLiteral("durationMs")).toLongLong() == 210000,
                QStringLiteral("no listed length, 20 s, Previous, then half of the 3:30 mpv reported: counts"),
                tally(title));
    }
    {
        // For the recommender: a restart is not a return to the song, so the
        // replay skipped a little way in stays the lukewarm listen it was.
        const QString title = QStringLiteral("Restarted then skipped");
        start({ song(title, 180000) });
        fake.duration(180000);
        fake.play(5000);
        player.previous();
        fake.newFile(0);
        fake.play(20000);
        start({ song(QStringLiteral("After the restart"), 60000) });
        fake.duration(60000);
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("SELECT listened_ms, label, repeat_in_session FROM play_events"
                                 " WHERE title = ? ORDER BY id"));
        q.addBindValue(title);
        QStringList rows;
        QList<double> labels;
        QList<int> repeats;
        if (q.exec()) {
            while (q.next()) {
                labels << q.value(1).toDouble();
                repeats << q.value(2).toInt();
                rows << QStringLiteral("%1 ms, label %2, repeat %3").arg(q.value(0).toLongLong())
                            .arg(q.value(1).toDouble()).arg(q.value(2).toInt());
            }
        }
        t.check(labels == QList<double>({ 0.0, 0.2 }) && repeats == QList<int>({ 0, 0 }),
                QStringLiteral("restarted at 0:05, skipped 20 s into the replay: 0.0 then 0.2, neither a repeat"),
                rows.join(QStringLiteral("; ")));
    }

    // 12 — what autoplay added was not chosen.
    {
        const QString title = QStringLiteral("From the radio");
        start({ song(title, 60000, { { QStringLiteral("fromRadio"), true } }) });
        fake.duration(60000);
        fake.play(30000);
        const Heard q = lastOf("qualified", title);
        t.check(count("qualified", title) == 1 && !q.chosen && !lastOf("started", title).chosen,
                QStringLiteral("a song autoplay added is sent as not chosen by the listener"), tally(title));
    }

    // 13 — a length learned late.
    {
        const QString title = QStringLiteral("No length yet");
        start({ song(title, 0, { { QStringLiteral("primaryArtist"), QStringLiteral("Selftest Artist") } }) });
        fake.play(300000);
        const int beforeLength = count("qualified", title);
        fake.duration(400000);
        const Heard q = lastOf("qualified", title);
        t.check(beforeLength == 0 && count("qualified", title) == 1 && q.position == 300000
                    && q.track.value(QStringLiteral("durationMs")).toLongLong() == 400000,
                QStringLiteral("unknown length: waits; told 6:40 after 5:00 heard, counts at once with it"),
                QStringLiteral("%1; length %2").arg(tally(title)).arg(q.track.value(QStringLiteral("durationMs")).toLongLong()));
        t.check(q.track.value(QStringLiteral("primaryArtist")).toString() == QLatin1String("Selftest Artist")
                    && Scrobbler::scrobbleArtist(q.track) == QLatin1String("Selftest Artist"),
                QStringLiteral("the primary artist travels through the queue to the listen"));
    }

    ListenTracker::setClock(nullptr);
    t.note(QStringLiteral("%1 listen signals in all").arg(heard.size()));
    return t.finish();
}

// ============================================================ --scrobble-test

int runScrobbleSelfTest(Library *library)
{
    Checks t("scrobble");
    if (!scratchDataDir(t))
        return t.finish();
    if (!SecretStore::available()) {
        t.note(QStringLiteral("no secret store here (%1): a Last.fm session cannot be kept, so nothing is recorded")
                   .arg(SecretStore::unavailableReason()));
        return t.finish();
    }

    clearQueue();
    library->setSetting(QStringLiteral("lastfm.user"), QString());
    library->setSetting(QStringLiteral("lastfm.enabled"), QStringLiteral("1"));
    SecretStore::remove(QStringLiteral("lastfm.session"));

    Server server;
    LastFmApi api;
    attach(api, server);
    Scrobbler::Timing fast;
    fast.afterEnqueueMs = 3600 * 1000;   // sent only when a check asks
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    // — what is kept —
    {
        Scrobbler offline(&api, library);
        offline.start();
        offline.listenQualified(sigurRos(), now - 200, true);
        t.check(offline.state() == QLatin1String("off") && queued() == 0,
                QStringLiteral("not connected: a listen that qualifies is not kept"),
                QStringLiteral("state %1, %2 queued").arg(offline.state()).arg(queued()));
    }
    {
        auto s = connectedScrobbler(api, library, fast);
        s->listenQualified(sigurRos(), now - 200, true);
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("SELECT account, artist, track, album, duration_s, started_at, chosen_by_user,"
                              " video_id, attempts, last_error, queued_at FROM scrobble_queue"));
        const bool row = q.next();
        const bool right = row && q.value(0).toString() == kUser && q.value(1).toString() == QStringLiteral("Sigur Rós")
                           && q.value(2).toString() == QStringLiteral("Hoppípolla")
                           && q.value(3).toString() == QStringLiteral("Takk…") && q.value(4).toInt() == 268
                           && q.value(5).toLongLong() == now - 200 && q.value(6).toInt() == 1
                           && q.value(7).toString() == QLatin1String("selftest01") && q.value(8).toInt() == 0
                           && q.value(9).toString().isEmpty() && qAbs(q.value(10).toLongLong() - now) <= 5;
        t.check(right && queued() == 1,
                QStringLiteral("connected: one row, with the account, the artist without \" - Topic\", album, "
                               "length, started_at and chosen_by_user 1"),
                row ? QStringLiteral("%1 | %2 | %3 | %4 s | started %5 | chosen %6")
                          .arg(q.value(0).toString(), q.value(2).toString(), q.value(3).toString())
                          .arg(q.value(4).toInt()).arg(q.value(5).toLongLong()).arg(q.value(6).toInt())
                    : QStringLiteral("no row"));
        t.check(server.requests.isEmpty(), QStringLiteral("queued, not sent: nothing goes until the flush"));

        QVariantMap radio = song(QStringLiteral("Radio pick"), 200000);
        s->listenQualified(radio, now - 100, false);
        QSqlQuery r(AppDatabase::connection());
        r.exec(QStringLiteral("SELECT chosen_by_user, artist FROM scrobble_queue WHERE track = 'Radio pick'"));
        t.check(r.next() && r.value(0).toInt() == 0 && r.value(1).toString() == QLatin1String("Selftest Artist"),
                QStringLiteral("a song autoplay chose: chosen_by_user 0"));

        QVariantMap noArtist = sigurRos();
        noArtist.remove(QStringLiteral("artist"));
        QVariantMap noTitle = sigurRos();
        noTitle.insert(QStringLiteral("title"), QStringLiteral("  "));
        s->listenQualified(noArtist, now, true);
        s->listenQualified(noTitle, now, true);
        t.check(queued() == 2, QStringLiteral("no artist, or no title: not kept"), QStringLiteral("%1 queued").arg(queued()));

        s->setEnabled(false);
        s->listenQualified(sigurRos(), now, true);
        const int whileOff = queued();
        s->listenStarted(sigurRos(), now, true);
        settle(30);
        t.check(whileOff == 2 && server.count(QStringLiteral("track.updateNowPlaying")) == 0
                    && library->settingValue(QStringLiteral("lastfm.enabled")) == QLatin1String("0"),
                QStringLiteral("Scrobble switched off: nothing kept, nothing sent, and the switch remembered"));
        s->setEnabled(true);
        t.check(s->pending() == 2 && s->statusLine().contains(QLatin1String("2 scrobbles waiting")),
                QStringLiteral("the row says how many are waiting"), s->statusLine());
    }

    // — the cap —
    {
        clearQueue();
        seed(QStringLiteral("cap-test"), Scrobbler::kMaxQueued);
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("SELECT MIN(id) FROM scrobble_queue"));
        const qint64 oldest = q.next() ? q.value(0).toLongLong() : -1;
        auto s = connectedScrobbler(api, library, fast);
        s->listenQualified(sigurRos(), now, true);
        q.exec(QStringLiteral("SELECT MIN(id), COUNT(*) FROM scrobble_queue"));
        const bool ok = q.next() && q.value(0).toLongLong() == oldest + 1 && q.value(1).toInt() == Scrobbler::kMaxQueued;
        t.check(ok && queued(kUser) == 1,
                QStringLiteral("a full queue (10,000) drops its oldest row for the new one"),
                QStringLiteral("oldest id %1 -> %2, %3 rows").arg(oldest).arg(q.value(0).toLongLong()).arg(q.value(1).toInt()));
        clearQueue();
    }

    // — 120 waiting: three requests, 50, 50 and 20, oldest first —
    {
        const QList<qint64> ids = seed(kUser, 120);
        const qint64 oldest = firstStartedAt(kUser);
        server.requests.clear();
        server.answer = [](const Params &sent) { return qMakePair(200, acceptAll(itemCount(sent))); };
        auto s = connectedScrobbler(api, library, fast);
        QList<int> sizes;
        QObject::connect(s.get(), &Scrobbler::batchAnswered, s.get(), [&sizes](int items, const QString &) { sizes << items; });
        s->flush();
        waitUntil([&]() { return queued(kUser) == 0; }, 5000);
        const QList<Params> sent = server.of(QStringLiteral("track.scrobble"));
        QStringList seen;
        bool signedOk = true;
        bool order = true;
        int notChosen = 0;
        qint64 previous = 0;
        for (const Params &request : sent) {
            seen << QString::number(itemCount(request));
            signedOk = signedOk && signedWith(request, kSecret) && param(request, QStringLiteral("sk")) == QString::fromLatin1(kSession)
                       && param(request, QStringLiteral("api_key")) == QString::fromLatin1(kKey)
                       && param(request, QStringLiteral("format")) == QLatin1String("json");
            for (int i = 0; i < itemCount(request); ++i) {
                const qint64 stamp = param(request, QStringLiteral("timestamp[%1]").arg(i)).toLongLong();
                order = order && stamp > previous;
                previous = stamp;
                notChosen += param(request, QStringLiteral("chosenByUser[%1]").arg(i)) == QLatin1String("0");
            }
        }
        t.check(seen.join(QLatin1Char('/')) == QLatin1String("50/50/20") && sizes == QList<int>({ 50, 50, 20 }),
                QStringLiteral("120 waiting go as three requests: 50/50/20"), seen.join(QLatin1Char('/')));
        t.check(order && !sent.isEmpty()
                    && param(sent.first(), QStringLiteral("timestamp[0]")).toLongLong() == oldest,
                QStringLiteral("oldest first, in order, across the three"));
        t.check(signedOk, QStringLiteral("every request signed with the account's secret, with its key and the session"));
        t.check(notChosen == 17, QStringLiteral("chosenByUser[i]=0 sent for exactly the 17 rows autoplay chose"),
                QString::number(notChosen));
        t.check(queued(kUser) == 0 && s->pending() == 0 && s->pause() == Scrobbler::Pause::None,
                QStringLiteral("all accepted: the queue is empty"), QString::number(queued(kUser)));
        Q_UNUSED(ids)
    }

    // — error 26, and the other refusals of the key: everything kept —
    for (int code : { 26, 10, 13 }) {
        clearQueue();
        seed(kUser, 120);
        server.requests.clear();
        server.answer = [code](const Params &) { return qMakePair(403, errorAnswer(code)); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return server.count(QStringLiteral("track.scrobble")) == 1 && s->pause() != Scrobbler::Pause::None; }, 3000);
        settle(200);
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("SELECT COUNT(*) FROM scrobble_queue WHERE attempts = 1 AND last_error LIKE 'error %1:%'").arg(code));
        const int marked = q.next() ? q.value(0).toInt() : -1;
        const qint64 wait = s->pausedUntil() - QDateTime::currentMSecsSinceEpoch();
        t.check(queued(kUser) == 120 && server.count(QStringLiteral("track.scrobble")) == 1
                    && s->pause() == Scrobbler::Pause::Hold && marked == 50
                    && wait > 29 * 60000 && wait <= 30 * 60000,
                QStringLiteral("error %1: all 120 kept, sending held for 30 min, the 50 tried marked").arg(code),
                QStringLiteral("%1 queued, %2 requests, pause %3 for %4 s, %5 marked")
                    .arg(queued(kUser)).arg(server.count(QStringLiteral("track.scrobble")))
                    .arg(Scrobbler::pauseName(s->pause())).arg(wait / 1000).arg(marked));
        if (code == 26)
            t.check(s->state() == QLatin1String("connected") && s->statusLine().contains(QLatin1String("error 26")),
                    QStringLiteral("  and the row says why"), s->statusLine());
    }

    // — 29: a quarter of an hour —
    {
        clearQueue();
        seed(kUser, 3);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(429, errorAnswer(29)); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        const qint64 wait = s->pausedUntil() - QDateTime::currentMSecsSinceEpoch();
        t.check(s->pause() == Scrobbler::Pause::RateLimit && wait > 14 * 60000 && wait <= 15 * 60000 && queued(kUser) == 3,
                QStringLiteral("error 29: kept, and a 15-minute pause"),
                QStringLiteral("pause %1 for %2 s").arg(Scrobbler::pauseName(s->pause())).arg(wait / 1000));
    }

    // — no answer, 11, 16, 5xx: back off, doubling, with jitter —
    {
        clearQueue();
        seed(kUser, 3);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(503, errorAnswer(11)); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        const qint64 wait = s->pausedUntil() - QDateTime::currentMSecsSinceEpoch();
        t.check(s->pause() == Scrobbler::Pause::Backoff && wait >= 29000 && wait <= 37500 && queued(kUser) == 3,
                QStringLiteral("error 11: kept, first retry after 30-37.5 s"),
                QStringLiteral("pause %1 for %2 ms").arg(Scrobbler::pauseName(s->pause())).arg(wait));
    }
    {
        clearQueue();
        seed(kUser, 3);
        server.requests.clear();
        int failures = 0;
        server.answer = [&failures](const Params &sent) {
            // No answer, 16, a proxy's page, then success.
            switch (failures++) {
            case 0: return qMakePair(0, QByteArray());
            case 1: return qMakePair(500, errorAnswer(16));
            case 2: return qMakePair(502, QByteArray("<html>Bad Gateway</html>"));
            default: return qMakePair(200, acceptAll(itemCount(sent)));
            }
        };
        Scrobbler::Timing quick;
        quick.afterEnqueueMs = 3600 * 1000;
        quick.backoffMinMs = 100;
        quick.backoffMaxMs = 1000;
        auto s = connectedScrobbler(api, library, quick);
        QList<qint64> waits;
        QObject::connect(s.get(), &Scrobbler::batchAnswered, s.get(), [&waits, &s](int, const QString &outcome) {
            if (outcome == QLatin1String("retry"))
                waits << s->pausedUntil() - QDateTime::currentMSecsSinceEpoch();
        });
        s->flush();
        waitUntil([&]() { return queued(kUser) == 0; }, 5000);
        QStringList shown;
        for (qint64 w : waits)
            shown << QString::number(w);
        const bool doubling = waits.size() == 3 && waits.at(0) >= 90 && waits.at(0) <= 125
                              && waits.at(1) >= 140 && waits.at(1) <= 250 && waits.at(2) >= 290 && waits.at(2) <= 500;
        t.check(doubling && queued(kUser) == 0 && s->pause() == Scrobbler::Pause::None,
                QStringLiteral("no answer, 16, a proxy's page: waits that double (100 ms scale), then sent"),
                QStringLiteral("waits %1 ms; %2 left").arg(shown.join(QStringLiteral(", "))).arg(queued(kUser)));
    }

    // — 9: the session was revoked —
    {
        clearQueue();
        seed(kUser, 5);
        SecretStore::write(QStringLiteral("lastfm.session"), kSession);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(403, errorAnswer(9)); };
        auto s = connectedScrobbler(api, library, fast);
        s->flush();
        waitUntil([&]() { return s->state() == QLatin1String("expired"); }, 3000);
        QByteArray left;
        const SecretStore::Status stored = SecretStore::read(QStringLiteral("lastfm.session"), &left);
        t.check(s->state() == QLatin1String("expired") && stored == SecretStore::Status::NotFound && queued(kUser) == 5
                    && s->statusLine().contains(QLatin1String("Reconnect")),
                QStringLiteral("error 9: the key is deleted, the row says Reconnect, all 5 kept"),
                QStringLiteral("state %1, key %2, %3 queued: %4").arg(s->state(), SecretStore::statusText(stored))
                    .arg(queued(kUser)).arg(s->statusLine()));
        s->listenQualified(sigurRos(), now, true);
        const int requests = int(server.requests.size());
        s->flush();
        settle(50);
        t.check(queued(kUser) == 6 && server.requests.size() == requests,
                QStringLiteral("while expired: listens still kept, nothing sent"));
    }

    // — each item's own answer —
    {
        clearQueue();
        const QList<qint64> ids = seed(kUser, 5);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(200, scrobbleAnswer({ 0, 1, 3, 5, 0 })); };
        auto s = connectedScrobbler(api, library, fast);
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("SELECT id, last_error FROM scrobble_queue"));
        const bool kept = q.next() && q.value(0).toLongLong() == ids.value(3)
                          && q.value(1).toString().contains(QLatin1String("daily")) && !q.next();
        const QDateTime until = QDateTime::fromMSecsSinceEpoch(s->pausedUntil(), QTimeZone::UTC);
        t.check(kept && s->pause() == Scrobbler::Pause::DailyLimit
                    && until.date() == QDateTime::currentDateTimeUtc().date().addDays(1) && until.time().hour() == 0,
                QStringLiteral("codes 0, 1, 3, 5, 0: four gone, the daily-limit one kept until tomorrow (UTC)"),
                QStringLiteral("%1 left, pause %2 until %3").arg(queued(kUser)).arg(Scrobbler::pauseName(s->pause()))
                    .arg(until.toString(Qt::ISODate)));
    }

    // — 6 on a batch: each item alone, once —
    {
        clearQueue();
        seed(kUser, 3);
        QSqlQuery mark(AppDatabase::connection());
        mark.exec(QStringLiteral("UPDATE scrobble_queue SET track = 'The bad one' WHERE id = (SELECT MIN(id) + 1 FROM scrobble_queue)"));
        server.requests.clear();
        server.answer = [](const Params &sent) {
            if (itemCount(sent) > 1 || param(sent, QStringLiteral("track[0]")) == QLatin1String("The bad one"))
                return qMakePair(400, errorAnswer(6));
            return qMakePair(200, acceptAll(1));
        };
        auto s = connectedScrobbler(api, library, fast);
        s->flush();
        waitUntil([&]() { return queued(kUser) == 0; }, 3000);
        QStringList shape;
        for (const Params &request : server.of(QStringLiteral("track.scrobble")))
            shape << QString::number(itemCount(request));
        t.check(queued(kUser) == 0 && shape.join(QLatin1Char('/')) == QLatin1String("3/1/1/1"),
                QStringLiteral("error 6 on a batch of 3: sent again one by one; the one refused alone is dropped"),
                QStringLiteral("requests %1, %2 left").arg(shape.join(QLatin1Char('/'))).arg(queued(kUser)));
    }

    // — 8, "operation failed", with a 500: Last.fm's own trouble. Nothing is
    // split and nothing dropped; it is waited out —
    {
        clearQueue();
        seed(kUser, 120);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(500, errorAnswer(8)); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        settle(200);
        t.check(queued(kUser) == 120 && server.count(QStringLiteral("track.scrobble")) == 1
                    && s->pause() == Scrobbler::Pause::Backoff,
                QStringLiteral("error 8 (HTTP 500) on a batch of 50: all 120 kept, one request, backing off"),
                QStringLiteral("%1 queued, %2 requests, pause %3").arg(queued(kUser))
                    .arg(server.count(QStringLiteral("track.scrobble"))).arg(Scrobbler::pauseName(s->pause())));
    }

    // — a refusal that says nothing about the item (4, authentication
    // failed): the batch is split, but the one refused alone is kept, and
    // sending held, so the same answer to every item cannot empty the queue —
    {
        clearQueue();
        seed(kUser, 3);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(403, errorAnswer(4)); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        settle(200);
        QStringList shape;
        for (const Params &request : server.of(QStringLiteral("track.scrobble")))
            shape << QString::number(itemCount(request));
        t.check(queued(kUser) == 3 && shape.join(QLatin1Char('/')) == QLatin1String("3/1")
                    && s->pause() == Scrobbler::Pause::Hold && s->statusLine().contains(QLatin1String("error 4"))
                    && !s->statusLine().contains(QLatin1String("key")),
                QStringLiteral("error 4 on a batch of 3, then on the first alone: all 3 kept, held, and the row says so"),
                QStringLiteral("requests %1, %2 left, pause %3: %4").arg(shape.join(QLatin1Char('/'))).arg(queued(kUser))
                    .arg(Scrobbler::pauseName(s->pause()), s->statusLine()));
    }

    // — an answer that does not account for every item —
    {
        clearQueue();
        seed(kUser, 3);
        server.requests.clear();
        server.answer = [](const Params &) { return qMakePair(200, scrobbleAnswer({ 0, 0 })); };
        auto s = connectedScrobbler(api, library, Scrobbler::Timing());
        s->flush();
        waitUntil([&]() { return s->pause() != Scrobbler::Pause::None; }, 3000);
        t.check(queued(kUser) == 3 && s->pause() == Scrobbler::Pause::Backoff,
                QStringLiteral("an answer listing 2 of 3: none trusted, all kept, tried later"));
    }

    // — another account's backlog —
    {
        clearQueue();
        seed(QStringLiteral("someone-else"), 3);
        seed(kUser, 2);
        server.requests.clear();
        server.answer = [](const Params &sent) { return qMakePair(200, acceptAll(itemCount(sent))); };
        auto s = connectedScrobbler(api, library, fast);
        s->flush();
        waitUntil([&]() { return queued(kUser) == 0; }, 3000);
        t.check(queued(QStringLiteral("someone-else")) == 3 && server.count(QStringLiteral("track.scrobble")) == 1
                    && itemCount(server.requests.value(0)) == 2,
                QStringLiteral("only this account's scrobbles are sent; another's stay"));
    }

    // — now playing —
    {
        clearQueue();
        server.requests.clear();
        server.answer = [](const Params &) {
            return qMakePair(200, QByteArray(R"({"nowplaying":{"ignoredMessage":{"code":"0","#text":""}}})"));
        };
        auto s = connectedScrobbler(api, library, fast);
        s->listenStarted(sigurRos(), now, true);
        waitUntil([&]() { return server.count(QStringLiteral("track.updateNowPlaying")) == 1; }, 3000);
        const Params sent = server.of(QStringLiteral("track.updateNowPlaying")).value(0);
        t.check(param(sent, QStringLiteral("artist")) == QStringLiteral("Sigur Rós")
                    && param(sent, QStringLiteral("track")) == QStringLiteral("Hoppípolla")
                    && param(sent, QStringLiteral("album")) == QStringLiteral("Takk…")
                    && param(sent, QStringLiteral("duration")) == QLatin1String("268")
                    && param(sent, QStringLiteral("sk")) == QString::fromLatin1(kSession)
                    && !hasParam(sent, QStringLiteral("timestamp")) && signedWith(sent, kSecret),
                QStringLiteral("a listen starting sends track.updateNowPlaying: artist, track, album, duration, signed"));
        t.check(queued() == 0, QStringLiteral("  and queues nothing"));
        s->listenResumed(sigurRos(), now, true);
        waitUntil([&]() { return server.count(QStringLiteral("track.updateNowPlaying")) == 2; }, 3000);
        t.check(server.count(QStringLiteral("track.updateNowPlaying")) == 2,
                QStringLiteral("resumed after a long pause: sent again"));

        server.answer = [](const Params &) { return qMakePair(403, errorAnswer(9)); };
        SecretStore::write(QStringLiteral("lastfm.session"), kSession);
        s->listenStarted(sigurRos(), now, true);
        waitUntil([&]() { return s->state() == QLatin1String("expired"); }, 3000);
        t.check(s->state() == QLatin1String("expired"),
                QStringLiteral("error 9 on now playing: expired as well"), s->state());
    }

    clearQueue();
    SecretStore::remove(QStringLiteral("lastfm.session"));
    library->setSetting(QStringLiteral("lastfm.user"), QString());
    library->setSetting(QStringLiteral("lastfm.enabled"), QStringLiteral("1"));
    return t.finish();
}

// ============================================================ --lastfm-connect-test

int runLastFmConnectSelfTest(Library *library)
{
    Checks t("connect");
    if (!scratchDataDir(t))
        return t.finish();

    const auto reset = [library]() {
        SecretStore::remove(QStringLiteral("lastfm.session"));
        library->setSetting(QStringLiteral("lastfm.user"), QString());
    };
    reset();

    // — this build —
    {
        LastFmApi build;
        Scrobbler s(&build, library);
        s.start();
        if (build.hasKey()) {
            t.note(QStringLiteral("this build has a Last.fm key; the no-key checks are skipped"));
        } else {
            s.connectAccount();
            t.check(s.state() == QLatin1String("unavailable")
                        && s.statusLine() == QLatin1String("This build has no Last.fm key."),
                    QStringLiteral("no key in this build: the row is unavailable and says why; Connect does nothing"),
                    QStringLiteral("state %1: %2").arg(s.state(), s.statusLine()));
        }
    }

    if (!SecretStore::available()) {
        t.note(QStringLiteral("no secret store here: %1").arg(SecretStore::unavailableReason()));
        return t.finish();
    }

    Server server;
    LastFmApi api;
    attach(api, server);
    const auto tokenAnswer = qMakePair(200, QByteArray(R"({"token":"TESTTOKEN6789"})"));
    const auto sessionAnswer = qMakePair(200, QByteArray(R"({"session":{"name":"monolist-test","key":"TESTSESSIONKEY0000","subscriber":0}})"));
    const auto notYet = qMakePair(403, errorAnswer(14));
    int sessionCalls = 0;
    int approveAfter = 0;   // getSession answers 14 this many times, then a session
    server.answer = [&](const Params &sent) {
        const QString method = param(sent, QStringLiteral("method"));
        if (method == QLatin1String("auth.getToken"))
            return tokenAnswer;
        if (method == QLatin1String("auth.getSession"))
            return ++sessionCalls > approveAfter ? sessionAnswer : notYet;
        return qMakePair(400, errorAnswer(6));
    };
    QList<QUrl> opened;
    bool browserOpens = true;
    const auto make = [&](const Scrobbler::Timing &timing) {
        auto s = std::make_unique<Scrobbler>(&api, library);
        s->setTiming(timing);
        s->setUrlOpener([&opened, &browserOpens](const QUrl &url) {
            opened << url;
            return browserOpens;
        });
        s->start();
        return s;
    };

    // — connect, polling —
    {
        Scrobbler::Timing timing;
        timing.pollMs = 40;
        approveAfter = 2;
        sessionCalls = 0;
        auto s = make(timing);
        const QString before = s->state();
        s->connectAccount();
        const QString during = s->state();
        waitUntil([&]() { return s->state() == QLatin1String("connected"); }, 5000);
        t.check(before == QLatin1String("off") && during == QLatin1String("waiting") && s->state() == QLatin1String("connected")
                    && s->accountName() == kUser,
                QStringLiteral("off, then waiting, then connected as monolist-test"),
                QStringLiteral("%1 -> %2 -> %3 as \"%4\"").arg(before, during, s->state(), s->accountName()));

        const QUrl page = opened.value(0);
        const QUrlQuery query(page);
        t.check(opened.size() == 1 && page.scheme() == QLatin1String("https") && page.host() == QLatin1String("www.last.fm")
                    && page.path() == QLatin1String("/api/auth/")
                    && query.queryItemValue(QStringLiteral("api_key")) == QString::fromLatin1(kKey)
                    && query.queryItemValue(QStringLiteral("token")) == kToken,
                QStringLiteral("the browser is sent once to last.fm/api/auth/ with the key and the token"));

        const QList<Params> asked = server.of(QStringLiteral("auth.getSession"));
        bool allSigned = !asked.isEmpty() && signedWith(server.of(QStringLiteral("auth.getToken")).value(0), kSecret);
        for (const Params &request : asked)
            allSigned = allSigned && signedWith(request, kSecret) && param(request, QStringLiteral("token")) == kToken;
        t.check(asked.size() == 3 && allSigned,
                QStringLiteral("getSession asked every poll until approved (14, 14, then the session), each signed"),
                QStringLiteral("%1 asked").arg(asked.size()));

        QByteArray stored;
        const SecretStore::Status read = SecretStore::read(QStringLiteral("lastfm.session"), &stored);
        t.check(read == SecretStore::Status::Ok && stored == kSession,
                QStringLiteral("the session key is kept in the secret store"), SecretStore::statusText(read));
        t.check(library->settingValue(QStringLiteral("lastfm.user")) == kUser,
                QStringLiteral("the user name is kept in settings"));
        QSqlQuery q(AppDatabase::connection());
        q.exec(QStringLiteral("SELECT key, value FROM settings"));
        QStringList leaks;
        while (q.next()) {
            const QString value = q.value(1).toString();
            if (value.contains(QString::fromLatin1(kSession)) || value.contains(kToken)
                || value.contains(QString::fromLatin1(kSecret)))
                leaks << q.value(0).toString();
        }
        t.check(leaks.isEmpty(), QStringLiteral("no setting holds the session key, the token or the secret"),
                leaks.join(QStringLiteral(", ")));
        const int polls = server.count(QStringLiteral("auth.getSession"));
        settle(150);
        t.check(server.count(QStringLiteral("auth.getSession")) == polls, QStringLiteral("connected: the polling stops"));
    }

    // — a restart finds it, and Disconnect removes it —
    {
        auto s = make(Scrobbler::Timing());
        t.check(s->state() == QLatin1String("connected") && s->accountName() == kUser,
                QStringLiteral("after a restart: still connected as monolist-test"), s->state());
        const QString file = QDir(SecretStore::folderPath()).filePath(QStringLiteral("lastfm.session.dpapi"));
        const bool fileBefore = SecretStore::folderPath().isEmpty() || QFileInfo::exists(file);
        s->disconnectAccount();
        QByteArray stored;
        t.check(s->state() == QLatin1String("off") && fileBefore && !QFileInfo::exists(file)
                    && SecretStore::read(QStringLiteral("lastfm.session"), &stored) == SecretStore::Status::NotFound
                    && library->settingValue(QStringLiteral("lastfm.user")).isEmpty(),
                QStringLiteral("Disconnect: off, the secret file is gone, the user name cleared"));
        t.check(s->statusLine().contains(QLatin1String("https://www.last.fm/settings/applications")),
                QStringLiteral("  and the row links to where the access can be revoked at Last.fm"));
        auto again = make(Scrobbler::Timing());
        t.check(again->state() == QLatin1String("off"), QStringLiteral("after another restart: off"));
    }

    // — a Disconnect that cannot delete the key: the row's button tries the
    // Disconnect again, never a new sign-in. Held open here, the key's file
    // cannot be deleted, as when another program has it open. —
    if (!SecretStore::folderPath().isEmpty()) {
        Scrobbler::Timing timing;
        timing.pollMs = 30;
        approveAfter = 0;
        sessionCalls = 0;
        auto s = make(timing);
        s->connectAccount();
        waitUntil([&]() { return s->state() == QLatin1String("connected"); }, 3000);
        const QString file = QDir(SecretStore::folderPath()).filePath(QStringLiteral("lastfm.session.dpapi"));
        QFile hold(file);
        const bool held = hold.open(QIODevice::ReadOnly);
        const int tokensBefore = server.count(QStringLiteral("auth.getToken"));
        s->disconnectAccount();
        const QString during = s->state();
        const bool flagged = s->disconnectFailed();
        const bool kept = QFileInfo::exists(file) && !s->accountName().isEmpty();
        hold.close();
        if (held && during == QLatin1String("off")) {
            t.note(QStringLiteral("the key's file could not be held open here; the failed Disconnect is not tried"));
        } else {
            t.check(held && during == QLatin1String("error") && flagged && kept,
                    QStringLiteral("Disconnect with the key's file held open: error, the account kept, marked as a "
                                   "Disconnect to try again"),
                    QStringLiteral("state %1, flagged %2: %3").arg(during).arg(flagged).arg(s->statusLine()));
            s->disconnectAccount();
            t.check(s->state() == QLatin1String("off") && !s->disconnectFailed() && !QFileInfo::exists(file)
                        && server.count(QStringLiteral("auth.getToken")) == tokensBefore,
                    QStringLiteral("  tried again once it is let go: off, the key gone, and no sign-in started"),
                    s->state());
        }
        s->disconnectAccount();
    }

    // — the button —
    {
        Scrobbler::Timing timing;
        timing.pollMs = 3600 * 1000;   // no polling: only the button asks
        approveAfter = 1;
        sessionCalls = 0;
        auto s = make(timing);
        s->connectAccount();
        waitUntil([&]() { return s->statusLine().contains(QLatin1String("Approve Monolist")); }, 3000);
        s->checkApproval();
        waitUntil([&]() { return sessionCalls == 1 && s->statusLine().contains(QLatin1String("not had your approval")); }, 3000);
        const QString after14 = s->statusLine();
        s->checkApproval();
        waitUntil([&]() { return s->state() == QLatin1String("connected"); }, 3000);
        t.check(after14.contains(QLatin1String("not had your approval")) && s->state() == QLatin1String("connected")
                    && sessionCalls == 2,
                QStringLiteral("I've approved it: too early says so; pressed again after approving, connects"),
                QStringLiteral("%1 asks; said \"%2\"").arg(sessionCalls).arg(after14));
        s->disconnectAccount();
    }

    // — coming back to the window —
    {
        Scrobbler::Timing timing;
        timing.pollMs = 3600 * 1000;
        approveAfter = 0;
        sessionCalls = 0;
        auto s = make(timing);
        s->connectAccount();
        waitUntil([&]() { return s->statusLine().contains(QLatin1String("Approve Monolist")); }, 3000);
        const int before = sessionCalls;
        if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
            Q_EMIT app->applicationStateChanged(Qt::ApplicationActive);
        waitUntil([&]() { return s->state() == QLatin1String("connected"); }, 3000);
        t.check(before == 0 && s->state() == QLatin1String("connected"),
                QStringLiteral("the window becoming active again asks at once, and connects"), s->state());
        s->disconnectAccount();
    }

    // — ten minutes, then only when asked —
    {
        Scrobbler::Timing timing;
        timing.pollMs = 30;
        timing.pollForMs = 250;
        approveAfter = 1000000;
        sessionCalls = 0;
        auto s = make(timing);
        s->connectAccount();
        settle(600);
        const int first = sessionCalls;
        settle(300);
        const int second = sessionCalls;
        const bool still = s->state() == QLatin1String("waiting") && s->statusLine().startsWith(QLatin1String("Still waiting"));
        s->checkApproval();
        waitUntil([&]() { return sessionCalls == second + 1; }, 3000);
        t.check(first >= 3 && second == first && still && sessionCalls == second + 1,
                QStringLiteral("polling stops after its time (10 min; 250 ms here); still waiting; the button still asks"),
                QStringLiteral("%1 polls, then %2; \"%3\"").arg(first).arg(second).arg(s->statusLine()));
        s->cancelConnect();
        const int cancelled = sessionCalls;
        settle(100);
        t.check(s->state() == QLatin1String("off") && sessionCalls == cancelled,
                QStringLiteral("Cancel: off, and nothing more is asked"));
    }

    // — the token expired (15) —
    {
        Scrobbler::Timing timing;
        timing.pollMs = 30;
        auto s = make(timing);
        server.answer = [&](const Params &sent) {
            const QString method = param(sent, QStringLiteral("method"));
            return method == QLatin1String("auth.getToken") ? tokenAnswer : qMakePair(403, errorAnswer(15));
        };
        s->connectAccount();
        waitUntil([&]() { return s->state() == QLatin1String("error"); }, 3000);
        t.check(s->state() == QLatin1String("error") && s->statusLine().contains(QLatin1String("expired")),
                QStringLiteral("error 15: the page expired; the row says so, for a new try"), s->statusLine());
        const int tokens = server.count(QStringLiteral("auth.getToken"));
        s->connectAccount();
        waitUntil([&]() { return server.count(QStringLiteral("auth.getToken")) == tokens + 1; }, 3000);
        t.check(server.count(QStringLiteral("auth.getToken")) == tokens + 1,
                QStringLiteral("  and trying again starts over with a new token"));
        s->cancelConnect();
    }

    // — getToken fails —
    for (const auto &[answer, expected, what] :
         { std::make_tuple(qMakePair(0, QByteArray()), QStringLiteral("did not answer"), QStringLiteral("no answer")),
           std::make_tuple(qMakePair(403, errorAnswer(26)), QStringLiteral("error 26"), QStringLiteral("error 26")),
           std::make_tuple(qMakePair(403, errorAnswer(10)), QStringLiteral("error 10"), QStringLiteral("error 10")) }) {
        const QPair<int, QByteArray> reply = answer;
        server.answer = [reply](const Params &) { return reply; };
        auto s = make(Scrobbler::Timing());
        s->connectAccount();
        waitUntil([&]() { return s->state() == QLatin1String("error"); }, 3000);
        t.check(s->state() == QLatin1String("error") && s->statusLine().contains(expected),
                QStringLiteral("getToken with %1: error, and says so").arg(what), s->statusLine());
    }

    // — no browser —
    {
        server.answer = [&](const Params &) { return tokenAnswer; };
        browserOpens = false;
        auto s = make(Scrobbler::Timing());
        s->connectAccount();
        waitUntil([&]() { return s->statusLine().contains(QLatin1String("could not open your browser")); }, 3000);
        t.check(s->state() == QLatin1String("waiting") && s->statusLine().contains(QLatin1String("<a href=\"https://www.last.fm/api/auth/")),
                QStringLiteral("no browser: still waiting, with the page as a link to open by hand"));
        s->cancelConnect();
        browserOpens = true;
    }

    // — an account remembered with its key gone: Reconnect —
    {
        library->setSetting(QStringLiteral("lastfm.user"), kUser);
        SecretStore::remove(QStringLiteral("lastfm.session"));
        approveAfter = 0;
        sessionCalls = 0;
        server.answer = [&](const Params &sent) {
            const QString method = param(sent, QStringLiteral("method"));
            if (method == QLatin1String("auth.getToken"))
                return tokenAnswer;
            return ++sessionCalls > approveAfter ? sessionAnswer : notYet;
        };
        Scrobbler::Timing timing;
        timing.pollMs = 30;
        auto s = make(timing);
        const QString at = s->state();
        s->connectAccount();
        waitUntil([&]() { return s->state() == QLatin1String("connected"); }, 3000);
        t.check(at == QLatin1String("expired") && s->state() == QLatin1String("connected"),
                QStringLiteral("remembered account, no key: starts expired; Reconnect connects"),
                QStringLiteral("%1 -> %2").arg(at, s->state()));
        s->disconnectAccount();
    }

    reset();
    return t.finish();
}

// ============================================================ --scrobble-send-test

int runScrobbleSendTest(Library *library, int rows, bool expectKept)
{
    Checks t("send");
    if (!scratchDataDir(t))
        return t.finish();
    // Only ever a stand-in on this computer: these are invented keys, and
    // Last.fm itself must never see them. The endpoint only follows
    // MONOLIST_LASTFM_URL for an invented account, to a loopback address.
    LastFmApi api;
    api.setTestAccount(kKey, kSecret);
    const QUrl target = api.endpoint();
    const QString host = target.host();
    if (qEnvironmentVariableIsEmpty("MONOLIST_LASTFM_URL")
        || !(host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1"))) {
        t.check(false, QStringLiteral("MONOLIST_LASTFM_URL names a server on this computer"),
                QStringLiteral("it is \"%1\"").arg(target.toString()));
        return t.finish();
    }
    t.note(QStringLiteral("sending to %1").arg(target.toString()));

    clearQueue();
    seed(kUser, rows);
    const qint64 oldest = firstStartedAt(kUser);
    t.note(QStringLiteral("%1 scrobbles queued for %2, oldest started %3").arg(rows).arg(kUser)
               .arg(QDateTime::fromSecsSinceEpoch(oldest, QTimeZone::UTC).toString(Qt::ISODate)));

    Scrobbler::Timing timing;
    timing.afterEnqueueMs = 3600 * 1000;
    auto s = connectedScrobbler(api, library, timing);

    QStringList nowPlaying;
    QObject::connect(s.get(), &Scrobbler::nowPlayingAnswered, s.get(), [&nowPlaying](const QString &outcome) { nowPlaying << outcome; });
    s->listenStarted(sigurRos(), QDateTime::currentSecsSinceEpoch(), true);
    waitUntil([&]() { return !nowPlaying.isEmpty(); }, 20000);
    t.note(QStringLiteral("now playing: %1").arg(nowPlaying.value(0, QStringLiteral("no answer"))));

    QStringList batches;
    QObject::connect(s.get(), &Scrobbler::batchAnswered, s.get(), [&batches](int items, const QString &outcome) {
        batches << QStringLiteral("%1 (%2)").arg(items).arg(outcome);
        qWarning("send: request %d: %d scrobbles, %s", int(batches.size()), items, qPrintable(outcome));
    });
    s->flush();
    waitUntil([&]() { return queued(kUser) == 0 || s->pause() != Scrobbler::Pause::None || s->state() != QLatin1String("connected"); },
              60000);
    settle(300);   // anything still going would show here

    t.note(QStringLiteral("requests answered: %1").arg(batches.join(QStringLiteral(", "))));
    t.note(QStringLiteral("left in the queue: %1; state %2; pause %3%4").arg(queued(kUser)).arg(s->state())
               .arg(Scrobbler::pauseName(s->pause()))
               .arg(s->pause() == Scrobbler::Pause::None ? QString()
                        : QStringLiteral(", next try in %1 s").arg((s->pausedUntil() - QDateTime::currentMSecsSinceEpoch()) / 1000)));
    t.note(QStringLiteral("the row says: %1").arg(s->statusLine().isEmpty() ? QStringLiteral("(nothing)") : s->statusLine()));

    if (expectKept) {
        t.check(queued(kUser) == rows && batches.size() == 1 && s->pause() == Scrobbler::Pause::Hold,
                QStringLiteral("a refused key: one request, then held, and all %1 kept").arg(rows));
    } else {
        QStringList expected;
        for (int left = rows; left > 0; left -= LastFmApi::kMaxBatch)
            expected << QStringLiteral("%1 (ok)").arg(qMin(left, LastFmApi::kMaxBatch));
        t.check(nowPlaying.value(0) == QLatin1String("ok"), QStringLiteral("now playing accepted"));
        t.check(batches == expected, QStringLiteral("sent as %1").arg(expected.join(QStringLiteral(", "))),
                batches.join(QStringLiteral(", ")));
        t.check(queued(kUser) == 0, QStringLiteral("the queue is empty"));
    }
    clearQueue();
    return t.finish();
}

// ============================================================ --scrobble-kill-test

bool startScrobbleKillTest(Library *library)
{
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        qWarning("kill: refusing to run without MONOLIST_DATA_DIR");
        return false;
    }
    clearQueue();

    // Everything here lives until the process is killed. Nothing leaves it:
    // the stand-in answers every call with silence, and the first send is an
    // hour away.
    auto *api = new LastFmApi(qApp);
    api->setTestAccount(kKey, kSecret);
    api->setTestResponder([](const Params &) { return qMakePair(0, QByteArray()); });
    auto *scrobbler = new Scrobbler(api, library, qApp);
    Scrobbler::Timing timing;
    timing.afterEnqueueMs = 3600 * 1000;
    scrobbler->setTiming(timing);
    scrobbler->useTestSession(kUser, kSession);

    auto *engine = new MpvEngine(qApp);
    if (!engine->isValid()) {
        qWarning("kill: no audio engine: %s", qPrintable(engine->lastError()));
        return false;
    }
    auto *resolver = new StreamResolver(qApp);
    auto *player = new PlaybackController(engine, resolver, nullptr, qApp);
    player->setAutoplay(false);
    scrobbler->setPlayer(player);

    auto qualified = std::make_shared<int>(0);
    QObject::connect(player, &PlaybackController::listenQualified, qApp,
                     [qualified](const QVariantMap &track, qint64 startedAt, bool chosen) {
        qWarning("kill: \"%s\" qualified: started_at %lld (%s), chosen_by_user %d",
                 qPrintable(track.value(QStringLiteral("title")).toString()), (long long)startedAt,
                 qPrintable(QDateTime::fromSecsSinceEpoch(startedAt, QTimeZone::UTC).toString(Qt::ISODate)),
                 chosen ? 1 : 0);
        ++*qualified;
    });

    FakeEngine fake{ *engine };
    player->playTracks({ song(QStringLiteral("Kill test, chosen"), 180000,
                              { { QStringLiteral("primaryArtist"), QStringLiteral("Selftest Artist") } }) },
                       0, QStringLiteral("selftest"));
    fake.newFile();
    fake.playing(true);
    fake.duration(180000);
    fake.play(90000);
    player->playTracks({ song(QStringLiteral("Kill test, autoplay"), 600000, { { QStringLiteral("fromRadio"), true } }) },
                       0, QStringLiteral("selftest"));
    fake.newFile();
    fake.duration(600000);
    fake.play(240000);

    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id, track, started_at, chosen_by_user FROM scrobble_queue ORDER BY id"));
    int rows = 0;
    while (q.next()) {
        ++rows;
        qWarning("kill: row %lld \"%s\" started_at %lld chosen_by_user %d", q.value(0).toLongLong(),
                 qPrintable(q.value(1).toString()), q.value(2).toLongLong(), q.value(3).toInt());
    }
    qWarning("kill: %d rows queued (expected 2); waiting to be killed", rows);
    return rows == 2 && *qualified == 2;
}
