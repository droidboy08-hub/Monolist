#include "innertube.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace {

// Long enough for a slow connection to finish: a search is a couple of
// hundred KB, a page can be four hundred, and a request that times out is a
// blank screen.
constexpr int kSearchTimeoutMs = 12000;
constexpr int kBrowseTimeoutMs = 20000;
constexpr int kSuggestTimeoutMs = 6000;

// Search filters as YouTube Music encodes them (a base64 protobuf): the values
// its web client sends, and ytmusicapi with it.
const QString kSongsFilter = QStringLiteral("EgWKAQIIAWoKEAkQBRAKEAMQBA%3D%3D");
const QString kVideosFilter = QStringLiteral("EgWKAQIQAWoKEAkQChAFEAMQBA%3D%3D");
const QString kArtistsFilter = QStringLiteral("EgWKAQIgAWoKEAkQBRAKEAMQBA%3D%3D");
const QString kAlbumsFilter = QStringLiteral("EgWKAQIYAWoKEAkQBRAKEAMQBA%3D%3D");
// Playlists are two sections there: YouTube Music's own ("featured") and its
// listeners' ("community"), each its own filter.
const QString kFeaturedPlaylistsFilter = QStringLiteral("EgeKAQQoADgBagwQDhAKEAMQBBAJEAU%3D");
const QString kCommunityPlaylistsFilter = QStringLiteral("EgeKAQQoAEABagwQDhAKEAMQBBAJEAU%3D");

QString filterParams(InnerTube::Filter filter)
{
    switch (filter) {
    case InnerTube::Filter::Songs:              return kSongsFilter;
    case InnerTube::Filter::Videos:             return kVideosFilter;
    case InnerTube::Filter::Albums:             return kAlbumsFilter;
    case InnerTube::Filter::Artists:            return kArtistsFilter;
    case InnerTube::Filter::FeaturedPlaylists:  return kFeaturedPlaylistsFilter;
    case InnerTube::Filter::CommunityPlaylists: return kCommunityPlaylistsFilter;
    }
    return kSongsFilter;
}

const QByteArray kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/149.0.0.0 Safari/537.36";

// What the visionOS client sends. It has to match the client named in the
// context, or the request is not that client.
const QByteArray kVisionUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/26.0 Safari/605.1.15";

// One /player call, generously: it is one request, and the tier below it
// costs seconds.
constexpr int kPlayerTimeoutMs = 8000;
// The second client is a second chance, not a second wait: asked only once
// the first has answered, it gets no retry and a shorter limit. /player
// answered in 0.5 s at the 90th percentile when this was set.
constexpr int kPlayerFallbackTimeoutMs = 3000;

// The visionOS app's versions, as /player is asked. 1.02 answered every call
// measured (96 of 96, September 2026). 0.1 answered the same twelve test
// tracks with plain URLs that played whole, where every other client that
// needs no cipher was refused (ANDROID_VR's older versions, TVHTML5_SIMPLY)
// or served the first megabyte and then 403 (ANDROID_VR 1.65.10). It covers
// one version being retired; YouTube refusing the app outright it does not.
const QString kVisionVersion = QStringLiteral("1.02");
const QString kVisionFallbackVersion = QStringLiteral("0.1");

bool isPlayerClient(InnerTube::Client client)
{
    return client == InnerTube::Client::Player || client == InnerTube::Client::PlayerFallback;
}

// How the log and the error name a /player client.
QString playerClientName(InnerTube::Client client)
{
    return QStringLiteral("VISIONOS ")
           + (client == InnerTube::Client::PlayerFallback ? kVisionFallbackVersion : kVisionVersion);
}

// Walks object keys and "#n" array indices; a step that does not exist yields
// an undefined value, so a changed layout reads as "nothing here" rather than
// crashing.
QJsonValue dig(QJsonValue value, std::initializer_list<const char *> path)
{
    for (const char *step : path) {
        if (step[0] == '#')
            value = value.toArray().at(QByteArray(step + 1).toInt());
        else
            value = value.toObject().value(QLatin1String(step));
    }
    return value;
}

QString joinRuns(const QJsonArray &runs)
{
    QString text;
    for (const QJsonValue &run : runs)
        text += run.toObject().value(QLatin1String("text")).toString();
    return text;
}

qint64 parseClock(const QString &text)
{
    qint64 seconds = 0;
    for (const QString &part : text.split(QLatin1Char(':')))
        seconds = seconds * 60 + part.toLongLong();
    return seconds * 1000;
}

// The country to ask from; empty follows the system. Set once at startup and
// whenever the user picks another in Settings.
QString g_region;
// Told when YouTube Music refuses that country.
std::function<void(const QString &)> g_regionRejected;

// The signed-in account (YtmSession), for the calls that ask for it. Set once
// at start, before any request; read only for a call that is not Anonymous.
InnerTube::AccountHook g_account;
// Where every request goes in a self-test; empty is YouTube.
QString g_testServer;
// Told of every artist name an answer links (ArtistLinks). Set once at start.
std::function<void(const QString &, const QString &, bool)> g_artistSeen;

// — the visitor id, one for the whole process —
//
// /player refuses most music (LOGIN_REQUIRED) unless the request names a
// visitor: the anonymous id YouTube hands every browser on its first visit.
// Each InnerTube object used to fetch its own at start-up, six copies of the
// 885 KB home page, and a track played the moment the window was up waited a
// median 434.5 ms for its object's copy. Now one id serves every object. It
// is kept in settings with the time it was fetched, so the next launch has it
// at once, and it comes from www.youtube.com/sw.js_data, under 3 KB, with the
// home page as the fallback.
//
// A stored id is something YouTube can follow from launch to launch, as it
// can the cookie it stands for. So it is never used past 30 days, one over a
// day old is replaced once something has played, and Clear history forgets
// it. It is not a secret — it opens nothing — which is why settings, rather
// than the secret store, is where it lives. The account's own id
// (setSessionVisitorData) goes only on calls that carry the account.
//
// The setting "youtube.visitor" switches it back: "launch" keeps the id for
// the launch only, as before, and "home" also takes it from the home page.
const QString kVisitorKey = QStringLiteral("youtube.visitor_data");
const QString kVisitorAtKey = QStringLiteral("youtube.visitor_data_at");
const QString kVisitorModeKey = QStringLiteral("youtube.visitor");
constexpr qint64 kVisitorMaxAgeSecs = 30 * 24 * 3600;
constexpr qint64 kVisitorRefreshAgeSecs = 24 * 3600;
// After the first play's /player answer: out of the way of its download.
constexpr int kVisitorRefreshDelayMs = 10000;

// One jar for every InnerTube object. YouTube's anonymous cookies
// (VISITOR_INFO1_LIVE, YSC) come on whichever answer sets them, and still
// reach browse, search and next, as they did when each object fetched its
// own id into its own jar. The account's cookies never enter it (post()).
class SharedJar : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::QNetworkCookieJar;
    void clear() { setAllCookies({}); }
};

struct Visitor {
    enum class Mode { Stored, Launch, HomePage };
    InnerTube::VisitorStore store;
    Mode mode = Mode::Stored;
    bool loaded = false;
    QString anonymous;         // what every anonymous /player names
    QDateTime fetchedAt;       // when YouTube gave it, UTC
    QString fromSettings;      // the id this launch found stored, for LOGIN_REQUIRED
    QString session;           // the signed-in account's own
    bool refreshDue = false;   // stored and over a day old: replaced after the first play
    bool pending = false;
    quint64 generation = 0;    // a new store drops what an older fetch brings back
    std::vector<std::function<void()>> waiters;
    QPointer<QNetworkAccessManager> network;
    QPointer<SharedJar> jar;
};
Visitor g_visitor;

// youtube.player_client, read through the same store once a launch
// (InnerTube::PlayerClients); a new store reads it afresh.
const QString kPlayerClientKey = QStringLiteral("youtube.player_client");
struct PlayerClientsSwitch {
    bool loaded = false;
    InnerTube::PlayerClients clients = InnerTube::PlayerClients::Both;
};
PlayerClientsSwitch g_playerClients;

// youtube.format, read the same way: "bitrate" takes the audio format with
// the highest bitrate, whatever its codec, as before the order in
// pickStream existed.
const QString kFormatRuleKey = QStringLiteral("youtube.format");
struct FormatRuleSwitch {
    bool loaded = false;
    bool byBitrate = false;
};
FormatRuleSwitch g_formatRule;

bool chooseByBitrate()
{
    FormatRuleSwitch &s = g_formatRule;
    if (s.loaded)
        return s.byBitrate;
    s.loaded = true;
    s.byBitrate = g_visitor.store.read && g_visitor.store.read(kFormatRuleKey) == QLatin1String("bitrate");
    if (s.byBitrate)
        qInfo("innertube: youtube.format=bitrate: /player's audio format is chosen by bitrate alone, as before");
    return s.byBitrate;
}

SharedJar *sharedJar()
{
    if (!g_visitor.jar)
        g_visitor.jar = new SharedJar(QCoreApplication::instance());
    return g_visitor.jar;
}

// Hands `manager` the shared jar. setCookieJar makes the manager its parent,
// and so the last object made would take the jar with it when it went; it
// belongs to the application instead. A jar is not thread-safe, so a manager
// on another thread would keep its own; every InnerTube lives on this one.
void shareJar(QNetworkAccessManager *manager)
{
    QCoreApplication *app = QCoreApplication::instance();
    if (!app || manager->thread() != app->thread())
        return;
    SharedJar *jar = sharedJar();
    manager->setCookieJar(jar);
    jar->setParent(app);
}

// The fetch's own manager, the application's, so a fetch outlives whichever
// object happened to ask for it.
QNetworkAccessManager *visitorNetwork()
{
    if (!g_visitor.network) {
        g_visitor.network = new QNetworkAccessManager(QCoreApplication::instance());
        shareJar(g_visitor.network);
    }
    return g_visitor.network;
}

void writeVisitor(const QString &id, const QDateTime &at)
{
    if (!g_visitor.store.write)
        return;
    g_visitor.store.write(kVisitorKey, id);
    g_visitor.store.write(kVisitorAtKey, at.isValid() ? at.toString(Qt::ISODate) : QString());
}

// What settings hold, read once a launch. Never the id itself in the log.
void loadVisitor()
{
    Visitor &v = g_visitor;
    if (v.loaded)
        return;
    v.loaded = true;
    if (!v.store.read)
        return;
    const QString mode = v.store.read(kVisitorModeKey);
    v.mode = mode == QLatin1String("launch") ? Visitor::Mode::Launch
           : mode == QLatin1String("home")   ? Visitor::Mode::HomePage
                                              : Visitor::Mode::Stored;
    const QString id = v.store.read(kVisitorKey);
    if (id.isEmpty())
        return;
    if (v.mode != Visitor::Mode::Stored) {
        writeVisitor({}, {});
        qInfo("innertube: youtube.visitor=%s keeps visitor ids for one launch; the stored one is dropped",
              qPrintable(mode));
        return;
    }
    const QDateTime at = QDateTime::fromString(v.store.read(kVisitorAtKey), Qt::ISODate);
    const qint64 age = at.isValid() ? at.secsTo(QDateTime::currentDateTimeUtc()) : 0;
    // A date well ahead of the clock means the clock went back: its age
    // cannot be told, so it is treated as too old.
    if (!at.isValid() || age < -3600 || age > kVisitorMaxAgeSecs) {
        writeVisitor({}, {});
        qInfo("innertube: the stored visitor id is %s; a new one is fetched",
              age > kVisitorMaxAgeSecs ? "over 30 days old" : "of no known age");
        return;
    }
    v.anonymous = id;
    v.fetchedAt = at;
    v.fromSettings = id;
    v.refreshDue = age > kVisitorRefreshAgeSecs;
    qInfo("innertube: using the stored visitor id, fetched %lld h ago%s", (long long)(qMax<qint64>(age, 0) / 3600),
          v.refreshDue ? "; a new one is fetched after the first play" : "");
}

// An id is not used past the limit, even by a launch that has run for days.
void expireVisitor()
{
    Visitor &v = g_visitor;
    if (v.anonymous.isEmpty() || !v.fetchedAt.isValid()
        || v.fetchedAt.secsTo(QDateTime::currentDateTimeUtc()) <= kVisitorMaxAgeSecs)
        return;
    v.anonymous.clear();
    v.fetchedAt = QDateTime();
    if (v.mode == Visitor::Mode::Stored)
        writeVisitor({}, {});
    qInfo("innertube: the visitor id is 30 days old; a new one is fetched");
}

bool looksLikeVisitorId(const QString &id)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9_%=\\-]{16,4096}$"));
    return shape.match(id).hasMatch();
}

