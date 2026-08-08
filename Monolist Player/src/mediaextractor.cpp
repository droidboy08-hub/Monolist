#include "mediaextractor.h"
#include "trackmodel.h"
#include "ytdlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ------------------------------------------------------------ SearchResultModel

SearchResultModel::SearchResultModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int SearchResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant SearchResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case SourceIdRole:     return item.sourceId;
    case TitleRole:        return item.title;
    case ArtistRole:       return item.artist;
    case AlbumRole:        return item.album;
    case ArtworkRole:      return item.artwork;
    case DurationRole:     return item.durationMs;
    case DurationTextRole: return TrackModel::formatDuration(item.durationMs);
    default:               return {};
    }
}

QHash<int, QByteArray> SearchResultModel::roleNames() const
{
    return {
        { SourceIdRole, "sourceId" },
        { TitleRole, "title" },
        { ArtistRole, "artist" },
        { AlbumRole, "album" },
        { ArtworkRole, "artwork" },
        { DurationRole, "durationMs" },
        { DurationTextRole, "durationText" }
    };
}

void SearchResultModel::replace(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
    Q_EMIT countChanged();
}

void SearchResultModel::clear()
{
    replace({});
}

QVariantMap SearchResultModel::get(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    const Item &item = m_items.at(row);
    return {
        { QStringLiteral("sourceId"),     item.sourceId },
        { QStringLiteral("title"),        item.title },
        { QStringLiteral("artist"),       item.artist },
        { QStringLiteral("album"),        item.album },
        { QStringLiteral("artwork"),      item.artwork },
        { QStringLiteral("durationMs"),   item.durationMs },
        { QStringLiteral("durationText"), TrackModel::formatDuration(item.durationMs) }
    };
}

// --------------------------------------------------------------- MediaExtractor

MediaExtractor::MediaExtractor(QObject *parent)
    : QObject(parent)
{
}

bool MediaExtractor::available() const
{
    return YtDlp::isAvailable();
}

void MediaExtractor::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged();
}

void MediaExtractor::setLastError(const QString &error)
{
    if (error == m_lastError)
        return;
    m_lastError = error;
    Q_EMIT lastErrorChanged();
}

void MediaExtractor::search(const QString &query)
{
    cancel();

    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        m_results.clear();
        Q_EMIT searchFinished({});
        return;
    }

    setBusy(true);
    setLastError(QString());

    YtDlpRequest *request = YtDlp::search(trimmed, 25, this);
    m_request = request;

    connect(request, &YtDlpRequest::succeededJson, this, [this](const QJsonDocument &document) {
        const QJsonArray entries = document.object().value(QStringLiteral("entries")).toArray();

        QList<SearchResultModel::Item> items;
        items.reserve(entries.size());
        QVariantList plain;

        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            const QString id = entry.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;

            SearchResultModel::Item item;
            item.sourceId = id;
            item.title = entry.value(QStringLiteral("title")).toString();
            // --flat-playlist reports the channel under one of these two keys
            // depending on yt-dlp version; take whichever is present.
            item.artist = entry.value(QStringLiteral("uploader")).toString();
            if (item.artist.isEmpty())
                item.artist = entry.value(QStringLiteral("channel")).toString();
            item.album = entry.value(QStringLiteral("album")).toString();
            item.durationMs = qint64(entry.value(QStringLiteral("duration")).toDouble() * 1000.0);

            const QJsonArray thumbnails = entry.value(QStringLiteral("thumbnails")).toArray();
            if (!thumbnails.isEmpty())
                item.artwork = thumbnails.last().toObject().value(QStringLiteral("url")).toString();

            items.append(item);
            plain.append(QVariantMap{
                { QStringLiteral("sourceId"),   item.sourceId },
                { QStringLiteral("title"),      item.title },
                { QStringLiteral("artist"),     item.artist },
                { QStringLiteral("album"),      item.album },
                { QStringLiteral("artwork"),    item.artwork },
                { QStringLiteral("durationMs"), item.durationMs }
            });
        }

        m_results.replace(items);
        setBusy(false);
        Q_EMIT searchFinished(plain);
    });

    connect(request, &YtDlpRequest::failed, this, [this](const QString &reason) {
        m_results.clear();
        setBusy(false);
        setLastError(reason);
        Q_EMIT failed(reason);
    });
}

void MediaExtractor::resolve(const QString &videoIdOrUrl)
{
    setBusy(true);
    setLastError(QString());

    YtDlpRequest *request = YtDlp::resolveAudio(videoIdOrUrl, this);
    m_request = request;

    connect(request, &YtDlpRequest::succeededJson, this, [this](const QJsonDocument &document) {
        const QJsonObject root = document.object();
        setBusy(false);
        Q_EMIT resolved(QVariantMap{
            { QStringLiteral("url"),        root.value(QStringLiteral("url")).toString() },
            { QStringLiteral("container"),  root.value(QStringLiteral("ext")).toString() },
            { QStringLiteral("title"),      root.value(QStringLiteral("title")).toString() },
            { QStringLiteral("artist"),     root.value(QStringLiteral("uploader")).toString() },
            { QStringLiteral("durationMs"), qint64(root.value(QStringLiteral("duration")).toDouble() * 1000.0) },
            { QStringLiteral("abr"),        root.value(QStringLiteral("abr")).toDouble() }
        });
    });

    connect(request, &YtDlpRequest::failed, this, [this](const QString &reason) {
        setBusy(false);
        setLastError(reason);
        Q_EMIT failed(reason);
    });
}

void MediaExtractor::cancel()
{
    if (m_request && m_request->isRunning())
        m_request->cancel();
    m_request.clear();
    setBusy(false);
}
