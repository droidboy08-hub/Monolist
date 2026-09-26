#include "mediaextractor.h"
#include "trackmodel.h"
#include "ytdlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

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
    case EntryIdRole:      return item.entryId;
    case IsVideoRole:      return item.isVideo;
    case PrimaryArtistRole: return item.primaryArtist;
    case CreditsRole:      return item.credits;
    case AlbumIdRole:      return item.albumId;
    default:               return {};
    }
}

SearchResultModel::Item SearchResultModel::fromTrack(const InnerTube::Track &track)
{
    Item item;
    item.sourceId = track.videoId;
    item.title = track.title;
    item.artist = track.artist;
    item.album = track.album;
    item.artwork = track.artwork;
    item.durationMs = track.durationMs;
    item.isVideo = track.isVideo;
    item.primaryArtist = track.primaryArtist;
    item.credits = InnerTube::creditsToVariant(track.credits);
    item.albumId = track.albumId;
    return item;
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
        { DurationTextRole, "durationText" },
        { EntryIdRole, "entryId" },
        { IsVideoRole, "isVideo" },
        { PrimaryArtistRole, "primaryArtist" },
        { CreditsRole, "credits" },
        { AlbumIdRole, "albumId" }
    };
}

void SearchResultModel::replace(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
    Q_EMIT countChanged();
}

