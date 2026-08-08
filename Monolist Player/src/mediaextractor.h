#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class YtDlpRequest;

// Search results, shaped so the existing TrackTable can render them unchanged —
// it requires title, artist, album and durationText, and this supplies the same
// role names the library model uses.
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
    };

    enum Roles { SourceIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole,
                 ArtworkRole, DurationRole, DurationTextRole };

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

// Search and metadata, backed by yt-dlp.
//
// The prototype returned a mock result after a timer; the React app scraped
// ytInitialData out of YouTube's search HTML through a CORS proxy and parsed it
// with a regex. Both are replaced by a single `yt-dlp ytsearch` call, which is
// the same data without the parser that breaks whenever the page markup shifts.
class MediaExtractor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(SearchResultModel *results READ results CONSTANT)
public:
    explicit MediaExtractor(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    bool available() const;
    QString lastError() const { return m_lastError; }
    SearchResultModel *results() { return &m_results; }

public Q_SLOTS:
    void search(const QString &query);
    void resolve(const QString &videoIdOrUrl);
    void cancel();

Q_SIGNALS:
    void busyChanged();
    void availableChanged();
    void lastErrorChanged();
    void searchFinished(const QVariantList &results);
    void resolved(const QVariantMap &stream);
    void failed(const QString &reason);

private:
    void setBusy(bool busy);
    void setLastError(const QString &error);

    SearchResultModel m_results;
    QPointer<YtDlpRequest> m_request;
    QString m_lastError;
    bool m_busy = false;
};
