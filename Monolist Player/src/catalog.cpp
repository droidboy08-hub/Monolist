#include "catalog.h"
#include "appdatabase.h"
#include "artistlinks.h"

#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

#include <utility>

namespace {

// YouTube's own limit for a playlist. A playlist is followed to its end, a
// hundred songs at a time; this only stops a list that never ends.
constexpr int kPageSongCap = 5000;
// How many rows go into a page's song table at once (feedPage).
constexpr int kFeedRows = 25;

} // namespace

Catalog::Catalog(QObject *parent)
    : QObject(parent)
{
}

// Where a shelf's "show all" can go, as the view names it: "page" for an
// album's or a playlist's page, "artist" for an artist's, "browse" for a
// page of cards (an artist's albums, the week's new releases). Anything else
// is left without the link. A channel's own sections ("Playlists by…")
// answer nothing to a caller who is not signed in, and a link to an empty
// page is worse than none.
QVariantMap Catalog::moreToMap(const InnerTube::Link &more)
{
    const QString &id = more.browseId;
    const QString &type = more.pageType;
    QString kind;
    if (id.startsWith(QLatin1String("VL")) || id.startsWith(QLatin1String("MPRE"))
        || type == QLatin1String("MUSIC_PAGE_TYPE_PLAYLIST") || type == QLatin1String("MUSIC_PAGE_TYPE_ALBUM"))
        kind = QStringLiteral("page");
    else if (type == QLatin1String("MUSIC_PAGE_TYPE_ARTIST") && more.params.isEmpty())
        kind = QStringLiteral("artist");
    else if (id.startsWith(QLatin1String("FEmusic_")) || id.startsWith(QLatin1String("MPAD"))
             || type == QLatin1String("MUSIC_PAGE_TYPE_ARTIST_DISCOGRAPHY"))
        kind = QStringLiteral("browse");
    if (kind.isEmpty() || id.isEmpty())
        return {};
    return {
        { QStringLiteral("kind"), kind },
        { QStringLiteral("browseId"), id },
        { QStringLiteral("params"), more.params }
    };
}

QVariantMap Catalog::shelfToMap(const InnerTube::Shelf &shelf)
{
    QVariantList items;
    items.reserve(shelf.cards.size());
    for (const InnerTube::Card &card : shelf.cards)
        items.append(InnerTube::cardToVariant(card));
    return {
        { QStringLiteral("title"), shelf.title },
        { QStringLiteral("strapline"), shelf.strapline },
        { QStringLiteral("more"), moreToMap(shelf.more) },
        { QStringLiteral("items"), items }
    };
}

QList<SearchResultModel::Item> Catalog::toItems(const QList<InnerTube::Track> &tracks)
{
    QList<SearchResultModel::Item> items;
    items.reserve(tracks.size());
    for (const InnerTube::Track &track : tracks)
        items.append(SearchResultModel::fromTrack(track));
    return items;
}

// As rows, in the shape every list hands the player.
QVariantList Catalog::toMaps(const QList<InnerTube::Track> &tracks)
{
    SearchResultModel rows;
    rows.replace(toItems(tracks));
    QVariantList list;
    list.reserve(rows.rowCount());
    for (int row = 0; row < rows.rowCount(); ++row)
        list.append(rows.get(row));
    return list;
}

// Home is two requests, the home feed and new releases, run side by side;
// the view updates once both have answered.
void Catalog::refresh()
{
    // A load is already out, asked from whatever country was set when it
    // started. Refusing this one would leave Home on that country after a
    // new one was picked mid-load, so it waits and runs when the load ends.
    if (m_pendingHome > 0) {
        m_refreshQueued = true;
        return;
    }
    m_retries = 0;   // asked for, so start counting again
    load();
}

