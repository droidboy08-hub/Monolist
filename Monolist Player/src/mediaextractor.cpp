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

bool SearchResultModel::contains(const QString &sourceId) const
{
    for (const Item &item : m_items) {
        if (item.sourceId == sourceId)
            return true;
    }
    return false;
}

bool SearchResultModel::move(int from, int to)
{
    const int count = int(m_items.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return false;
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    m_items.move(from, to);
    endMoveRows();
    return true;
}

void SearchResultModel::clear()
{
    replace({});
}

QVariantMap SearchResultModel::get(int row) const
{
    if (row < 0 || row >= m_items.size())
        return {};
    return toMap(m_items.at(row));
}

QVariantMap SearchResultModel::toMap(const Item &item)
{
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
            [this](const QString &query, const QList<InnerTube::Track> &tracks, const QString &next) {
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
                m_next = next;
                finishSearch(items, QStringLiteral("YouTube Music"));
                Q_EMIT moreChanged();
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
        QString next;
    };
    auto parts = std::make_shared<QList<Part>>();
    if (m_filter == QLatin1String("albums")) {
        parts->append({ InnerTube::Filter::Albums, QString(), {}, {}, {} });
    } else if (m_filter == QLatin1String("artists")) {
        parts->append({ InnerTube::Filter::Artists, QString(), {}, {}, {} });
    } else {
        // Named for whose playlists they are, not where they came from: the
        // line beside the chips already says "From YouTube Music" about the
        // search itself, a few lines above the first of these.
        parts->append({ InnerTube::Filter::FeaturedPlaylists, QStringLiteral("YouTube Music's playlists"), {}, {}, {} });
        parts->append({ InnerTube::Filter::CommunityPlaylists, QStringLiteral("Listeners' playlists"), {}, {}, {} });
    }

    const quint64 generation = ++m_cardGeneration;
    auto waiting = std::make_shared<int>(int(parts->size()));
    for (int i = 0; i < parts->size(); ++i) {
        m_innerTube.searchCards(query, parts->at(i).filter,
                                [this, generation, parts, waiting, i](const QList<InnerTube::Card> &cards,
                                                                      const QString &next, const QString &error) {
            if (generation != m_cardGeneration)
                return;   // a newer search, or none
            (*parts)[i].cards = cards;
            (*parts)[i].error = error;
            (*parts)[i].next = next;
            if (--*waiting > 0)
                return;

            QVariantList sections;
            QString failure;
            m_cardParts.clear();
            for (const Part &part : std::as_const(*parts)) {
                if (!part.error.isEmpty())
                    failure = part.error;
                if (part.cards.isEmpty())
                    continue;
                QVariantList items;
                CardPart shown{ {}, part.next };
                for (const InnerTube::Card &card : part.cards) {
                    items.append(InnerTube::cardToVariant(card));
                    shown.shown.insert(card.browseId);
                }
                sections.append(QVariantMap{ { QStringLiteral("title"), part.title },
                                             { QStringLiteral("items"), items } });
                m_cardParts.append(shown);
            }
            setCardSections(sections);
            Q_EMIT moreChanged();
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

    // As many as yt-dlp's search is asked for (YtDlp::search's most): it has
    // no next page to ask for later.
    YtDlpRequest *request = YtDlp::search(query, 50, this);
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

bool MediaExtractor::hasMore() const
{
    if (!m_cardParts.isEmpty())
        return !m_cardParts.constLast().next.isEmpty();
    return !m_next.isEmpty();
}

QVariantList MediaExtractor::cardMore() const
{
    QVariantList more;
    for (const CardPart &part : m_cardParts)
        more.append(!part.next.isEmpty());
    return more;
}

void MediaExtractor::setLoadingMore(bool loading)
{
    if (loading == m_loadingMore)
        return;
    m_loadingMore = loading;
    Q_EMIT moreChanged();
}

// One page at a time, and only when the page asks: as the reader nears the
// end of what is shown, or presses a section's "show more". Anonymous, as
// the search was. A page that fails is not asked for again by itself (the
// page would ask on every scroll); a new search starts afresh. A page that
// brings nothing new ends it too, so a token that went round in circles
// cannot keep the page asking.
void MediaExtractor::loadMore(int section)
{
    if (m_loadingMore || m_busy)
        return;
    const quint64 generation = m_moreGeneration;

    if (m_cardParts.isEmpty()) {
        if (m_next.isEmpty())
            return;
        setLoadingMore(true);
        m_innerTube.searchMore(m_next, [this, generation](const InnerTube::SearchPage &page, const QString &error) {
            if (generation != m_moreGeneration)
                return;   // a newer search, or none
            if (!error.isEmpty()) {
                qWarning("Monolist: the search's next page failed (%s); the results end here.", qPrintable(error));
                m_next.clear();
                setLoadingMore(false);
                return;
            }
            QList<SearchResultModel::Item> items;
            QSet<QString> added;
            for (const InnerTube::Track &track : page.tracks) {
                if (m_results.contains(track.videoId) || added.contains(track.videoId))
                    continue;
                added.insert(track.videoId);
                items.append(SearchResultModel::fromTrack(track));
            }
            m_results.append(items);
            m_next = items.isEmpty() ? QString() : page.next;
            setLoadingMore(false);
        });
        return;
    }

    if (section < 0)
        section = int(m_cardParts.size()) - 1;
    if (section >= m_cardParts.size() || m_cardParts.at(section).next.isEmpty())
        return;
    setLoadingMore(true);
    m_innerTube.searchMore(m_cardParts.at(section).next,
                           [this, generation, section](const InnerTube::SearchPage &page, const QString &error) {
        if (generation != m_moreGeneration)
            return;
        CardPart &part = m_cardParts[section];
        if (!error.isEmpty()) {
            qWarning("Monolist: the search's next page failed (%s); the results end here.", qPrintable(error));
            part.next.clear();
            setLoadingMore(false);
            return;
        }
        QVariantList cards;
        for (const InnerTube::Card &card : page.cards) {
            if (part.shown.contains(card.browseId))
                continue;
            part.shown.insert(card.browseId);
            cards.append(InnerTube::cardToVariant(card));
        }
        part.next = cards.isEmpty() ? QString() : page.next;
        if (!cards.isEmpty())
            Q_EMIT cardsAppended(section, cards);
        setLoadingMore(false);
    });
}

void MediaExtractor::cancel()
{
    m_innerTube.cancelSearch();
    ++m_cardGeneration;
    // The pages after go with the search they belonged to.
    ++m_moreGeneration;
    m_next.clear();
    m_cardParts.clear();
    setLoadingMore(false);
    Q_EMIT moreChanged();
    if (m_request && m_request->isRunning())
        m_request->cancel();
    m_request.clear();
    setBusy(false);
}
