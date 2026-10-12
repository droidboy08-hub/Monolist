#include "importsource.h"

#include "innertube.h"
#include "jiosaavn.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace {

// Where every request but YouTube's goes in a self-test; empty is the world.
QString g_testServer;

// Long enough for Apple Music's 1.7 MB page on a slow connection.
constexpr int kTimeoutMs = 20000;
// JioSaavn's and Deezer's pages: a hundred songs each, as the research read them.
constexpr int kPageRows = 100;
// Spotify's embed page holds the first 100 songs of a list and no more.
constexpr int kSpotifyEmbedRows = 100;

// Who is asking, honestly: the app and where it lives.
QByteArray userAgent()
{
    return QStringLiteral("Monolist/%1 (+https://github.com/droidboy08-hub/Monolist)")
        .arg(QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0.1")
                                                              : QCoreApplication::applicationVersion())
        .toUtf8();
}

// Walks object keys and "#n" array indices, as innertube.cpp's does.
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

// A JSON field that may come as a string or a number.
QString text(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toInteger());
    return {};
}

// The first number in "1,234 songs • 8+ hours" or "50 songs, 2 hours".
int leadingCount(const QString &details)
{
    static const QRegularExpression number(QStringLiteral("(\\d[\\d,.\\x{202F}\\x{00A0}]*)"));
    const QRegularExpressionMatch match = number.match(details);
    if (!match.hasMatch())
        return 0;
    QString digits = match.captured(1);
    digits.remove(QRegularExpression(QStringLiteral("[^\\d]")));
    return digits.toInt();
}

// How many songs a YouTube list's header says it holds: the number before
// "songs" or "tracks" in "2.8K views • 305 tracks • 20+ hours", never the
// views'. 0 when it says it in no way this can read (another language).
int songCount(const QString &details)
{
    static const QRegularExpression count(QStringLiteral("(\\d[\\d,.\\x{202F}\\x{00A0}]*)\\s*(songs?|tracks?|videos?|episodes?)\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = count.match(details);
    if (!match.hasMatch())
        return 0;
    QString digits = match.captured(1);
    digits.remove(QRegularExpression(QStringLiteral("[^\\d]")));
    return digits.toInt();
}

// A script element's JSON, by its id, whatever order its attributes come in.
QByteArray scriptById(const QByteArray &html, const QByteArray &id)
{
    const qsizetype at = html.indexOf("id=\"" + id + "\"");
    if (at < 0)
        return {};
    const qsizetype open = html.lastIndexOf("<script", at);
    const qsizetype start = html.indexOf('>', at);
    if (open < 0 || start < 0)
        return {};
    const qsizetype end = html.indexOf("</script>", start);
    if (end < 0)
        return {};
    return html.mid(start + 1, end - start - 1);
}

QString platformName(const QString &platform)
{
    if (platform == QLatin1String("ytmusic"))
        return QStringLiteral("YouTube Music");
    if (platform == QLatin1String("youtube"))
        return QStringLiteral("YouTube");
    if (platform == QLatin1String("spotify"))
        return QStringLiteral("Spotify");
    if (platform == QLatin1String("jiosaavn"))
        return QStringLiteral("JioSaavn");
    if (platform == QLatin1String("deezer"))
        return QStringLiteral("Deezer");
    if (platform == QLatin1String("apple"))
        return QStringLiteral("Apple Music");
    return platform;
}

Import::Link accepted(Import::Link link, const QString &platform, const QString &type, const QString &id)
{
    link.ok = true;
    link.platform = platform;
    link.type = type;
    link.id = id;
    link.label = platformName(platform) + QLatin1Char(' ') + type;
    return link;
}

Import::Link refused(Import::Link link, const QString &platform, const QString &reason)
{
    link.ok = false;
    link.platform = platform;
    link.reason = reason;
    return link;
}

const QString kNotYet = QStringLiteral(
    "Monolist can't read %1 playlists yet. Export the playlist as a CSV file (TuneMyMusic can) and open it "
    "here, or paste its list of songs.");
const QString kShortLink = QStringLiteral(
    "That's a %1 short link. Open it in your browser, and paste the address it goes to.");
const QString kUnknown = QStringLiteral(
    "Monolist doesn't know that link. It can import playlists and albums from YouTube, YouTube Music, Spotify, "
    "JioSaavn, Deezer and Apple Music, a pasted list of songs, or a CSV file.");

// — YouTube —

Import::Link youtubeList(Import::Link link, const QString &platform, QString list)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9_-]{2,}$"));
    if (list.startsWith(QLatin1String("VL")) && list.size() > 2)
        list = list.mid(2);
    if (!shape.match(list).hasMatch())
        return refused(link, platform, QStringLiteral("That link's playlist id doesn't look right. Copy the "
                                                      "playlist's link again (Share, then Copy link)."));
    if (list == QLatin1String("LL") || list == QLatin1String("WL") || list == QLatin1String("LM"))
        return refused(link, platform, QStringLiteral(
            "That's your own Liked or Watch later list, which only your account can open, and an import never "
            "uses your account. Copy it into a playlist of its own on YouTube, make that Public or Unlisted, "
            "and paste its link."));
    // YouTube Music's own playlists begin RDCLAK5uy_ and are lists like any
    // other; every other RD… is a mix made up as it plays.
    if (list.startsWith(QLatin1String("RD")) && !list.startsWith(QLatin1String("RDCLAK5uy_")))
        return refused(link, platform, QStringLiteral(
            "That's a mix YouTube makes up as you listen, so it has no fixed list of songs to import. Save it as "
            "a playlist on YouTube first, or paste a playlist's link."));
    if (list.startsWith(QLatin1String("OLAK5uy_")))
        return accepted(link, platform, QStringLiteral("album"), QStringLiteral("VL") + list);
    return accepted(link, platform, QStringLiteral("playlist"), QStringLiteral("VL") + list);
}

Import::Link youtubeLink(Import::Link link, const QUrl &url, const QString &host, const QStringList &path)
{
    const QString platform = host.startsWith(QLatin1String("music.")) ? QStringLiteral("ytmusic")
                                                                     : QStringLiteral("youtube");
    const QString list = QUrlQuery(url).queryItemValue(QStringLiteral("list"));
    if (!list.isEmpty())
        return youtubeList(link, platform, list);
    const QString first = path.value(0);
    if (first == QLatin1String("browse") && path.size() >= 2) {
        const QString id = path.at(1);
        if (id.startsWith(QLatin1String("MPREb_")))
            return accepted(link, platform, QStringLiteral("album"), id);
        if (id.startsWith(QLatin1String("VL")))
            return youtubeList(link, platform, id);
        if (id.startsWith(QLatin1String("UC")))
            return refused(link, platform, QStringLiteral("That's an artist's or a channel's page, not a playlist. "
                                                          "Open one of its playlists or albums and paste that link."));
        return refused(link, platform, QStringLiteral("That page isn't a playlist or an album."));
    }
    if (first == QLatin1String("channel") || first == QLatin1String("c") || first == QLatin1String("user")
        || first.startsWith(QLatin1Char('@')))
        return refused(link, platform, QStringLiteral("That's a channel, not a playlist. Open one of its playlists "
                                                      "and paste that link."));
    if (first == QLatin1String("watch") || first == QLatin1String("shorts") || host == QLatin1String("youtu.be"))
        return refused(link, platform, QStringLiteral("That's a link to one video, not a playlist. Paste a "
                                                      "playlist's link: it has \"list=\" in it."));
    return refused(link, platform, QStringLiteral("That YouTube link isn't a playlist or an album. A playlist's "
                                                  "link has \"list=\" in it."));
}

