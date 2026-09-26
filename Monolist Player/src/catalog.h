#pragma once

#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include "innertube.h"
#include "mediaextractor.h"

// What there is to listen to: YouTube Music's home feed and new releases for
// Home, the songs played lately, and album and playlist pages. Exposed to QML
// as the "Catalog" singleton.
//
// Song lists are SearchResultModels, so TrackTable renders them and
// Player.playModel queues them like any other list. Card shelves are plain
// lists of maps: they are only ever shown a few at a time and never edited.
class Catalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY homeChanged)
    Q_PROPERTY(QString error READ error NOTIFY homeChanged)
    // The first song shelf on the home feed ("Quick picks") and its title.
    Q_PROPERTY(SearchResultModel *quickPicks READ quickPicks CONSTANT)
    Q_PROPERTY(QString quickPicksTitle READ quickPicksTitle NOTIFY homeChanged)
    // [{ title, strapline, more, items: [{ type, browseId, videoId, title, subtitle, artwork }] }]
    // `more` is where the shelf's "show all" goes (see shelfToMap), or empty.
    Q_PROPERTY(QVariantList shelves READ shelves NOTIFY homeChanged)
    // The newest release, for the poster: { browseId, title, subtitle, artwork }.
    Q_PROPERTY(QVariantMap featured READ featured NOTIFY homeChanged)
    Q_PROPERTY(SearchResultModel *recent READ recent CONSTANT)
    // The album or playlist page open now:
    // { browseId, type, title, subtitle, artist, details, description, artwork, error }
    Q_PROPERTY(QVariantMap page READ page NOTIFY pageChanged)
    Q_PROPERTY(SearchResultModel *pageTracks READ pageTracks CONSTANT)
    Q_PROPERTY(bool pageLoading READ pageLoading NOTIFY pageChanged)
    // A long playlist arrives a hundred songs at a time: whether it has
    // more to come, and whether some are on their way now or still going
    // into the list.
    Q_PROPERTY(bool pageHasMore READ pageHasMore NOTIFY pageMoreChanged)
    Q_PROPERTY(bool pageLoadingMore READ pageLoadingMore NOTIFY pageMoreChanged)
    // A shelf's "show all" page open now:
    // { key, title, kicker, sections: [{ title, items: [card] }], error },
    // and the songs it lists, if it lists any.
    Q_PROPERTY(QVariantMap listing READ listing NOTIFY listingChanged)
    Q_PROPERTY(SearchResultModel *listingSongs READ listingSongs CONSTANT)
    Q_PROPERTY(bool listingLoading READ listingLoading NOTIFY listingChanged)
    // Apart from `listing`, so that "loading more" coming and going does not
    // redraw the cards already shown.
    Q_PROPERTY(bool listingHasMore READ listingHasMore NOTIFY listingMoreChanged)
    Q_PROPERTY(bool listingLoadingMore READ listingLoadingMore NOTIFY listingMoreChanged)
    // The album or playlist a card's play button is fetching, or empty.
    Q_PROPERTY(QString collectionLoading READ collectionLoading NOTIFY collectionLoadingChanged)
    // The artist page open now:
    // { browseId, channel, name, description, audience, artwork, songsTitle,
    //   songsId, canShuffle, canRadio, shelves: [{ title, more, items: [card] }], error }
    // `channel` is set for a plain channel's page rather than an artist's.
    // While a name is being looked up it holds only { name, lookingUp }.
    Q_PROPERTY(QVariantMap artist READ artist NOTIFY artistChanged)
    Q_PROPERTY(SearchResultModel *artistSongs READ artistSongs CONSTANT)
    Q_PROPERTY(bool artistLoading READ artistLoading NOTIFY artistChanged)
    // "shuffle" or "radio" while that is being fetched, otherwise empty.
    Q_PROPERTY(QString artistMixLoading READ artistMixLoading NOTIFY artistMixLoadingChanged)
public:
    explicit Catalog(QObject *parent = nullptr);

    bool loading() const { return m_pendingHome > 0; }
    QString error() const { return m_error; }
    SearchResultModel *quickPicks() { return &m_quickPicks; }
    QString quickPicksTitle() const { return m_quickPicksTitle; }
    QVariantList shelves() const { return m_shelves; }
    QVariantMap featured() const { return m_featured; }
    SearchResultModel *recent() { return &m_recent; }
    QVariantMap page() const { return m_page; }
    SearchResultModel *pageTracks() { return &m_pageTracks; }
    bool pageLoading() const { return m_pageLoading; }
    bool pageHasMore() const { return !m_pageNext.isEmpty(); }
    bool pageLoadingMore() const { return m_pageLoadingMore; }
    QVariantMap listing() const { return m_listing; }
    SearchResultModel *listingSongs() { return &m_listingSongs; }
    bool listingLoading() const { return m_listingLoading; }
    bool listingHasMore() const { return !m_listingItemsNext.isEmpty() || !m_listingSectionsNext.isEmpty(); }
    bool listingLoadingMore() const { return m_listingLoadingMore; }
    QString collectionLoading() const { return m_collectionLoading; }
    QVariantMap artist() const { return m_artist; }
    SearchResultModel *artistSongs() { return &m_artistSongs; }
    bool artistLoading() const { return m_artistLoading; }
    QString artistMixLoading() const { return m_mixLoading; }