void SearchResultModel::append(const QList<Item> &items)
{
    if (items.isEmpty())
        return;
    const int first = int(m_items.size());
    beginInsertRows(QModelIndex(), first, first + int(items.size()) - 1);
    m_items += items;
    endInsertRows();
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
        { QStringLiteral("durationText"), TrackModel::formatDuration(item.durationMs) },
        { QStringLiteral("entryId"),      item.entryId },
        { QStringLiteral("isVideo"),      item.isVideo },
        { QStringLiteral("primaryArtist"), item.primaryArtist },
        { QStringLiteral("credits"),      item.credits },
        { QStringLiteral("albumId"),      item.albumId }
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
                    // parser. youtube.com is asked next: one request, about a
                    // second, where yt-dlp costs eight.
                    m_innerTube.searchYouTube(query);
                    return;
                }
                QList<SearchResultModel::Item> items;
                items.reserve(tracks.size());
                for (const InnerTube::Track &track : tracks)
                    items.append(SearchResultModel::fromTrack(track));
                finishSearch(items, QStringLiteral("YouTube Music"));
            });

    connect(&m_innerTube, &InnerTube::searchFailed, this,
            [this](const QString &query, const QString &reason) {
                if (query != m_query)
                    return;
                qWarning("Monolist: YouTube Music search failed (%s); trying youtube.com.", qPrintable(reason));
                m_innerTube.searchYouTube(query);
            });

    // Second rung: youtube.com itself. Videos with their channel rather than
    // songs with their album, but it answers in about a second.
    connect(&m_innerTube, &InnerTube::youtubeSearchFinished, this,
            [this](const QString &query, const QList<InnerTube::Track> &tracks) {
                if (query != m_query)
                    return;
                if (tracks.isEmpty()) {
                    searchWithYtDlp(query);
                    return;
                }
                QList<SearchResultModel::Item> items;
                items.reserve(tracks.size());
                for (const InnerTube::Track &track : tracks)
                    items.append({ track.videoId, track.title, YtDlp::cleanArtist(track.artist),
                                   track.album, track.artwork, track.durationMs, 0, track.isVideo });
                finishSearch(items, QStringLiteral("YouTube"));
            });

    connect(&m_innerTube, &InnerTube::youtubeSearchFailed, this,
            [this](const QString &query, const QString &reason) {
                if (query != m_query)
                    return;
                qWarning("Monolist: youtube.com search failed (%s); trying yt-dlp.", qPrintable(reason));
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

namespace {

bool isCardFilter(const QString &filter)
{
    return filter == QLatin1String("albums") || filter == QLatin1String("artists")
        || filter == QLatin1String("playlists");
}

} // namespace

void MediaExtractor::setFilter(const QString &filter)
{
    if (filter == m_filter)
        return;
    if (filter != QLatin1String("songs") && filter != QLatin1String("videos") && !isCardFilter(filter))
        return;
    m_filter = filter;
    Q_EMIT filterChanged();
    if (!m_query.isEmpty())
        search(m_query);
}

void MediaExtractor::setCardSections(const QVariantList &sections)
{
    if (sections.isEmpty() && m_cardSections.isEmpty())
        return;
    m_cardSections = sections;
    Q_EMIT cardSectionsChanged();
}

void MediaExtractor::search(const QString &query)
{
    const QString trimmed = query.trimmed();
    // First, so that anything cancelled below reads as stale in its handler.
    m_query = trimmed;
    cancel();
    // Whatever went wrong belonged to the last query. Cleared even when the
    // field was emptied, or the old failure sits on the search page with
    // nothing typed above it.
    setLastError(QString());

    if (trimmed.isEmpty()) {
        m_results.clear();
        setCardSections({});
        Q_EMIT searchFinished({});
        return;
    }

    setBusy(true);
    // Songs and cards are never shown together: the other kind's answers
    // go as soon as the filter changes, not when the new ones arrive.
    if (isCardFilter(m_filter)) {
        m_results.clear();
        searchCards(trimmed);
        return;
    }
    setCardSections({});
    m_innerTube.search(trimmed, m_filter == QLatin1String("videos") ? InnerTube::Filter::Videos
                                                                     : InnerTube::Filter::Songs);
}

// Albums, artists or playlists: YouTube Music is the only place that has
// them, so there is no youtube.com or yt-dlp to fall back on, and a failure
// is said as one. Playlists are two searches, YouTube Music's own and its
// listeners', run side by side and shown together once both have answered.
void MediaExtractor::searchCards(const QString &query)
{
    struct Part {
        InnerTube::Filter filter;
        QString title;
        QList<InnerTube::Card> cards;
        QString error;
    };
    auto parts = std::make_shared<QList<Part>>();
    if (m_filter == QLatin1String("albums")) {
        parts->append({ InnerTube::Filter::Albums, QString(), {}, {} });
    } else if (m_filter == QLatin1String("artists")) {
        parts->append({ InnerTube::Filter::Artists, QString(), {}, {} });
    } else {
        parts->append({ InnerTube::Filter::FeaturedPlaylists, QStringLiteral("From YouTube Music"), {}, {} });
        parts->append({ InnerTube::Filter::CommunityPlaylists, QStringLiteral("From listeners"), {}, {} });
    }

    const quint64 generation = ++m_cardGeneration;
    auto waiting = std::make_shared<int>(int(parts->size()));
    for (int i = 0; i < parts->size(); ++i) {
        m_innerTube.searchCards(query, parts->at(i).filter,
                                [this, generation, parts, waiting, i](const QList<InnerTube::Card> &cards,
                                                                      const QString &error) {
            if (generation != m_cardGeneration)
                return;   // a newer search, or none
            (*parts)[i].cards = cards;
            (*parts)[i].error = error;
            if (--*waiting > 0)
                return;

            QVariantList sections;
            QString failure;
            for (const Part &part : std::as_const(*parts)) {
                if (!part.error.isEmpty())
                    failure = part.error;
                if (part.cards.isEmpty())
                    continue;
                QVariantList items;
                for (const InnerTube::Card &card : part.cards)
                    items.append(InnerTube::cardToVariant(card));
                sections.append(QVariantMap{ { QStringLiteral("title"), part.title },
                                             { QStringLiteral("items"), items } });
            }
            setCardSections(sections);
            setSource(QStringLiteral("YouTube Music"));
            setBusy(false);
            // Half an answer is still worth showing; nothing at all because
            // the request failed is said, rather than "No results".
            if (sections.isEmpty() && !failure.isEmpty()) {
                const QString reason = QStringLiteral("YouTube Music did not answer: %1").arg(failure);
                setLastError(reason);
                Q_EMIT failed(reason);
                return;
            }
            Q_EMIT cardSearchFinished();
        });
    }
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

            // A plain YouTube search finds videos; whether one is worth
            // watching is not known from a flat listing, so trust the filter.
            item.isVideo = m_filter == QLatin1String("videos");
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
    ++m_cardGeneration;
    if (m_request && m_request->isRunning())
        m_request->cancel();
    m_request.clear();
    setBusy(false);
}
