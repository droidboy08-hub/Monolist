#include "downloadmodels.h"
#include "appdatabase.h"
#include "trackmodel.h"

#include <QFileInfo>
#include <QSqlQuery>
#include <QVariant>

namespace {

// What each yt-dlp post-processor is doing, in the interface's words.
QString describeStep(const QString &postProcessor)
{
    if (postProcessor == QLatin1String("ExtractAudio"))
        return QStringLiteral("Extracting audio");
    if (postProcessor == QLatin1String("Metadata"))
        return QStringLiteral("Writing tags");
    if (postProcessor == QLatin1String("ThumbnailsConvertor"))
        return QStringLiteral("Preparing cover art");
    if (postProcessor == QLatin1String("EmbedThumbnail"))
        return QStringLiteral("Adding cover art");
    if (postProcessor == QLatin1String("SponsorBlock") || postProcessor == QLatin1String("ModifyChapters"))
        return QStringLiteral("Trimming non-music parts");
    return QStringLiteral("Finishing");
}

} // namespace

// ---------------------------------------------------------- DownloadQueueModel

DownloadQueueModel::DownloadQueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DownloadQueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant DownloadQueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case VideoIdRole:  return item.videoId;
    case TitleRole:    return item.title.isEmpty() ? item.videoId : item.title;   // until yt-dlp reports it
    case ArtistRole:   return item.artist;
    case ArtworkRole:  return item.artwork;
    case DurationRole: return item.durationMs;
    case StateRole:    return stateName(item.state);
    case ProgressRole: return item.progress;
    case DetailRole:   return detailText(item);
    default:           return {};
    }
}

QHash<int, QByteArray> DownloadQueueModel::roleNames() const
{
    return {
        { VideoIdRole, "videoId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { ArtworkRole, "artwork" },
        { DurationRole, "durationMs" },
        { StateRole, "phase" },   // not "state": that would shadow Item.state in a delegate
        { ProgressRole, "progress" },
        { DetailRole, "detail" }
    };
}

int DownloadQueueModel::indexOf(const QString &videoId) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).videoId == videoId)
            return row;
    }
    return -1;
}

// The pointer is only good until the model next changes; callers copy.
const DownloadQueueModel::Item *DownloadQueueModel::find(const QString &videoId) const
{
    const int row = indexOf(videoId);
    return row < 0 ? nullptr : &m_items.at(row);
}

void DownloadQueueModel::upsert(const Item &item)
{
    const int row = indexOf(item.videoId);
    if (row >= 0) {
        m_items[row] = item;
        const QModelIndex changed = index(row, 0);
        Q_EMIT dataChanged(changed, changed);
        return;
    }
    const int end = int(m_items.size());
    beginInsertRows(QModelIndex(), end, end);
    m_items.append(item);
    endInsertRows();
    Q_EMIT countChanged();
}

void DownloadQueueModel::remove(const QString &videoId)
{
    const int row = indexOf(videoId);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    Q_EMIT countChanged();
}

QString DownloadQueueModel::stateName(State state)
{
    switch (state) {
    case State::Queued:      return QStringLiteral("queued");
    case State::Downloading: return QStringLiteral("downloading");
    case State::Processing:  return QStringLiteral("processing");
    case State::Failed:      return QStringLiteral("failed");
    }
    return {};
}

QString DownloadQueueModel::detailText(const Item &item)
{
    switch (item.state) {
    case State::Queued:
        return QStringLiteral("Waiting");
    case State::Processing:
        return describeStep(item.step) + QStringLiteral("…");
    case State::Failed:
        return item.error.isEmpty() ? QStringLiteral("Failed") : item.error;
    case State::Downloading: {
        QStringList parts;
        if (item.total > 0) {
            parts << QStringLiteral("%1 of %2").arg(DownloadLibraryModel::formatBytes(item.received),
                                                    DownloadLibraryModel::formatBytes(item.total));
        } else if (item.received > 0) {
            parts << DownloadLibraryModel::formatBytes(item.received);
        }
        if (item.speed > 0)
            parts << DownloadLibraryModel::formatBytes(qint64(item.speed)) + QStringLiteral("/s");
        if (item.eta >= 0 && item.total > 0)
            parts << TrackModel::formatDuration(qint64(item.eta) * 1000) + QStringLiteral(" left");
        return parts.isEmpty() ? QStringLiteral("Starting") : parts.join(QStringLiteral(" · "));
    }
    }
    return {};
}

// -------------------------------------------------------- DownloadLibraryModel

DownloadLibraryModel::DownloadLibraryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DownloadLibraryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant DownloadLibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case VideoIdRole:      return item.videoId;
    case TitleRole:        return item.title;
    case ArtistRole:       return item.artist;
    case ArtworkRole:      return item.artwork;
    case DurationRole:     return item.durationMs;
    case DurationTextRole: return TrackModel::formatDuration(item.durationMs);
    case FilePathRole:     return item.filePath;
    case FormatRole:       return QFileInfo(item.filePath).suffix().toUpper();
    case SizeTextRole:     return formatBytes(item.bytes);
    default:               return {};
    }
}

QHash<int, QByteArray> DownloadLibraryModel::roleNames() const
{
    return {
        { VideoIdRole, "videoId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { ArtworkRole, "artwork" },
        { DurationRole, "durationMs" },
        { DurationTextRole, "durationText" },
        { FilePathRole, "filePath" },
        { FormatRole, "format" },
        { SizeTextRole, "sizeText" }
    };
}

void DownloadLibraryModel::reload()
{
    beginResetModel();
    m_items.clear();
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT video_id, title, artist, artwork, file_path, duration_ms, bytes"
        " FROM downloads ORDER BY downloaded_at DESC, rowid DESC"));
    while (query.next()) {
        Item item;
        item.videoId = query.value(0).toString();
        item.title = query.value(1).toString();
        item.artist = query.value(2).toString();
        item.artwork = query.value(3).toString();
        item.filePath = query.value(4).toString();
        item.durationMs = query.value(5).toLongLong();
        item.bytes = query.value(6).toLongLong();
        if (QFileInfo::exists(item.filePath))   // not deleted outside the app
            m_items.append(item);
    }
    endResetModel();
    Q_EMIT countChanged();
}

QString DownloadLibraryModel::totalSizeText() const
{
    qint64 total = 0;
    for (const Item &item : m_items)
        total += item.bytes;
    return formatBytes(total);
}

QVariantMap DownloadLibraryModel::get(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    const Item &item = m_items.at(row);
    return {
        { QStringLiteral("videoId"), item.videoId },
        { QStringLiteral("title"), item.title },
        { QStringLiteral("artist"), item.artist },
        { QStringLiteral("artwork"), item.artwork },
        { QStringLiteral("durationMs"), item.durationMs },
        { QStringLiteral("filePath"), item.filePath }
    };
}

QString DownloadLibraryModel::formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return {};
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    const double megabytes = double(bytes) / (1024.0 * 1024.0);
    if (megabytes < 1024.0)
        return QStringLiteral("%1 MB").arg(megabytes, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(megabytes / 1024.0, 0, 'f', 2);
}
