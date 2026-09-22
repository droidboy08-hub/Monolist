#include "playlistmodel.h"
#include "appdatabase.h"

#include <QSqlQuery>
#include <QVariant>

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent) {}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const PlaylistItem &item = m_items.at(index.row());
    switch (role) {
    case IdRole:         return item.id;
    case NameRole:       return item.name;
    case TrackCountRole: return item.trackCount;
    case NumberRole:     return QStringLiteral("%1").arg(index.row() + 1, 2, 10, QLatin1Char('0'));
    case ArtworksRole:   return item.artworks;
    default:             return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    return {
        { IdRole, "playlistId" },
        { NameRole, "name" },
        { TrackCountRole, "trackCount" },
        { NumberRole, "number" },
        { ArtworksRole, "artworks" }
    };
}

QStringList PlaylistModel::artworksFor(int playlistId)
{
    QStringList artworks;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT artwork FROM playlist_tracks WHERE playlist_id = ? AND artwork <> ''"
        " GROUP BY artwork ORDER BY MIN(position) LIMIT 4"));
    q.addBindValue(playlistId);
    if (q.exec()) {
        while (q.next())
            artworks << q.value(0).toString();
    }
    return artworks;
}

void PlaylistModel::reload()
{
    beginResetModel();
    m_items.clear();
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT p.id, p.name, (SELECT COUNT(*) FROM playlist_tracks t WHERE t.playlist_id = p.id)"
        " FROM playlists p ORDER BY p.position ASC, p.id ASC"));
    while (q.next()) {
        PlaylistItem item;
        item.id = q.value(0).toInt();
        item.name = q.value(1).toString();
        item.trackCount = q.value(2).toInt();
        m_items.append(item);
    }
    for (PlaylistItem &item : m_items)
        item.artworks = artworksFor(item.id);
    endResetModel();
    Q_EMIT countChanged();
}

QVariantMap PlaylistModel::get(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    const PlaylistItem &item = m_items.at(row);
    return { { QStringLiteral("playlistId"), item.id },
             { QStringLiteral("name"), item.name },
             { QStringLiteral("trackCount"), item.trackCount },
             { QStringLiteral("artworks"), item.artworks } };
}

int PlaylistModel::indexOf(int playlistId) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == playlistId)
            return row;
    }
    return -1;
}