// — Spotify —

Import::Link spotifyLink(Import::Link link, const QString &type, const QString &id)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9]{22}$"));
    if (type == QLatin1String("artist") || type == QLatin1String("show") || type == QLatin1String("episode")
        || type == QLatin1String("user") || type == QLatin1String("genre"))
        return refused(link, QStringLiteral("spotify"), QStringLiteral(
            "Monolist can import a Spotify playlist, album or song, not an artist's page or a podcast."));
    if (type != QLatin1String("playlist") && type != QLatin1String("album") && type != QLatin1String("track"))
        return refused(link, QStringLiteral("spotify"), QStringLiteral(
            "That Spotify link isn't a playlist, an album or a song."));
    if (!shape.match(id).hasMatch())
        return refused(link, QStringLiteral("spotify"), QStringLiteral(
            "That Spotify link's id doesn't look right. Copy the link again (Share, then Copy link)."));
    return accepted(link, QStringLiteral("spotify"),
                    type == QLatin1String("track") ? QStringLiteral("song") : type, id);
}

// — JioSaavn —

Import::Link saavnLink(Import::Link link, const QStringList &path)
{
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_,.\\-]{3,}$"));
    const QString first = path.value(0);
    QString type;
    if (first == QLatin1String("featured") || (first == QLatin1String("s") && path.value(1) == QLatin1String("playlist")))
        type = QStringLiteral("playlist");
    else if (first == QLatin1String("album"))
        type = QStringLiteral("album");
    else if (first == QLatin1String("song"))
        type = QStringLiteral("song");
    if (type.isEmpty())
        return refused(link, QStringLiteral("jiosaavn"), QStringLiteral(
            "Monolist can import a JioSaavn playlist, album or song. Open one, and paste its link."));
    const QString id = path.constLast();
    if (path.size() < 2 || !token.match(id).hasMatch())
        return refused(link, QStringLiteral("jiosaavn"), QStringLiteral(
            "That JioSaavn link doesn't look whole. Copy it again (Share, then Copy link)."));
    return accepted(link, QStringLiteral("jiosaavn"), type, id);
}

// — Deezer —

Import::Link deezerLink(Import::Link link, QStringList path)
{
    static const QRegularExpression language(QStringLiteral("^[a-z]{2}(-[a-z]{2})?$"));
    if (!path.isEmpty() && language.match(path.first()).hasMatch())
        path.removeFirst();
    const QString type = path.value(0);
    const QString id = path.value(1);
    static const QRegularExpression digits(QStringLiteral("^\\d{1,20}$"));
    if (type != QLatin1String("playlist") && type != QLatin1String("album") && type != QLatin1String("track"))
        return refused(link, QStringLiteral("deezer"), QStringLiteral(
            "Monolist can import a Deezer playlist, album or song, not an artist's page or a podcast."));
    if (!digits.match(id).hasMatch())
        return refused(link, QStringLiteral("deezer"), QStringLiteral(
            "That Deezer link's id doesn't look right. Copy the link again."));
    return accepted(link, QStringLiteral("deezer"), type == QLatin1String("track") ? QStringLiteral("song") : type, id);
}

// — Apple Music —

Import::Link appleLink(Import::Link link, const QUrl &url, QStringList path)
{
    static const QRegularExpression region(QStringLiteral("^[a-z]{2}$"));
    QString storefront = QStringLiteral("us");
    if (!path.isEmpty() && region.match(path.first()).hasMatch())
        storefront = path.takeFirst();
    const QString type = path.value(0);
    if (type == QLatin1String("song"))
        return refused(link, QStringLiteral("apple"), QStringLiteral(
            "Paste the album's link instead: on the album, choose the song's … menu, then Share, then Copy Link."));
    if (type != QLatin1String("playlist") && type != QLatin1String("album"))
        return refused(link, QStringLiteral("apple"), QStringLiteral(
            "Monolist can import an Apple Music playlist or album, not an artist's page or a station."));
    if (path.size() < 2)
        return refused(link, QStringLiteral("apple"), QStringLiteral(
            "That Apple Music link doesn't look whole. Copy it again (Share, then Copy Link)."));
    const QString id = path.constLast();
    static const QRegularExpression playlistId(QStringLiteral("^pl\\.[A-Za-z0-9\\-]+$"));
    static const QRegularExpression albumId(QStringLiteral("^\\d{1,20}$"));
    if (type == QLatin1String("playlist") ? !playlistId.match(id).hasMatch() : !albumId.match(id).hasMatch())
        return refused(link, QStringLiteral("apple"), QStringLiteral(
            "That Apple Music link's id doesn't look right. Copy it again (Share, then Copy Link)."));
    const QString song = QUrlQuery(url).queryItemValue(QStringLiteral("i"));
    link.region = storefront;
    link.slug = path.size() >= 3 ? path.at(1) : QStringLiteral("-");
    if (type == QLatin1String("album") && albumId.match(song).hasMatch()) {
        link.songId = song;
        return accepted(link, QStringLiteral("apple"), QStringLiteral("song"), id);
    }
    return accepted(link, QStringLiteral("apple"), type, id);
}

// — rows —

Import::Row rowFromTrack(const InnerTube::Track &track)
{
    Import::Row row;
    row.title = track.title;
    for (const InnerTube::Credit &credit : track.credits) {
        if (credit.name && !credit.text.trimmed().isEmpty())
            row.artists.append(credit.text.trimmed());
    }
    if (row.artists.isEmpty())
        row.artists = Saavn::splitArtists(track.artist);
    row.album = track.album;
    row.durationMs = track.durationMs;
    row.videoId = track.videoId;
    row.sourceId = track.videoId;
    row.artwork = track.artwork;
    row.isVideo = track.isVideo;
    return row;
}

// JioSaavn's covers come 150 px square in its answers; 500 is there too.
QString largerSaavnImage(QString url)
{
    url.replace(QStringLiteral("150x150"), QStringLiteral("500x500"));
    url.replace(QStringLiteral("50x50"), QStringLiteral("500x500"));
    return url;
}

Import::Row rowFromSaavn(const QJsonObject &song, const QString &album)
{
    const Saavn::Row read = Saavn::rowFrom(song);
    Import::Row row;
    row.title = read.title;
    row.artists = read.artists;
    row.album = read.album.isEmpty() ? album : read.album;
    row.durationMs = qint64(read.durationSec) * 1000;
    row.sourceId = read.id;
    return row;
}

Import::Row rowFromDeezer(const QJsonObject &track, const QString &album)
{
    Import::Row row;
    row.title = track.value(QStringLiteral("title")).toString().trimmed();
    const QString artist = dig(track, { "artist", "name" }).toString().trimmed();
    if (!artist.isEmpty())
        row.artists.append(artist);
    row.album = dig(track, { "album", "title" }).toString().trimmed();
    if (row.album.isEmpty())
        row.album = album;
    row.durationMs = qint64(track.value(QStringLiteral("duration")).toInteger()) * 1000;
    row.isrc = track.value(QStringLiteral("isrc")).toString().trimmed();
    row.sourceId = text(track.value(QStringLiteral("id")));
    return row;
}