void Catalog::load()
{
    if (m_pendingHome > 0)
        return;
    m_pendingHome = 2;
    m_error.clear();
    Q_EMIT homeChanged();

    m_innerTube.browse(QStringLiteral("FEmusic_home"), [this](const QJsonObject &root, const QString &error) {
        // Superseded while it was out: the queued refresh asks again, and
        // this answer is for what was asked before.
        if (m_refreshQueued) {
            finishHome();
            return;
        }
        if (error.isEmpty()) {
            m_homeShelves.clear();
            bool songsTaken = false;
            for (const InnerTube::Shelf &shelf : InnerTube::parseShelves(root)) {
                // The first run of songs is Home's own list; later ones are rare
                // and would repeat its look, so they are left out.
                if (!shelf.songs.isEmpty()) {
                    if (!songsTaken) {
                        m_quickPicks.replace(toItems(shelf.songs));
                        m_quickPicksTitle = shelf.title;
                        songsTaken = true;
                    }
                    continue;
                }
                m_homeShelves.append(shelfToMap(shelf));
            }
        } else {
            m_error = error;
        }
        finishHome();
    });

    m_innerTube.browse(QStringLiteral("FEmusic_new_releases"), [this](const QJsonObject &root, const QString &error) {
        if (m_refreshQueued) {
            finishHome();
            return;
        }
        if (error.isEmpty()) {
            m_releaseShelves.clear();
            m_featured.clear();
            for (const InnerTube::Shelf &shelf : InnerTube::parseShelves(root)) {
                // Albums and singles only: new music videos are a video
                // player's front page, not a music player's.
                if (shelf.cards.isEmpty() || shelf.cards.first().type != QLatin1String("album"))
                    continue;
                QVariantMap map = shelfToMap(shelf);
                if (m_featured.isEmpty())
                    m_featured = InnerTube::cardToVariant(shelf.cards.first());
                // The country is part of what this shelf is: another one
                // lists different records. Settings picks it.
                map.insert(QStringLiteral("title"), QStringLiteral("New releases"));
                map.insert(QStringLiteral("strapline"), QStringLiteral("ALBUMS & SINGLES · %1").arg(InnerTube::region()));
                m_releaseShelves.append(map);
            }
        } else if (m_error.isEmpty()) {
            m_error = error;
        }
        finishHome();
    });
}

void Catalog::finishHome()
{
    if (--m_pendingHome > 0)
        return;
    // Asked again while this load was out — another country picked in
    // Settings. Load again rather than settle on, or retry, the old one; Home
    // stays "loading" throughout, since the next load starts before anything
    // is announced.
    if (std::exchange(m_refreshQueued, false)) {
        refresh();
        return;
    }
    m_shelves = m_releaseShelves + m_homeShelves;
    // One failed request out of two still leaves a page worth showing.
    if (!m_shelves.isEmpty() || m_quickPicks.rowCount() > 0) {
        m_error.clear();
        m_retries = 0;
    } else if (m_retries < 2) {
        // Nothing at all: a request that dropped, most likely. Ask again by
        // itself rather than leaving Home empty until someone presses RETRY.
        ++m_retries;
        QTimer::singleShot(m_retries * 4000, this, &Catalog::load);
    }
    Q_EMIT homeChanged();
}

void Catalog::reloadRecent()
{
    QList<SearchResultModel::Item> items;
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT video_id, title, artist, album, artwork, duration_ms, is_video FROM recent"
        " ORDER BY played_at DESC, rowid DESC LIMIT 10"));
    while (query.next()) {
        items.append({ query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                       query.value(3).toString(), query.value(4).toString(), query.value(5).toLongLong(),
                       0, query.value(6).toBool() });
    }
    m_recent.replace(items);
}