// sw.js_data is the service worker's start-up data: ")]}'" and an array
// without keys, which held the id at [0][2][0][0][13] and again at [0][2][6]
// when this was written (September 2026). Taken only if it looks like one;
// anything else sends the fetch on to the home page.
QString visitorFromSwJsData(const QByteArray &body)
{
    const qsizetype start = body.indexOf('[');
    if (start < 0)
        return {};
    const QJsonValue root = QJsonDocument::fromJson(body.mid(start)).array();
    for (const QJsonValue &candidate : { dig(root, { "#0", "#2", "#0", "#0", "#13" }),
                                         dig(root, { "#0", "#2", "#6" }) }) {
        const QString id = candidate.toString();
        if (looksLikeVisitorId(id))
            return id;
    }
    return {};
}

QString visitorFromHomePage(const QByteArray &body)
{
    // A custom delimiter: the pattern itself contains `)"`, which would
    // close an ordinary R"( … )" early.
    static const QRegularExpression visitor(QStringLiteral(R"RX("visitorData":"(.*?)")RX"));
    const QRegularExpressionMatch match = visitor.match(QString::fromUtf8(body));
    if (!match.hasMatch())
        return {};
    // The page carries it JSON-escaped ("\x3d" and friends), so it is
    // unescaped by parsing it as the JSON string it is.
    const QJsonDocument quoted = QJsonDocument::fromJson("[\"" + match.captured(1).toUtf8() + "\"]");
    return quoted.array().at(0).toString();
}

void settleVisitor(const QString &id, const char *source, const QString &why, qint64 ms, qint64 bytes)
{
    Visitor &v = g_visitor;
    v.pending = false;
    if (!id.isEmpty()) {
        v.anonymous = id;
        v.fetchedAt = QDateTime::currentDateTimeUtc();
        v.refreshDue = false;
        const bool store = v.mode == Visitor::Mode::Stored && v.store.write;
        if (store)
            writeVisitor(v.anonymous, v.fetchedAt);
        qInfo("innertube: a new visitor id from %s, in %lld ms (%lld bytes)%s", source, (long long)ms,
              (long long)bytes, store ? "; stored" : "");
    } else {
        // In the EU most likely the consent page. An id already held, stored
        // or from earlier, is still better than none.
        qWarning("innertube: YouTube gave no visitor id (%s); %s", qPrintable(why),
                 v.anonymous.isEmpty() ? "/player goes without one" : "keeping the one held");
    }
    // Whether or not it worked: anyone waiting should stop waiting. An empty
    // id still gets a request out, and it may still be answered.
    const auto waiters = std::exchange(v.waiters, {});
    for (const auto &waiter : waiters)
        waiter();
}

void fetchVisitorFrom(bool homePage)
{
    const QString base = g_testServer.isEmpty() ? QStringLiteral("https://www.youtube.com") : g_testServer;
    QNetworkRequest request(QUrl(base + (homePage ? QStringLiteral("/") : QStringLiteral("/sw.js_data"))));
    request.setHeader(QNetworkRequest::UserAgentHeader, kVisionUserAgent);
    request.setTransferTimeout(kPlayerTimeoutMs);
    // Asked as a first visit. With the jar's cookies YouTube hands back the
    // id they name, and a renewed id would be the old one with a new date,
    // which would make the 30-day limit a fiction. What the answer sets goes
    // into the shared jar as usual.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    const quint64 generation = g_visitor.generation;
    QElapsedTimer clock;
    clock.start();
    QNetworkAccessManager *network = visitorNetwork();
    QNetworkReply *reply = network->get(request);
    QObject::connect(reply, &QNetworkReply::finished, network, [reply, homePage, generation, clock]() {
        reply->deleteLater();
        if (g_visitor.generation != generation)
            return;   // started afresh meanwhile (a new store)
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = ok ? reply->readAll() : QByteArray();
        const QString id = homePage ? visitorFromHomePage(body) : visitorFromSwJsData(body);
        const QString why = !ok ? reply->errorString()
                          : reply->url().host().startsWith(QLatin1String("consent."))
                              ? QStringLiteral("redirected to the consent page")
                              : QStringLiteral("none in the answer");
        if (id.isEmpty() && !homePage) {
            qInfo("innertube: sw.js_data gave no visitor id (%s); asking the home page", qPrintable(why));
            fetchVisitorFrom(/*homePage=*/true);
            return;
        }
        settleVisitor(id, homePage ? "the home page" : "sw.js_data", why, clock.elapsed(), body.size());
    });
}

// Starts a fetch unless one is on its way; the waiters hear when it answers,
// with an id or without.
void fetchVisitor()
{
    Visitor &v = g_visitor;
    if (v.pending)
        return;
    v.pending = true;
    fetchVisitorFrom(v.mode == Visitor::Mode::HomePage);
}

// For a refused call: `then` runs once there is an id other than `used`,
// or a fetch has come back without one. A fetch on its way is joined.
void renewVisitor(const QString &used, std::function<void()> then)
{
    Visitor &v = g_visitor;
    if (!v.anonymous.isEmpty() && v.anonymous != used)
        return then();
    v.waiters.push_back(std::move(then));
    fetchVisitor();
}

// Once the first play has its answer: a stored id over a day old is
// replaced in the background, so the next launch starts with a fresh one.
void renewVisitorSoon()
{
    g_visitor.refreshDue = false;
    QTimer::singleShot(g_testServer.isEmpty() ? kVisitorRefreshDelayMs : 0, visitorNetwork(), []() {
        fetchVisitor();
    });
}

// The web client's own locale, so results follow the user's language and the
// region they are browsing. YouTube Music and YouTube name themselves
// differently and number their versions differently.
QJsonObject clientContext(InnerTube::Client client)
{
    const QLocale locale = QLocale::system();
    QString language = locale.name().section(QLatin1Char('_'), 0, 0);
    const QString region = InnerTube::region();
    if (language.isEmpty() || language == QLatin1String("C"))
        language = QStringLiteral("en");
    const QString today = QDate::currentDate().toString(QStringLiteral("yyyyMMdd"));

    QJsonObject client_{
        { QStringLiteral("hl"), language },
        { QStringLiteral("gl"), region }
    };
    switch (client) {
    case InnerTube::Client::Music:
        client_.insert(QStringLiteral("clientName"), QStringLiteral("WEB_REMIX"));
        client_.insert(QStringLiteral("clientVersion"), QStringLiteral("1.%1.01.00").arg(today));
        break;
    case InnerTube::Client::YouTube:
        client_.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
        client_.insert(QStringLiteral("clientVersion"), QStringLiteral("2.%1.00.00").arg(today));
        break;
    case InnerTube::Client::Player:
    case InnerTube::Client::PlayerFallback:
        // The visionOS app, described exactly as it describes itself. The
        // whole reason this client is here is that /player answers it with
        // plain stream URLs — no signature to undo, no throttling parameter
        // to descramble in a JavaScript engine, no PO token. Change any of
        // these strings and it stops being that client. The fallback is the
        // same app with only the version changed.
        client_.insert(QStringLiteral("clientName"), QStringLiteral("VISIONOS"));
        client_.insert(QStringLiteral("clientVersion"), client == InnerTube::Client::PlayerFallback
                                                            ? kVisionFallbackVersion : kVisionVersion);
        client_.insert(QStringLiteral("deviceMake"), QStringLiteral("Apple"));
        client_.insert(QStringLiteral("deviceModel"), QStringLiteral("RealityDevice17,1"));
        client_.insert(QStringLiteral("osName"), QStringLiteral("visionOS"));
        client_.insert(QStringLiteral("osVersion"), QStringLiteral("26.5.23O471"));
        client_.insert(QStringLiteral("userAgent"), QString::fromLatin1(kVisionUserAgent));
        break;
    }
    return QJsonObject{ { QStringLiteral("client"), client_ } };
}

// Cover art comes from googleusercontent.com with its size in the URL
// ("=w60-h60-l90-rj"). Ask for one worth showing; video thumbnails from
// i.ytimg.com are left as they are.
QString largerArtwork(const QString &url)
{
    static const QRegularExpression size(QStringLiteral(R"(=w\d+-h\d+)"));
    QString result = url;
    if (result.contains(QLatin1String("googleusercontent.com")))
        result.replace(size, QStringLiteral("=w544-h544"));
    return result;
}

// An artist's header picture as a square portrait. The header asks for a wide
// strip (=w2880-h1200-p…), and for an artist whose picture is a square
// avatar YouTube pads it out to that width (the "-dc…" option), so cropping
// the strip at the client would cut a logo in half. The image server makes
// the square itself from the same picture: the whole avatar, or the centre
// of a wide photograph ("-p" crops to fill), as YouTube Music's own artist
// cards show it.
QString squarePortrait(const QString &url)
{
    const qsizetype options = url.lastIndexOf(QLatin1Char('='));
    const bool imageServer = url.contains(QLatin1String("googleusercontent.com"))
                             || url.contains(QLatin1String("ggpht.com"));
    if (!imageServer || options < 0 || url.indexOf(QLatin1Char('/'), options) >= 0)
        return url;
    return url.left(options) + QStringLiteral("=w544-h544-p-l90-rj");
}

bool isTypeLabel(const QString &text)
{
    static const QStringList labels = {
        QStringLiteral("Song"), QStringLiteral("Video"), QStringLiteral("Episode"),
        QStringLiteral("Podcast"), QStringLiteral("Album"), QStringLiteral("Single"),
        QStringLiteral("EP"), QStringLiteral("Artist"), QStringLiteral("Playlist")
    };
    return labels.contains(text);
}

// The first run that links to an artist's or a channel's page: in
// "Bruno Mars & Lady Gaga", "Bruno Mars". Empty when none does.
QString firstArtistRun(const QJsonArray &runs)
{
    for (const QJsonValue &run : runs) {
        const QString pageType = dig(run, { "navigationEndpoint", "browseEndpoint",
                                            "browseEndpointContextSupportedConfigs",
                                            "browseEndpointContextMusicConfig", "pageType" }).toString();
        if (pageType == QLatin1String("MUSIC_PAGE_TYPE_ARTIST")
            || pageType == QLatin1String("MUSIC_PAGE_TYPE_USER_CHANNEL")) {
            const QString text = run.toObject().value(QLatin1String("text")).toString().trimmed();
            if (!text.isEmpty())
                return text;
        }
    }
    return {};
}

// The page a run links to when it is an artist's or a channel's, and which
// of the two: an artist's page is the one with albums and top songs, a
// channel's is a plain uploader's. Empty for any other run.
QString artistLinkOf(const QJsonValue &run, bool *artistPage = nullptr)
{
    const QJsonValue browse = dig(run, { "navigationEndpoint", "browseEndpoint" });
    const QString pageType = dig(browse, { "browseEndpointContextSupportedConfigs",
                                           "browseEndpointContextMusicConfig", "pageType" }).toString();
    const bool artist = pageType == QLatin1String("MUSIC_PAGE_TYPE_ARTIST");
    if (!artist && pageType != QLatin1String("MUSIC_PAGE_TYPE_USER_CHANNEL"))
        return {};
    if (artistPage)
        *artistPage = artist;
    return dig(browse, { "browseId" }).toString();
}

// Tells ArtistLinks, if anyone is listening, that this name opens this page.
void noteArtist(const QString &name, const QString &browseId, bool artistPage)
{
    const QString trimmed = name.trimmed();
    if (g_artistSeen && !trimmed.isEmpty() && !browseId.isEmpty())
        g_artistSeen(trimmed, browseId, artistPage);
}