// A Deezer `next` link is followed only where it stays: Deezer's API, the
// same list's tracks.
bool deezerNextAllowed(const Import::Link &link, const QUrl &next)
{
    const QString kind = link.type == QLatin1String("album") ? QStringLiteral("album") : QStringLiteral("playlist");
    return next.isValid() && next.scheme() == QLatin1String("https")
           && next.host() == QLatin1String("api.deezer.com")
           && next.path() == QStringLiteral("/%1/%2/tracks").arg(kind, link.id)
           && QUrlQuery(next).queryItemValue(QStringLiteral("index")).toInt() < Import::kMaxRows;
}

QString deezerError(const Import::Link &link, const QJsonObject &error, bool *retryable)
{
    const int code = error.value(QStringLiteral("code")).toInt();
    // 4 is its quota ("Quota limit exceeded"): a pause, then on.
    *retryable = code == 4 || code == 700;
    if (code == 4)
        return QStringLiteral("Deezer asked Monolist to slow down");
    if (code == 800)
        return QStringLiteral("Deezer says this %1 doesn't exist.").arg(link.type);
    return QStringLiteral("Deezer won't show this %1 (it may be private): %2")
        .arg(link.type, error.value(QStringLiteral("message")).toString());
}

// — CSV —

// RFC 4180, as Exportify and spreadsheets write it: quoted fields may hold
// the delimiter, line breaks and doubled quotes.
QList<QStringList> parseDelimited(const QString &data, QChar delimiter)
{
    QList<QStringList> records;
    QStringList record;
    QString field;
    bool quoted = false;
    bool any = false;
    for (qsizetype i = 0; i < data.size(); ++i) {
        const QChar c = data.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < data.size() && data.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == QLatin1Char('"') && field.isEmpty()) {
            quoted = true;
            any = true;
        } else if (c == delimiter) {
            record.append(field);
            field.clear();
            any = true;
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < data.size() && data.at(i + 1) == QLatin1Char('\n'))
                ++i;
            if (any || !field.isEmpty()) {
                record.append(field);
                records.append(record);
            }
            record.clear();
            field.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
    }
    if (any || !field.isEmpty()) {
        record.append(field);
        records.append(record);
    }
    return records;
}

int columnOf(const QStringList &headers, const QStringList &names)
{
    for (const QString &name : names) {
        for (int i = 0; i < headers.size(); ++i) {
            if (headers.at(i) == name)
                return i;
        }
    }
    return -1;
}

// "3:45", "1:02:03", or a number of seconds or (`milliseconds`) ms.
qint64 durationFrom(const QString &value, bool milliseconds)
{
    const QString v = value.trimmed();
    if (v.isEmpty())
        return 0;
    if (v.contains(QLatin1Char(':'))) {
        qint64 seconds = 0;
        for (const QString &part : v.split(QLatin1Char(':'))) {
            bool ok = false;
            const int n = part.trimmed().toInt(&ok);
            if (!ok)
                return 0;
            seconds = seconds * 60 + n;
        }
        return seconds * 1000;
    }
    bool ok = false;
    const double n = v.toDouble(&ok);
    if (!ok || n <= 0)
        return 0;
    // A bare number over 10,000 is in milliseconds whatever the header says.
    return milliseconds || n > 10000 ? qint64(n) : qint64(n * 1000);
}

} // namespace

namespace Import {

QString youtubeTrouble(const QString &error)
{
    if (error.contains(QLatin1String("Too Many Requests")) || error.contains(QLatin1String("429")))
        return QStringLiteral("YouTube Music asked Monolist to slow down");
    if (error.contains(QLatin1String("not JSON")))
        return QStringLiteral("YouTube Music answered with a page that isn't an answer");
    if (error.isEmpty())
        return QStringLiteral("YouTube Music sent an empty answer");
    static const QRegularExpression status(QStringLiteral("server replied: (.+)$"));
    const QRegularExpressionMatch match = status.match(error);
    if (match.hasMatch())
        return QStringLiteral("YouTube Music refused (%1)").arg(match.captured(1).trimmed());
    return QStringLiteral("YouTube Music did not answer");
}

Link parseLink(const QString &input)
{
    Link link;
    const QString trimmed = input.trimmed();
    link.url = trimmed;
    if (trimmed.isEmpty())
        return link;
    if (trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size() > 1) {
        link.looksLikeList = true;
        link.reason = QStringLiteral("That looks like a list of songs; it goes under A list or a CSV file.");
        return link;
    }

    static const QRegularExpression spotifyUri(
        QStringLiteral("^spotify:(?:user:[^:]+:)?(playlist|album|track|artist|show|episode):([A-Za-z0-9]+)$"));
    if (const QRegularExpressionMatch match = spotifyUri.match(trimmed); match.hasMatch())
        return spotifyLink(link, match.captured(1), match.captured(2));

    QString candidate = trimmed;
    if (!candidate.contains(QLatin1String("://"))) {
        // "open.spotify.com/playlist/…", typed or pasted without its scheme.
        static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9.-]+\\.[A-Za-z]{2,}/\\S*$"));
        if (!bare.match(candidate).hasMatch())
            return link;
        candidate.prepend(QStringLiteral("https://"));
    }
    const QUrl url(candidate, QUrl::TolerantMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != QLatin1String("https") && scheme != QLatin1String("http"))) {
        link.reason = kUnknown;
        return link;
    }
    QString host = url.host().toLower();
    for (const QString &prefix : { QStringLiteral("www."), QStringLiteral("m.") }) {
        if (host.startsWith(prefix))
            host = host.mid(prefix.size());
    }
    QStringList path = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);

    if (host == QLatin1String("youtube.com") || host == QLatin1String("music.youtube.com")
        || host == QLatin1String("youtu.be"))
        return youtubeLink(link, url, host, path);

    if (host == QLatin1String("open.spotify.com") || host == QLatin1String("play.spotify.com")) {
        if (!path.isEmpty() && path.first().startsWith(QLatin1String("intl-")))
            path.removeFirst();
        if (!path.isEmpty() && path.first() == QLatin1String("embed"))
            path.removeFirst();
        // The old form: /user/<name>/playlist/<id>.
        if (path.value(0) == QLatin1String("user") && path.value(2) == QLatin1String("playlist"))
            path = path.mid(2);
        return spotifyLink(link, path.value(0), path.value(1));
    }
    if (host == QLatin1String("spotify.link") || host == QLatin1String("spoti.fi"))
        return refused(link, QStringLiteral("spotify"), kShortLink.arg(QStringLiteral("Spotify")));

    if (host == QLatin1String("jiosaavn.com") || host == QLatin1String("saavn.com"))
        return saavnLink(link, path);

    if (host == QLatin1String("deezer.com"))
        return deezerLink(link, path);
    if (host == QLatin1String("deezer.page.link") || host == QLatin1String("link.deezer.com"))
        return refused(link, QStringLiteral("deezer"), kShortLink.arg(QStringLiteral("Deezer")));

    if (host == QLatin1String("music.apple.com") || host == QLatin1String("itunes.apple.com")
        || host == QLatin1String("embed.music.apple.com"))
        return appleLink(link, url, path);

    if (host == QLatin1String("soundcloud.com") || host == QLatin1String("on.soundcloud.com"))
        return refused(link, QStringLiteral("soundcloud"), kNotYet.arg(QStringLiteral("SoundCloud")));
    if (host == QLatin1String("tidal.com") || host == QLatin1String("listen.tidal.com"))
        return refused(link, QStringLiteral("tidal"), kNotYet.arg(QStringLiteral("Tidal")));
    if (host.startsWith(QLatin1String("music.amazon.")) || (host.startsWith(QLatin1String("amazon."))
                                                            && url.path().contains(QLatin1String("music"))))
        return refused(link, QStringLiteral("amazon"), kNotYet.arg(QStringLiteral("Amazon Music")));

    link.reason = kUnknown;
    return link;
}