void Catalog::openPage(const QString &browseId)
{
    if (browseId.isEmpty())
        return;
    const bool alreadyHere = browseId == m_pageId && !m_page.contains(QStringLiteral("error"))
                          && (m_pageLoading || m_pageTracks.rowCount() > 0);
    if (alreadyHere)
        return;   // open already, or on its way

    m_pageId = browseId;
    m_pageLoading = true;
    m_page = { { QStringLiteral("browseId"), browseId } };
    m_pageTracks.clear();
    m_pageHeader = {};
    m_pageNext.clear();
    m_pageKeys.clear();
    m_pageFeed.clear();
    m_pageFeedScheduled = false;
    m_pageRequestOut = false;
    m_pageLoadingMore = false;
    m_pageWantsAll = false;
    Q_EMIT pageChanged();
    Q_EMIT pageMoreChanged();

    m_innerTube.browse(browseId, [this, browseId](const QJsonObject &root, const QString &error) {
        if (browseId != m_pageId)
            return;   // another page was opened meanwhile
        m_pageLoading = false;
        if (!error.isEmpty()) {
            m_page.insert(QStringLiteral("error"), error);
            Q_EMIT pageChanged();
            return;
        }
        InnerTube::Collection collection = InnerTube::parseCollection(browseId, root);
        for (const InnerTube::Track &track : std::as_const(collection.tracks))
            m_pageKeys.insert(track.setVideoId.isEmpty() ? track.videoId : track.setVideoId);
        m_pageNext = collection.continuation;
        m_pageHeader = collection;
        m_pageHeader.tracks.clear();   // only the header is wanted from here on
        m_page = {
            { QStringLiteral("browseId"), browseId },
            { QStringLiteral("type"), collection.type },
            { QStringLiteral("title"), collection.title },
            { QStringLiteral("subtitle"), collection.subtitle },
            { QStringLiteral("artist"), collection.artist },
            { QStringLiteral("credits"), InnerTube::creditsToVariant(collection.artistCredits) },
            { QStringLiteral("details"), collection.details },
            { QStringLiteral("description"), collection.description },
            { QStringLiteral("artwork"), collection.artwork }
        };
        if (collection.title.isEmpty() && collection.tracks.isEmpty())
            m_page.insert(QStringLiteral("error"), QStringLiteral("YouTube Music did not return this page."));
        // The first rows at once, which fill the screen; the rest of the
        // hundred just after, below it (feedPage).
        const QList<SearchResultModel::Item> items = toItems(collection.tracks);
        m_pageTracks.replace(items.mid(0, kFeedRows));
        m_pageFeed = items.mid(kFeedRows);
        schedulePageFeed();
        updatePageLoadingMore();
        Q_EMIT pageChanged();
        Q_EMIT pageMoreChanged();
    });
}

void Catalog::loadMorePage()
{
    if (m_pageNext.isEmpty() || m_pageLoading || m_pageLoadingMore)
        return;
    fetchPagePart();
}

void Catalog::loadRestOfPage()
{
    // No token: whatever is still being added to the list is the rest, and
    // "loading more" ends when it is in.
    if (m_pageNext.isEmpty() || m_pageLoading)
        return;
    m_pageWantsAll = true;
    // A part already on its way carries on to the rest when it lands.
    if (!m_pageRequestOut)
        fetchPagePart();
}

void Catalog::updatePageLoadingMore()
{
    const bool loading = m_pageRequestOut || !m_pageFeed.isEmpty();
    if (loading == m_pageLoadingMore)
        return;
    m_pageLoadingMore = loading;
    Q_EMIT pageMoreChanged();
}

void Catalog::schedulePageFeed()
{
    if (m_pageFeedScheduled || m_pageFeed.isEmpty())
        return;
    m_pageFeedScheduled = true;
    const QString browseId = m_pageId;
    QTimer::singleShot(0, this, [this, browseId]() {
        if (browseId != m_pageId)
            return;   // for a page no longer open; openPage started afresh
        m_pageFeedScheduled = false;
        feedPage();
    });
}

