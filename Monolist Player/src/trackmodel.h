#pragma once

#include <QAbstractListModel>

struct TrackItem {
    int id = 0;
    QString title;
    QString artist;
    QString album;
    qint64 durationMs = 0;
    QString sourceUrl;   // local file path, or a direct URL
    QString sourceId;    // upstream video id, when the track came from a search
    QString artwork;
    bool favourite = false;
};

class TrackModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole,
                 DurationRole, DurationTextRole, SourceRole, SourceIdRole,
                 ArtworkRole, FavouriteRole };

    explicit TrackModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void toggleFavourite(int row);

    // Row of the track with this database id or upstream video id; -1 if absent.
    Q_INVOKABLE int indexOfTrack(int trackId) const;
    Q_INVOKABLE int indexOfSource(const QString &sourceId) const;

    static QString formatDuration(qint64 ms);

Q_SIGNALS:
    void countChanged();

private:
    QList<TrackItem> m_items;
};