// ------------------------------------------------------------------ pasted

Page readList(const QString &text)
{
    Page page;
    page.name = QStringLiteral("Imported songs");
    static const QRegularExpression numbering(QStringLiteral("^\\d{1,4}[.)]\\s+"));
    static const QRegularExpression length(
        QStringLiteral("\\s*[\\(\\[]?\\b(\\d{1,2}:\\d{2}(?::\\d{2})?)[\\)\\]]?\\s*$"));
    static const QRegularExpression dash(QStringLiteral("\\s+[-\\x{2013}\\x{2014}]\\s+"));
    static const QRegularExpression by(QStringLiteral("^(.+)\\s+by\\s+(.+)$"), QRegularExpression::CaseInsensitiveOption);
    int noArtist = 0;
    int skipped = 0;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1String("//")))
            continue;
        if (page.rows.size() >= kMaxRows) {
            ++skipped;
            continue;
        }
        Row row;
        line.remove(numbering);
        if (const QRegularExpressionMatch match = length.match(line); match.hasMatch() && match.capturedStart() > 0) {
            row.durationMs = durationFrom(match.captured(1), false);
            line.truncate(match.capturedStart());
            line = line.trimmed();
        }
        if (line.contains(QLatin1Char('\t'))) {
            const QStringList parts = line.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
            row.title = parts.value(0).trimmed();
            if (parts.size() > 1) {
                row.artists = Saavn::splitArtists(parts.at(1));
                row.eitherWay = true;
            }
            row.album = parts.value(2).trimmed();
        } else if (const QRegularExpressionMatch match = dash.match(line); match.hasMatch()) {
            // "Artist - Title" by custom, but the other way round as often.
            row.artists = Saavn::splitArtists(line.left(match.capturedStart()).trimmed());
            row.title = line.mid(match.capturedEnd()).trimmed();
            row.eitherWay = true;
            if (row.artists.size() > 1) {
                // Several names on the left: that side is the artist, surely.
                row.eitherWay = false;
            }
        } else if (const QRegularExpressionMatch match = by.match(line); match.hasMatch()) {
            row.title = match.captured(1).trimmed();
            row.artists = Saavn::splitArtists(match.captured(2));
        } else {
            row.title = line;
        }
        if (row.title.isEmpty())
            continue;
        if (row.artists.isEmpty())
            ++noArtist;
        page.rows.append(row);
    }
    page.total = int(page.rows.size());
    if (page.rows.isEmpty())
        page.error = QStringLiteral("There are no songs in that text. Put one song a line, as \"Artist - Title\".");
    if (noArtist > 0)
        page.warnings << QStringLiteral("%1 of the lines name no artist; songs found for them wait for you to check.")
                             .arg(noArtist);
    if (skipped > 0)
        page.warnings << QStringLiteral("Only the first %1 songs are imported; %2 more were left out.")
                             .arg(kMaxRows).arg(skipped);
    return page;
}

Page readCsv(const QByteArray &data, const QString &fileName)
{
    Page page;
    QString name = fileName;
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    page.name = name.trimmed().isEmpty() ? QStringLiteral("Imported songs") : name.trimmed();

    QString content = QString::fromUtf8(data);
    if (content.startsWith(QChar(0xFEFF)))
        content.remove(0, 1);
    const QString firstLine = content.section(QLatin1Char('\n'), 0, 0);
    QChar delimiter = QLatin1Char(',');
    if (firstLine.count(QLatin1Char('\t')) > firstLine.count(delimiter))
        delimiter = QLatin1Char('\t');
    if (firstLine.count(QLatin1Char(';')) > firstLine.count(delimiter))
        delimiter = QLatin1Char(';');
    const QList<QStringList> records = parseDelimited(content, delimiter);
    if (records.isEmpty()) {
        page.error = QStringLiteral("That file is empty.");
        return page;
    }
    QStringList headers;
    for (const QString &header : records.first())
        headers.append(header.trimmed().toLower());
    const int title = columnOf(headers, { QStringLiteral("track name"), QStringLiteral("track title"),
                                          QStringLiteral("title"), QStringLiteral("song"), QStringLiteral("song name"),
                                          QStringLiteral("song title"), QStringLiteral("name"), QStringLiteral("track") });
    const int artist = columnOf(headers, { QStringLiteral("artist name(s)"), QStringLiteral("artist names"),
                                           QStringLiteral("artist name"), QStringLiteral("artist(s)"),
                                           QStringLiteral("artists"), QStringLiteral("artist"),
                                           QStringLiteral("performer"), QStringLiteral("album artist name(s)") });
    const int album = columnOf(headers, { QStringLiteral("album name"), QStringLiteral("album"),
                                          QStringLiteral("album title"), QStringLiteral("release") });
    int duration = columnOf(headers, { QStringLiteral("track duration (ms)"), QStringLiteral("duration (ms)"),
                                       QStringLiteral("duration_ms"), QStringLiteral("duration ms") });
    const bool inMs = duration >= 0;
    if (duration < 0)
        duration = columnOf(headers, { QStringLiteral("duration"), QStringLiteral("length"), QStringLiteral("time") });
    const int isrc = columnOf(headers, { QStringLiteral("isrc") });
    const int id = columnOf(headers, { QStringLiteral("track uri"), QStringLiteral("spotify uri"),
                                       QStringLiteral("spotify - id"), QStringLiteral("spotify id") });
    if (title < 0) {
        page.error = QStringLiteral("This file has no column named like a song's title (Track Name, Title or Song). "
                                    "Monolist reads CSV files from Exportify and TuneMyMusic, and any with those "
                                    "column names.");
        return page;
    }
    int skipped = 0;
    int noArtist = 0;
    for (qsizetype i = 1; i < records.size(); ++i) {
        const QStringList &record = records.at(i);
        Row row;
        row.title = record.value(title).trimmed();
        if (row.title.isEmpty())
            continue;
        if (page.rows.size() >= kMaxRows) {
            ++skipped;
            continue;
        }
        if (artist >= 0)
            row.artists = Saavn::splitArtists(record.value(artist));
        if (album >= 0)
            row.album = record.value(album).trimmed();
        if (duration >= 0)
            row.durationMs = durationFrom(record.value(duration), inMs);
        if (isrc >= 0)
            row.isrc = record.value(isrc).trimmed().toUpper();
        if (id >= 0)
            row.sourceId = record.value(id).trimmed();
        if (row.artists.isEmpty())
            ++noArtist;
        page.rows.append(row);
    }
    page.total = int(page.rows.size());
    if (page.rows.isEmpty())
        page.error = QStringLiteral("That file has a title column but no songs in it.");
    if (artist < 0)
        page.warnings << QStringLiteral("The file has no artist column; songs found for it wait for you to check.");
    else if (noArtist > 0)
        page.warnings << QStringLiteral("%1 of the songs name no artist; songs found for them wait for you to check.")
                             .arg(noArtist);
    if (skipped > 0)
        page.warnings << QStringLiteral("Only the first %1 songs are imported; %2 more were left out.")
                             .arg(kMaxRows).arg(skipped);
    return page;
}

// ------------------------------------------------------------------ reader

Reader::Reader(InnerTube *innerTube, QObject *parent)
    : QObject(parent)
    , m_innerTube(innerTube)
    , m_network(new QNetworkAccessManager(this))
{
    m_clock.start();
}

void Reader::setTestServer(const QString &base)
{
    g_testServer = base;
}

