#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <functional>
#include <utility>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

// YouTube Music's own API (InnerTube): the one music.youtube.com itself calls.
//
// A yt-dlp search spends most of its ~8 seconds starting a Python runtime. The
// same search here is one HTTPS round trip, a few hundred milliseconds on a warm
// connection, and it comes back in YouTube Music's shape: songs with their
// artists, album and square cover art, rather than videos with channel names.
// Suggestions are fast enough to show while typing.
//
// Streams are still resolved by yt-dlp: InnerTube's player endpoint needs the
// signature and challenge work that yt-dlp already does well.
//
// The API is unofficial and changes without notice, so every parser here is
// defensive, and MediaExtractor falls back to yt-dlp whenever a search fails.
class InnerTube : public QObject
{
    Q_OBJECT
public:
    struct Track {
        QString videoId;
        QString title;
        QString artist;
        QString album;
        QString artwork;
        qint64 durationMs = 0;
        // A real video, rather than YouTube Music's own audio track (which is
        // a still picture of the cover and not worth showing).
        bool isVideo = false;
    };

    // An album, playlist, artist or video, as the home feed and charts show them.
    struct Card {
        QString type;       // "album", "playlist", "artist", "song" or "video"
        QString browseId;   // albums, playlists, artists
        QString videoId;    // songs, videos
        QString title;
        QString subtitle;   // "Album • Seth Ballad", "Nirvana, Radiohead, ..."
        QString artwork;
    };

    // One row of a browse page: a run of songs (Quick picks) or of cards.
    struct Shelf {
        QString title;
        QString strapline;  // the small line above the title
        QList<Track> songs;
        QList<Card> cards;
    };

    // An album or playlist page.
    struct Collection {
        QString browseId;
        QString type;         // "album" or "playlist"
        QString title;
        QString subtitle;     // "Album • 2026"
        QString artist;       // an album's artist line
        QString details;      // "20 songs • 1 hour, 38 minutes"
        QString description;
        QString artwork;
        QList<Track> tracks;
    };

    enum class Filter { Songs, Videos };

    // Which of YouTube's front ends to ask. Music knows songs, albums and
    // artists; YouTube knows every video, and answers when Music does not.
    //
    // Player is neither: it is the client YouTube's visionOS app identifies
    // itself as, and it is the only one of the three that /player will hand a
    // plain, directly playable stream URL to. WEB_REMIX is refused without a
    // PO token and WEB answers with SABR, so the existing two cannot be
    // reused for this no matter how convenient that would be.
    enum class Client { Music, YouTube, Player };

    explicit InnerTube(QObject *parent = nullptr);

    // The country every request is made from ("US", "JP", …), which decides
    // what YouTube Music offers: new releases, charts and the ranking of
    // search results. Empty means the one the system is set to. YouTube also
    // reads the connection's own location, so this steers rather than decides.
    static void setRegion(const QString &code);
    static QString region();          // the code actually sent
    static QString systemRegion();    // what the system is set to

    // YouTube Music serves some countries and refuses the rest outright (400
    // Bad Request for every call, which looks exactly like being offline). A
    // refused country is dropped here and the request is made again from the
    // system's own; the handler is told, so the choice can be forgotten and
    // the user told why.
    static void setRegionRejectedHandler(std::function<void(const QString &code)> handler);

    // The countries it serves, by their ISO code. Asked of the API itself,
    // country by country, rather than taken from a page that goes stale.
    static const QSet<QString> &servedRegions();

    // One browse request (the home feed, charts, new releases, an album or a
    // playlist); `done` gets the response, or an error. Any number can run.
    void browse(const QString &browseId,
                std::function<void(const QJsonObject &root, const QString &error)> done);
    static QList<Shelf> parseShelves(const QJsonObject &root);
    static Collection parseCollection(const QString &browseId, const QJsonObject &root);