// A hundred rows of a song table take a quarter of a second to make on a slow
// machine, all in one frame, and the page would stop under the pointer while
// they were made. A few at a time, a frame each, keep it scrolling; the rows
// land below what is on screen, so the list is not seen to build.
void Catalog::feedPage()
{
    if (!m_pageFeed.isEmpty()) {
        const QList<SearchResultModel::Item> chunk = m_pageFeed.mid(0, kFeedRows);
        m_pageFeed.remove(0, chunk.size());
        m_pageTracks.append(chunk);
    }
    schedulePageFeed();
    updatePageLoadingMore();
}

void Catalog::fetchPagePart()
{
    const QString browseId = m_pageId;
    const QString token = m_pageNext;
    m_pageRequestOut = true;
    updatePageLoadingMore();

    m_innerTube.continueBrowse(token, [this, browseId, token](const QJsonObject &root, const QString &error) {
        // Another page opened meanwhile, or this one opened afresh.
        if (browseId != m_pageId || token != m_pageNext)
            return;
        m_pageRequestOut = false;
        if (!error.isEmpty()) {
            // The token is kept, so scrolling on, or pressing Play again,
            // asks once more.
            m_pageWantsAll = false;
            updatePageLoadingMore();
            const QString title = m_page.value(QStringLiteral("title")).toString();
            Q_EMIT notice(QStringLiteral("The rest of %1 would not load: %2")
                              .arg(title.isEmpty() ? QStringLiteral("this playlist") : title, error));
            return;
        }

        const InnerTube::Continuation part = InnerTube::parseContinuation(root);
        QList<InnerTube::Track> fresh;
        for (InnerTube::Track track : part.tracks) {
            const QString key = track.setVideoId.isEmpty() ? track.videoId : track.setVideoId;
            if (m_pageKeys.contains(key))
                continue;
            m_pageKeys.insert(key);
            InnerTube::completeTrack(track, m_pageHeader);
            fresh.append(track);
        }
        // A part with nothing new in it is the end, whatever its token says:
        // some of YouTube Music's own playlists answer every "more" with
        // their first hundred songs again, and would be followed for ever.
        m_pageNext = fresh.isEmpty() ? QString() : part.next;
        const qsizetype room = kPageSongCap - m_pageTracks.rowCount() - m_pageFeed.size();
        if (fresh.size() >= room) {
            fresh = fresh.mid(0, qMax<qsizetype>(0, room));
            m_pageNext.clear();
        }
        m_pageFeed += toItems(fresh);
        schedulePageFeed();

        // The next part is asked for while this one is still being added.
        if (m_pageWantsAll && !m_pageNext.isEmpty())
            fetchPagePart();
        else
            m_pageWantsAll = false;
        updatePageLoadingMore();
        Q_EMIT pageMoreChanged();   // pageHasMore may have changed with it
    });
}

void Catalog::openListing(const QString &browseId, const QString &params, const QString &title)
{
    if (browseId.isEmpty())
        return;
    const QString key = browseId + QLatin1Char('|') + params;
    const bool alreadyHere = key == m_listingKey && !m_listing.contains(QStringLiteral("error"))
                          && (m_listingLoading || m_listing.contains(QStringLiteral("sections")));
    if (alreadyHere)
        return;   // open already, or on its way

    m_listingKey = key;
    m_listingLoading = true;
    m_listingLoadingMore = false;
    m_listingItemsNext.clear();
    m_listingSectionsNext.clear();
    m_listing = { { QStringLiteral("key"), key }, { QStringLiteral("title"), title } };
    m_listingSongs.clear();
    Q_EMIT listingChanged();
    Q_EMIT listingMoreChanged();

    m_innerTube.browse(browseId, params, [this, key, title](const QJsonObject &root, const QString &error) {
        if (key != m_listingKey)
            return;   // another was opened meanwhile
        m_listingLoading = false;
        if (!error.isEmpty()) {
            m_listing.insert(QStringLiteral("error"), error);
            Q_EMIT listingChanged();
            return;
        }
        const InnerTube::Listing listing = InnerTube::parseListing(root);
        QVariantList sections;
        QList<InnerTube::Track> songs;
        for (const InnerTube::Shelf &shelf : listing.sections) {
            songs += shelf.songs;
            if (!shelf.cards.isEmpty())
                sections.append(shelfToMap(shelf));
        }
        // The shelf's own name, which is what was clicked; the page's (an
        // artist's name over their albums) goes above it.
        const QString heading = title.isEmpty() ? listing.title : title;
        m_listing = {
            { QStringLiteral("key"), key },
            { QStringLiteral("title"), heading },
            { QStringLiteral("kicker"), listing.title.compare(heading, Qt::CaseInsensitive) == 0
                                            ? QString() : listing.title },
            { QStringLiteral("sections"), sections }
        };
        if (sections.isEmpty() && songs.isEmpty())
            m_listing.insert(QStringLiteral("error"), QStringLiteral("YouTube Music sent nothing for this shelf."));
        m_listingSongs.replace(toItems(songs));
        m_listingItemsNext = listing.itemsContinuation;
        m_listingSectionsNext = listing.sectionsContinuation;
        Q_EMIT listingChanged();
        Q_EMIT listingMoreChanged();
    });
}