void Reader::setGap(int gapMs, int jitterMs)
{
    m_gapMs = qMax(0, gapMs);
    m_jitterMs = qMax(0, jitterMs);
}

// Every request waits its turn: the gap, and a little at random, after the
// one before it. The turn is taken when it is given out, so two asked at
// once still go a gap apart.
void Reader::whenFree(std::function<void()> send)
{
    const qint64 now = m_clock.elapsed();
    qint64 at = now;
    if (m_lastRequestMs >= 0)
        at = qMax(now, m_lastRequestMs + m_gapMs
                           + (m_jitterMs > 0 ? QRandomGenerator::global()->bounded(m_jitterMs + 1) : 0));
    m_lastRequestMs = at;
    sendAt(at, std::move(send));
}

// Never before `at`: a coarse timer may fire a little early, and the gap is
// the owner's rule, so the timer is precise and any shortfall is waited out.
// One that fires late moves the turn on with it, so the next request keeps
// its gap from when this one really went.
void Reader::sendAt(qint64 at, std::function<void()> send)
{
    const int wait = int(qMax<qint64>(0, at - m_clock.elapsed()));
    QTimer::singleShot(wait, Qt::PreciseTimer, this, [this, at, send]() {
        const qint64 now = m_clock.elapsed();
        if (now < at) {
            sendAt(at, send);
            return;
        }
        m_lastRequestMs = qMax(m_lastRequestMs, now);
        send();
    });
}

void Reader::get(const QUrl &url, const QByteArray &accept, Answer done)
{
    whenFree([this, url, accept, done]() {
        QUrl target = url;
        if (!g_testServer.isEmpty()) {
            QString local = g_testServer + QLatin1Char('/') + url.host() + url.path(QUrl::FullyEncoded);
            if (url.hasQuery())
                local += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
            target = QUrl(local);
        }
        QNetworkRequest request(target);
        request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
        request.setRawHeader("Accept", accept);
        request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
        // No cookies either way: nothing is sent, nothing is kept.
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        // A redirect is followed only on the same host, over HTTPS.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        request.setTransferTimeout(kTimeoutMs);
        QNetworkReply *reply = m_network->get(request);
        const QString host = target.host();
        connect(reply, &QNetworkReply::redirected, reply, [reply, host](const QUrl &to) {
            const QUrl resolved = reply->url().resolved(to);
            if (resolved.scheme() == QLatin1String("https") && resolved.host().compare(host, Qt::CaseInsensitive) == 0)
                Q_EMIT reply->redirectAllowed();
            else
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [reply, done]() {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll();
            if (status == 0) {
                done(0, QByteArray(), QStringLiteral("no answer (network error %1)").arg(int(reply->error())));
                return;
            }
            done(status, body, status >= 200 && status < 300 ? QString()
                                                               : QStringLiteral("answered HTTP %1").arg(status));
        });
    });
}

void Reader::first(const Link &link, std::function<void(const Page &)> done)
{
    next(link, QString(), std::move(done));
}

void Reader::next(const Link &link, const QString &cursor, std::function<void(const Page &)> done)
{
    if (!link.ok) {
        Page page;
        page.error = link.reason.isEmpty() ? QStringLiteral("That isn't a link Monolist can import.") : link.reason;
        QTimer::singleShot(0, this, [done, page]() { done(page); });
        return;
    }
    if (link.platform == QLatin1String("youtube") || link.platform == QLatin1String("ytmusic"))
        readYouTube(link, cursor, std::move(done));
    else if (link.platform == QLatin1String("spotify"))
        readSpotify(link, std::move(done));
    else if (link.platform == QLatin1String("jiosaavn"))
        readSaavn(link, cursor.isEmpty() ? 1 : qMax(1, cursor.toInt()), std::move(done));
    else if (link.platform == QLatin1String("deezer"))
        readDeezer(link, cursor, std::move(done));
    else if (link.platform == QLatin1String("apple"))
        readApple(link, std::move(done));
    else {
        // A pasted list or a file is read whole before an import starts.
        Page page;
        page.error = QStringLiteral("There is nothing more to read.");
        QTimer::singleShot(0, this, [done, page]() { done(page); });
    }
}

// ---------------------------------------------------------------- YouTube

void Reader::readYouTube(const Link &link, const QString &cursor, std::function<void(const Page &)> done)
{
    const QString token = cursor.section(QLatin1Char('\n'), 0, 0);
    const QString visitor = cursor.section(QLatin1Char('\n'), 1, 1);
    whenFree([this, link, token, visitor, done]() {
        if (!m_innerTube) {
            Page page;
            page.error = QStringLiteral("YouTube Music can't be asked now.");
            page.retryable = true;
            done(page);
            return;
        }
        const auto answered = [token, visitor, link, done](const QJsonObject &root, const QString &error) {
            if (!error.isEmpty()) {
                qWarning("import: YouTube Music's list: %s", qPrintable(error));
                Page page;
                page.error = youtubeTrouble(error);
                page.retryable = true;
                done(page);
                return;
            }
            done(token.isEmpty() ? parseYouTube(link.id, root) : parseYouTubeMore(root, visitor));
        };
        // Always signed out, whatever account is held; never retried at once.
        if (token.isEmpty())
            m_innerTube->browse(link.id, QString(), answered, InnerTube::Auth::Anonymous, /*retries=*/0);
        else
            m_innerTube->continueBrowse(token, answered, InnerTube::Auth::Anonymous, visitor, /*retries=*/0);
    });
}

Page Reader::parseYouTube(const QString &browseId, const QJsonObject &root)
{
    Page page;
    // A list that is private or not there is answered 200 with nothing in
    // it (about 800 bytes, October 2026).
    if (!root.contains(QLatin1String("contents"))) {
        page.error = QStringLiteral("This playlist is private, or it doesn't exist. Make it Public or Unlisted on "
                                    "YouTube, then paste its link again.");
        return page;
    }
    const InnerTube::Collection collection = InnerTube::parseCollection(browseId, root);
    page.name = collection.title;
    page.owner = collection.artist;
    // A playlist's owner is in its header's row of avatars, where an
    // album's artist would be a strapline.
    if (page.owner.isEmpty())
        page.owner = dig(root, { "contents", "twoColumnBrowseResultsRenderer", "tabs", "#0", "tabRenderer", "content",
                                 "sectionListRenderer", "contents", "#0", "musicResponsiveHeaderRenderer", "facepile",
                                 "avatarStackViewModel", "text", "content" }).toString().trimmed();
    page.artwork = collection.artwork;
    page.total = songCount(collection.details);
    for (const InnerTube::Track &track : collection.tracks)
        page.rows.append(rowFromTrack(track));
    if (!collection.continuation.isEmpty())
        page.cursor = collection.continuation + QLatin1Char('\n') + InnerTube::parseVisitorData(root);
    if (page.name.isEmpty() && page.rows.isEmpty()) {
        page.error = QStringLiteral("This playlist is private, or it doesn't exist. Make it Public or Unlisted on "
                                    "YouTube, then paste its link again.");
        return page;
    }
    if (page.rows.isEmpty() && page.cursor.isEmpty())
        page.error = QStringLiteral("This list has no songs that can be played here.");
    if (page.total == 0)
        page.total = int(page.rows.size());
    return page;
}

Page Reader::parseYouTubeMore(const QJsonObject &root, const QString &visitor)
{
    Page page;
    if (!root.contains(QLatin1String("onResponseReceivedActions")) && !root.contains(QLatin1String("continuationContents"))) {
        page.error = QStringLiteral("YouTube Music sent an empty page");
        page.retryable = true;
        return page;
    }
    const InnerTube::Continuation part = InnerTube::parseContinuation(root);
    for (const InnerTube::Track &track : part.tracks)
        page.rows.append(rowFromTrack(track));
    if (!part.next.isEmpty())
        page.cursor = part.next + QLatin1Char('\n') + visitor;
    return page;
}

// ---------------------------------------------------------------- Spotify

void Reader::readSpotify(const Link &link, std::function<void(const Page &)> done)
{
    const QString type = link.type == QLatin1String("song") ? QStringLiteral("track") : link.type;
    const QUrl embed(QStringLiteral("https://open.spotify.com/embed/%1/%2").arg(type, link.id));
    get(embed, "text/html", [this, link, type, done](int status, const QByteArray &body, const QString &error) {
        if (status == 404) {
            Page page;
            page.error = QStringLiteral("Spotify says this %1 doesn't exist, or it's private.").arg(link.type);
            done(page);
            return;
        }
        if (!error.isEmpty()) {
            Page page;
            page.error = QStringLiteral("Spotify did not answer (%1)").arg(error);
            page.retryable = true;
            done(page);
            return;
        }
        Page page = parseSpotifyEmbed(link, body);
        if (!page.error.isEmpty() || link.type == QLatin1String("song") || page.rows.size() < kSpotifyEmbedRows) {
            done(page);
            return;
        }
        // A hundred songs may be all of it or the first hundred of more: the
        // public page says which, once.
        const QUrl open(QStringLiteral("https://open.spotify.com/%1/%2").arg(type, link.id));
        get(open, "text/html", [link, page, done](int, const QByteArray &body, const QString &error) mutable {
            QHash<QString, QString> albums;
            const int total = error.isEmpty() ? spotifyTotal(link, body, &albums) : 0;
            for (Row &row : page.rows) {
                if (row.album.isEmpty())
                    row.album = albums.value(row.sourceId);
            }
            const QString what = link.type == QLatin1String("album") ? QStringLiteral("album") : QStringLiteral("playlist");
            if (total > page.rows.size()) {
                page.total = total;
                page.warnings << QStringLiteral(
                    "Spotify shows only the first %1 of %2 songs without signing in; to bring all, export the %3 "
                    "as CSV (exportify.app) and open it here.").arg(page.rows.size()).arg(total).arg(what);
            } else if (total == 0) {
                page.warnings << QStringLiteral(
                    "Spotify shows only the first %1 songs without signing in, and this %2 may have more; to "
                    "bring all, export it as CSV (exportify.app) and open it here.").arg(page.rows.size()).arg(what);
            }
            done(page);
        });
    });
}

Page Reader::parseSpotifyEmbed(const Link &link, const QByteArray &html)
{
    Page page;
    const QJsonObject data = QJsonDocument::fromJson(scriptById(html, "__NEXT_DATA__")).object();
    const QJsonObject entity = dig(data, { "props", "pageProps", "state", "data", "entity" }).toObject();
    if (entity.isEmpty()) {
        page.error = QStringLiteral("Spotify's page has changed, and Monolist can't read it. Paste the list of "
                                    "songs instead.");
        return page;
    }
    page.name = entity.value(QStringLiteral("name")).toString();
    if (page.name.isEmpty())
        page.name = entity.value(QStringLiteral("title")).toString();
    // The largest picture: the sources come in no promised order. A
    // playlist's are its coverArt; an album's and a song's embed has none,
    // and its pictures are its visualIdentity's, by maxWidth.
    int widest = -1;
    for (const QJsonValue &source : dig(entity, { "coverArt", "sources" }).toArray()) {
        const int width = source.toObject().value(QStringLiteral("width")).toInt(0);
        if (width > widest) {
            widest = width;
            page.artwork = source.toObject().value(QStringLiteral("url")).toString();
        }
    }
    if (page.artwork.isEmpty()) {
        widest = -1;
        for (const QJsonValue &image : dig(entity, { "visualIdentity", "image" }).toArray()) {
            const int width = image.toObject().value(QStringLiteral("maxWidth")).toInt(0);
            const QString url = image.toObject().value(QStringLiteral("url")).toString();
            if (width > widest && !url.isEmpty()) {
                widest = width;
                page.artwork = url;
            }
        }
    }
    const auto splitLine = [](QString line) {
        line.replace(QChar(0x00A0), QLatin1Char(' '));
        QStringList names;
        for (const QString &name : line.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
            if (!name.trimmed().isEmpty())
                names.append(name.trimmed());
        }
        return names;
    };

    if (link.type == QLatin1String("song")) {
        Row row;
        row.title = page.name;
        for (const QJsonValue &artist : entity.value(QStringLiteral("artists")).toArray()) {
            const QString name = artist.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty())
                row.artists.append(name);
        }
        row.durationMs = entity.value(QStringLiteral("duration")).toInteger();
        row.sourceId = entity.value(QStringLiteral("uri")).toString();
        page.owner = row.artists.join(QStringLiteral(", "));
        if (!row.title.isEmpty())
            page.rows.append(row);
    } else {
        page.owner = entity.value(QStringLiteral("subtitle")).toString();
        for (const QJsonValue &value : entity.value(QStringLiteral("trackList")).toArray()) {
            const QJsonObject track = value.toObject();
            Row row;
            row.title = track.value(QStringLiteral("title")).toString().trimmed();
            row.artists = splitLine(track.value(QStringLiteral("subtitle")).toString());
            row.durationMs = track.value(QStringLiteral("duration")).toInteger();
            row.sourceId = track.value(QStringLiteral("uri")).toString();
            if (link.type == QLatin1String("album"))
                row.album = page.name;
            if (!row.title.isEmpty())
                page.rows.append(row);
        }
    }
    page.total = int(page.rows.size());
    if (page.rows.isEmpty())
        page.error = QStringLiteral("This Spotify %1 has no songs in it.").arg(link.type);
    return page;
}