// What YouTube Music puts between two names in one credit, and nothing
// else: an unlinked run that is not one of these is a name without a page.
bool isJoiner(const QString &text)
{
    static const QRegularExpression joiner(
        QStringLiteral(R"(^\s*(,|&|and|x|×|vs\.?|feat\.?|ft\.?|featuring|with|/|\+|、|・)?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    return joiner.match(text).hasMatch();
}

// One credit's runs, piece by piece: each linked name with its page, and
// the joiners between them as written. The ends are trimmed as the joined
// text is, so the pieces always add up to it.
void appendCredits(const QJsonArray &runs, QList<InnerTube::Credit> &credits)
{
    QList<InnerTube::Credit> pieces;
    for (const QJsonValue &run : runs) {
        InnerTube::Credit credit;
        credit.text = run.toObject().value(QLatin1String("text")).toString();
        bool artistPage = false;
        credit.browseId = artistLinkOf(run, &artistPage);
        if (!credit.browseId.isEmpty())
            noteArtist(credit.text, credit.browseId, artistPage);
        credit.name = !credit.browseId.isEmpty() || !isJoiner(credit.text);
        pieces.append(credit);
    }
    while (!pieces.isEmpty()) {
        QString &first = pieces.first().text;
        while (!first.isEmpty() && first.front().isSpace())
            first.remove(0, 1);
        if (!first.isEmpty())
            break;
        pieces.removeFirst();
    }
    while (!pieces.isEmpty()) {
        QString &last = pieces.last().text;
        while (!last.isEmpty() && last.back().isSpace())
            last.chop(1);
        if (!last.isEmpty())
            break;
        pieces.removeLast();
    }
    if (pieces.isEmpty())
        return;
    if (!credits.isEmpty())
        credits.append(InnerTube::Credit{ QStringLiteral(", "), QString(), false });
    credits += pieces;
}

// "Artist & Artist • Album • 2:05" for a song, "Channel • 1.2M views • 3:31"
// for a video, sometimes led by a "Song" or "Video" label. Split on the bullets
// and tell the parts apart by what they link to rather than by position.
void parseSubtitle(const QJsonArray &runs, InnerTube::Track &track)
{
    QList<QJsonArray> groups{ QJsonArray() };
    for (const QJsonValue &run : runs) {
        if (run.toObject().value(QLatin1String("text")).toString() == QStringLiteral(" • "))
            groups.append(QJsonArray());
        else
            groups.last().append(run);
    }

    static const QRegularExpression clock(QStringLiteral(R"(^(\d+:)?\d{1,2}:\d{2}$)"));
    static const QRegularExpression count(QStringLiteral(R"(\b(views|plays|listeners)$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    QStringList artists;
    for (const QJsonArray &group : std::as_const(groups)) {
        const QString text = joinRuns(group).trimmed();
        if (text.isEmpty() || isTypeLabel(text) || count.match(text).hasMatch())
            continue;
        if (clock.match(text).hasMatch()) {
            track.durationMs = parseClock(text);
            continue;
        }
        QString pageType;
        QString browseId;
        for (const QJsonValue &run : group) {
            pageType = dig(run, { "navigationEndpoint", "browseEndpoint",
                                  "browseEndpointContextSupportedConfigs",
                                  "browseEndpointContextMusicConfig", "pageType" }).toString();
            if (!pageType.isEmpty()) {
                browseId = dig(run, { "navigationEndpoint", "browseEndpoint", "browseId" }).toString();
                break;
            }
        }
        if (pageType == QLatin1String("MUSIC_PAGE_TYPE_ALBUM")) {
            track.album = text;
            track.albumId = browseId;
        } else if (pageType.isEmpty() && !artists.isEmpty()) {
            continue;   // an unlinked line after the artist: a date, a count
        } else {
            artists << text;   // an artist or channel page, or an artist without one
            appendCredits(group, track.credits);
            // The first credit, from its own run rather than by splitting the
            // joined line; an artist with no page is credited as written.
            if (track.primaryArtist.isEmpty()) {
                const QString linked = firstArtistRun(group);
                track.primaryArtist = linked.isEmpty() ? text : linked;
            }
        }
    }
    track.artist = artists.join(QStringLiteral(", "));
    // Nothing in the line linked anywhere: the pieces would only repeat
    // `artist`, and an empty list says "look the name up" just as well.
    const bool linked = std::any_of(track.credits.cbegin(), track.credits.cend(),
                                    [](const InnerTube::Credit &credit) { return !credit.browseId.isEmpty(); });
    if (!linked)
        track.credits.clear();
}

const QRegularExpression &clockPattern()
{
    static const QRegularExpression clock(QStringLiteral(R"(^(\d+:)?\d{1,2}:\d{2}$)"));
    return clock;
}

// A song row (musicResponsiveListItemRenderer), wherever it appears. Search
// results pack "Artist • Album • 2:05" into the second column; album and
// playlist pages give artist and album a column each and the duration a
// fixed column. The columns are joined with bullets so one parser reads both.
// "MUSIC_VIDEO_TYPE_ATV" is YouTube Music's own audio track: a still picture
// of the cover. Anything else (OMV, UGC) is a video worth showing.
bool isRealVideo(const QJsonValue &watchEndpoint)
{
    const QString kind = dig(watchEndpoint, { "watchEndpointMusicSupportedConfigs",
                                              "watchEndpointMusicConfig", "musicVideoType" }).toString();
    return !kind.isEmpty() && kind != QLatin1String("MUSIC_VIDEO_TYPE_ATV");
}

InnerTube::Track parseListItem(const QJsonValue &item)
{
    InnerTube::Track track;
    track.videoId = dig(item, { "playlistItemData", "videoId" }).toString();
    track.setVideoId = dig(item, { "playlistItemData", "playlistSetVideoId" }).toString();
    const QJsonValue play = dig(item, { "overlay", "musicItemThumbnailOverlayRenderer", "content",
                                        "musicPlayButtonRenderer", "playNavigationEndpoint",
                                        "watchEndpoint" });
    if (track.videoId.isEmpty())
        track.videoId = dig(play, { "videoId" }).toString();
    track.isVideo = isRealVideo(play);

    const QJsonArray columns = item.toObject().value(QLatin1String("flexColumns")).toArray();
    track.title = joinRuns(dig(columns.at(0), { "musicResponsiveListItemFlexColumnRenderer",
                                                "text", "runs" }).toArray()).trimmed();
    QJsonArray runs;
    for (int column = 1; column < columns.size(); ++column) {
        const QJsonArray columnRuns = dig(columns.at(column), { "musicResponsiveListItemFlexColumnRenderer",
                                                                "text", "runs" }).toArray();
        if (columnRuns.isEmpty())
            continue;
        if (!runs.isEmpty())
            runs.append(QJsonObject{ { QStringLiteral("text"), QStringLiteral(" • ") } });
        for (const QJsonValue &run : columnRuns)
            runs.append(run);
    }
    parseSubtitle(runs, track);

    for (const QJsonValue &fixed : item.toObject().value(QLatin1String("fixedColumns")).toArray()) {
        const QString text = joinRuns(dig(fixed, { "musicResponsiveListItemFixedColumnRenderer",
                                                   "text", "runs" }).toArray()).trimmed();
        if (clockPattern().match(text).hasMatch())
            track.durationMs = parseClock(text);
    }

    const QJsonArray thumbnails = dig(item, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                              "thumbnails" }).toArray();
    if (!thumbnails.isEmpty())
        track.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
    return track;
}

QString pageTypeOf(const QJsonValue &endpoint)
{
    const QString pageType = dig(endpoint, { "browseEndpoint", "browseEndpointContextSupportedConfigs",
                                             "browseEndpointContextMusicConfig", "pageType" }).toString();
    if (pageType == QLatin1String("MUSIC_PAGE_TYPE_ALBUM"))
        return QStringLiteral("album");
    if (pageType == QLatin1String("MUSIC_PAGE_TYPE_PLAYLIST"))
        return QStringLiteral("playlist");
    if (pageType == QLatin1String("MUSIC_PAGE_TYPE_ARTIST") || pageType == QLatin1String("MUSIC_PAGE_TYPE_USER_CHANNEL"))
        return QStringLiteral("artist");
    return {};
}

// A card (musicTwoRowItemRenderer): what it opens decides its type.
InnerTube::Card parseCard(const QJsonValue &item)
{
    InnerTube::Card card;
    card.title = joinRuns(dig(item, { "title", "runs" }).toArray()).trimmed();
    card.subtitle = joinRuns(dig(item, { "subtitle", "runs" }).toArray()).trimmed();
    const QJsonArray thumbnails = dig(item, { "thumbnailRenderer", "musicThumbnailRenderer", "thumbnail",
                                              "thumbnails" }).toArray();
    if (!thumbnails.isEmpty())
        card.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());

    const QJsonValue endpoint = dig(item, { "navigationEndpoint" });
    const QJsonValue watch = dig(endpoint, { "watchEndpoint" });
    if (!watch.isUndefined()) {
        card.videoId = dig(watch, { "videoId" }).toString();
        // "ATV" is YouTube Music's audio track, as opposed to a music video.
        const QString kind = dig(watch, { "watchEndpointMusicSupportedConfigs",
                                          "watchEndpointMusicConfig", "musicVideoType" }).toString();
        card.type = kind == QLatin1String("MUSIC_VIDEO_TYPE_ATV") ? QStringLiteral("song") : QStringLiteral("video");
        // What plays is credited to an artist, not to the whole line under
        // the card, which also carries a type label or a view count.
        InnerTube::Track credits;
        parseSubtitle(dig(item, { "subtitle", "runs" }).toArray(), credits);
        card.artist = credits.artist;
        card.primaryArtist = credits.primaryArtist;
    } else {
        card.browseId = dig(endpoint, { "browseEndpoint", "browseId" }).toString();
        card.type = pageTypeOf(endpoint);
        // An artist's card is its name and its page, which is worth keeping
        // for when the name turns up again without a link.
        if (card.type == QLatin1String("artist")) {
            const bool artistPage = dig(endpoint, { "browseEndpoint", "browseEndpointContextSupportedConfigs",
                                                    "browseEndpointContextMusicConfig", "pageType" }).toString()
                                    == QLatin1String("MUSIC_PAGE_TYPE_ARTIST");
            noteArtist(card.title, card.browseId, artistPage);
        }
    }
    return card;
}

// A row that opens a page rather than playing (a charted artist, an album in
// a list), as a card; `track` is the row already read as a song.
InnerTube::Card cardFromRow(const QJsonValue &row, const InnerTube::Track &track)
{
    InnerTube::Card card;
    card.title = track.title;
    card.subtitle = track.artist;
    card.artwork = track.artwork;
    card.browseId = dig(row, { "navigationEndpoint", "browseEndpoint", "browseId" }).toString();
    card.type = pageTypeOf(dig(row, { "navigationEndpoint" }));
    bool artistPage = false;
    if (!artistLinkOf(row, &artistPage).isEmpty())
        noteArtist(card.title, card.browseId, artistPage);
    return card;
}

// Where a browse endpoint goes.
InnerTube::Link linkOf(const QJsonValue &browseEndpoint)
{
    InnerTube::Link link;
    link.browseId = dig(browseEndpoint, { "browseId" }).toString();
    link.params = dig(browseEndpoint, { "params" }).toString();
    link.pageType = dig(browseEndpoint, { "browseEndpointContextSupportedConfigs",
                                          "browseEndpointContextMusicConfig", "pageType" }).toString();
    return link;
}

// The token for more of a list, in either of the two ways YouTube writes it:
// a last item that is only a continuation (continuationItemRenderer, the
// current way), or the list's own "continuations" (the older way, still
// used by search and some grids).
QString continuationOf(const QJsonValue &renderer, const QJsonArray &items)
{
    const QJsonValue last = items.isEmpty() ? QJsonValue() : items.at(items.size() - 1);
    const QString token = dig(last, { "continuationItemRenderer", "continuationEndpoint",
                                      "continuationCommand", "token" }).toString();
    if (!token.isEmpty())
        return token;
    return dig(renderer, { "continuations", "#0", "nextContinuationData", "continuation" }).toString();
}

// One item of a list, whatever it is: a song, a card, or a row that opens a
// page. Anything the app can neither play nor open (a podcast, a
// continuation marker) is dropped.
void readListItem(const QJsonValue &entry, QList<InnerTube::Track> &songs, QList<InnerTube::Card> &cards)
{
    const QJsonValue twoRow = dig(entry, { "musicTwoRowItemRenderer" });
    if (!twoRow.isUndefined()) {
        const InnerTube::Card card = parseCard(twoRow);
        if (!card.title.isEmpty() && !card.type.isEmpty() && (!card.browseId.isEmpty() || !card.videoId.isEmpty()))
            cards.append(card);
        return;
    }
    const QJsonValue row = dig(entry, { "musicResponsiveListItemRenderer" });
    if (row.isUndefined())
        return;
    const InnerTube::Track track = parseListItem(row);
    if (!track.videoId.isEmpty()) {
        if (!track.title.isEmpty())
            songs.append(track);
        return;
    }
    const InnerTube::Card card = cardFromRow(row, track);
    if (!card.browseId.isEmpty() && !card.title.isEmpty())
        cards.append(card);
}

// A row of cards (musicCarouselShelfRenderer): its titles, its "more"
// button, and its cards or songs, whichever it holds.
InnerTube::Shelf parseCarousel(const QJsonValue &carousel)
{
    InnerTube::Shelf shelf;
    const QJsonValue header = dig(carousel, { "header", "musicCarouselShelfBasicHeaderRenderer" });
    shelf.title = joinRuns(dig(header, { "title", "runs" }).toArray()).trimmed();
    shelf.strapline = joinRuns(dig(header, { "strapline", "runs" }).toArray()).trimmed();
    // The button at the header's end, or else the title itself when that is
    // what links (some shelves do one, some the other).
    shelf.more = linkOf(dig(header, { "moreContentButton", "buttonRenderer", "navigationEndpoint",
                                      "browseEndpoint" }));
    if (shelf.more.browseId.isEmpty())
        shelf.more = linkOf(dig(header, { "title", "runs", "#0", "navigationEndpoint", "browseEndpoint" }));

    for (const QJsonValue &entry : carousel.toObject().value(QLatin1String("contents")).toArray()) {
        const QJsonValue twoRow = dig(entry, { "musicTwoRowItemRenderer" });
        if (!twoRow.isUndefined()) {
            const InnerTube::Card card = parseCard(twoRow);
            if (!card.title.isEmpty() && (!card.browseId.isEmpty() || !card.videoId.isEmpty()))
                shelf.cards.append(card);
            continue;
        }
        const QJsonValue row = dig(entry, { "musicResponsiveListItemRenderer" });
        if (row.isUndefined())
            continue;
        const InnerTube::Track track = parseListItem(row);
        if (!track.videoId.isEmpty()) {
            shelf.songs.append(track);
        } else {
            // A row that opens a page rather than playing: a charted artist.
            const InnerTube::Card card = cardFromRow(row, track);
            if (!card.browseId.isEmpty())
                shelf.cards.append(card);
        }
    }
    return shelf;
}

// Only what a view can open or play: albums, playlists, artists, songs and
// videos. A podcast's card opens a page this app does not have, so it is
// left out rather than dead.
void dropUnopenable(InnerTube::Shelf &shelf)
{
    shelf.cards.erase(std::remove_if(shelf.cards.begin(), shelf.cards.end(),
                                     [](const InnerTube::Card &card) { return card.type.isEmpty(); }),
                      shelf.cards.end());
}

// A grid (gridRenderer) as a shelf: its cards, under its own heading when it
// has one.
InnerTube::Shelf parseGrid(const QJsonValue &grid)
{
    InnerTube::Shelf shelf;
    shelf.title = joinRuns(dig(grid, { "header", "gridHeaderRenderer", "title", "runs" }).toArray()).trimmed();
    for (const QJsonValue &entry : grid.toObject().value(QLatin1String("items")).toArray())
        readListItem(entry, shelf.songs, shelf.cards);
    return shelf;
}

// What a play button's endpoint plays: a song in a watch playlist, or the
// playlist from its start.
InnerTube::Watch watchOf(const QJsonValue &endpoint)
{
    InnerTube::Watch watch;
    QJsonValue target = dig(endpoint, { "watchEndpoint" });
    if (target.isUndefined())
        target = dig(endpoint, { "watchPlaylistEndpoint" });
    watch.videoId = dig(target, { "videoId" }).toString();
    watch.playlistId = dig(target, { "playlistId" }).toString();
    watch.params = dig(target, { "params" }).toString();
    return watch;
}

// One stream a /player answer offers, as the choice below sees it.
struct Offer {
    QString url;
    int itag = 0;
    QString codec;          // from the mimeType, never assumed from the itag
    int peakBitrate = 0;    // `bitrate`, the peak, which the old rule compared
    int bitrate = 0;        // averageBitrate where given, else the peak
    int sampleRate = 0;     // Hz, 0 when not given
    bool drc = false;       // a dynamic-range-compressed copy
    bool ownSound = true;   // the video's own sound, not a dubbed track
    bool muxed = false;     // the sound with a picture (itag 18)
};

// The sound's codec, as a mimeType names it: `audio/webm; codecs="opus"`,
// `audio/mp4; codecs="mp4a.40.2"`, `video/mp4; codecs="avc1.42001E,
// mp4a.40.2"`. A muxed format names its picture's codec too, which is not
// what is heard, so that one is passed over.
QString codecOf(const QString &mimeType)
{
    const qsizetype at = mimeType.indexOf(QLatin1String("codecs="));
    if (at < 0)
        return QStringLiteral("unknown");
    QString list = mimeType.mid(at + 7);
    list.remove(QLatin1Char('"'));
    static const char *const pictures[] = { "avc1", "avc3", "vp8", "vp9", "vp09", "av01", "hev1", "hvc1" };
    for (QString codec : list.split(QLatin1Char(','))) {
        codec = codec.trimmed().toLower();
        if (codec == QLatin1String("mp4a.40.5") || codec == QLatin1String("mp4a.40.29"))
            return QStringLiteral("he-aac");
        if (codec.startsWith(QLatin1String("mp4a")))
            return QStringLiteral("aac");
        const bool picture = std::any_of(std::begin(pictures), std::end(pictures), [&codec](const char *name) {
            return codec.startsWith(QLatin1String(name));
        });
        if (!picture && !codec.isEmpty())
            return codec;
    }
    return QStringLiteral("unknown");
}

// YouTube offers some formats a second time, dynamic-range compressed, under
// the same itag. Compression that has already been applied cannot be undone,
// and any loudness levelling would be stacked on top of it. Neither of the
// fields that mark such a copy is documented, so both are read: isDrc, and
// "drc" among the format's xtags (a protobuf, base64url-encoded).
bool isDrc(const QJsonObject &format)
{
    if (format.value(QStringLiteral("isDrc")).toBool())
        return true;
    const QByteArray xtags = format.value(QStringLiteral("xtags")).toString().toLatin1();
    return !xtags.isEmpty() && QByteArray::fromBase64(xtags, QByteArray::Base64UrlEncoding).contains("drc");
}

Offer offerOf(const QJsonObject &format, bool muxed)
{
    Offer offer;
    offer.url = format.value(QStringLiteral("url")).toString();
    offer.itag = format.value(QStringLiteral("itag")).toInt();
    offer.codec = codecOf(format.value(QStringLiteral("mimeType")).toString());
    offer.peakBitrate = format.value(QStringLiteral("bitrate")).toInt();
    const int average = format.value(QStringLiteral("averageBitrate")).toInt();
    offer.bitrate = average > 0 ? average : offer.peakBitrate;
    // A string in every answer seen, but a number would do as well.
    const QJsonValue rate = format.value(QStringLiteral("audioSampleRate"));
    offer.sampleRate = rate.isString() ? rate.toString().toInt() : rate.toInt();
    offer.drc = !muxed && isDrc(format);
    // A video with dubbed sound lists each track's formats under the same
    // itags; audioIsDefault marks its own. With no audioTrack there is only
    // the one.
    const QJsonValue track = format.value(QStringLiteral("audioTrack"));
    offer.ownSound = !track.isObject()
                     || track.toObject().value(QStringLiteral("audioIsDefault")).toBool(true);
    offer.muxed = muxed;
    return offer;
}

// The order of choice: Opus 774 (about 256 kbps, offered to Premium
// accounts), Opus 251 (about 130-160), AAC 141 (256, Premium), AAC 140 (about
// 130). Opus comes at 48 kHz, the rate the outputs here run at, and
// YouTube's AAC at 44.1 kHz, which has to be resampled on the way out. Any
// other format comes after these four.
int rankOf(int itag)
{
    switch (itag) {
    case 774: return 0;
    case 251: return 1;
    case 141: return 2;
    case 140: return 3;
    default:  return 4;
    }
}

// Whether `a` is the better choice than `b`: an ordinary format before a DRC
// copy (so a copy is played only when nothing else is offered), the video's
// own sound before a dub, the order above, then bitrate among the rest.
// Nothing here depends on the order the answer lists its formats in, so a
// second /player call for the same song makes the same choice; only two
// entries alike in every one of these ways are left in the listed order.
bool better(const Offer &a, const Offer &b)
{
    if (a.drc != b.drc)
        return !a.drc;
    if (a.ownSound != b.ownSound)
        return a.ownSound;
    const int rankA = rankOf(a.itag);
    const int rankB = rankOf(b.itag);
    if (rankA != rankB)
        return rankA < rankB;
    if (a.bitrate != b.bitrate)
        return a.bitrate > b.bitrate;
    return a.itag > b.itag;
}

// What pickStream chose, and what it chose from, for the log.
struct Choice {
    Offer chosen;          // url empty when nothing is playable without the cipher
    QStringList offered;   // the audio-only itags with a plain URL, as listed
};

// The stream a /player answer offers for listening: the best audio-only one
// that carries a plain URL, by the order above, or with `byBitrate` (the
// setting youtube.format=bitrate) the one with the highest peak bitrate
// whatever its codec, as before. That rule is what made the app play 44.1 kHz
// AAC where Opus was offered, whenever 140's peak came out a little above
// 251's. A format offering only `signatureCipher` needs the player
// JavaScript run to unpick it, which is exactly the work these clients exist
// to avoid, so it is passed over, and if that leaves nothing, the caller
// moves on.
Choice pickStream(const QJsonObject &root, bool byBitrate)
{
    Choice choice;
    const QJsonArray formats = dig(root, { "streamingData", "adaptiveFormats" }).toArray();
    for (const QJsonValue &value : formats) {
        const QJsonObject format = value.toObject();
        if (format.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        if (!format.value(QStringLiteral("mimeType")).toString().startsWith(QLatin1String("audio")))
            continue;
        const Offer offer = offerOf(format, /*muxed=*/false);
        choice.offered << QString::number(offer.itag) + (offer.drc ? QStringLiteral("-drc") : QString())
                              + (offer.ownSound ? QString() : QStringLiteral("-dub"));
        const bool take = choice.chosen.url.isEmpty()
                          || (byBitrate ? offer.peakBitrate > choice.chosen.peakBitrate : better(offer, choice.chosen));
        if (take)
            choice.chosen = offer;
    }
    // Nothing audio-only: fall back to the progressive list, where itag 18
    // lives — one file with the sound and a small picture muxed together. It
    // is the wrong choice for a music player in general, because the video
    // bytes are downloaded and then thrown away (mpv is loaded with vid=no),
    // which is why it is not preferred above. But some tracks publish nothing
    // else, and half a megabyte of wasted picture on those few beats sending
    // the track down to yt-dlp and waiting three seconds.
    if (choice.chosen.url.isEmpty()) {
        const QJsonArray progressive = dig(root, { "streamingData", "formats" }).toArray();
        for (const QJsonValue &value : progressive) {
            const QJsonObject format = value.toObject();
            if (format.value(QStringLiteral("url")).toString().isEmpty())
                continue;
            // Muxed formats carry no `bitrate` worth comparing across codecs;
            // take the highest itag that answers, which orders 18 ahead of
            // the smaller ones.
            if (format.value(QStringLiteral("itag")).toInt() > choice.chosen.itag)
                choice.chosen = offerOf(format, /*muxed=*/true);
        }
    }
    return choice;
}

// "opus, 48 kHz, 133 kbps" for the log: what the chosen stream is, as the
// answer describes it.
QString describe(const Offer &offer)
{
    QStringList parts{ offer.codec };
    if (offer.sampleRate > 0)
        parts << QString::number(offer.sampleRate / 1000.0) + QStringLiteral(" kHz");
    const QString kbps = QString::number(qRound(offer.bitrate / 1000.0)) + QStringLiteral(" kbps");
    // A muxed format's bitrate is the picture's and the sound's together.
    parts << (offer.muxed ? QStringLiteral("muxed with a picture, ") + kbps + QStringLiteral(" in all") : kbps);
    if (offer.drc)
        parts << QStringLiteral("DRC");
    if (!offer.ownSound)
        parts << QStringLiteral("dubbed");
    return parts.join(QStringLiteral(", "));
}

} // namespace

// One player() call as it goes (innertube.h).
struct InnerTube::PlayerAsk {
    QString videoId;
    std::function<void(const QString &url, int itag, const QString &error)> done;
    QElapsedTimer clock;
    bool fallbackLeft = false;   // PlayerFallback still to be asked
    bool renewed = false;        // asked again with a new visitor id
    QStringList refusals;        // "VISIONOS 1.02: LOGIN_REQUIRED: …", in order
};

void InnerTube::setRegion(const QString &code)
{
    g_region = code.trimmed().toUpper();
}

// YouTube Music is not offered everywhere: a country it does not serve is
// refused with 400 Bad Request for every call — search, home, radio, lyrics —
// which from the inside looks exactly like being offline. This list was
// measured by asking the API from each country in turn.
const QSet<QString> &InnerTube::servedRegions()
{
    static const QSet<QString> served = {
        QStringLiteral("AE"), QStringLiteral("AM"), QStringLiteral("AR"), QStringLiteral("AT"),
        QStringLiteral("AU"), QStringLiteral("AZ"), QStringLiteral("BA"), QStringLiteral("BD"),
        QStringLiteral("BE"), QStringLiteral("BG"), QStringLiteral("BH"), QStringLiteral("BO"),
        QStringLiteral("BR"), QStringLiteral("BY"), QStringLiteral("CA"), QStringLiteral("CH"),
        QStringLiteral("CL"), QStringLiteral("CO"), QStringLiteral("CR"), QStringLiteral("CY"),
        QStringLiteral("CZ"), QStringLiteral("DE"), QStringLiteral("DK"), QStringLiteral("DO"),
        QStringLiteral("DZ"), QStringLiteral("EC"), QStringLiteral("EE"), QStringLiteral("EG"),
        QStringLiteral("ES"), QStringLiteral("FI"), QStringLiteral("FR"), QStringLiteral("GB"),
        QStringLiteral("GE"), QStringLiteral("GH"), QStringLiteral("GR"), QStringLiteral("GT"),
        QStringLiteral("HK"), QStringLiteral("HN"), QStringLiteral("HR"), QStringLiteral("HU"),
        QStringLiteral("ID"), QStringLiteral("IE"), QStringLiteral("IL"), QStringLiteral("IN"),
        QStringLiteral("IQ"), QStringLiteral("IS"), QStringLiteral("IT"), QStringLiteral("JM"),
        QStringLiteral("JO"), QStringLiteral("JP"), QStringLiteral("KE"), QStringLiteral("KH"),
        QStringLiteral("KR"), QStringLiteral("KW"), QStringLiteral("KZ"), QStringLiteral("LA"),
        QStringLiteral("LB"), QStringLiteral("LI"), QStringLiteral("LK"), QStringLiteral("LT"),
        QStringLiteral("LU"), QStringLiteral("LV"), QStringLiteral("LY"), QStringLiteral("MA"),
        QStringLiteral("MD"), QStringLiteral("ME"), QStringLiteral("MK"), QStringLiteral("MT"),
        QStringLiteral("MX"), QStringLiteral("MY"), QStringLiteral("NG"), QStringLiteral("NI"),
        QStringLiteral("NL"), QStringLiteral("NO"), QStringLiteral("NP"), QStringLiteral("NZ"),
        QStringLiteral("OM"), QStringLiteral("PA"), QStringLiteral("PE"), QStringLiteral("PG"),
        QStringLiteral("PH"), QStringLiteral("PK"), QStringLiteral("PL"), QStringLiteral("PR"),
        QStringLiteral("PT"), QStringLiteral("PY"), QStringLiteral("QA"), QStringLiteral("RO"),
        QStringLiteral("RS"), QStringLiteral("RU"), QStringLiteral("SA"), QStringLiteral("SE"),
        QStringLiteral("SG"), QStringLiteral("SI"), QStringLiteral("SK"), QStringLiteral("SN"),
        QStringLiteral("SV"), QStringLiteral("TH"), QStringLiteral("TN"), QStringLiteral("TR"),
        QStringLiteral("TW"), QStringLiteral("TZ"), QStringLiteral("UA"), QStringLiteral("UG"),
        QStringLiteral("US"), QStringLiteral("UY"), QStringLiteral("VE"), QStringLiteral("VN"),
        QStringLiteral("YE"), QStringLiteral("ZA"), QStringLiteral("ZW")
    };
    return served;
}

// The system's own country, unless YouTube Music does not serve it — in which
// case asking as the system would fail every call, so the nearest thing to a
// neutral choice is used instead.
QString InnerTube::systemRegion()
{
    const QString code = QLocale::territoryToCode(QLocale::system().territory());
    if (code.size() == 2 && servedRegions().contains(code))
        return code;
    return QStringLiteral("US");
}

QString InnerTube::region()
{
    return g_region.isEmpty() ? systemRegion() : g_region;
}

void InnerTube::setRegionRejectedHandler(std::function<void(const QString &)> handler)
{
    g_regionRejected = std::move(handler);
}

void InnerTube::setAccountHook(AccountHook hook)
{
    g_account = std::move(hook);
}

void InnerTube::setArtistHook(std::function<void(const QString &, const QString &, bool)> hook)
{
    g_artistSeen = std::move(hook);
}

QVariantList InnerTube::creditsToVariant(const QList<Credit> &credits)
{
    QVariantList list;
    list.reserve(credits.size());
    for (const Credit &credit : credits) {
        list.append(QVariantMap{
            { QStringLiteral("text"), credit.text },
            { QStringLiteral("id"), credit.browseId },
            { QStringLiteral("link"), credit.name }
        });
    }
    return list;
}

void InnerTube::setTestServer(const QString &baseUrl)
{
    g_testServer = baseUrl;
}

void InnerTube::setVisitorStore(VisitorStore store)
{
    Visitor &v = g_visitor;
    ++v.generation;
    v.store = std::move(store);
    v.mode = Visitor::Mode::Stored;
    v.loaded = false;
    v.anonymous.clear();
    v.fetchedAt = QDateTime();
    v.fromSettings.clear();
    v.refreshDue = false;
    v.pending = false;
    if (v.jar)
        v.jar->clear();
    g_playerClients = {};
    g_formatRule = {};
    // Nobody is left waiting on a fetch whose answer will now be dropped.
    const auto waiters = std::exchange(v.waiters, {});
    for (const auto &waiter : waiters)
        waiter();
}

void InnerTube::forgetVisitorData()
{
    Visitor &v = g_visitor;
    v.anonymous.clear();
    v.fetchedAt = QDateTime();
    v.fromSettings.clear();
    v.refreshDue = false;
    writeVisitor({}, {});
    if (v.jar)
        v.jar->clear();
    qInfo("innertube: forgot the visitor id and YouTube's anonymous cookies");
}

void InnerTube::setSessionVisitorData(const QString &visitorData)
{
    g_visitor.session = visitorData;
}

void InnerTube::setPlayerClients(PlayerClients clients)
{
    g_playerClients.loaded = true;
    g_playerClients.clients = clients;
}

InnerTube::PlayerClients InnerTube::playerClients()
{
    PlayerClientsSwitch &s = g_playerClients;
    if (s.loaded)
        return s.clients;
    s.loaded = true;
    const QString value = g_visitor.store.read ? g_visitor.store.read(kPlayerClientKey) : QString();
    s.clients = value == QLatin1String("first")  ? PlayerClients::First
              : value == QLatin1String("second") ? PlayerClients::Second
                                                 : PlayerClients::Both;
    if (s.clients != PlayerClients::Both)
        qInfo("innertube: youtube.player_client=%s: /player is asked as %s alone", qPrintable(value),
              qPrintable(playerClientName(s.clients == PlayerClients::First ? Client::Player
                                                                            : Client::PlayerFallback)));
    return s.clients;
}

InnerTube::InnerTube(QObject *parent, bool warmUp)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    shareJar(m_network);
    if (!warmUp || !g_testServer.isEmpty())
        return;
    // Open the TLS connections now, so the first search and the first track
    // are one round trip like every later one, rather than paying for DNS and
    // the handshake. www.youtube.com is where /player is asked.
    m_network->connectToHostEncrypted(QStringLiteral("music.youtube.com"));
    m_network->connectToHostEncrypted(QStringLiteral("www.youtube.com"));
    // The visitor id: the stored one, or else fetched now, off the critical
    // path and once however many objects are made.
    loadVisitor();
    if (g_visitor.anonymous.isEmpty())
        fetchVisitor();
}

void InnerTube::withVisitorData(std::function<void()> then)
{
    loadVisitor();
    expireVisitor();
    if (!g_visitor.anonymous.isEmpty())
        return then();
    // The process holds the waiters, so one whose object has gone by the
    // time the fetch answers must not be called.
    g_visitor.waiters.push_back([self = QPointer<InnerTube>(this), then = std::move(then)]() {
        if (self)
            then();
    });
    fetchVisitor();   // unless one is on its way already
}

QNetworkReply *InnerTube::post(Client client, const QString &endpoint, QJsonObject body, int timeoutMs,
                               Auth auth, quint64 *session)
{
    QString host = QStringLiteral("https://www.youtube.com");
    QByteArray agent = kUserAgent;
    if (client == Client::Music) {
        host = QStringLiteral("https://music.youtube.com");
    } else if (isPlayerClient(client)) {
        agent = kVisionUserAgent;
    }
    if (!g_testServer.isEmpty())
        host = g_testServer;

    // /player names the anonymous visitor (withVisitorData). The account
    // only ever goes on Music calls, so the two never meet.
    const QString visitor = isPlayerClient(client) ? g_visitor.anonymous : QString();
    const auto nameVisitor = [](QJsonObject &context, const QString &id) {
        QJsonObject inner = context.value(QStringLiteral("client")).toObject();
        inner.insert(QStringLiteral("visitorData"), id);
        context.insert(QStringLiteral("client"), inner);
    };
    QJsonObject context = clientContext(client);
    if (!visitor.isEmpty())
        nameVisitor(context, visitor);

    QNetworkRequest request(QUrl(host + QStringLiteral("/youtubei/v1/") + endpoint
                                 + QStringLiteral("?prettyPrint=false")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, agent);
    request.setRawHeader("Origin", host.toUtf8());
    request.setRawHeader("Referer", (host + QLatin1Char('/')).toUtf8());
    if (!visitor.isEmpty())
        request.setRawHeader("X-Goog-Visitor-Id", visitor.toUtf8());
    request.setTransferTimeout(timeoutMs);

    // The account, on the few calls that ask for it and only while there is
    // one. Everything above is what an anonymous call sends, and an
    // anonymous call sends nothing more.
    quint64 account = 0;
    if (auth != Auth::Anonymous && client == Client::Music && g_account.headers) {
        QByteArray cookie;
        QByteArray authorization;
        account = g_account.headers(auth, host.toUtf8(), &cookie, &authorization);
        if (account != 0 && !cookie.isEmpty()) {
            // Neither way through the network manager's own jar. How Qt
            // would mix a Cookie header set here with the jar's anonymous
            // cookies is not documented, and those must not ride along with
            // the account's; and the account's rotated cookies must not land
            // in the jar, where every anonymous call would carry them.
            // YtmSession keeps those itself.
            request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
            request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
            // Never followed. A redirected request is a copy of this one,
            // raw headers and all, so the cookies and the Authorization
            // would go wherever the redirect points; and what that host set
            // would be kept as music.youtube.com's. send() treats a redirect
            // as no answer.
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            request.setRawHeader("Cookie", cookie);
            if (!authorization.isEmpty())
                request.setRawHeader("Authorization", authorization);
            request.setRawHeader("X-Origin", host.toUtf8());
            request.setRawHeader("X-Goog-AuthUser", "0");
            // The account's own visitor id, once the sign-in work supplies
            // one. Until then its calls name none, as they always have.
            if (!g_visitor.session.isEmpty()) {
                nameVisitor(context, g_visitor.session);
                request.setRawHeader("X-Goog-Visitor-Id", g_visitor.session.toUtf8());
            }
        } else {
            account = 0;
        }
    }
    if (session)
        *session = account;
    body.insert(QStringLiteral("context"), context);
    return m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void InnerTube::release(Slot &slot)
{
    // Moved on first: abort() delivers finished() synchronously, and that
    // handler must see itself as superseded, not as a failed request.
    ++slot.generation;
    if (QNetworkReply *reply = slot.reply) {
        slot.reply = nullptr;
        reply->abort();
    }
}

void InnerTube::send(Client client, const QString &endpoint, const QJsonObject &body, int timeoutMs,
                     Slot *slot,
                     std::function<void(const QJsonObject &, const QString &)> done, int retries,
                     Auth auth)
{
    quint64 account = 0;
    QNetworkReply *reply = post(client, endpoint, body, timeoutMs, auth, &account);
    // The request this attempt belongs to. Its retries carry it forward, and
    // whichever of them finds the slot moved on stops there.
    const quint64 generation = slot ? slot->generation : 0;
    if (slot)
        slot->reply = reply;
    const auto superseded = [slot, generation]() {
        return slot && slot->generation != generation;
    };

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, client, endpoint, body, timeoutMs, slot, done, retries, auth, account, superseded]() {
                reply->deleteLater();
                // A newer request of the same kind took this one's place, or it
                // was cancelled: whoever did that has already moved on.
                if (superseded())
                    return;
                if (slot)
                    slot->reply = nullptr;

                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (account != 0) {
                    // The session's cookies, rotated by the server; kept by
                    // YtmSession, since this request left the jar alone.
                    const QList<QNetworkCookie> rotated =
                        reply->header(QNetworkRequest::SetCookieHeader).value<QList<QNetworkCookie>>();
                    if (!rotated.isEmpty() && g_account.cookies)
                        g_account.cookies(account, rotated);

                    // Refused with the account. 401 and 403 say the session
                    // is over; 400 may be the cookies as much as the country,
                    // and a broken session must never cost the user the
                    // country they chose, so a call that carried the account
                    // never reaches the branch below that drops it. Either
                    // way the call is made again without the account, and a
                    // country that really is refused is then found out by an
                    // anonymous request, as it always was.
                    // A redirect (not followed, see post()) is no verdict
                    // either way: the check hears nothing, and anything else
                    // is asked again without the account.
                    const bool redirected = status >= 300 && status < 400;
                    if (status == 400 || status == 401 || status == 403 || redirected) {
                        if ((status == 401 || status == 403) && g_account.rejected)
                            g_account.rejected(account, status);
                        if (superseded())
                            return;
                        // YtmSession's check wants the verdict, not an
                        // anonymous answer standing in for it.
                        if (auth == Auth::Checking) {
                            done({}, redirected ? QStringLiteral("YouTube Music answered with a redirect (HTTP %1)")
                                                      .arg(status)
                                                : reply->errorString());
                            return;
                        }
                        send(client, endpoint, body, timeoutMs, slot, done, retries, Auth::Anonymous);
                        return;
                    }
                }

                if (reply->error() != QNetworkReply::NoError) {
                    // A country YouTube Music does not serve is refused with
                    // 400, every time, for everything. Rather than look
                    // broken, drop the country and ask again as the system.
                    if (status == 400 && !g_region.isEmpty()) {
                        const QString refused = g_region;
                        g_region.clear();
                        if (g_regionRejected)
                            g_regionRejected(refused);
                        // Being told can start a newer request of this kind;
                        // if it did, that one is the answer now.
                        if (superseded())
                            return;
                        send(client, endpoint, body, timeoutMs, slot, done, retries, auth);
                        return;
                    }
                    // A dropped connection or a timeout is ordinary on a home
                    // connection; the second attempt usually works.
                    if (retries > 0 && reply->error() != QNetworkReply::OperationCanceledError) {
                        QTimer::singleShot(1200, this, [this, client, endpoint, body, timeoutMs,
                                                        slot, done, retries, auth, superseded]() {
                            // Replaced or cancelled while waiting. Sending now
                            // would put this old request back in the slot,
                            // where the newer one's answer would be dropped
                            // as stale and nobody would ever hear back.
                            if (superseded())
                                return;
                            send(client, endpoint, body, timeoutMs, slot, done, retries - 1, auth);
                        });
                        return;
                    }
                    done({}, reply->errorString());
                    return;
                }

                const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
                if (!document.isObject()) {
                    done({}, QStringLiteral("YouTube sent a response that is not JSON."));
                    return;
                }
                done(document.object(), QString());
            });
}

void InnerTube::player(const QString &videoId,
                       std::function<void(const QString &url, int itag, const QString &error)> done)
{
    if (videoId.isEmpty()) {
        done({}, 0, QStringLiteral("no video id"));
        return;
    }
    auto ask = std::make_shared<PlayerAsk>();
    ask->videoId = videoId;
    ask->done = std::move(done);
    ask->clock.start();
    const PlayerClients clients = playerClients();
    ask->fallbackLeft = clients == PlayerClients::Both;
    askPlayer(clients == PlayerClients::Second ? Client::PlayerFallback : Client::Player, ask);
}

void InnerTube::askPlayer(Client client, std::shared_ptr<PlayerAsk> ask)
{
    withVisitorData([this, client, ask]() {
        // The id post() puts on this call: afterwards it tells whether the
        // call went with the stored id, and whether a newer one has come.
        const QString visitor = g_visitor.anonymous;
        const QJsonObject body{
            { QStringLiteral("videoId"), ask->videoId },
            // Both are what the app itself sends; without them anything
            // flagged is refused rather than played.
            { QStringLiteral("contentCheckOk"), true },
            { QStringLiteral("racyCheckOk"), true }
        };
        // The client asked after another has answered is a second chance:
        // a shorter limit, and no retry.
        const bool second = !ask->refusals.isEmpty();
        send(client, QStringLiteral("player"), body, second ? kPlayerFallbackTimeoutMs : kPlayerTimeoutMs,
             /*slot=*/nullptr,
             [this, client, visitor, ask](const QJsonObject &root, const QString &error) {
                 const QString &videoId = ask->videoId;
                 if (!error.isEmpty()) {
                     // No answer at all: not asked of the other client, which
                     // would only wait on the same server again.
                     ask->refusals << playerClientName(client) + QStringLiteral(": ") + error;
                     const QString why = ask->refusals.join(QStringLiteral("; "));
                     qInfo("innertube: %s not resolved in %lld ms: %s", qPrintable(videoId),
                           (long long)ask->clock.elapsed(), qPrintable(why));
                     ask->done({}, 0, why);
                     return;
                 }
                 const QString status = dig(root, { "playabilityStatus", "status" }).toString();
                 if (status != QLatin1String("OK")) {
                     // LOGIN_REQUIRED, UNPLAYABLE, AGE_VERIFICATION_REQUIRED:
                     // all of them mean the next client, or the tier below,
                     // should try.
                     const QString reason = dig(root, { "playabilityStatus", "reason" }).toString();
                     const QString refused = reason.isEmpty() ? status : status + QStringLiteral(": ") + reason;
                     // Except once. A stored id YouTube no longer honours is
                     // answered LOGIN_REQUIRED, exactly as having none is, and
                     // a new id and a second /player cost a few hundred ms
                     // where yt-dlp costs seconds. Only for the id this launch
                     // found stored: with one fetched this launch,
                     // LOGIN_REQUIRED is the track's own answer.
                     if (status == QLatin1String("LOGIN_REQUIRED") && !ask->renewed && ask->refusals.isEmpty()
                         && !visitor.isEmpty() && visitor == g_visitor.fromSettings) {
                         ask->renewed = true;
                         qInfo("innertube: %s refused with the stored visitor id (LOGIN_REQUIRED); "
                               "fetching a new id and asking once more", qPrintable(videoId));
                         renewVisitor(visitor, [self = QPointer<InnerTube>(this), client, visitor, refused, ask]() {
                             if (!self)
                                 return;
                             if (g_visitor.anonymous.isEmpty() || g_visitor.anonymous == visitor) {
                                 qInfo("innertube: %s: no new visitor id to ask with", qPrintable(ask->videoId));
                                 self->playerRefused(client, refused, ask);
                                 return;
                             }
                             self->askPlayer(client, ask);
                         });
                         return;
                     }
                     if (ask->renewed && ask->refusals.isEmpty())
                         qInfo("innertube: %s refused again with a new visitor id: %s", qPrintable(videoId),
                               qPrintable(status));
                     playerRefused(client, refused, ask);
                     return;
                 }
                 if (ask->renewed && ask->refusals.isEmpty())
                     qInfo("innertube: %s answered with the new visitor id", qPrintable(videoId));
                 // The first play has its answer: a stored id over a day old
                 // is replaced now, in the background.
                 if (g_visitor.refreshDue)
                     renewVisitorSoon();

                 const bool byBitrate = chooseByBitrate();
                 const Choice choice = pickStream(root, byBitrate);
                 if (choice.chosen.url.isEmpty()) {
                     playerRefused(client, QStringLiteral("no plain audio stream offered"), ask);
                     return;
                 }
                 // What was chosen, from what, on every resolve: the one place
                 // that tells which rendition played and why, and that Opus
                 // was not offered when the song plays as AAC.
                 const QString from = choice.offered.isEmpty() ? QStringLiteral("no audio-only format offered")
                                                               : QStringLiteral("of ") + choice.offered.join(QLatin1Char(' '));
                 qInfo("innertube: %s chose itag %d: %s (%s%s)", qPrintable(videoId), choice.chosen.itag,
                       qPrintable(describe(choice.chosen)), qPrintable(from),
                       byBitrate ? ", by bitrate alone: youtube.format=bitrate" : "");
                 // Which client answered is in the log only when it was not
                 // the usual one, which is exactly when it matters.
                 if (client != Client::Player) {
                     const QString after = ask->refusals.isEmpty()
                         ? QString() : QStringLiteral(", after ") + ask->refusals.join(QStringLiteral("; "));
                     qInfo("innertube: %s answered as %s in %lld ms%s", qPrintable(videoId),
                           qPrintable(playerClientName(client)), (long long)ask->clock.elapsed(), qPrintable(after));
                 }
                 ask->done(choice.chosen.url, choice.chosen.itag, QString());
             },
             /*retries=*/second ? 0 : 1);
    });
}

void InnerTube::playerRefused(Client client, const QString &why, std::shared_ptr<PlayerAsk> ask)
{
    ask->refusals << playerClientName(client) + QStringLiteral(": ") + why;
    // The one other client, once, and only after an answer: never a walk
    // through several, each with its own wait.
    if (client == Client::Player && ask->fallbackLeft) {
        ask->fallbackLeft = false;
        qInfo("innertube: %s: %s gave no plain stream in %lld ms (%s); asking as %s", qPrintable(ask->videoId),
              qPrintable(playerClientName(client)), (long long)ask->clock.elapsed(), qPrintable(why),
              qPrintable(playerClientName(Client::PlayerFallback)));
        askPlayer(Client::PlayerFallback, ask);
        return;
    }
    // Why, and how long it took: the tier below is seconds, and this line is
    // the only record of what sent the track there.
    const QString all = ask->refusals.join(QStringLiteral("; "));
    qInfo("innertube: %s not resolved in %lld ms: %s", qPrintable(ask->videoId), (long long)ask->clock.elapsed(),
          qPrintable(all));
    ask->done({}, 0, all);
}

void InnerTube::search(const QString &query, Filter filter)
{
    release(m_search);

    const QJsonObject body{
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), filter == Filter::Videos ? kVideosFilter : kSongsFilter }
    };
    send(Client::Music, QStringLiteral("search"), body, kSearchTimeoutMs, &m_search,
         [this, query](const QJsonObject &root, const QString &error) {
             if (error.isEmpty())
                 Q_EMIT searchFinished(query, parseSearch(root));
             else
                 Q_EMIT searchFailed(query, error);
         });
}