void Catalog::loadMoreListing()
{
    if (m_listingLoading || m_listingLoadingMore || !listingHasMore())
        return;
    fetchListingPart();
}

void Catalog::fetchListingPart()
{
    // The last section's own items first; the sections after it once those
    // are all in, so the page grows downwards in the order it reads.
    const bool items = !m_listingItemsNext.isEmpty();
    const QString token = items ? m_listingItemsNext : m_listingSectionsNext;
    const QString key = m_listingKey;
    m_listingLoadingMore = true;
    Q_EMIT listingMoreChanged();

    m_innerTube.continueBrowse(token, [this, key, items, token](const QJsonObject &root, const QString &error) {
        if (key != m_listingKey || token != (items ? m_listingItemsNext : m_listingSectionsNext))
            return;
        m_listingLoadingMore = false;
        if (!error.isEmpty()) {
            Q_EMIT listingMoreChanged();
            Q_EMIT notice(QStringLiteral("The rest of this shelf would not load: %1").arg(error));
            return;
        }
        const InnerTube::Continuation part = InnerTube::parseContinuation(root);
        QVariantList sections = m_listing.value(QStringLiteral("sections")).toList();
        if (!part.cards.isEmpty()) {
            QVariantMap last = sections.isEmpty() ? QVariantMap() : sections.takeLast().toMap();
            QVariantList cards = last.value(QStringLiteral("items")).toList();
            for (const InnerTube::Card &card : part.cards)
                cards.append(InnerTube::cardToVariant(card));
            last.insert(QStringLiteral("items"), cards);
            sections.append(last);
        }
        for (const InnerTube::Shelf &shelf : part.shelves) {
            if (!shelf.cards.isEmpty())
                sections.append(shelfToMap(shelf));
            m_listingSongs.append(toItems(shelf.songs));
        }
        m_listingSongs.append(toItems(part.tracks));
        m_listing.insert(QStringLiteral("sections"), sections);
        // A part that brought nothing is the end, as for a playlist.
        const bool empty = part.cards.isEmpty() && part.tracks.isEmpty() && part.shelves.isEmpty();
        if (items)
            m_listingItemsNext = empty ? QString() : part.next;
        else
            m_listingSectionsNext = empty ? QString() : part.next;
        if (!empty)
            Q_EMIT listingChanged();
        Q_EMIT listingMoreChanged();
    });
}