int Reader::spotifyTotal(const Link &link, const QByteArray &html, QHash<QString, QString> *albums)
{
    const QByteArray encoded = scriptById(html, "initialState").trimmed();
    if (encoded.isEmpty())
        return 0;
    const QJsonObject state = QJsonDocument::fromJson(QByteArray::fromBase64(encoded)).object();
    const QString type = link.type == QLatin1String("song") ? QStringLiteral("track") : link.type;
    const QJsonObject item = dig(state, { "entities", "items" }).toObject()
                                 .value(QStringLiteral("spotify:%1:%2").arg(type, link.id)).toObject();
    QJsonObject list;
    for (const char *key : { "content", "tracksV2", "tracks" }) {
        list = item.value(QLatin1String(key)).toObject();
        if (list.contains(QLatin1String("totalCount")))
            break;
    }
    if (albums) {
        for (const QJsonValue &entry : list.value(QStringLiteral("items")).toArray()) {
            QJsonValue track = dig(entry, { "itemV2", "data" });
            if (track.isUndefined())
                track = dig(entry, { "track" });
            const QString uri = dig(track, { "uri" }).toString();
            const QString album = dig(track, { "albumOfTrack", "name" }).toString();
            if (!uri.isEmpty() && !album.isEmpty())
                albums->insert(uri, album);
        }
    }
    return list.value(QStringLiteral("totalCount")).toInt(0);
}

// ---------------------------------------------------------------- JioSaavn

