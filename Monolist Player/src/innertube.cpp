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

// The web client's own locale, so results follow the user's region.
QJsonObject clientContext()
{
    const QLocale locale = QLocale::system();
    QString language = locale.name().section(QLatin1Char('_'), 0, 0);
    QString region = QLocale::territoryToCode(locale.territory());
    if (language.isEmpty() || language == QLatin1String("C"))
        language = QStringLiteral("en");
    if (region.isEmpty())
        region = QStringLiteral("US");
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

} // namespace

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
            const QJsonValue item = dig(entry, { "musicResponsiveListItemRenderer" });

            Track track;
            track.videoId = dig(item, { "playlistItemData", "videoId" }).toString();
            if (track.videoId.isEmpty()) {
                track.videoId = dig(item, { "overlay", "musicItemThumbnailOverlayRenderer", "content",
                                            "musicPlayButtonRenderer", "playNavigationEndpoint",
                                            "watchEndpoint", "videoId" }).toString();
            }
            if (track.videoId.isEmpty())
                continue;   // an album, artist or playlist: nothing to play directly

            const QJsonArray columns = item.toObject().value(QLatin1String("flexColumns")).toArray();
            track.title = joinRuns(dig(columns.at(0), { "musicResponsiveListItemFlexColumnRenderer",
                                                        "text", "runs" }).toArray()).trimmed();
            parseSubtitle(dig(columns.at(1), { "musicResponsiveListItemFlexColumnRenderer",
                                               "text", "runs" }).toArray(), track);

            const QJsonArray thumbnails = dig(item, { "thumbnail", "musicThumbnailRenderer", "thumbnail",
                                                      "thumbnails" }).toArray();
            if (!thumbnails.isEmpty())
                track.artwork = largerArtwork(thumbnails.last().toObject().value(QLatin1String("url")).toString());

            if (!track.title.isEmpty())
                tracks.append(track);
        }
    }
    return tracks;
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
