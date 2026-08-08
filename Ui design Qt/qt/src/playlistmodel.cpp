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
    default:             return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    return {
        { IdRole, "playlistId" },
        { NameRole, "name" },
        { TrackCountRole, "trackCount" },
        { NumberRole, "number" }
    };
}

void PlaylistModel::reload()
{
    beginResetModel();
    m_items.clear();
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id, name, track_count FROM playlists ORDER BY position ASC"));
    while (q.next())
        m_items.append({ q.value(0).toInt(), q.value(1).toString(), q.value(2).toInt() });
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
             { QStringLiteral("trackCount"), item.trackCount } };
}
