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