    // A song's lyrics as YouTube Music shows them: plain text, from a partner
    // it names ("Source: Musixmatch"). Two requests: the watch page says
    // where the lyrics are, a browse fetches them. `done` gets empty text
    // when the song has none, and an error only when a request failed.
    void lyrics(const QString &videoId,
                std::function<void(const QString &text, const QString &source, const QString &error)> done);

    // A newer call of the same kind cancels the one still in flight. The kinds
    // are independent: a search starting must not cancel the suggestions for
    // what is being typed.
    void search(const QString &query, Filter filter);
    // The same query against youtube.com, for when Music fails or finds
    // nothing: videos with their channel, not songs with their album, but one
    // request rather than yt-dlp's Python start-up.
    void searchYouTube(const QString &query);
    void suggest(const QString &input);
    // The "radio" YouTube Music builds from one song: songs like it, the seed
    // itself first. What autoplay continues with when a queue runs out.
    void radio(const QString &videoId);
    void cancelSearch();
    void cancelSuggestions();
    void cancelRadio();

    // One /player call: the audio stream for a track, in one round trip and
    // in this process, where the alternative is starting yt-dlp.
    //
    // `url` is empty whenever anything at all went wrong and `error` says
    // what; the caller is expected to fall through to yt-dlp rather than show
    // it, because yt-dlp still resolves things this cannot — age-gated and
    // made-for-kids tracks, and live streams.
    //
    // Deliberately not cancellable: unlike search or suggestions there is no
    // "newer one of the same kind", and giving it a slot would make a prefetch
    // and a foreground resolve abort each other.
    void player(const QString &videoId,
                std::function<void(const QString &url, int itag, const QString &error)> done);

Q_SIGNALS:
    void searchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void searchFailed(const QString &query, const QString &reason);
    void youtubeSearchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void youtubeSearchFailed(const QString &query, const QString &reason);
    void suggestionsReady(const QString &input, const QStringList &suggestions);
    void radioReady(const QString &seedVideoId, const QList<InnerTube::Track> &tracks);
    void radioFailed(const QString &seedVideoId, const QString &reason);

private:
    // Where the one request of a kind (search, suggestions, radio) is kept,
    // so a newer one or a cancel can replace it. The reply alone is not
    // enough: between a failed attempt and its retry there is no reply in
    // flight, only a timer, and a retry that fired after a newer request had
    // started would take that request's place and silence it. So every new
    // request and every cancel also moves the generation on, and anything
    // started for an older generation — a reply, a retry waiting to be sent —
    // drops itself when it finds the slot has moved.
    struct Slot {
        QPointer<QNetworkReply> reply;
        quint64 generation = 0;
    };
    // Moves the slot on and aborts the reply it holds, if any.
    static void release(Slot &slot);

    QNetworkReply *post(Client client, const QString &endpoint, QJsonObject body, int timeoutMs);
    // One request, its answer as JSON. A dropped connection or a timeout is
    // ordinary on a home connection, so it is tried once more before failing;
    // `slot`, where given, holds the reply so a newer request can cancel it,
    // and a cancelled or replaced request is dropped silently rather than
    // retried.
    void send(Client client, const QString &endpoint, const QJsonObject &body, int timeoutMs,
              Slot *slot,
              std::function<void(const QJsonObject &root, const QString &error)> done,
              int retries = 1);

    static QList<Track> parseSearch(const QJsonObject &root);
    static QList<Track> parseYouTubeSearch(const QJsonObject &root);
    static QStringList parseSuggestions(const QJsonObject &root);
    static QList<Track> parseRadio(const QJsonObject &root);

    // /player is refused with LOGIN_REQUIRED for most music without one of
    // these: an anonymous visitor id, scraped from the YouTube home page and
    // good for the session. Fetched once, in the background, at start-up, so
    // the first track does not pay for it.
    void fetchVisitorData();
    void withVisitorData(std::function<void()> then);

    QNetworkAccessManager *m_network;
    QString m_visitorData;
    bool m_visitorPending = false;
    std::vector<std::function<void()>> m_visitorWaiters;
    Slot m_search;
    Slot m_suggest;
    Slot m_radio;
};
