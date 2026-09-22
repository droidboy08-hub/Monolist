#include "albummodel.h"
#include "appdatabase.h"

#include <QSqlQuery>
#include <QVariant>

AlbumModel::AlbumModel(Kind kind, QObject *parent)
    : QAbstractListModel(parent)
    , m_kind(kind) {}

int AlbumModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant AlbumModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const AlbumItem &item = m_items.at(index.row());
    switch (role) {
    case IdRole:       return item.id;
    case TitleRole:    return item.title;
    case ArtistRole:   return item.artist;
    case YearRole:     return item.year;
    case FormatRole:   return item.format;
    case ArtworkRole:  return item.artwork;
    case BrowseIdRole: return item.browseId;
    default:           return {};
    }
}

QHash<int, QByteArray> AlbumModel::roleNames() const
{
    return {
        { IdRole, "albumId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { YearRole, "year" },
        { FormatRole, "format" },
        { ArtworkRole, "artwork" },
        { BrowseIdRole, "browseId" }
    };
}

void AlbumModel::reload()
{
    beginResetModel();
    m_items.clear();
    QSqlQuery q(AppDatabase::connection());
    // Rows without a page to open are the interface prototype's; nothing can
    // be done with them, so they are not shown.
    q.exec(QStringLiteral(
        "SELECT id, title, artist, year, format, artwork, browse_id FROM albums"
        " WHERE browse_id <> '' AND %1"
        " ORDER BY saved_at DESC, id DESC")
               .arg(m_kind == Playlists ? QStringLiteral("format = 'PLAYLIST'")
                                        : QStringLiteral("format <> 'PLAYLIST'")));
    while (q.next())
        m_items.append({ q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                         q.value(3).toString(), q.value(4).toString(), q.value(5).toString(),
                         q.value(6).toString() });
    endResetModel();
    Q_EMIT countChanged();
}