void InnerTube::searchYouTube(const QString &query)
{
    release(m_search);

    send(Client::YouTube, QStringLiteral("search"), { { QStringLiteral("query"), query } },
         kSearchTimeoutMs, &m_search,
         [this, query](const QJsonObject &root, const QString &error) {
             if (error.isEmpty())
                 Q_EMIT youtubeSearchFinished(query, parseYouTubeSearch(root));
             else
                 Q_EMIT youtubeSearchFailed(query, error);
         });
}

void InnerTube::suggest(const QString &input)
{
    release(m_suggest);

    // Suggestions are a nicety and are asked for while typing: no retry, and
    // a failure simply shows none.
    send(Client::Music, QStringLiteral("music/get_search_suggestions"),
         { { QStringLiteral("input"), input } }, kSuggestTimeoutMs, &m_suggest,
         [this, input](const QJsonObject &root, const QString &error) {
             if (error.isEmpty())
                 Q_EMIT suggestionsReady(input, parseSuggestions(root));
         },
         /*retries=*/0);
}

void InnerTube::radio(const QString &videoId)
{
    cancelRadio();

    // "RDAMVM" + id is the radio playlist YouTube Music starts from a song;
    // "wAEB" asks for it in radio mode, as its own client does.
    const QJsonObject body{
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("playlistId"), QStringLiteral("RDAMVM") + videoId },
        { QStringLiteral("params"), QStringLiteral("wAEB") },
        { QStringLiteral("isAudioOnly"), true },
        { QStringLiteral("enablePersistentPlaylistPanel"), true }
    };
    send(Client::Music, QStringLiteral("next"), body, kSearchTimeoutMs, &m_radio,
         [this, videoId](const QJsonObject &root, const QString &error) {
             if (!error.isEmpty()) {
                 Q_EMIT radioFailed(videoId, error);
                 return;
             }
             const QList<Track> tracks = parseRadio(root);
             if (tracks.isEmpty())
                 Q_EMIT radioFailed(videoId, QStringLiteral("YouTube Music returned no radio for this song."));
             else
                 Q_EMIT radioReady(videoId, tracks);
         });
}

