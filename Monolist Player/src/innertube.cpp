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

#include <initializer_list>

namespace {

const QString kEndpoint = QStringLiteral("https://music.youtube.com/youtubei/v1/");

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

// The web client's own locale, so results follow the user's language and the
// region they are browsing.
QJsonObject clientContext()
{
    const QLocale locale = QLocale::system();
    QString language = locale.name().section(QLatin1Char('_'), 0, 0);
    const QString region = InnerTube::region();
    if (language.isEmpty() || language == QLatin1String("C"))
        language = QStringLiteral("en");
    // "1.<today>.01.00" is the shape the web client reports its version in.
    const QString version = QStringLiteral("1.%1.01.00")
                                .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    return QJsonObject{
        { QStringLiteral("client"), QJsonObject{
              { QStringLiteral("clientName"), QStringLiteral("WEB_REMIX") },
              { QStringLiteral("clientVersion"), version },
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
InnerTube::Track parseListItem(const QJsonValue &item)
{
    InnerTube::Track track;
    track.videoId = dig(item, { "playlistItemData", "videoId" }).toString();
    if (track.videoId.isEmpty()) {
        track.videoId = dig(item, { "overlay", "musicItemThumbnailOverlayRenderer", "content",
                                    "musicPlayButtonRenderer", "playNavigationEndpoint",
                                    "watchEndpoint", "videoId" }).toString();
    }

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

QString InnerTube::systemRegion()
{
    const QString code = QLocale::territoryToCode(QLocale::system().territory());
    return code.size() == 2 ? code : QStringLiteral("US");
}

QString InnerTube::region()
{
    return g_region.isEmpty() ? systemRegion() : g_region;
}

InnerTube::InnerTube(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    // Open the TLS connection now, so the first search is one round trip
    // like every later one, rather than paying for DNS and the handshake.
    m_network->connectToHostEncrypted(QStringLiteral("music.youtube.com"));
}

QNetworkReply *InnerTube::post(const QString &endpoint, QJsonObject body)
{
    body.insert(QStringLiteral("context"), clientContext());
    QNetworkRequest request(QUrl(kEndpoint + endpoint + QStringLiteral("?prettyPrint=false")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Origin", "https://music.youtube.com");
    request.setRawHeader("Referer", "https://music.youtube.com/");
    request.setTransferTimeout(8000);
    return m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void InnerTube::search(const QString &query, Filter filter)
{
    // Detach before aborting: abort() delivers finished() synchronously, and
    // that handler must see itself as superseded, not as a failed search.
    if (QNetworkReply *previous = m_search) {
        m_search = nullptr;
        previous->abort();
    }

    QNetworkReply *reply = post(QStringLiteral("search"), {
        { QStringLiteral("query"), query },
        { QStringLiteral("params"), filter == Filter::Songs ? kSongsFilter : kVideosFilter }
    });
    m_search = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, query]() {
        reply->deleteLater();
        if (reply != m_search)
            return;
        m_search = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT searchFailed(query, reply->errorString());
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Q_EMIT searchFailed(query, QStringLiteral("YouTube Music sent a response that is not JSON."));
            return;
        }
        Q_EMIT searchFinished(query, parseSearch(document.object()));
    });
}

void InnerTube::suggest(const QString &input)
{
    if (QNetworkReply *previous = m_suggest) {
        m_suggest = nullptr;
        previous->abort();
    }

    QNetworkReply *reply = post(QStringLiteral("music/get_search_suggestions"),
                                { { QStringLiteral("input"), input } });
    m_suggest = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, input]() {
        reply->deleteLater();
        if (reply != m_suggest)
            return;
        m_suggest = nullptr;
        if (reply->error() != QNetworkReply::NoError)
            return;   // suggestions are a nicety; a failure just shows none
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        Q_EMIT suggestionsReady(input, parseSuggestions(document.object()));
    });
}

void InnerTube::radio(const QString &videoId)
{
    cancelRadio();

    // "RDAMVM" + id is the radio playlist YouTube Music starts from a song;
    // "wAEB" asks for it in radio mode, as its own client does.
    QNetworkReply *reply = post(QStringLiteral("next"), {
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("playlistId"), QStringLiteral("RDAMVM") + videoId },
        { QStringLiteral("params"), QStringLiteral("wAEB") },
        { QStringLiteral("isAudioOnly"), true },
        { QStringLiteral("enablePersistentPlaylistPanel"), true }
    });
    m_radio = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, videoId]() {
        reply->deleteLater();
        if (reply != m_radio)
            return;
        m_radio = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT radioFailed(videoId, reply->errorString());
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QList<Track> tracks = parseRadio(document.object());
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

void InnerTube::browse(const QString &browseId,
                       std::function<void(const QJsonObject &, const QString &)> done)
{
    QNetworkReply *reply = post(QStringLiteral("browse"), { { QStringLiteral("browseId"), browseId } });
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, reply->errorString());
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (!document.isObject()) {
            done({}, QStringLiteral("YouTube Music sent a response that is not JSON."));
            return;
        }
        done(document.object(), QString());
    });
}

void InnerTube::lyrics(const QString &videoId,
                       std::function<void(const QString &, const QString &, const QString &)> done)
{
    QNetworkReply *reply = post(QStringLiteral("next"), {
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("isAudioOnly"), true }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, {}, reply->errorString());
            return;
        }
        // The watch page's second tab is Lyrics; without a browse id it is
        // greyed out, and the song has none.
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
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
