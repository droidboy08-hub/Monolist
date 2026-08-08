#pragma once

#include <QAbstractListModel>

struct AlbumItem {
    int id = 0;
    QString title;
    QString artist;
    QString year;
    QString format;
    QString artwork;
};

class AlbumModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, TitleRole, ArtistRole, YearRole, FormatRole, ArtworkRole };

    explicit AlbumModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reload();

private:
    QList<AlbumItem> m_items;
};
