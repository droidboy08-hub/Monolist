#include "innertube.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <initializer_list>

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

const QByteArray kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/149.0.0.0 Safari/537.36";

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
    const bool music = client == InnerTube::Client::Music;
    return QJsonObject{
        { QStringLiteral("client"), QJsonObject{
              { QStringLiteral("clientName"), music ? QStringLiteral("WEB_REMIX") : QStringLiteral("WEB") },
              { QStringLiteral("clientVersion"), music ? QStringLiteral("1.%1.01.00").arg(today)
                                                       : QStringLiteral("2.%1.00.00").arg(today) },
              { QStringLiteral("hl"), language },
              { QStringLiteral("gl"), region } } }
    };
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

bool isTypeLabel(const QString &text)
{
    static const QStringList labels = {
        QStringLiteral("Song"), QStringLiteral("Video"), QStringLiteral("Episode"),
        QStringLiteral("Podcast"), QStringLiteral("Album"), QStringLiteral("Single"),
        QStringLiteral("EP"), QStringLiteral("Artist"), QStringLiteral("Playlist")
    };
    return labels.contains(text);
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
        for (const QJsonValue &run : group) {
            pageType = dig(run, { "navigationEndpoint", "browseEndpoint",
                                  "browseEndpointContextSupportedConfigs",
                                  "browseEndpointContextMusicConfig", "pageType" }).toString();
            if (!pageType.isEmpty())
                break;
        }
        if (pageType == QLatin1String("MUSIC_PAGE_TYPE_ALBUM"))
            track.album = text;
        else if (pageType.isEmpty() && !artists.isEmpty())
            continue;   // an unlinked line after the artist: a date, a count
        else
            artists << text;   // an artist or channel page, or an artist without one
    }
    track.artist = artists.join(QStringLiteral(", "));
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
    } else {
        card.browseId = dig(endpoint, { "browseEndpoint", "browseId" }).toString();
        card.type = pageTypeOf(endpoint);
    }
    return card;
}

} // namespace

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

InnerTube::InnerTube(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    // Open the TLS connection now, so the first search is one round trip
    // like every later one, rather than paying for DNS and the handshake.
    m_network->connectToHostEncrypted(QStringLiteral("music.youtube.com"));
}