void Reader::readSaavn(const Link &link, int pageNumber, std::function<void(const Page &)> done)
{
    QUrl url(QStringLiteral("https://www.jiosaavn.com/api.php"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("__call"), QStringLiteral("webapi.get"));
    query.addQueryItem(QStringLiteral("token"), link.id);
    query.addQueryItem(QStringLiteral("type"), link.type);
    query.addQueryItem(QStringLiteral("p"), QString::number(pageNumber));
    query.addQueryItem(QStringLiteral("n"), QString::number(kPageRows));
    query.addQueryItem(QStringLiteral("includeMetaTags"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("_format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("_marker"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("api_version"), QStringLiteral("4"));
    query.addQueryItem(QStringLiteral("ctx"), QStringLiteral("web6dot0"));
    url.setQuery(query);
    get(url, "application/json", [link, pageNumber, done](int, const QByteArray &body, const QString &error) {
        Page page;
        if (!error.isEmpty()) {
            page.error = QStringLiteral("JioSaavn did not answer (%1)").arg(error);
            page.retryable = true;
            done(page);
            return;
        }
        // Its answers have now and then had a line of something before the JSON.
        const qsizetype start = body.indexOf('{');
        const QJsonDocument document = QJsonDocument::fromJson(start >= 0 ? body.mid(start) : QByteArray());
        if (!document.isObject()) {
            page.error = QStringLiteral("JioSaavn sent something that isn't a list");
            page.retryable = true;
            done(page);
            return;
        }
        done(parseSaavn(link, document.object(), pageNumber));
    });
}

Page Reader::parseSaavn(const Link &link, const QJsonObject &root, int pageNumber)
{
    Page page;
    if (link.type == QLatin1String("song")) {
        // {"songs": [ … ]}, or the song under its own id.
        QJsonObject song = root.value(QStringLiteral("songs")).toArray().at(0).toObject();
        if (song.isEmpty()) {
            for (auto it = root.constBegin(); it != root.constEnd() && song.isEmpty(); ++it) {
                if (it.value().isObject() && it.value().toObject().contains(QLatin1String("title")))
                    song = it.value().toObject();
            }
        }
        const Row row = rowFromSaavn(song, QString());
        if (row.title.isEmpty()) {
            page.error = QStringLiteral("JioSaavn says this song doesn't exist.");
            return page;
        }
        page.name = row.title;
        page.owner = row.artists.join(QStringLiteral(", "));
        page.artwork = largerSaavnImage(song.value(QStringLiteral("image")).toString());
        page.rows.append(row);
        page.total = 1;
        return page;
    }

    page.name = Saavn::decodeEntities(text(root.value(QStringLiteral("title")))).trimmed();
    page.artwork = largerSaavnImage(root.value(QStringLiteral("image")).toString());
    page.total = text(root.value(QStringLiteral("list_count"))).toInt();
    if (page.total == 0)
        page.total = text(dig(root, { "more_info", "song_count" })).toInt();
    if (link.type == QLatin1String("album")) {
        QStringList names;
        for (const QJsonValue &artist : dig(root, { "more_info", "artistMap", "primary_artists" }).toArray())
            names.append(Saavn::decodeEntities(artist.toObject().value(QStringLiteral("name")).toString()));
        page.owner = names.join(QStringLiteral(", "));
        if (page.owner.isEmpty())
            page.owner = Saavn::decodeEntities(text(root.value(QStringLiteral("subtitle"))));
    } else {
        const QString first = text(dig(root, { "more_info", "firstname" })).trimmed();
        const QString last = text(dig(root, { "more_info", "lastname" })).trimmed();
        page.owner = (first + QLatin1Char(' ') + last).trimmed();
        if (page.owner.isEmpty())
            page.owner = QStringLiteral("JioSaavn");
    }
    // A page past the end is an empty list, or none at all ("list": "").
    const QJsonArray list = root.value(QStringLiteral("list")).toArray();
    for (const QJsonValue &value : list) {
        const Row row = rowFromSaavn(value.toObject(), link.type == QLatin1String("album") ? page.name : QString());
        if (!row.title.isEmpty())
            page.rows.append(row);
    }
    if (pageNumber == 1 && page.rows.isEmpty()) {
        page.error = page.name.isEmpty() ? QStringLiteral("JioSaavn says this %1 doesn't exist.").arg(link.type)
                                         : QStringLiteral("This JioSaavn %1 has no songs in it.").arg(link.type);
        return page;
    }
    const int readSoFar = (pageNumber - 1) * kPageRows + int(list.size());
    if (!list.isEmpty() && (page.total == 0 || readSoFar < page.total) && readSoFar < kMaxRows)
        page.cursor = QString::number(pageNumber + 1);
    return page;
}

// ---------------------------------------------------------------- Deezer

void Reader::readDeezer(const Link &link, const QString &cursor, std::function<void(const Page &)> done)
{
    const QString type = link.type == QLatin1String("song") ? QStringLiteral("track") : link.type;
    const QUrl url = cursor.isEmpty() ? QUrl(QStringLiteral("https://api.deezer.com/%1/%2").arg(type, link.id))
                                      : QUrl(cursor.section(QLatin1Char('\n'), 0, 0));
    const QString album = cursor.section(QLatin1Char('\n'), 1);
    if (!cursor.isEmpty() && !deezerNextAllowed(link, url)) {
        Page page;
        page.error = QStringLiteral("Deezer pointed somewhere else for the rest of the list");
        QTimer::singleShot(0, this, [done, page]() { done(page); });
        return;
    }
    get(url, "application/json", [link, cursor, album, done](int, const QByteArray &body, const QString &error) {
        Page page;
        if (!error.isEmpty()) {
            page.error = QStringLiteral("Deezer did not answer (%1)").arg(error);
            page.retryable = true;
            done(page);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(body);
        if (!document.isObject()) {
            page.error = QStringLiteral("Deezer sent something that isn't a list");
            page.retryable = true;
            done(page);
            return;
        }
        done(cursor.isEmpty() ? parseDeezerHeader(link, document.object())
                              : parseDeezerTracks(link, document.object(), album));
    });
}

Page Reader::parseDeezerHeader(const Link &link, const QJsonObject &root)
{
    Page page;
    if (root.value(QStringLiteral("error")).isObject()) {
        page.error = deezerError(link, root.value(QStringLiteral("error")).toObject(), &page.retryable);
        return page;
    }
    page.name = root.value(QStringLiteral("title")).toString().trimmed();
    if (link.type == QLatin1String("song")) {
        const Row row = rowFromDeezer(root, QString());
        page.owner = row.artists.join(QStringLiteral(", "));
        page.artwork = dig(root, { "album", "cover_xl" }).toString();
        if (!row.title.isEmpty())
            page.rows.append(row);
        page.total = int(page.rows.size());
        if (page.rows.isEmpty())
            page.error = QStringLiteral("Deezer says this song doesn't exist.");
        return page;
    }
    const bool album = link.type == QLatin1String("album");
    page.owner = album ? dig(root, { "artist", "name" }).toString() : dig(root, { "creator", "name" }).toString();
    for (const char *key : { "picture_xl", "cover_xl", "picture_big", "cover_big", "picture_medium", "cover_medium" }) {
        if (page.artwork.isEmpty())
            page.artwork = root.value(QLatin1String(key)).toString();
    }
    page.total = root.value(QStringLiteral("nb_tracks")).toInt();
    QList<Row> inline_;
    bool everyIsrc = true;
    for (const QJsonValue &value : dig(root, { "tracks", "data" }).toArray()) {
        const Row row = rowFromDeezer(value.toObject(), album ? page.name : QString());
        if (row.title.isEmpty())
            continue;
        everyIsrc = everyIsrc && !row.isrc.isEmpty();
        inline_.append(row);
    }
    const QString base = QStringLiteral("https://api.deezer.com/%1/%2/tracks?index=%3&limit=%4")
                             .arg(album ? QStringLiteral("album") : QStringLiteral("playlist"), link.id);
    // An album's rows come without their ISRC in any case, so the songs on
    // the header are kept; a playlist's are kept when they carry it, and
    // otherwise read again from its own pages, which do.
    if (album || (everyIsrc && !inline_.isEmpty())) {
        page.rows = inline_;
        if (page.total > page.rows.size() && page.rows.size() < kMaxRows)
            page.cursor = base.arg(page.rows.size()).arg(kPageRows) + (album ? QStringLiteral("\n") + page.name : QString());
    } else if (page.total > 0) {
        page.cursor = base.arg(0).arg(kPageRows);
    }
    if (page.rows.isEmpty() && page.cursor.isEmpty())
        page.error = QStringLiteral("This Deezer %1 has no songs in it.").arg(link.type);
    if (page.total == 0)
        page.total = int(page.rows.size());
    return page;
}

Page Reader::parseDeezerTracks(const Link &link, const QJsonObject &root, const QString &album)
{
    Page page;
    if (root.value(QStringLiteral("error")).isObject()) {
        page.error = deezerError(link, root.value(QStringLiteral("error")).toObject(), &page.retryable);
        return page;
    }
    page.total = root.value(QStringLiteral("total")).toInt();
    for (const QJsonValue &value : root.value(QStringLiteral("data")).toArray()) {
        const Row row = rowFromDeezer(value.toObject(), album);
        if (!row.title.isEmpty())
            page.rows.append(row);
    }
    const QUrl next(root.value(QStringLiteral("next")).toString());
    if (!page.rows.isEmpty() && deezerNextAllowed(link, next))
        page.cursor = next.toString(QUrl::FullyEncoded) + (album.isEmpty() ? QString() : QStringLiteral("\n") + album);
    return page;
}

// ---------------------------------------------------------------- Apple Music

void Reader::readApple(const Link &link, std::function<void(const Page &)> done)
{
    const QString type = link.type == QLatin1String("playlist") ? QStringLiteral("playlist") : QStringLiteral("album");
    const QUrl url(QStringLiteral("https://music.apple.com/%1/%2/%3/%4").arg(link.region, type, link.slug, link.id));
    get(url, "text/html", [link, done](int status, const QByteArray &body, const QString &error) {
        Page page;
        if (status == 404) {
            page.error = QStringLiteral("Apple Music says this %1 doesn't exist.").arg(link.type);
            done(page);
            return;
        }
        if (!error.isEmpty()) {
            page.error = QStringLiteral("Apple Music did not answer (%1)").arg(error);
            page.retryable = true;
            done(page);
            return;
        }
        done(parseApple(link, body));
    });
}

Page Reader::parseApple(const Link &link, const QByteArray &html)
{
    Page page;
    const QJsonDocument document = QJsonDocument::fromJson(scriptById(html, "serialized-server-data"));
    const QJsonValue root = document.isArray() ? QJsonValue(document.array().at(0)) : QJsonValue(document.object());
    const QJsonValue data = dig(root, { "data", "#0", "data" });
    const QJsonArray sections = dig(data, { "sections" }).toArray();
    if (sections.isEmpty()) {
        page.error = QStringLiteral("Apple Music's page has changed, and Monolist can't read it. Paste the list of "
                                    "songs instead.");
        return page;
    }
    QString headerArtist;
    for (const QJsonValue &section : sections) {
        const QString kind = section.toObject().value(QStringLiteral("itemKind")).toString();
        const QJsonArray items = section.toObject().value(QStringLiteral("items")).toArray();
        if (kind == QLatin1String("containerDetailHeaderLockup") && page.name.isEmpty()) {
            const QJsonValue header = items.at(0);
            page.name = dig(header, { "title" }).toString();
            headerArtist = dig(header, { "subtitleLinks", "#0", "title" }).toString();
            page.owner = headerArtist;
            QString art = dig(header, { "artwork", "dictionary", "url" }).toString();
            art.replace(QStringLiteral("{w}x{h}bb.{f}"), QStringLiteral("600x600bb.jpg"));
            page.artwork = art;
        } else if (kind == QLatin1String("trackLockup")) {
            for (const QJsonValue &item : items) {
                Row row;
                row.title = dig(item, { "title" }).toString().trimmed();
                row.durationMs = dig(item, { "duration" }).toInteger();
                for (const QJsonValue &artist : dig(item, { "subtitleLinks" }).toArray()) {
                    const QString name = artist.toObject().value(QStringLiteral("title")).toString().trimmed();
                    if (!name.isEmpty())
                        row.artists.append(name);
                }
                // An album's rows leave out what its header says.
                if (row.artists.isEmpty())
                    row.artists = Saavn::splitArtists(dig(item, { "artistName" }).toString());
                if (row.artists.isEmpty() && !headerArtist.isEmpty())
                    row.artists.append(headerArtist);
                row.album = dig(item, { "tertiaryLinks", "#0", "title" }).toString();
                if (row.album.isEmpty() && link.type != QLatin1String("playlist"))
                    row.album = page.name;
                row.sourceId = text(dig(item, { "contentDescriptor", "identifiers", "storeAdamID" }));
                if (!row.title.isEmpty())
                    page.rows.append(row);
            }
        } else if (kind == QLatin1String("containerDetailTracklistFooterLockup")) {
            // "50 songs, 2 hours 46 minutes" on a playlist; an album's footer
            // has its date before that line and its ℗ after, both with a
            // year in them: the count is the number before "songs". In a
            // storefront's own language, the line of two counts that is not
            // the ℗ or © one.
            const QString description = dig(items.at(0), { "description" }).toString();
            page.total = songCount(description);
            if (page.total == 0) {
                static const QRegularExpression counts(QStringLiteral("^\\d[\\d,.\\x{202F}\\x{00A0}]*\\s+\\S+,\\s*\\d"));
                for (const QString &line : description.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    if (!line.contains(QChar(0x2117)) && !line.contains(QChar(0x00A9)) && counts.match(line.trimmed()).hasMatch()) {
                        page.total = leadingCount(line);
                        break;
                    }
                }
            }
        }
    }
    if (link.type == QLatin1String("song")) {
        QList<Row> one;
        for (const Row &row : std::as_const(page.rows)) {
            if (row.sourceId == link.songId)
                one.append(row);
        }
        page.rows = one;
        if (one.isEmpty()) {
            page.error = QStringLiteral("That song isn't on its album's page.");
            return page;
        }
        page.name = one.first().title;
        page.owner = one.first().artists.join(QStringLiteral(", "));
        page.total = 1;
        return page;
    }
    if (page.rows.isEmpty()) {
        page.error = QStringLiteral("This Apple Music %1 has no songs Monolist can read.").arg(link.type);
        return page;
    }
    // More than the page holds: the rest is behind Apple's own API, which
    // only its apps' tokens open.
    if (!dig(data, { "nextIntent" }).isUndefined()) {
        page.warnings << QStringLiteral("Apple Music shows only the first %1 songs without signing in; the rest "
                                        "can't be read.").arg(page.rows.size());
        page.total = qMax(page.total, int(page.rows.size()));
    }
    if (page.total < page.rows.size())
        page.total = int(page.rows.size());
    return page;
}

} // namespace Import