void InnerTube::cancelRadio()
{
    release(m_radio);
}

void InnerTube::cancelSearch()
{
    release(m_search);
}

void InnerTube::cancelSuggestions()
{
    release(m_suggest);
}

QList<InnerTube::Track> InnerTube::parseSearch(const QJsonObject &root)
{
    QList<Track> tracks;
    const QJsonArray sections = dig(root, { "contents", "tabbedSearchResultsRenderer", "tabs", "#0",
                                            "tabRenderer", "content", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        const QJsonArray items = dig(section, { "musicShelfRenderer", "contents" }).toArray();
        for (const QJsonValue &entry : items) {
            const Track track = parseListItem(dig(entry, { "musicResponsiveListItemRenderer" }));
            // No video id: an album, artist or playlist, nothing to play directly.
            if (!track.videoId.isEmpty() && !track.title.isEmpty())
                tracks.append(track);
        }
    }
    return tracks;
}

// youtube.com's own search: videos, with the channel where YouTube Music
// would name the artist and no album at all. A fallback, not a replacement.
QList<InnerTube::Track> InnerTube::parseYouTubeSearch(const QJsonObject &root)
{
    QList<Track> tracks;
    const QJsonArray sections = dig(root, { "contents", "twoColumnSearchResultsRenderer",
                                            "primaryContents", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        const QJsonArray items = dig(section, { "itemSectionRenderer", "contents" }).toArray();
        for (const QJsonValue &entry : items) {
            const QJsonValue item = dig(entry, { "videoRenderer" });
            if (item.isUndefined())
                continue;   // a channel, a playlist, an advert, a shelf
            Track track;
            track.videoId = dig(item, { "videoId" }).toString();
            track.title = joinRuns(dig(item, { "title", "runs" }).toArray()).trimmed();
            track.artist = joinRuns(dig(item, { "ownerText", "runs" }).toArray()).trimmed();
            const QString length = dig(item, { "lengthText", "simpleText" }).toString();
            if (!length.isEmpty())
                track.durationMs = parseClock(length);
            const QJsonArray thumbnails = dig(item, { "thumbnail", "thumbnails" }).toArray();
            if (!thumbnails.isEmpty())
                track.artwork = thumbnails.last().toObject().value(QLatin1String("url")).toString();
            // Everything here is a video; a live stream has no length.
            track.isVideo = true;
            if (!track.videoId.isEmpty() && !track.title.isEmpty() && track.durationMs > 0)
                tracks.append(track);
        }
    }
    return tracks;
}

void InnerTube::browse(const QString &browseId,
                       std::function<void(const QJsonObject &, const QString &)> done, Auth auth)
{
    // A page is bigger than a search (the home feed is a few hundred KB), so
    // it is given longer before the connection is called dead.
    send(Client::Music, QStringLiteral("browse"), { { QStringLiteral("browseId"), browseId } },
         kBrowseTimeoutMs, nullptr, std::move(done), /*retries=*/1, auth);
}

void InnerTube::browse(const QString &browseId, const QString &params,
                       std::function<void(const QJsonObject &, const QString &)> done)
{
    QJsonObject body{ { QStringLiteral("browseId"), browseId } };
    if (!params.isEmpty())
        body.insert(QStringLiteral("params"), params);
    send(Client::Music, QStringLiteral("browse"), body, kBrowseTimeoutMs, nullptr, std::move(done));
}

// The token goes in the body, as music.youtube.com's own client sends it
// now. The older tokens (a list's "nextContinuationData") are answered the
// same way, so one call serves both.
void InnerTube::continueBrowse(const QString &token,
                               std::function<void(const QJsonObject &, const QString &)> done)
{
    send(Client::Music, QStringLiteral("browse"), { { QStringLiteral("continuation"), token } },
         kBrowseTimeoutMs, nullptr, std::move(done));
}

void InnerTube::accountMenu(Auth auth, std::function<void(const QJsonObject &, const QString &)> done)
{
    send(Client::Music, QStringLiteral("account/account_menu"), {}, kBrowseTimeoutMs, nullptr,
         std::move(done), /*retries=*/1, auth);
}

// The header of the menu behind the avatar, as ytmusicapi's get_account_info
// reads it: the account's name, and its channel handle below it.
QString InnerTube::parseAccountName(const QJsonObject &root)
{
    const QJsonValue header = dig(root, { "actions", "#0", "openPopupAction", "popup", "multiPageMenuRenderer",
                                          "header", "activeAccountHeaderRenderer" });
    QString name = joinRuns(dig(header, { "accountName", "runs" }).toArray()).trimmed();
    if (name.isEmpty())
        name = dig(header, { "accountName", "simpleText" }).toString().trimmed();
    return name;
}

QString InnerTube::parseLoggedIn(const QJsonObject &root)
{
    const QJsonArray services = dig(root, { "responseContext", "serviceTrackingParams" }).toArray();
    for (const QJsonValue &service : services) {
        for (const QJsonValue &param : service.toObject().value(QLatin1String("params")).toArray()) {
            const QJsonObject pair = param.toObject();
            if (pair.value(QLatin1String("key")).toString() != QLatin1String("logged_in"))
                continue;
            // A string on the wire; a number is read the same.
            const QJsonValue value = pair.value(QLatin1String("value"));
            if (value.isString())
                return value.toString();
            if (value.isDouble())
                return QString::number(value.toInt());
        }
    }
    return QString();
}

void InnerTube::lyrics(const QString &videoId,
                       std::function<void(const QString &, const QString &, const QString &)> done)
{
    send(Client::Music, QStringLiteral("next"), {
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("isAudioOnly"), true }
    }, kSearchTimeoutMs, nullptr, [this, done](const QJsonObject &root, const QString &error) {
        if (!error.isEmpty()) {
            done({}, {}, error);
            return;
        }
        // The watch page's second tab is Lyrics; without a browse id it is
        // greyed out, and the song has none.
        const QJsonArray tabs = dig(root, { "contents", "singleColumnMusicWatchNextResultsRenderer",
                                            "tabbedRenderer", "watchNextTabbedResultsRenderer",
                                            "tabs" }).toArray();
        QString browseId;
        for (const QJsonValue &tab : tabs) {
            const QString id = dig(tab, { "tabRenderer", "endpoint", "browseEndpoint", "browseId" }).toString();
            if (id.startsWith(QLatin1String("MPLYt"))) {
                browseId = id;
                break;
            }
        }
        if (browseId.isEmpty()) {
            done({}, {}, {});
            return;
        }
        browse(browseId, [done](const QJsonObject &page, const QString &error) {
            if (!error.isEmpty()) {
                done({}, {}, error);
                return;
            }
            const QJsonValue shelf = dig(page, { "contents", "sectionListRenderer", "contents", "#0",
                                                 "musicDescriptionShelfRenderer" });
            const QString text = joinRuns(dig(shelf, { "description", "runs" }).toArray()).trimmed();
            // "Source: Musixmatch"
            QString source = joinRuns(dig(shelf, { "footer", "runs" }).toArray()).trimmed();
            source.remove(QRegularExpression(QStringLiteral(R"(^\s*Source:\s*)"),
                                             QRegularExpression::CaseInsensitiveOption));
            done(text, source, {});
        });
    });
}

QList<InnerTube::Shelf> InnerTube::parseShelves(const QJsonObject &root)
{
    QList<Shelf> shelves;
    const QJsonArray sections = dig(root, { "contents", "singleColumnBrowseResultsRenderer", "tabs", "#0",
                                            "tabRenderer", "content", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        // Carousels only: the taste builder and genre chips are not content.
        const QJsonValue carousel = dig(section, { "musicCarouselShelfRenderer" });
        if (carousel.isUndefined())
            continue;

        const Shelf shelf = parseCarousel(carousel);
        if (!shelf.songs.isEmpty() || !shelf.cards.isEmpty())
            shelves.append(shelf);
    }
    return shelves;
}

InnerTube::Collection InnerTube::parseCollection(const QString &browseId, const QJsonObject &root)
{
    Collection collection;
    collection.browseId = browseId;
    const QJsonValue header = dig(root, { "contents", "twoColumnBrowseResultsRenderer", "tabs", "#0",
                                          "tabRenderer", "content", "sectionListRenderer", "contents", "#0",
                                          "musicResponsiveHeaderRenderer" });
    collection.title = joinRuns(dig(header, { "title", "runs" }).toArray()).trimmed();
    collection.subtitle = joinRuns(dig(header, { "subtitle", "runs" }).toArray()).trimmed();
    collection.artist = joinRuns(dig(header, { "straplineTextOne", "runs" }).toArray()).trimmed();
    appendCredits(dig(header, { "straplineTextOne", "runs" }).toArray(), collection.artistCredits);
    if (std::none_of(collection.artistCredits.cbegin(), collection.artistCredits.cend(),
                     [](const Credit &credit) { return !credit.browseId.isEmpty(); }))
        collection.artistCredits.clear();
    collection.primaryArtist = firstArtistRun(dig(header, { "straplineTextOne", "runs" }).toArray());
    if (collection.primaryArtist.isEmpty())
        collection.primaryArtist = collection.artist;
    collection.details = joinRuns(dig(header, { "secondSubtitle", "runs" }).toArray()).trimmed();
    collection.description = joinRuns(dig(header, { "description", "musicDescriptionShelfRenderer",
                                                    "description", "runs" }).toArray()).trimmed();
    const QJsonArray thumbnails = dig(header, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                                "thumbnails" }).toArray();
    if (!thumbnails.isEmpty())
        collection.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
    collection.type = browseId.startsWith(QLatin1String("MPRE")) ? QStringLiteral("album")
                                                                 : QStringLiteral("playlist");

    const QJsonValue section = dig(root, { "contents", "twoColumnBrowseResultsRenderer", "secondaryContents",
                                           "sectionListRenderer", "contents", "#0" });
    QJsonValue shelf = dig(section, { "musicShelfRenderer" });
    if (shelf.isUndefined())
        shelf = dig(section, { "musicPlaylistShelfRenderer" });
    const QJsonArray rows = dig(shelf, { "contents" }).toArray();

    for (const QJsonValue &entry : rows) {
        Track track = parseListItem(dig(entry, { "musicResponsiveListItemRenderer" }));
        if (track.videoId.isEmpty() || track.title.isEmpty())
            continue;   // unavailable in this region, or not a song
        completeTrack(track, collection);
        collection.tracks.append(track);
    }
    // Only the list's own "more": the section list's is the shelf of related
    // playlists under it, which is not more of this one.
    collection.continuation = continuationOf(shelf, rows);
    return collection;
}

void InnerTube::completeTrack(Track &track, const Collection &collection)
{
    // An album's rows leave out what the header already says.
    if (track.artist.isEmpty()) {
        track.artist = collection.artist;
        track.primaryArtist = collection.primaryArtist;
        track.credits = collection.artistCredits;
    }
    if (collection.type == QLatin1String("album")) {
        if (track.album.isEmpty())
            track.album = collection.title;
        if (track.albumId.isEmpty())
            track.albumId = collection.browseId;
        track.artwork = collection.artwork;
    } else if (track.artwork.isEmpty()) {
        track.artwork = collection.artwork;
    }
}

// The answer to continueBrowse, in whichever shape the list it continues
// takes: the current one, where the new items are "appended" to the list
// already shown, or the older one, where they come as the list's own
// continuation (a playlist's, a shelf's, a grid's, or a page's sections).
InnerTube::Continuation InnerTube::parseContinuation(const QJsonObject &root)
{
    Continuation part;
    const QJsonArray appended = dig(root, { "onResponseReceivedActions", "#0", "appendContinuationItemsAction",
                                            "continuationItems" }).toArray();
    if (!appended.isEmpty()) {
        for (const QJsonValue &entry : appended)
            readListItem(entry, part.tracks, part.cards);
        part.next = continuationOf(QJsonValue(), appended);
        return part;
    }

    const QJsonObject contents = root.value(QLatin1String("continuationContents")).toObject();
    for (const char *key : { "musicPlaylistShelfContinuation", "musicShelfContinuation" }) {
        const QJsonValue list = contents.value(QLatin1String(key));
        if (list.isUndefined())
            continue;
        const QJsonArray items = dig(list, { "contents" }).toArray();
        for (const QJsonValue &entry : items)
            readListItem(entry, part.tracks, part.cards);
        part.next = continuationOf(list, items);
        return part;
    }
    const QJsonValue grid = contents.value(QLatin1String("gridContinuation"));
    if (!grid.isUndefined()) {
        const QJsonArray items = dig(grid, { "items" }).toArray();
        for (const QJsonValue &entry : items)
            readListItem(entry, part.tracks, part.cards);
        part.next = continuationOf(grid, items);
        return part;
    }
    const QJsonValue sections = contents.value(QLatin1String("sectionListContinuation"));
    if (!sections.isUndefined()) {
        for (const QJsonValue &section : dig(sections, { "contents" }).toArray()) {
            Shelf shelf;
            if (!dig(section, { "musicCarouselShelfRenderer" }).isUndefined())
                shelf = parseCarousel(dig(section, { "musicCarouselShelfRenderer" }));
            else if (!dig(section, { "gridRenderer" }).isUndefined())
                shelf = parseGrid(dig(section, { "gridRenderer" }));
            dropUnopenable(shelf);
            if (!shelf.songs.isEmpty() || !shelf.cards.isEmpty())
                part.shelves.append(shelf);
        }
        part.next = continuationOf(sections, QJsonArray());
    }
    return part;
}

// What a shelf's "more" button opens: the page's title, then its sections —
// a grid of cards (an artist's albums, the week's new releases), a run of
// songs, or carousels — in the page's own order.
InnerTube::Listing InnerTube::parseListing(const QJsonObject &root)
{
    Listing listing;
    for (const char *kind : { "musicHeaderRenderer", "musicVisualHeaderRenderer", "musicImmersiveHeaderRenderer",
                              "musicResponsiveHeaderRenderer" }) {
        listing.title = joinRuns(dig(root, { "header", kind, "title", "runs" }).toArray()).trimmed();
        if (!listing.title.isEmpty())
            break;
    }

    const QJsonValue list = dig(root, { "contents", "singleColumnBrowseResultsRenderer", "tabs", "#0",
                                        "tabRenderer", "content", "sectionListRenderer" });
    for (const QJsonValue &section : dig(list, { "contents" }).toArray()) {
        Shelf shelf;
        QString more;
        const QJsonValue grid = dig(section, { "gridRenderer" });
        const QJsonValue songs = dig(section, { "musicShelfRenderer" });
        const QJsonValue carousel = dig(section, { "musicCarouselShelfRenderer" });
        if (!grid.isUndefined()) {
            shelf = parseGrid(grid);
            more = continuationOf(grid, dig(grid, { "items" }).toArray());
        } else if (!songs.isUndefined()) {
            shelf.title = joinRuns(dig(songs, { "title", "runs" }).toArray()).trimmed();
            const QJsonArray items = dig(songs, { "contents" }).toArray();
            for (const QJsonValue &entry : items)
                readListItem(entry, shelf.songs, shelf.cards);
            more = continuationOf(songs, items);
        } else if (!carousel.isUndefined()) {
            shelf = parseCarousel(carousel);
        }
        dropUnopenable(shelf);
        if (shelf.songs.isEmpty() && shelf.cards.isEmpty())
            continue;
        listing.sections.append(shelf);
        // Only the last section can grow: more of an earlier one would land
        // below the sections after it.
        listing.itemsContinuation = more;
    }
    listing.sectionsContinuation = continuationOf(list, QJsonArray());
    return listing;
}

QVariantMap InnerTube::cardToVariant(const Card &card)
{
    return {
        { QStringLiteral("type"), card.type },
        { QStringLiteral("browseId"), card.browseId },
        { QStringLiteral("videoId"), card.videoId },
        { QStringLiteral("title"), card.title },
        { QStringLiteral("subtitle"), card.subtitle },
        { QStringLiteral("artwork"), card.artwork },
        { QStringLiteral("artist"), card.artist },
        { QStringLiteral("primaryArtist"), card.primaryArtist }
    };
}

// An artist's page: the immersive header (the wide photograph, the name, the
// Shuffle and Mix buttons), the top songs, and carousels of cards. A plain
// channel is laid out alike under a plainer header, so all three headers are
// read, and whatever the page leaves out is left empty.
InnerTube::Artist InnerTube::parseArtist(const QString &browseId, const QJsonObject &root)
{
    Artist artist;
    artist.browseId = browseId;

    QJsonValue header = dig(root, { "header", "musicImmersiveHeaderRenderer" });
    // Only an artist's own page has the photograph across the top.
    const bool artistPage = !header.isUndefined();
    artist.artistPage = artistPage;
    if (header.isUndefined())
        header = dig(root, { "header", "musicVisualHeaderRenderer" });
    if (header.isUndefined())
        header = dig(root, { "header", "musicHeaderRenderer" });

    artist.name = joinRuns(dig(header, { "title", "runs" }).toArray()).trimmed();
    artist.description = joinRuns(dig(header, { "description", "runs" }).toArray()).trimmed();
    artist.audience = joinRuns(dig(header, { "monthlyListenerCount", "runs" }).toArray()).trimmed();
    if (artist.audience.isEmpty()) {
        const QString subscribers = joinRuns(dig(header, { "subscriptionButton", "subscribeButtonRenderer",
                                                           "subscriberCountText", "runs" }).toArray()).trimmed();
        if (!subscribers.isEmpty())
            artist.audience = subscribers + QStringLiteral(" subscribers");
    }
    // Square, as the page shows it; the view asks for the size it draws.
    QJsonArray thumbnails = dig(header, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                          "thumbnails" }).toArray();
    if (thumbnails.isEmpty())
        thumbnails = dig(header, { "foregroundThumbnail", "musicThumbnailRenderer", "thumbnail",
                                   "thumbnails" }).toArray();
    if (!thumbnails.isEmpty())
        artist.artwork = squarePortrait(thumbnails.last().toObject().value(QLatin1String("url")).toString());
    artist.shuffle = watchOf(dig(header, { "playButton", "buttonRenderer", "navigationEndpoint" }));
    artist.radio = watchOf(dig(header, { "startRadioButton", "buttonRenderer", "navigationEndpoint" }));
    noteArtist(artist.name, browseId, artistPage);

    const QJsonArray sections = dig(root, { "contents", "singleColumnBrowseResultsRenderer", "tabs", "#0",
                                            "tabRenderer", "content", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        const QJsonValue songs = dig(section, { "musicShelfRenderer" });
        if (!songs.isUndefined() && artist.songs.isEmpty()) {
            const QJsonArray title = dig(songs, { "title", "runs" }).toArray();
            artist.songsTitle = joinRuns(title).trimmed();
            // "Show all": every song of the artist's, as a playlist page.
            artist.songsId = dig(title, { "#0", "navigationEndpoint", "browseEndpoint", "browseId" }).toString();
            if (artist.songsId.isEmpty())
                artist.songsId = dig(songs, { "bottomEndpoint", "browseEndpoint", "browseId" }).toString();
            for (const QJsonValue &entry : songs.toObject().value(QLatin1String("contents")).toArray()) {
                const Track track = parseListItem(dig(entry, { "musicResponsiveListItemRenderer" }));
                if (!track.videoId.isEmpty() && !track.title.isEmpty())
                    artist.songs.append(track);
            }
            continue;
        }

        const QJsonValue carousel = dig(section, { "musicCarouselShelfRenderer" });
        if (!carousel.isUndefined()) {
            Shelf shelf = parseCarousel(carousel);
            dropUnopenable(shelf);
            if (!shelf.cards.isEmpty())
                artist.shelves.append(shelf);
            continue;
        }

        // The "About" shelf at the foot, for a header that has none.
        const QJsonValue about = dig(section, { "musicDescriptionShelfRenderer" });
        if (!about.isUndefined() && artist.description.isEmpty())
            artist.description = joinRuns(dig(about, { "description", "runs" }).toArray()).trimmed();
    }
    return artist;
}

// A search for artists: one shelf of rows, each an artist's page.
QList<InnerTube::ArtistHit> InnerTube::parseArtistSearch(const QJsonObject &root)
{
    QList<ArtistHit> hits;
    const QJsonArray sections = dig(root, { "contents", "tabbedSearchResultsRenderer", "tabs", "#0",
                                            "tabRenderer", "content", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        for (const QJsonValue &entry : dig(section, { "musicShelfRenderer", "contents" }).toArray()) {
            const QJsonValue row = dig(entry, { "musicResponsiveListItemRenderer" });
            bool artistPage = false;
            ArtistHit hit;
            hit.browseId = artistLinkOf(row, &artistPage);
            if (hit.browseId.isEmpty())
                continue;
            hit.name = joinRuns(dig(row, { "flexColumns", "#0", "musicResponsiveListItemFlexColumnRenderer",
                                           "text", "runs" }).toArray()).trimmed();
            const QJsonArray thumbnails = dig(row, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                                     "thumbnails" }).toArray();
            if (!thumbnails.isEmpty())
                hit.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
            if (hit.name.isEmpty())
                continue;
            noteArtist(hit.name, hit.browseId, artistPage);
            hits.append(hit);
        }
    }
    return hits;
}

// A search for albums, artists or playlists: one shelf of rows, each opening
// a page. The line under the name is kept whole ("Single • Radiohead •
// 2021", "Artist • 217M monthly audience", "YouTube Music • 126 songs"), as
// a card's is, so the view can take the kind from its front; the names in
// it are read as well, so each one's page is known from here on.
QList<InnerTube::Card> InnerTube::parseCardSearch(const QJsonObject &root)
{
    QList<Card> cards;
    const QJsonArray sections = dig(root, { "contents", "tabbedSearchResultsRenderer", "tabs", "#0",
                                            "tabRenderer", "content", "sectionListRenderer",
                                            "contents" }).toArray();
    for (const QJsonValue &section : sections) {
        for (const QJsonValue &entry : dig(section, { "musicShelfRenderer", "contents" }).toArray()) {
            const QJsonValue row = dig(entry, { "musicResponsiveListItemRenderer" });
            const QJsonValue endpoint = dig(row, { "navigationEndpoint" });
            Card card;
            card.browseId = dig(endpoint, { "browseEndpoint", "browseId" }).toString();
            card.type = pageTypeOf(endpoint);
            // A podcast or a profile: a page this app does not have.
            if (card.browseId.isEmpty() || card.type.isEmpty())
                continue;
            const QJsonArray columns = row.toObject().value(QLatin1String("flexColumns")).toArray();
            card.title = joinRuns(dig(columns.at(0), { "musicResponsiveListItemFlexColumnRenderer",
                                                       "text", "runs" }).toArray()).trimmed();
            const QJsonArray subtitle = dig(columns.at(1), { "musicResponsiveListItemFlexColumnRenderer",
                                                             "text", "runs" }).toArray();
            card.subtitle = joinRuns(subtitle).trimmed();
            const QJsonArray thumbnails = dig(row, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                                     "thumbnails" }).toArray();
            if (!thumbnails.isEmpty())
                card.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
            if (card.title.isEmpty())
                continue;
            if (card.type == QLatin1String("artist")) {
                bool artistPage = false;
                artistLinkOf(row, &artistPage);
                noteArtist(card.title, card.browseId, artistPage);
            } else {
                Track credits;
                parseSubtitle(subtitle, credits);   // tells ArtistLinks the names it links
            }
            cards.append(card);
        }
    }
    return cards;
}

