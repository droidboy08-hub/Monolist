#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class InnerTube;
class QNetworkAccessManager;

// Where an imported playlist's songs come from (ROADMAP X03; PlaylistImport
// runs it): a link to a playlist, album or song on YouTube, YouTube Music,
// Spotify, JioSaavn, Deezer or Apple Music, a pasted list of songs, or a CSV
// file. What each one is read from, and how much of it, was tried live and
// anonymously on 2026-10-10 (importresearch/REPORT.md):
//  - YouTube and YouTube Music: InnerTube's browse, "VL" and the list's id,
//    a hundred songs a page with continuations. The songs come with their
//    video ids, so nothing is searched for.
//  - Spotify: the embed page (/embed/playlist/<id>), whose __NEXT_DATA__
//    holds the first 100 songs, and, for a list that may hold more, the
//    public page once for the real total. No token, no API, nothing that
//    signs in: more than 100 cannot be read that way, and the user is told
//    so, with the way round it (an Exportify CSV).
//  - JioSaavn: api.php's webapi.get, a page at a time (p, n).
//  - Deezer: its public API, a hundred a page, following its own `next`.
//  - Apple Music: the public page's serialized-server-data, the first 300.
// Every request is anonymous, with an honest User-Agent and no cookies, by
// the owner's rule; YouTube's go through a cookieless InnerTube, never as
// the account. The reader spaces its own requests 2-3 s apart.
namespace Import {

// One song as a source lists it.
struct Row {
    QString title;
    QStringList artists;
    QString album;
    qint64 durationMs = 0;    // 0 when the source does not say
    QString isrc;
    QString sourceId;         // the source's own id for the song
    // YouTube's lists only: the song itself, nothing to search for.
    QString videoId;
    QString artwork;
    bool isVideo = false;
    // A pasted "A - B" line: either half may be the title.
    bool eitherWay = false;
};

// A link, read: where it points and whether it can be imported.
struct Link {
    bool ok = false;
    QString platform;         // youtube, ytmusic, spotify, jiosaavn, deezer, apple
    QString type;             // playlist, album, song
    QString id;               // what the source is asked for: a browse id, an id, a token
    QString region;           // Apple Music's storefront ("us")
    QString slug;             // Apple Music's
    QString songId;           // Apple Music's ?i=: one song of the album
    QString url;              // as given, trimmed
    QString label;            // "Spotify playlist"
    QString reason;           // why not, plainly, when not ok; empty while it is not a link yet
    bool looksLikeList = false;   // several lines: a list of songs, for "Paste a list"
    QString kind() const { return platform + QLatin1Char('-') + type; }
};
Link parseLink(const QString &text);

// What a source holds, as one page of it reads: the header (on the first
// page), the songs, and where the rest is.
struct Page {
    QString name;
    QString owner;
    QString artwork;
    int total = 0;            // what the source says it holds; 0 when it does not say
    QList<Row> rows;
    QString cursor;           // where the next page is; empty at the end
    QStringList warnings;     // plain sentences for the preview
    QString error;            // it could not be read: why, plainly
    // The error was a refusal or no answer (worth trying again later),
    // rather than a list that is not there.
    bool retryable = false;
};

// A pasted list of songs: a line a song, "Artist - Title", "Title - Artist"
// (whichever matches better), "Title by Artist", a length at the end
// ("(3:45)"), numbering and blank lines ignored, a tab between columns.
Page readList(const QString &text);
// A CSV file, its columns found by their headers' names: Exportify's
// (Track Name, Artist Name(s), Album Name, Track Duration (ms), ISRC) and
// TuneMyMusic's among them. `fileName` names the playlist.
Page readCsv(const QByteArray &data, const QString &fileName);

// At most this many songs from any one source.
constexpr int kMaxRows = 5000;

// What went wrong with a call to YouTube Music, in words for the user
// rather than Qt's (which quote the whole address): asked to slow down, a
// page that is not an answer (consent, "unusual traffic"), or no answer.
QString youtubeTrouble(const QString &error);

class Reader : public QObject
{
    Q_OBJECT
public:
    // `innerTube` asks YouTube; it must be cookieless (setCookieless).
    Reader(InnerTube *innerTube, QObject *parent = nullptr);

    // For the self-test only: every request for Spotify, JioSaavn, Deezer
    // or Apple Music goes to <base>/<host><path> instead, on this computer.
    // YouTube's go where InnerTube::setTestServer says.
    static void setTestServer(const QString &base);
    // The gap between two requests, and the most added to it at random.
    void setGap(int gapMs, int jitterMs);

    // The first page, with the header. `done` is called exactly once.
    void first(const Link &link, std::function<void(const Page &page)> done);
    // The page at `cursor` (a Page's own); an empty cursor is the first
    // page again.
    void next(const Link &link, const QString &cursor, std::function<void(const Page &page)> done);

    // The page's parsers, on answers as they came; for the self-test, and
    // for what the reader asks.
    static Page parseSpotifyEmbed(const Link &link, const QByteArray &html);
    static int spotifyTotal(const Link &link, const QByteArray &html, QHash<QString, QString> *albums);
    static Page parseSaavn(const Link &link, const QJsonObject &root, int page);
    static Page parseDeezerHeader(const Link &link, const QJsonObject &root);
    static Page parseDeezerTracks(const Link &link, const QJsonObject &root, const QString &album);
    static Page parseApple(const Link &link, const QByteArray &html);
    static Page parseYouTube(const QString &browseId, const QJsonObject &root);
    static Page parseYouTubeMore(const QJsonObject &root, const QString &visitor);

private:
    using Answer = std::function<void(int status, const QByteArray &body, const QString &error)>;
    // One GET, no sooner than the gap after the last request.
    void get(const QUrl &url, const QByteArray &accept, Answer done);
    void whenFree(std::function<void()> send);
    void sendAt(qint64 at, std::function<void()> send);
    void readYouTube(const Link &link, const QString &cursor, std::function<void(const Page &)> done);
    void readSpotify(const Link &link, std::function<void(const Page &)> done);
    void readSaavn(const Link &link, int page, std::function<void(const Page &)> done);
    void readDeezer(const Link &link, const QString &cursor, std::function<void(const Page &)> done);
    void readApple(const Link &link, std::function<void(const Page &)> done);

    QPointer<InnerTube> m_innerTube;
    QNetworkAccessManager *m_network;
    QElapsedTimer m_clock;
    qint64 m_lastRequestMs = -1;
    int m_gapMs = 2000;
    int m_jitterMs = 1000;
};

} // namespace Import
