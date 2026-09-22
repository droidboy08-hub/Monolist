#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariantMap>

// The work in flight: queued, downloading, being post-processed, or failed and
// waiting for a retry. A finished item leaves this list and appears in
// DownloadLibraryModel instead.
class DownloadQueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum class State { Queued, Downloading, Processing, Failed };

    struct Item {
        QString videoId;
        QString title;
        QString artist;
        QString artwork;
        qint64 durationMs = 0;
        State state = State::Queued;
        qreal progress = 0.0;       // 0..1
        qint64 received = 0;
        qint64 total = -1;          // bytes; < 0 while unknown
        double speed = -1.0;        // bytes/s; < 0 while unknown
        int eta = -1;               // seconds; < 0 while unknown
        QString step;               // yt-dlp post-processor, while processing
        QString error;              // when failed
    };

    enum Roles { VideoIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, ArtworkRole,
                 DurationRole, StateRole, ProgressRole, DetailRole };

    explicit DownloadQueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Used by DownloadManager, which owns the lifecycle.
    const Item *find(const QString &videoId) const;
    void upsert(const Item &item);
    void remove(const QString &videoId);

    static QString stateName(State state);
    static QString detailText(const Item &item);

Q_SIGNALS:
    void countChanged();

private:
    int indexOf(const QString &videoId) const;

    QList<Item> m_items;
};

// Everything saved for offline use, newest first. Backed by the downloads
// table; rows whose file has since gone missing are left out.
class DownloadLibraryModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString totalSizeText READ totalSizeText NOTIFY countChanged)
public:
    struct Item {
        QString videoId;
        QString title;
        QString artist;
        QString artwork;
        QString filePath;
        qint64 durationMs = 0;
        qint64 bytes = 0;
    };

    enum Roles { VideoIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, ArtworkRole,
                 DurationRole, DurationTextRole, FilePathRole, FormatRole, SizeTextRole };

    explicit DownloadLibraryModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();
    QString totalSizeText() const;
    Q_INVOKABLE QVariantMap get(int row) const;

    // "850 KB", "3.4 MB", "1.2 GB".
    static QString formatBytes(qint64 bytes);

Q_SIGNALS:
    void countChanged();

private:
    QList<Item> m_items;
};