void InnerTube::searchCards(const QString &query, Filter filter,
                            std::function<void(const QList<Card> &, const QString &)> done)
{
    const QJsonObject body{
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), filterParams(filter) }
    };
    send(Client::Music, QStringLiteral("search"), body, kSearchTimeoutMs, /*slot=*/nullptr,
         [done](const QJsonObject &root, const QString &error) {
             if (!error.isEmpty())
                 done({}, error);
             else
                 done(parseCardSearch(root), QString());
         });
}

void InnerTube::searchTracks(const QString &query, Filter filter,
                             std::function<void(const QList<Track> &, const QString &)> done)
{
    const QJsonObject body{
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), filter == Filter::Videos ? kVideosFilter : kSongsFilter }
    };
    send(Client::Music, QStringLiteral("search"), body, kSearchTimeoutMs, /*slot=*/nullptr,
         [done](const QJsonObject &root, const QString &error) {
             if (!error.isEmpty())
                 done({}, error);
             else
                 done(parseSearch(root), QString());
         });
}

void InnerTube::searchArtists(const QString &query,
                              std::function<void(const QList<ArtistHit> &, const QString &)> done)
{
    const QJsonObject body{
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), kArtistsFilter }
    };
    send(Client::Music, QStringLiteral("search"), body, kSearchTimeoutMs, /*slot=*/nullptr,
         [done](const QJsonObject &root, const QString &error) {
             if (!error.isEmpty())
                 done({}, error);
             else
                 done(parseArtistSearch(root), QString());
         });
}

