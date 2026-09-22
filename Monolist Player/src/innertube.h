#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>

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

    explicit InnerTube(QObject *parent = nullptr);

    // The country every request is made from ("US", "JP", …), which decides
    // what YouTube Music offers: new releases, charts and the ranking of
    // search results. Empty means the one the system is set to. YouTube also
    // reads the connection's own location, so this steers rather than decides.
    static void setRegion(const QString &code);
    static QString region();          // the code actually sent
    static QString systemRegion();    // what the system is set to

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
    void suggest(const QString &input);
    // The "radio" YouTube Music builds from one song: songs like it, the seed
    // itself first. What autoplay continues with when a queue runs out.
    void radio(const QString &videoId);
    void cancelSearch();
    void cancelSuggestions();
    void cancelRadio();

Q_SIGNALS:
    void searchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void searchFailed(const QString &query, const QString &reason);
    void suggestionsReady(const QString &input, const QStringList &suggestions);
    void radioReady(const QString &seedVideoId, const QList<InnerTube::Track> &tracks);
    void radioFailed(const QString &seedVideoId, const QString &reason);

private:
    QNetworkReply *post(const QString &endpoint, QJsonObject body);
    static QList<Track> parseSearch(const QJsonObject &root);
    static QStringList parseSuggestions(const QJsonObject &root);
    static QList<Track> parseRadio(const QJsonObject &root);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_search;
    QPointer<QNetworkReply> m_suggest;
    QPointer<QNetworkReply> m_radio;
};