public Q_SLOTS:
    void refresh();
    void reloadRecent();
    void openPage(const QString &browseId);
    // The open page's songs as maps, for "Download all".
    QVariantList pageTrackList() const;
    // The next hundred songs of a long playlist, as the reader nears the end
    // of the ones shown.
    void loadMorePage();
    // All the rest, a hundred at a time (up to kPageSongCap songs), for what
    // takes the whole playlist: Play, Shuffle, Download all, Add all.
    void loadRestOfPage();

    // A shelf's "show all": the page its "more" button names, with the
    // parameters that pick it, headed `title` (the shelf's own) while it
    // loads. Kept, like an album page, until another is opened.
    void openListing(const QString &browseId, const QString &params, const QString &title);
    // More of it, as the reader nears the end.
    void loadMoreListing();

    // A card's play button: the album's or playlist's songs, fetched without
    // opening its page, handed over by collectionReady to be played.
    // `origin` travels with them, for the player. `title` names it if it
    // will not load.
    void playCollection(const QString &browseId, const QString &title, const QString &origin);

    // An artist's page, by its channel id; `name`, where the caller knows
    // it, heads the page while the rest loads.
    void openArtist(const QString &browseId, const QString &name = QString());
    // An artist known only by name — a song kept without its links, a
    // suggestion from the catalogue. YouTube Music's artists are searched
    // for it and the one of that name opened (artistResolved); when none
    // has it, artistNotFound, and the interface searches instead.
    void openArtistNamed(const QString &name);
    // The open artist's Shuffle or Mix ("shuffle" or "radio"), fetched as
    // YouTube Music would play it; artistMixReady hands the songs over.
    void loadArtistMix(const QString &kind);
    QVariantList artistSongList() const;

Q_SIGNALS:
    void homeChanged();
    void pageChanged();
    void pageMoreChanged();
    void listingChanged();
    void listingMoreChanged();
    void collectionLoadingChanged();
    void collectionReady(const QString &origin, const QVariantList &tracks);
    void artistChanged();
    void artistMixLoadingChanged();
    void artistResolved(const QString &name, const QString &browseId);
    void artistNotFound(const QString &name);
    void artistMixReady(const QString &kind, const QVariantList &tracks);
    // Something the user should be told, for the toast.
    void notice(const QString &text);

private:
    // The two requests Home is made of. refresh() resets the retry count and
    // calls this (or queues itself behind a load already out); a failed load
    // calls it again by itself, twice.
    void load();
    void finishHome();
    static QVariantMap shelfToMap(const InnerTube::Shelf &shelf);
    static QVariantMap moreToMap(const InnerTube::Link &more);
    static QList<SearchResultModel::Item> toItems(const QList<InnerTube::Track> &tracks);
    static QVariantList toMaps(const QList<InnerTube::Track> &tracks);
    // One more part of the open page, or of the open listing.
    void fetchPagePart();
    void fetchListingPart();
    // The open page's rows go into its table a few at a time (feedPage);
    // "loading more" lasts while a part is on its way or still going in.
    void schedulePageFeed();
    void feedPage();
    void updatePageLoadingMore();

    InnerTube m_innerTube;
    SearchResultModel m_quickPicks;
    SearchResultModel m_recent;
    SearchResultModel m_pageTracks;

    int m_pendingHome = 0;
    int m_retries = 0;
    // A refresh asked for while Home was loading — a country picked in
    // Settings before the first load answered, say. It runs when that load
    // ends, and the load's own answers, now stale, are not shown.
    bool m_refreshQueued = false;
    QString m_error;
    QString m_quickPicksTitle;
    QVariantList m_homeShelves;       // from the home feed
    QVariantList m_releaseShelves;    // from new releases
    QVariantList m_shelves;           // both, in the order Home shows them
    QVariantMap m_featured;

    QString m_pageId;                 // the page whose answer is still wanted
    QVariantMap m_page;
    bool m_pageLoading = false;
    // The rest of a long playlist: the header its later songs are completed
    // from, the token for the next part, the rows shown so far (by their
    // playlist row id), the rows still to go into the table, whether a part
    // is on its way, and whether the whole rest was asked for.
    InnerTube::Collection m_pageHeader;
    QString m_pageNext;
    QSet<QString> m_pageKeys;
    QList<SearchResultModel::Item> m_pageFeed;
    bool m_pageFeedScheduled = false;
    bool m_pageRequestOut = false;
    bool m_pageLoadingMore = false;   // m_pageRequestOut or a feed, as last told
    bool m_pageWantsAll = false;

    QString m_listingKey;             // browse id and parameters, as "id|params"
    QVariantMap m_listing;
    SearchResultModel m_listingSongs;
    bool m_listingLoading = false;
    bool m_listingLoadingMore = false;
    QString m_listingItemsNext;
    QString m_listingSectionsNext;

    QString m_collectionLoading;
    quint64 m_collectionGeneration = 0;

    void showArtist(const QString &browseId, const QString &name);
    // The artist page whose answer is still wanted: a channel id, or
    // "name:" and the name being looked up.
    QString m_artistKey;
    QVariantMap m_artist;
    SearchResultModel m_artistSongs;
    bool m_artistLoading = false;
    InnerTube::Watch m_shuffle;       // the open artist's Shuffle and Mix
    InnerTube::Watch m_radio;
    QString m_mixLoading;
    quint64 m_mixGeneration = 0;      // moves on for each Shuffle or Mix asked for
};