// The same call as the radio's, for a playlist YouTube Music named itself:
// an artist's shuffle ("RDAO…") or mix ("RDEM…"), answered as a queue.
void InnerTube::watchPlaylist(const Watch &watch,
                              std::function<void(const QList<Track> &, const QString &)> done)
{
    if (watch.playlistId.isEmpty() && watch.videoId.isEmpty()) {
        done({}, QStringLiteral("nothing to play"));
        return;
    }
    QJsonObject body{
        { QStringLiteral("isAudioOnly"), true },
        { QStringLiteral("enablePersistentPlaylistPanel"), true }
    };
    if (!watch.playlistId.isEmpty())
        body.insert(QStringLiteral("playlistId"), watch.playlistId);
    if (!watch.videoId.isEmpty())
        body.insert(QStringLiteral("videoId"), watch.videoId);
    if (!watch.params.isEmpty())
        body.insert(QStringLiteral("params"), watch.params);
    send(Client::Music, QStringLiteral("next"), body, kSearchTimeoutMs, /*slot=*/nullptr,
         [done](const QJsonObject &root, const QString &error) {
             if (!error.isEmpty()) {
                 done({}, error);
                 return;
             }
             const QList<Track> tracks = parseRadio(root);
             done(tracks, tracks.isEmpty() ? QStringLiteral("YouTube Music sent no songs for it.") : QString());
         });
}

