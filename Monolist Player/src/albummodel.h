#pragma once

#include <QAbstractListModel>

struct AlbumItem {
    int id = 0;
    QString title;
    QString artist;
    QString year;
    QString format;      // "ALBUM", "SINGLE", "EP" or "PLAYLIST"
    QString artwork;
    QString browseId;    // the YouTube Music page it opens
};

// Albums and playlists saved from YouTube Music, the latest saved first. One
// instance lists the albums (singles and EPs among them), another the
// playlists.
class AlbumModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Kind { Albums, Playlists };
    enum Roles { IdRole = Qt::UserRole + 1, TitleRole, ArtistRole, YearRole, FormatRole, ArtworkRole,
                 BrowseIdRole };

    explicit AlbumModel(Kind kind, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();

Q_SIGNALS:
    void countChanged();

private:
    Kind m_kind;
    QList<AlbumItem> m_items;
};
