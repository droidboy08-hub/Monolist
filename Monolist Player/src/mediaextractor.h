#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QSet>
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
        QString primaryArtist;  // the first credit alone, for Last.fm; may be empty
        // The artist line piece by piece, each name with its page
        // (InnerTube::creditsToVariant); empty where only the name was kept.
        QVariantList credits;
        QString albumId;        // the album's page, where known
    };

    enum Roles { SourceIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole,
                 ArtworkRole, DurationRole, DurationTextRole, EntryIdRole, IsVideoRole,
                 PrimaryArtistRole, CreditsRole, AlbumIdRole };

    // A song as YouTube Music sent it, as a row.
    static Item fromTrack(const InnerTube::Track &track);
    // A row as QML and the player take a track: what get() returns.
    static QVariantMap toMap(const Item &item);

    explicit SearchResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const QList<Item> &items);
    // Adds rows at the end, as rows rather than a reset: a view keeps the
    // rows it has already made and makes only the new ones, which is what
    // lets a long playlist grow under the reader without redrawing it.
    void append(const QList<Item> &items);
    // One row to `to`, as a move: the view carries over the row it has
    // rather than making every row again. False when nothing moved.
    bool move(int from, int to);
    void clear();
    // Whether a song is among the rows: a search's next page brings now
    // and then one already shown.
    bool contains(const QString &sourceId) const;

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
    // Which YouTube Music section to search: "songs", "videos", "albums",
    // "artists" or "playlists".
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Albums, artists and playlists come back as cards rather than songs, in
    // titled sections: [{ title, items: [card] }] (InnerTube::cardToVariant).
    // A playlist search has two, YouTube Music's own playlists and its
    // listeners'. Empty while songs or videos are shown.
    Q_PROPERTY(QVariantList cardSections READ cardSections NOTIFY cardSectionsChanged)
    // Where the current results came from: "YouTube Music" or "yt-dlp".
    Q_PROPERTY(QString source READ source NOTIFY resultsSourceChanged)
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
    // No cap on a search (the owner's, 2026-10-08): YouTube Music answers
    // twenty at a time, and the next twenty are asked for (loadMore) as the
    // page nears the end of those shown, until it has no more.
    // For songs and videos, and for the last of the card sections: the page
    // asks by itself. A section above another has its own "show more"
    // (cardMore: one bool per section, in cardSections' order), and its
    // cards come by cardsAppended rather than a new cardSections, so the
    // grids keep the cards they have.
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY moreChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY moreChanged)
    Q_PROPERTY(QVariantList cardMore READ cardMore NOTIFY moreChanged)
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
    QVariantList cardSections() const { return m_cardSections; }
    bool hasMore() const;
    bool loadingMore() const { return m_loadingMore; }
    QVariantList cardMore() const;

public Q_SLOTS:
    void search(const QString &query);
    // The search's next page, added below what is shown; one at a time.
    // `section`: which card section, -1 for the songs or the last section.
    void loadMore(int section = -1);
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
    void cardSectionsChanged();
    void moreChanged();
    // A card section's next page: cards to add to its grid.
    void cardsAppended(int section, const QVariantList &cards);
    void searchFinished(const QVariantList &results);
    // A search for albums, artists or playlists answered: cardSections.
    void cardSearchFinished();
    void resolved(const QVariantMap &stream);
    void failed(const QString &reason);

private:
    void searchWithYtDlp(const QString &query);
    void searchCards(const QString &query);
    void setCardSections(const QVariantList &sections);
    void setLoadingMore(bool loading);
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
    QVariantList m_cardSections;
    // Moves on with every card search and every cancel: a card search is
    // not cancellable in flight, so an answer for an older one is dropped
    // by this instead.
    quint64 m_cardGeneration = 0;
    bool m_busy = false;

    // Where the search goes on: the token for the songs' or videos' next
    // page, and each card section's own (a playlist search has two), with
    // the cards it shows, to tell a card it has already from a new one.
    QString m_next;
    struct CardPart {
        QSet<QString> shown;
        QString next;
    };
    QList<CardPart> m_cardParts;     // in cardSections' order
    // Moves on with every search: a next page asked for an older one is
    // dropped by this.
    quint64 m_moreGeneration = 0;
    bool m_loadingMore = false;
};