QNetworkReply *InnerTube::post(Client client, const QString &endpoint, QJsonObject body, int timeoutMs)
{
    const bool music = client == Client::Music;
    const QString host = music ? QStringLiteral("https://music.youtube.com")
                               : QStringLiteral("https://www.youtube.com");
    body.insert(QStringLiteral("context"), clientContext(client));
    QNetworkRequest request(QUrl(host + QStringLiteral("/youtubei/v1/") + endpoint
                                 + QStringLiteral("?prettyPrint=false")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Origin", host.toUtf8());
    request.setRawHeader("Referer", (host + QLatin1Char('/')).toUtf8());
    request.setTransferTimeout(timeoutMs);
    return m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void InnerTube::send(Client client, const QString &endpoint, const QJsonObject &body, int timeoutMs,
                     QPointer<QNetworkReply> *slot,
                     std::function<void(const QJsonObject &, const QString &)> done, int retries)
{
    QNetworkReply *reply = post(client, endpoint, body, timeoutMs);
    if (slot)
        *slot = reply;

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, client, endpoint, body, timeoutMs, slot, done, retries]() {
                reply->deleteLater();
                // A newer request of the same kind took this one's place, or it
                // was cancelled: whoever did that has already moved on.
                if (slot && *slot != reply)
                    return;
                if (slot)
                    *slot = nullptr;

                if (reply->error() != QNetworkReply::NoError) {
                    // A country YouTube Music does not serve is refused with
                    // 400, every time, for everything. Rather than look
                    // broken, drop the country and ask again as the system.
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (status == 400 && !g_region.isEmpty()) {
                        const QString refused = g_region;
                        g_region.clear();
                        if (g_regionRejected)
                            g_regionRejected(refused);
                        send(client, endpoint, body, timeoutMs, slot, done, retries);
                        return;
                    }
                    // A dropped connection or a timeout is ordinary on a home
                    // connection; the second attempt usually works.
                    if (retries > 0 && reply->error() != QNetworkReply::OperationCanceledError) {
                        QTimer::singleShot(1200, this, [this, client, endpoint, body, timeoutMs,
                                                        slot, done, retries]() {
                            send(client, endpoint, body, timeoutMs, slot, done, retries - 1);
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

void InnerTube::search(const QString &query, Filter filter)
{
    // Detach before aborting: abort() delivers finished() synchronously, and
    // that handler must see itself as superseded, not as a failed search.
    if (QNetworkReply *previous = m_search) {
        m_search = nullptr;
        previous->abort();
    }

    const QJsonObject body{
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), filter == Filter::Songs ? kSongsFilter : kVideosFilter }
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
    if (QNetworkReply *previous = m_search) {
        m_search = nullptr;
        previous->abort();
    }

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
    if (QNetworkReply *previous = m_suggest) {
        m_suggest = nullptr;
        previous->abort();
    }

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
    if (QNetworkReply *reply = m_radio) {
        m_radio = nullptr;
        reply->abort();
    }
}

void InnerTube::cancelSearch()
{
    if (QNetworkReply *reply = m_search) {
        m_search = nullptr;
        reply->abort();
    }
}

void InnerTube::cancelSuggestions()
{
    if (QNetworkReply *reply = m_suggest) {
        m_suggest = nullptr;
        reply->abort();
    }
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
                       std::function<void(const QJsonObject &, const QString &)> done)
{
    // A page is bigger than a search (the home feed is a few hundred KB), so
    // it is given longer before the connection is called dead.
    send(Client::Music, QStringLiteral("browse"), { { QStringLiteral("browseId"), browseId } },
         kBrowseTimeoutMs, nullptr, std::move(done));
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

        Shelf shelf;
        const QJsonValue header = dig(carousel, { "header", "musicCarouselShelfBasicHeaderRenderer" });
        shelf.title = joinRuns(dig(header, { "title", "runs" }).toArray()).trimmed();
        shelf.strapline = joinRuns(dig(header, { "strapline", "runs" }).toArray()).trimmed();

        for (const QJsonValue &entry : carousel.toObject().value(QLatin1String("contents")).toArray()) {
            const QJsonValue twoRow = dig(entry, { "musicTwoRowItemRenderer" });
            if (!twoRow.isUndefined()) {
                const Card card = parseCard(twoRow);
                if (!card.title.isEmpty() && (!card.browseId.isEmpty() || !card.videoId.isEmpty()))
                    shelf.cards.append(card);
                continue;
            }
            const QJsonValue row = dig(entry, { "musicResponsiveListItemRenderer" });
            if (row.isUndefined())
                continue;
            const Track track = parseListItem(row);
            if (!track.videoId.isEmpty()) {
                shelf.songs.append(track);
            } else {
                // A row that opens a page rather than playing: a charted artist.
                Card card;
                card.title = track.title;
                card.subtitle = track.artist;
                card.artwork = track.artwork;
                card.browseId = dig(row, { "navigationEndpoint", "browseEndpoint", "browseId" }).toString();
                card.type = pageTypeOf(dig(row, { "navigationEndpoint" }));
                if (!card.browseId.isEmpty())
                    shelf.cards.append(card);
            }
        }
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
    collection.details = joinRuns(dig(header, { "secondSubtitle", "runs" }).toArray()).trimmed();
    collection.description = joinRuns(dig(header, { "description", "musicDescriptionShelfRenderer",
                                                    "description", "runs" }).toArray()).trimmed();
    const QJsonArray thumbnails = dig(header, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                                "thumbnails" }).toArray();
    if (!thumbnails.isEmpty())
        collection.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());
    collection.type = browseId.startsWith(QLatin1String("MPRE")) ? QStringLiteral("album")
                                                                 : QStringLiteral("playlist");

    const QJsonValue shelf = dig(root, { "contents", "twoColumnBrowseResultsRenderer", "secondaryContents",
                                         "sectionListRenderer", "contents", "#0" });
    QJsonArray rows = dig(shelf, { "musicShelfRenderer", "contents" }).toArray();
    if (rows.isEmpty())
        rows = dig(shelf, { "musicPlaylistShelfRenderer", "contents" }).toArray();

    const bool album = collection.type == QLatin1String("album");
    for (const QJsonValue &entry : rows) {
        Track track = parseListItem(dig(entry, { "musicResponsiveListItemRenderer" }));
        if (track.videoId.isEmpty() || track.title.isEmpty())
            continue;   // unavailable in this region, or not a song
        // An album's rows leave out what the header already says.
        if (track.artist.isEmpty())
            track.artist = collection.artist;
        if (album) {
            if (track.album.isEmpty())
                track.album = collection.title;
            track.artwork = collection.artwork;
        } else if (track.artwork.isEmpty()) {
            track.artwork = collection.artwork;
        }
        collection.tracks.append(track);
    }
    return collection;
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