void Catalog::playCollection(const QString &browseId, const QString &title, const QString &origin)
{
    if (browseId.isEmpty())
        return;
    // Its page, open and loaded already: its songs as they are.
    if (browseId == m_pageId && !m_pageLoading && m_pageFeed.isEmpty() && m_pageTracks.rowCount() > 0) {
        Q_EMIT collectionReady(origin, pageTrackList());
        return;
    }
    const quint64 generation = ++m_collectionGeneration;
    m_collectionLoading = browseId;
    Q_EMIT collectionLoadingChanged();

    // Its first page only, for a long playlist: starting at once is what a
    // play button on a card is for, and the rest is a click away on the
    // playlist's own page, whose Play loads all of it first.
    m_innerTube.browse(browseId, [this, browseId, title, origin, generation](const QJsonObject &root,
                                                                            const QString &error) {
        // Another card was pressed meanwhile: that is the one wanted now.
        if (generation != m_collectionGeneration)
            return;
        m_collectionLoading.clear();
        Q_EMIT collectionLoadingChanged();
        const QString name = title.isEmpty() ? QStringLiteral("That album") : title;
        if (!error.isEmpty()) {
            Q_EMIT notice(QStringLiteral("%1 would not load: %2").arg(name, error));
            return;
        }
        const InnerTube::Collection collection = InnerTube::parseCollection(browseId, root);
        if (collection.tracks.isEmpty()) {
            Q_EMIT notice(QStringLiteral("YouTube Music has nothing in %1 to play here.").arg(name));
            return;
        }
        Q_EMIT collectionReady(origin, toMaps(collection.tracks));
    });
}

QVariantList Catalog::pageTrackList() const
{
    QVariantList list;
    for (int row = 0; row < m_pageTracks.rowCount(); ++row)
        list.append(m_pageTracks.get(row));
    return list;
}

QVariantList Catalog::artistSongList() const
{
    QVariantList list;
    for (int row = 0; row < m_artistSongs.rowCount(); ++row)
        list.append(m_artistSongs.get(row));
    return list;
}

void Catalog::openArtist(const QString &browseId, const QString &name)
{
    if (browseId.isEmpty())
        return;
    const bool alreadyHere = browseId == m_artistKey && !m_artist.contains(QStringLiteral("error"))
                          && (m_artistLoading || m_artist.contains(QStringLiteral("name")));
    if (alreadyHere)
        return;   // open already, or on its way
    showArtist(browseId, name);
}

void Catalog::showArtist(const QString &browseId, const QString &name)
{
    m_artistKey = browseId;
    m_artistLoading = true;
    // The name, where it is known already, so the page has its heading at
    // once rather than after the answer.
    const bool same = m_artist.value(QStringLiteral("browseId")).toString() == browseId;
    const QString knownName = !name.isEmpty() ? name
                            : same ? m_artist.value(QStringLiteral("name")).toString() : QString();
    m_artist = { { QStringLiteral("browseId"), browseId } };
    if (!knownName.isEmpty())
        m_artist.insert(QStringLiteral("name"), knownName);
    m_shuffle = {};
    m_radio = {};
    m_artistSongs.clear();
    Q_EMIT artistChanged();

    m_innerTube.browse(browseId, [this, browseId](const QJsonObject &root, const QString &error) {
        if (browseId != m_artistKey)
            return;   // another artist was opened meanwhile
        m_artistLoading = false;
        if (!error.isEmpty()) {
            m_artist.insert(QStringLiteral("error"), error);
            Q_EMIT artistChanged();
            return;
        }
        const InnerTube::Artist artist = InnerTube::parseArtist(browseId, root);
        QVariantList shelves;
        for (const InnerTube::Shelf &shelf : artist.shelves)
            shelves.append(shelfToMap(shelf));
        m_shuffle = artist.shuffle;
        m_radio = artist.radio;
        m_artist = {
            { QStringLiteral("browseId"), browseId },
            { QStringLiteral("channel"), !artist.artistPage },
            { QStringLiteral("name"), artist.name },
            { QStringLiteral("description"), artist.description },
            { QStringLiteral("audience"), artist.audience },
            { QStringLiteral("artwork"), artist.artwork },
            { QStringLiteral("songsTitle"), artist.songsTitle },
            { QStringLiteral("songsId"), artist.songsId },
            { QStringLiteral("canShuffle"), !artist.shuffle.playlistId.isEmpty() },
            { QStringLiteral("canRadio"), !artist.radio.playlistId.isEmpty() },
            { QStringLiteral("shelves"), shelves }
        };
        if (artist.name.isEmpty() && artist.songs.isEmpty() && shelves.isEmpty())
            m_artist.insert(QStringLiteral("error"), QStringLiteral("YouTube Music did not return this artist."));
        m_artistSongs.replace(toItems(artist.songs));
        Q_EMIT artistChanged();
    });
}

