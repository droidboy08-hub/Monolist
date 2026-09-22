#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include "innertube.h"

class YtDlpRequest;

// A list of songs: search results, and every other song list the interface
// shows (Home's, an album's, a playlist's, the liked songs). TrackTable renders
// any of them, and Player.playModel queues any of them, through the same role
// names the library model uses.
class SearchResultModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    struct Item {
        QString sourceId;
        QString title;
        QString artist;
        QString album;
        QString artwork;
        qint64 durationMs = 0;
        int entryId = 0;     // the row in a playlist, where a list is one
        bool isVideo = false;   // has a picture worth showing
    };

    enum Roles { SourceIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole,
                 ArtworkRole, DurationRole, DurationTextRole, EntryIdRole, IsVideoRole };

    explicit SearchResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const QList<Item> &items);
    void clear();

    Q_INVOKABLE QVariantMap get(int row) const;

Q_SIGNALS:
    void countChanged();

private:
    QList<Item> m_items;
};

// Search and metadata.
//
// Searches go to YouTube Music's own API first (InnerTube): one HTTPS request,
// a few hundred milliseconds, and results that are songs with artists, albums
// and cover art. If that fails, for any reason, the same query runs through
// yt-dlp, which is slower (it starts a Python runtime each time) but tracks
// YouTube's changes on its own.
//
// The prototype returned a mock result after a timer; the React app scraped
// ytInitialData out of YouTube's search HTML through a CORS proxy.
class MediaExtractor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(SearchResultModel *results READ results CONSTANT)
    // "songs" or "videos": which YouTube Music section to search.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Where the current results came from: "YouTube Music" or "yt-dlp".
    Q_PROPERTY(QString source READ source NOTIFY resultsSourceChanged)
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
public:
    explicit MediaExtractor(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    bool available() const;
    QString lastError() const { return m_lastError; }
    SearchResultModel *results() { return &m_results; }
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QString source() const { return m_source; }
    QStringList suggestions() const { return m_suggestions; }

public Q_SLOTS:
    void search(const QString &query);
    void suggest(const QString &input);
    void clearSuggestions();
    void resolve(const QString &videoIdOrUrl);
    void cancel();

Q_SIGNALS:
    void busyChanged();
    void availableChanged();
    void lastErrorChanged();
    void filterChanged();
    void resultsSourceChanged();
    void suggestionsChanged();
    void searchFinished(const QVariantList &results);
    void resolved(const QVariantMap &stream);
    void failed(const QString &reason);

private:
    void searchWithYtDlp(const QString &query);
    void finishSearch(const QList<SearchResultModel::Item> &items, const QString &source);
    void setBusy(bool busy);
    void setLastError(const QString &error);
    void setSource(const QString &source);

    SearchResultModel m_results;
    InnerTube m_innerTube;
    QPointer<YtDlpRequest> m_request;
    QString m_query;                 // the search whose answer is still wanted
    QString m_suggestFor;            // likewise for suggestions
    QString m_filter = QStringLiteral("songs");
    QString m_source;
    QString m_lastError;
    QStringList m_suggestions;
    bool m_busy = false;
};
