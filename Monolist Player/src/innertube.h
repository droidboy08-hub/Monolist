#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

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

    enum class Filter { Songs, Videos };

    explicit InnerTube(QObject *parent = nullptr);

    // A newer call of the same kind cancels the one still in flight. The two
    // kinds are independent: a search starting must not cancel the
    // suggestions for what is being typed.
    void search(const QString &query, Filter filter);
    void suggest(const QString &input);
    void cancelSearch();
    void cancelSuggestions();

Q_SIGNALS:
    void searchFinished(const QString &query, const QList<InnerTube::Track> &tracks);
    void searchFailed(const QString &query, const QString &reason);
    void suggestionsReady(const QString &input, const QStringList &suggestions);

private:
    QNetworkReply *post(const QString &endpoint, QJsonObject body);
    static QList<Track> parseSearch(const QJsonObject &root);
    static QStringList parseSuggestions(const QJsonObject &root);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_search;
    QPointer<QNetworkReply> m_suggest;
};
