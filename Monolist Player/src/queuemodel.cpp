#include "queuemodel.h"
#include "trackmodel.h"

#include <QRandomGenerator>

#include <algorithm>
#include <limits>

// ---------------------------------------------------------------- QueueTrack

QueueTrack QueueTrack::fromMap(const QVariantMap &map)
{
    // The library and search models say "sourceId" and "sourceUrl"; the
    // downloads model says "videoId" and "filePath". Take either.
    const auto first = [&map](const char *a, const char *b) {
        const QString value = map.value(QLatin1String(a)).toString();
        return value.isEmpty() ? map.value(QLatin1String(b)).toString() : value;
    };

    QueueTrack track;
    track.videoId = first("sourceId", "videoId");
    track.title = map.value(QStringLiteral("title")).toString();
    track.artist = map.value(QStringLiteral("artist")).toString();
    track.album = map.value(QStringLiteral("album")).toString();
    track.artwork = map.value(QStringLiteral("artwork")).toString();
    track.sourceUrl = first("sourceUrl", "filePath");
    track.durationMs = map.value(QStringLiteral("durationMs")).toLongLong();
    track.trackId = map.value(QStringLiteral("trackId")).toInt();
    track.fromRadio = map.value(QStringLiteral("fromRadio")).toBool();
    track.isVideo = map.value(QStringLiteral("isVideo")).toBool();
    return track;
}

QVariantMap QueueTrack::toMap() const
{
    return {
        { QStringLiteral("trackId"), trackId },
        { QStringLiteral("sourceId"), videoId },
        { QStringLiteral("title"), title },
        { QStringLiteral("artist"), artist },
        { QStringLiteral("album"), album },
        { QStringLiteral("artwork"), artwork },
        { QStringLiteral("durationMs"), durationMs },
        { QStringLiteral("durationText"), TrackModel::formatDuration(durationMs) },
        { QStringLiteral("sourceUrl"), sourceUrl },
        { QStringLiteral("fromRadio"), fromRadio },
        { QStringLiteral("isVideo"), isVideo }
    };
}

// ---------------------------------------------------------------- QueueModel

QueueModel::QueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const QueueTrack &track = m_items.at(index.row());
    switch (role) {
    case VideoIdRole:      return track.videoId;
    case TitleRole:        return track.title;
    case ArtistRole:       return track.artist;
    case AlbumRole:        return track.album;
    case ArtworkRole:      return track.artwork;
    case DurationRole:     return track.durationMs;
    case DurationTextRole: return TrackModel::formatDuration(track.durationMs);
    case FromRadioRole:    return track.fromRadio;
    case IsCurrentRole:    return index.row() == m_current;
    case IsPastRole:       return index.row() < m_current;
    default:               return {};
    }
}

