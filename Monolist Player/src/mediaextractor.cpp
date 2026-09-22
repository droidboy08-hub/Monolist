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
    connect(&m_innerTube, &InnerTube::searchFinished, this,
            [this](const QString &query, const QList<InnerTube::Track> &tracks) {
                if (query != m_query)
                    return;   // superseded by a newer search
                if (tracks.isEmpty()) {
                    // Either nothing matched or the layout moved under the
                    // parser; yt-dlp settles which.
                    searchWithYtDlp(query);
                    return;
                }
                QList<SearchResultModel::Item> items;
                items.reserve(tracks.size());
                for (const InnerTube::Track &track : tracks)
                    items.append({ track.videoId, track.title, track.artist, track.album,
                                   track.artwork, track.durationMs });
                finishSearch(items, QStringLiteral("YouTube Music"));
            });

    connect(&m_innerTube, &InnerTube::searchFailed, this,
            [this](const QString &query, const QString &reason) {
                if (query != m_query)
                    return;
                qWarning("Monolist: YouTube Music search failed (%s); trying yt-dlp.", qPrintable(reason));
                searchWithYtDlp(query);
            });

    connect(&m_innerTube, &InnerTube::suggestionsReady, this,
            [this](const QString &input, const QStringList &suggestions) {
                if (input != m_suggestFor)
                    return;
                m_suggestions = suggestions.mid(0, 8);
                Q_EMIT suggestionsChanged();
            });
}

// Searching needs only the network now; yt-dlp is the fallback, not a
// requirement.
bool MediaExtractor::available() const
{
    return true;
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

void MediaExtractor::setSource(const QString &source)
{
    if (source == m_source)
        return;
    m_source = source;
    Q_EMIT resultsSourceChanged();
}

void MediaExtractor::setFilter(const QString &filter)
{
    if (filter == m_filter || (filter != QLatin1String("songs") && filter != QLatin1String("videos")))
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    if (!m_query.isEmpty())
        search(m_query);
}

void MediaExtractor::search(const QString &query)
{
    const QString trimmed = query.trimmed();
    // First, so that anything cancelled below reads as stale in its handler.
    m_query = trimmed;
    cancel();

    if (trimmed.isEmpty()) {
        m_results.clear();
        Q_EMIT searchFinished({});
        return;
    }

    setBusy(true);
    setLastError(QString());
    m_innerTube.search(trimmed, m_filter == QLatin1String("videos") ? InnerTube::Filter::Videos
                                                                     : InnerTube::Filter::Songs);
}

void MediaExtractor::finishSearch(const QList<SearchResultModel::Item> &items, const QString &source)
{
    QVariantList plain;
    plain.reserve(items.size());
    for (const SearchResultModel::Item &item : items) {
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
    setSource(source);
    setBusy(false);
    Q_EMIT searchFinished(plain);
}

void MediaExtractor::searchWithYtDlp(const QString &query)
{
    if (!YtDlp::isAvailable()) {
        m_results.clear();
        setBusy(false);
        const QString reason = QStringLiteral("YouTube Music did not answer, and yt-dlp is not installed to fall back on.");
        setLastError(reason);
        Q_EMIT failed(reason);
        return;
    }

    YtDlpRequest *request = YtDlp::search(query, 25, this);
    m_request = request;

    connect(request, &YtDlpRequest::succeededJson, this, [this, query](const QJsonDocument &document) {
        if (query != m_query)
            return;
        const QJsonArray entries = document.object().value(QStringLiteral("entries")).toArray();

        QList<SearchResultModel::Item> items;
        items.reserve(entries.size());
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
            item.artist = YtDlp::cleanArtist(item.artist);
            item.album = entry.value(QStringLiteral("album")).toString();
            item.durationMs = qint64(entry.value(QStringLiteral("duration")).toDouble() * 1000.0);

            const QJsonArray thumbnails = entry.value(QStringLiteral("thumbnails")).toArray();
            if (!thumbnails.isEmpty())
                item.artwork = thumbnails.last().toObject().value(QStringLiteral("url")).toString();

            items.append(item);
        }
        finishSearch(items, QStringLiteral("yt-dlp"));
    });

    connect(request, &YtDlpRequest::failed, this, [this, query](const QString &reason) {
        if (query != m_query)
            return;
        m_results.clear();
        setBusy(false);
        setLastError(reason);
        Q_EMIT failed(reason);
    });
}

void MediaExtractor::suggest(const QString &input)
{
    const QString trimmed = input.trimmed();
    m_suggestFor = trimmed;
    if (trimmed.isEmpty()) {
        clearSuggestions();
        return;
    }
    m_innerTube.suggest(trimmed);
}

void MediaExtractor::clearSuggestions()
{
    m_innerTube.cancelSuggestions();
    m_suggestFor.clear();
    if (m_suggestions.isEmpty())
        return;
    m_suggestions.clear();
    Q_EMIT suggestionsChanged();
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
    m_innerTube.cancelSearch();
    if (m_request && m_request->isRunning())
        m_request->cancel();
    m_request.clear();
    setBusy(false);
}
