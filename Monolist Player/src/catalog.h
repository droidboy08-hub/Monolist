#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include "innertube.h"
#include "mediaextractor.h"

class YtmSession;

// Shelves as a list model, for a page that adds them a few at a time (Home's
// feed below its first page): each addition makes only the new ones, where a
// list property would have every shelf above made again.
class ShelfModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles { ShelfRole = Qt::UserRole + 1 };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_shelves.size());
    }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return { { ShelfRole, "shelf" } }; }

    void append(const QVariantList &shelves);
    void clear();

Q_SIGNALS:
    void countChanged();

private:
    QVariantList m_shelves;
};

// What there is to listen to: YouTube Music's home feed and new releases for
// Home, the songs played lately, and album and playlist pages. Exposed to QML
// as the "Catalog" singleton.
//
// The home feed is the signed-in account's own while there is one and
// Settings lets Home use it (followAccount); new releases, and every page,
// are asked anonymously as they always were.
//
// Song lists are SearchResultModels, so TrackTable renders them and
// Player.playModel queues them like any other list. Card shelves are plain
// lists of maps: they are only ever shown a few at a time and never edited.
class Catalog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY homeChanged)
    Q_PROPERTY(QString error READ error NOTIFY homeChanged)
    // When what Home shows was saved ("30 Sep, 14:02"), while it is the last
    // launch's rather than a fresh answer: from launch until a load has
    // answered in full. Empty once Home is fresh.
    Q_PROPERTY(QString savedAt READ savedAt NOTIFY homeChanged)
    // The first song shelf on the home feed ("Quick picks"), its title and
    // the small line above it ("For Demo Listener" on the account's feed).
    Q_PROPERTY(SearchResultModel *quickPicks READ quickPicks CONSTANT)
    Q_PROPERTY(QString quickPicksTitle READ quickPicksTitle NOTIFY homeChanged)
    Q_PROPERTY(QString quickPicksStrapline READ quickPicksStrapline NOTIFY homeChanged)
    // Home shows the YouTube Music account's own feed: asked as the account,
    // and answered as it (logged_in=1). Its shelves are then in the feed's
    // own order, Quick picks among them before shelf `quickPicksAt` (-1: at
    // the top, as the signed-out Home has it), and new releases after them.
    Q_PROPERTY(bool personalFeed READ personalFeed NOTIFY homeChanged)
    Q_PROPERTY(int quickPicksAt READ quickPicksAt NOTIFY homeChanged)
    // [{ title, strapline, more, items: [{ type, browseId, videoId, title, subtitle, artwork }] }]
    // `more` is where the shelf's "show all" goes (see shelfToMap), or empty.
    Q_PROPERTY(QVariantList shelves READ shelves NOTIFY homeChanged)
    // The feed's shelves below its first page, a page more each time the
    // reader nears the end (loadMoreHome), after `shelves`.
    Q_PROPERTY(ShelfModel *moreShelves READ moreShelves CONSTANT)
    Q_PROPERTY(bool homeHasMore READ homeHasMore NOTIFY homeMoreChanged)
    Q_PROPERTY(bool homeLoadingMore READ homeLoadingMore NOTIFY homeMoreChanged)
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
    // Only whether a part is on its way: once none is, and there is no more,
    // every song is here (pageTrackList), even while the table is still
    // making their rows. What takes the whole playlist waits for this alone.
    Q_PROPERTY(bool pageFetching READ pageFetching NOTIFY pageMoreChanged)
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

    // The YouTube Music session Home's feed may be asked as. Its
    // sessionChanged and useForHomeChanged ask for the feed again whenever
    // they change whose feed it would be; the account's feed leaves Home at
    // once when it stops being wanted. Unset (or null), Home is signed out.
    void followAccount(YtmSession *account);
    // Whether the feed Home shows, or the one on its way, was asked as the
    // account; and `logged_in` from the last feed that came (InnerTube::
    // parseLoggedIn: "1", "0", or empty). For the self-tests and the log.
    bool feedAsAccount() const { return m_feedAsAccount; }
    QString feedLoggedIn() const { return m_feedLoggedIn; }
    // Which pages are the account's own (YtmImport::isAccountPage: Liked
    // music and the account's playlists, private ones among them): those
    // are opened, paged and played with the account (Auth::IfSignedIn),
    // every other page signed out as before. Unset, every page is.
    void setAccountPages(std::function<bool(const QString &browseId)> isAccountPage);
    // Which pages are the account's changed (the import turned off): a page
    // or listing open with the account that no longer is one is closed,
    // rather than paged on with the account.
    void accountPagesChanged();

    bool loading() const { return m_pendingHome > 0; }
    QString error() const { return m_error; }
    QString savedAt() const { return m_savedAt; }
    // Home as the last good answers left it, for this country, shown before
    // anything is asked: called once at launch, before refresh(). False when
    // there is nothing saved. Only the signed-out feed is ever saved; the
    // account's is always asked for afresh.
    bool showSaved();
    SearchResultModel *quickPicks() { return &m_quickPicks; }
    QString quickPicksTitle() const { return m_quickPicksTitle; }
    QString quickPicksStrapline() const { return m_quickPicksStrapline; }
    bool personalFeed() const { return m_personalFeed; }
    int quickPicksAt() const { return m_quickPicksAt; }
    QVariantList shelves() const { return m_shelves; }
    ShelfModel *moreShelves() { return &m_moreShelves; }
    bool homeHasMore() const { return !m_homeNext.isEmpty(); }
    bool homeLoadingMore() const { return m_homeLoadingMore; }
    // The feed's next page of shelves, asked as its first page was (the
    // account's feed as the account), and at most a few pages a feed: each
    // is a request. Nothing while one is out, or Home is loading.
    Q_INVOKABLE void loadMoreHome();
    QVariantMap featured() const { return m_featured; }
    SearchResultModel *recent() { return &m_recent; }
    QVariantMap page() const { return m_page; }
    SearchResultModel *pageTracks() { return &m_pageTracks; }
    bool pageLoading() const { return m_pageLoading; }
    bool pageHasMore() const { return !m_pageNext.isEmpty(); }
    bool pageLoadingMore() const { return m_pageLoadingMore; }
    bool pageFetching() const { return m_pageRequestOut; }
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
    // Every song of the open page that has arrived, as maps, those not yet
    // in the table included: for Play, Shuffle, Download all and Add all.
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
    void homeMoreChanged();
    void pageChanged();
    void pageMoreChanged();
    void listingChanged();
    void listingMoreChanged();
    // More cards at the end of the listing's section `section`, which
    // `listing` holds too: the view adds them to the grid it has, where a
    // listingChanged would make every card again.
    void listingAppended(int section, const QVariantList &cards);
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
    // The two requests Home is made of, as flags: the feed, which may be the
    // account's, and new releases, which never are.
    enum HomePart { Feed = 1, Releases = 2 };
    // Asks for those parts now, or once a load already out has ended.
    // refresh() asks for both; a change of account asks for the feed alone.
    void ask(int parts);
    // Sends them. ask() resets the retry count first; a load that brought
    // nothing at all calls this again by itself, twice, for both.
    void load(int parts);
    void finishHome();
    void accountChanged();
    // Reads a home feed's answer into Home's parts; `asAccount` when it was
    // asked (or checked) as the account.
    void applyFeed(const QJsonObject &root, bool asAccount);
    // Home's shelves in the order it shows them, from the feed's and new
    // releases'.
    void composeShelves();
    // A feed's shelves, as Home shows them: a run of songs after Quick picks
    // as cards that play, and what is the account's own noted for authFor.
    QVariantMap feedShelfToMap(const InnerTube::Shelf &shelf, bool personal);
    // The feed's pages after its first forgotten: a new feed, or none.
    void resetMoreHome();
    // New releases' answer into its shelves and the poster.
    void applyReleases(const QJsonObject &root);
    // A good answer kept for the next launch, as it came.
    void saveAnswer(const QString &part, const QJsonObject &root);
    // How a page is asked for: as the account for its own pages (the
    // account's playlists, Liked music, and what its feed showed, a mix
    // made for it), signed out for any other.
    InnerTube::Auth authFor(const QString &browseId) const;
    // Closes the page open with the account, saying why, with nothing of it
    // left waiting (its parts, a Play that wanted all of it).
    void dropAccountPage(const QString &why);
    void dropAccountListing(const QString &why);
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
    // The parts asked for while Home was loading — a country picked in
    // Settings before the first load answered, say, or the account confirmed.
    // They are asked again when that load ends, and the load's own answers
    // for them, now stale, are not shown.
    int m_queuedParts = 0;
    QPointer<YtmSession> m_account;
    bool m_feedAsAccount = false;
    QString m_feedLoggedIn;
    QString m_error;
    QString m_savedAt;
    QString m_quickPicksTitle;
    QString m_quickPicksStrapline;
    bool m_personalFeed = false;
    int m_quickPicksAt = -1;
    int m_feedQuickPicksAt = -1;      // where Quick picks sat among the feed's shelves
    // The pages the account's feed showed (a mix made for it, one of its
    // playlists), which are opened as the account while its feed is shown.
    QSet<QString> m_feedAccountIds;
    QVariantList m_homeShelves;       // from the home feed
    QVariantList m_releaseShelves;    // from new releases
    QVariantList m_shelves;           // both, in the order Home shows them
    // The feed after its first page: its shelves so far, the token for the
    // next page, and how many pages have come; the generation moves on with
    // every new feed, so a page asked for an old one is not shown under it.
    ShelfModel m_moreShelves;
    QString m_homeNext;
    QString m_homeVisitor;            // the signed-out feed's visitorData, which its next pages need
    bool m_homeLoadingMore = false;
    int m_homePagesMore = 0;
    quint64 m_homeGeneration = 0;
    QVariantMap m_featured;

    QString m_pageId;                 // the page whose answer is still wanted
    // Moves on with every openPage. The id alone could not tell a page's
    // answer from the one its earlier opening is still waiting for (A, B,
    // then A again on a slow link), and the older, landing second, reset
    // the list under the parts loaded since.
    quint64 m_pageGeneration = 0;
    QVariantMap m_page;
    bool m_pageLoading = false;
    std::function<bool(const QString &)> m_accountPages;
    // How the open page was asked for, which its later parts are asked for
    // the same way; and the listing's.
    InnerTube::Auth m_pageAuth = InnerTube::Auth::Anonymous;
    InnerTube::Auth m_listingAuth = InnerTube::Auth::Anonymous;
    // A part that would not load: not asked for again as the reader scrolls
    // (which would ask for ever, the account's page with the account), only
    // when they ask for the rest again (Play) or open the page afresh.
    QString m_pageFailedToken;
    QString m_listingFailedToken;
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
    bool m_pageFetchingTold = false;  // m_pageRequestOut, as last told
    bool m_pageWantsAll = false;

    QString m_listingKey;             // browse id and parameters, as "id|params"
    quint64 m_listingGeneration = 0;  // as m_pageGeneration, for openListing
    QString m_listingOwner;           // the artist a discography's cards are by, or empty
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