QHash<int, QByteArray> QueueModel::roleNames() const
{
    return {
        { VideoIdRole, "videoId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { AlbumRole, "album" },
        { ArtworkRole, "artwork" },
        { DurationRole, "durationMs" },
        { DurationTextRole, "durationText" },
        { FromRadioRole, "fromRadio" },
        { IsCurrentRole, "isCurrent" },
        { IsPastRole, "isPast" }
    };
}

int QueueModel::upcomingCount() const
{
    return m_current < 0 ? int(m_items.size()) : int(m_items.size()) - m_current - 1;
}

int QueueModel::radioStartIndex() const
{
    for (int row = m_current + 1; row < m_items.size(); ++row) {
        if (m_items.at(row).fromRadio)
            return row;
    }
    return -1;
}

const QueueTrack *QueueModel::at(int row) const
{
    return (row >= 0 && row < m_items.size()) ? &m_items.at(row) : nullptr;
}

bool QueueModel::containsVideo(const QString &videoId) const
{
    return std::any_of(m_items.cbegin(), m_items.cend(),
                       [&videoId](const QueueTrack &track) { return track.videoId == videoId; });
}

QVariantMap QueueModel::get(int row) const
{
    const QueueTrack *track = at(row);
    return track ? track->toMap() : QVariantMap();
}

void QueueModel::stamp(QList<QueueTrack> &tracks)
{
    for (QueueTrack &track : tracks)
        track.uid = m_nextUid++;
}

void QueueModel::setCurrentIndex(int row)
{
    row = qBound(-1, row, int(m_items.size()) - 1);
    if (row == m_current)
        return;
    const int old = m_current;
    m_current = row;
    // Rows between the old and new position change from past to upcoming or
    // back, and the two ends change "current".
    const int first = qMax(0, qMin(old, row));
    const int last = qMax(old, row);
    if (last >= 0)
        Q_EMIT dataChanged(index(first, 0), index(last, 0), { IsCurrentRole, IsPastRole });
    Q_EMIT currentIndexChanged();
    Q_EMIT upcomingChanged();
}

void QueueModel::replace(QList<QueueTrack> tracks, int current)
{
    beginResetModel();
    stamp(tracks);
    m_items = tracks;
    m_current = m_items.isEmpty() ? -1 : qBound(0, current, int(m_items.size()) - 1);
    m_originalOrder.clear();
    endResetModel();
    Q_EMIT countChanged();
    Q_EMIT currentIndexChanged();
    Q_EMIT upcomingChanged();
}

void QueueModel::insert(int row, QList<QueueTrack> tracks)
{
    if (tracks.isEmpty())
        return;
    row = qBound(0, row, int(m_items.size()));
    stamp(tracks);

    // While shuffled, remember where these belong once the order is restored:
    // "play next" at the front of the upcoming order, anything else at the end.
    if (!m_originalOrder.isEmpty()) {
        QList<quint64> uids;
        for (const QueueTrack &track : std::as_const(tracks))
            uids.append(track.uid);
        if (row == m_current + 1)
            m_originalOrder = uids + m_originalOrder;
        else
            m_originalOrder += uids;
    }

    beginInsertRows(QModelIndex(), row, row + int(tracks.size()) - 1);
    for (int i = 0; i < tracks.size(); ++i)
        m_items.insert(row + i, tracks.at(i));
    const bool shifted = m_current >= row;
    if (shifted)
        m_current += int(tracks.size());
    endInsertRows();

    Q_EMIT countChanged();
    if (shifted)
        Q_EMIT currentIndexChanged();
    Q_EMIT upcomingChanged();
}

void QueueModel::removeAt(int row)
{
    if (row < 0 || row >= m_items.size() || row == m_current)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_originalOrder.removeAll(m_items.at(row).uid);
    m_items.removeAt(row);
    const bool shifted = row < m_current;
    if (shifted)
        --m_current;
    endRemoveRows();
    Q_EMIT countChanged();
    if (shifted)
        Q_EMIT currentIndexChanged();
    Q_EMIT upcomingChanged();
}

void QueueModel::clearUpcoming()
{
    const int first = m_current + 1;
    if (first >= m_items.size())
        return;
    beginRemoveRows(QModelIndex(), first, int(m_items.size()) - 1);
    m_items.erase(m_items.begin() + first, m_items.end());
    m_originalOrder.clear();
    endRemoveRows();
    Q_EMIT countChanged();
    Q_EMIT upcomingChanged();
}

void QueueModel::shuffleUpcoming()
{
    const int first = m_current + 1;
    if (m_items.size() - first < 2)
        return;
    beginResetModel();
    m_originalOrder.clear();
    for (int row = first; row < m_items.size(); ++row)
        m_originalOrder.append(m_items.at(row).uid);
    std::shuffle(m_items.begin() + first, m_items.end(), *QRandomGenerator::global());
    endResetModel();
    Q_EMIT upcomingChanged();
}

void QueueModel::restoreOrder()
{
    if (m_originalOrder.isEmpty())
        return;
    const int first = m_current + 1;
    beginResetModel();
    // Tracks known from before the shuffle go back to their places; anything
    // the order does not know keeps its relative position, at the end.
    const auto rank = [this](const QueueTrack &track) {
        const qsizetype at = m_originalOrder.indexOf(track.uid);
        return at < 0 ? std::numeric_limits<qsizetype>::max() : at;
    };
    if (first < m_items.size()) {
        std::stable_sort(m_items.begin() + first, m_items.end(),
                         [&rank](const QueueTrack &a, const QueueTrack &b) { return rank(a) < rank(b); });
    }
    m_originalOrder.clear();
    endResetModel();
    Q_EMIT upcomingChanged();
}