void Catalog::openArtistNamed(const QString &name)
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty())
        return;
    const QString key = QStringLiteral("name:") + wanted;
    if (key == m_artistKey && m_artistLoading)
        return;   // being looked up already

    m_artistKey = key;
    m_artistLoading = true;
    m_artist = { { QStringLiteral("name"), wanted }, { QStringLiteral("lookingUp"), true } };
    m_shuffle = {};
    m_radio = {};
    m_artistSongs.clear();
    Q_EMIT artistChanged();

    m_innerTube.searchArtists(wanted, [this, key, wanted](const QList<InnerTube::ArtistHit> &hits,
                                                          const QString &error) {
        if (key != m_artistKey)
            return;
        // The search itself failed: no answer either way, so the page says
        // so rather than going on looking.
        if (!error.isEmpty()) {
            m_artistLoading = false;
            m_artist.insert(QStringLiteral("lookingUp"), false);
            m_artist.insert(QStringLiteral("error"), error);
            Q_EMIT artistChanged();
            return;
        }
        // The artist of exactly that name, else the same name written
        // another way ("Guns N’ Roses"). The first hit alone is not enough:
        // a search for someone YouTube Music does not have still answers,
        // with whoever sounds nearest, and that is the wrong page to open.
        const InnerTube::ArtistHit *found = nullptr;
        for (const InnerTube::ArtistHit &hit : hits) {
            if (hit.name == wanted) {
                found = &hit;
                break;
            }
        }
        if (!found) {
            const QString loose = ArtistLinks::looseKey(wanted);
            for (const InnerTube::ArtistHit &hit : hits) {
                if (!loose.isEmpty() && ArtistLinks::looseKey(hit.name) == loose) {
                    found = &hit;
                    break;
                }
            }
        }
        if (!found) {
            m_artistLoading = false;
            m_artist.insert(QStringLiteral("error"),
                            QStringLiteral("YouTube Music has no artist called %1.").arg(wanted));
            Q_EMIT artistChanged();
            Q_EMIT artistNotFound(wanted);
            return;
        }
        const QString browseId = found->browseId;
        showArtist(browseId, found->name);
        Q_EMIT artistResolved(wanted, browseId);
    });
}

void Catalog::loadArtistMix(const QString &kind)
{
    const InnerTube::Watch watch = kind == QLatin1String("radio") ? m_radio : m_shuffle;
    const QString name = m_artist.value(QStringLiteral("name")).toString();
    if (watch.playlistId.isEmpty())
        return;
    const quint64 generation = ++m_mixGeneration;
    m_mixLoading = kind;
    Q_EMIT artistMixLoadingChanged();

    m_innerTube.watchPlaylist(watch, [this, kind, name, generation](const QList<InnerTube::Track> &tracks,
                                                                    const QString &error) {
        // Pressed again, or the other button: the newer press is what the
        // listener is waiting for.
        if (generation != m_mixGeneration)
            return;
        m_mixLoading.clear();
        Q_EMIT artistMixLoadingChanged();
        if (!error.isEmpty()) {
            Q_EMIT notice(kind == QLatin1String("radio")
                          ? QStringLiteral("%1's radio would not load: %2").arg(name, error)
                          : QStringLiteral("%1's shuffle would not load: %2").arg(name, error));
            return;
        }
        Q_EMIT artistMixReady(kind, toMaps(tracks));
    });
}
