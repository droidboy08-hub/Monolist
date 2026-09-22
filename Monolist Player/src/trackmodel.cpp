#include "trackmodel.h"
#include "appdatabase.h"

#include <QSqlQuery>
#include <QVariant>

TrackModel::TrackModel(QObject *parent)
    : QAbstractListModel(parent) {}

int TrackModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QString TrackModel::formatDuration(qint64 ms)
{
    const qint64 total = ms / 1000;
    const qint64 minutes = total / 60;
    const qint64 seconds = total % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QVariant TrackModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const TrackItem &item = m_items.at(index.row());
    switch (role) {
    case IdRole:           return item.id;
    case TitleRole:        return item.title;
    case ArtistRole:       return item.artist;
    case AlbumRole:        return item.album;
    case DurationRole:     return item.durationMs;
    case DurationTextRole: return formatDuration(item.durationMs);
    case SourceRole:       return item.sourceUrl;
    case SourceIdRole:     return item.sourceId;
    case ArtworkRole:      return item.artwork;
    case FavouriteRole:    return item.favourite;
    default:               return {};
    }
}

QHash<int, QByteArray> TrackModel::roleNames() const
{
    return {
        { IdRole, "trackId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { AlbumRole, "album" },
        { DurationRole, "durationMs" },
        { DurationTextRole, "durationText" },
        { SourceRole, "sourceUrl" },
        { SourceIdRole, "sourceId" },
        { ArtworkRole, "artwork" },
        { FavouriteRole, "favourite" }
    };
}

void TrackModel::reload()
{
    beginResetModel();
    m_items.clear();
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id, title, artist, album, duration_ms, source_url, source_id,"
                          " artwork, favourite FROM tracks ORDER BY position ASC"));
    while (q.next())
        m_items.append({ q.value(0).toInt(), q.value(1).toString(), q.value(2).toString(),
                         q.value(3).toString(), q.value(4).toLongLong(), q.value(5).toString(),
                         q.value(6).toString(), q.value(7).toString(), q.value(8).toBool() });
    endResetModel();
    Q_EMIT countChanged();
}

QVariantMap TrackModel::get(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    const TrackItem &item = m_items.at(row);
    return {
        { QStringLiteral("trackId"), item.id },
        { QStringLiteral("title"), item.title },
        { QStringLiteral("artist"), item.artist },
        { QStringLiteral("album"), item.album },
        { QStringLiteral("durationMs"), item.durationMs },
        { QStringLiteral("durationText"), formatDuration(item.durationMs) },
        { QStringLiteral("sourceUrl"), item.sourceUrl },
        { QStringLiteral("sourceId"), item.sourceId },
        { QStringLiteral("artwork"), item.artwork },
        { QStringLiteral("favourite"), item.favourite }
    };
}

int TrackModel::indexOfTrack(int trackId) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == trackId)
            return row;
    }
    return -1;
}

int TrackModel::indexOfSource(const QString &sourceId) const
{
    if (sourceId.isEmpty())
        return -1;
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).sourceId == sourceId)
            return row;
    }
    return -1;
}

void TrackModel::addTrack(const QVariantMap &track, bool favourite)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id, artwork, favourite)"
        " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, ?, ?, '', ?, ?, ?"));
    q.addBindValue(track.value(QStringLiteral("title")).toString());
    q.addBindValue(track.value(QStringLiteral("artist")).toString());
    q.addBindValue(track.value(QStringLiteral("album")).toString());
    q.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    q.addBindValue(track.value(QStringLiteral("sourceId")).toString());
    q.addBindValue(track.value(QStringLiteral("artwork")).toString());
    q.addBindValue(favourite ? 1 : 0);
    q.exec();
    reload();
}

void TrackModel::toggleFavourite(int row)
{
    if (row < 0 || row >= m_items.size())
        return;
    TrackItem &item = m_items[row];
    item.favourite = !item.favourite;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE tracks SET favourite = ? WHERE id = ?"));
    q.addBindValue(item.favourite ? 1 : 0);
    q.addBindValue(item.id);
    q.exec();
    const QModelIndex idx = index(row, 0);
    Q_EMIT dataChanged(idx, idx, { FavouriteRole });
}