QList<InnerTube::Track> InnerTube::parseRadio(const QJsonObject &root)
{
    QList<Track> tracks;
    const QJsonArray entries = dig(root, { "contents", "singleColumnMusicWatchNextResultsRenderer",
                                           "tabbedRenderer", "watchNextTabbedResultsRenderer", "tabs", "#0",
                                           "tabRenderer", "content", "musicQueueRenderer", "content",
                                           "playlistPanelRenderer", "contents" }).toArray();
    for (const QJsonValue &entry : entries) {
        // A song with a music-video counterpart comes wrapped, the audio
        // version as the primary.
        QJsonValue item = dig(entry, { "playlistPanelVideoRenderer" });
        if (item.isUndefined())
            item = dig(entry, { "playlistPanelVideoWrapperRenderer", "primaryRenderer",
                                "playlistPanelVideoRenderer" });

        Track track;
        track.videoId = dig(item, { "videoId" }).toString();
        if (track.videoId.isEmpty())
            continue;
        track.isVideo = isRealVideo(dig(item, { "navigationEndpoint", "watchEndpoint" }));
        track.title = joinRuns(dig(item, { "title", "runs" }).toArray()).trimmed();
        parseSubtitle(dig(item, { "longBylineText", "runs" }).toArray(), track);
        const QString length = joinRuns(dig(item, { "lengthText", "runs" }).toArray()).trimmed();
        if (!length.isEmpty())
            track.durationMs = parseClock(length);
        const QJsonArray thumbnails = dig(item, { "thumbnail", "thumbnails" }).toArray();
        if (!thumbnails.isEmpty())
            track.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
        tracks.append(track);
    }
    return tracks;
}

QStringList InnerTube::parseSuggestions(const QJsonObject &root)
{
    QStringList suggestions;
    for (const QJsonValue &section : root.value(QLatin1String("contents")).toArray()) {
        const QJsonArray entries = dig(section, { "searchSuggestionsSectionRenderer", "contents" }).toArray();
        for (const QJsonValue &entry : entries) {
            const QString text = joinRuns(dig(entry, { "searchSuggestionRenderer", "suggestion",
                                                       "runs" }).toArray()).trimmed();
            if (!text.isEmpty() && !suggestions.contains(text))
                suggestions << text;
        }
    }
    return suggestions;
}
