#include "catalog.h"
#include "appdatabase.h"
#include "artistlinks.h"
#include "ytmsession.h"

#include <QDateTime>
#include <QJsonDocument>
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

// Whose records these are, on the album cards whose own line names nobody:
// an artist's page, and its discography, print "Album • 2019" under each of
// theirs. Saved to the library from its menu (Library::setSaved), such a card
// would otherwise be kept with no artist, and nothing would fill it in later.
void markOwner(QVariantList &cards, const QString &owner)
{
    if (owner.isEmpty())
        return;
    for (QVariant &value : cards) {
        QVariantMap card = value.toMap();
        if (card.value(QStringLiteral("type")).toString() != QLatin1String("album")
            || !card.value(QStringLiteral("artist")).toString().isEmpty())
            continue;
        card.insert(QStringLiteral("owner"), owner);
        value = card;
    }
}

void markSectionsOwner(QVariantList &sections, const QString &owner)
{
    if (owner.isEmpty())
        return;
    for (QVariant &value : sections) {
        QVariantMap section = value.toMap();
        QVariantList items = section.value(QStringLiteral("items")).toList();
        markOwner(items, owner);
        section.insert(QStringLiteral("items"), items);
        value = section;
    }
}

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

void Catalog::followAccount(YtmSession *account)
{
    if (m_account)
        m_account->disconnect(this);
    m_account = account;
    if (!account)
        return;
    connect(account, &YtmSession::sessionChanged, this, &Catalog::accountChanged);
    connect(account, &YtmSession::useForHomeChanged, this, &Catalog::accountChanged);
    // A rest begun or over, a check after one: whether Home may be the
    // account's changed with the session the same.
    connect(account, &YtmSession::accountUseChanged, this, &Catalog::accountChanged);
}

// Signed in, out, refused, or the setting turned: the feed is asked for again
// only when that changes whose it would be. An import is sessionChanged twice,
// once taken and once confirmed, and only the second makes it the account's.
void Catalog::accountChanged()
{
    // A page or listing of the account's own (a private playlist, Liked
    // music, a mix made for it), open when the session ends or changes:
    // gone from the screen with it.
    const bool active = m_account && m_account->stateValue() == YtmSession::State::Active;
    if (m_pageAuth != InnerTube::Auth::Anonymous && !active) {
        dropAccountPage(QStringLiteral("This playlist is your YouTube Music account's, and you are no longer signed "
                                       "in."));
    }
    if (m_listingAuth != InnerTube::Auth::Anonymous && !active)
        dropAccountListing(QStringLiteral("This shelf is your YouTube Music account's, and you are no longer signed in."));

    const bool asAccount = m_account && m_account->accountForHome();
    // What is on screen, or on its way, as the account: the feed asked as
    // it, or one that answered as it.
    const bool showing = m_feedAsAccount || m_personalFeed;
    if (!asAccount && !showing)
        return;   // signed-out Home, and it should be
    if (asAccount && (m_personalFeed || (m_feedAsAccount && m_pendingHome > 0)))
        return;   // the account's feed is shown, or on its way
    // The account's feed leaves Home at once, rather than staying until the
    // signed-out one has come (which may not come): after a sign-out nothing
    // of the account is left on screen. The other way round, the signed-out
    // feed stays until the account's replaces it.
    if (!asAccount) {
        m_quickPicks.clear();
        m_quickPicksTitle.clear();
        m_quickPicksStrapline.clear();
        m_homeShelves.clear();
        m_feedAccountIds.clear();
        m_feedLoggedIn.clear();
        resetMoreHome();
        m_personalFeed = false;
        m_feedAsAccount = false;
        composeShelves();
        Q_EMIT homeChanged();
    }
    // Just confirmed: the check has this moment been answered the account's
    // feed, which is shown as it is rather than asked for a second time.
    if (asAccount && m_pendingHome == 0) {
        const QJsonObject checked = m_account->takeCheckedHome();
        if (!checked.isEmpty()) {
            qInfo("catalog: Home's feed is the account's, from the check's own answer (nothing asked again)");
            m_feedAsAccount = true;
            m_error.clear();
            applyFeed(checked, true);
            composeShelves();
            Q_EMIT homeChanged();
            return;
        }
    }
    qInfo("catalog: Home's feed is asked for again, %s", asAccount ? "as the account" : "signed out");
    ask(Feed);
}

bool Catalog::showSaved()
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT part, saved_at, body FROM home_cache WHERE region = ?"));
    query.addBindValue(InnerTube::region());
    if (!query.exec())
        return false;
    qint64 oldest = 0;
    bool shown = false;
    while (query.next()) {
        const QJsonObject root = QJsonDocument::fromJson(qUncompress(query.value(2).toByteArray())).object();
        if (root.isEmpty())
            continue;
        const QString part = query.value(0).toString();
        if (part == QLatin1String("feed"))
            applyFeed(root, false);
        else if (part == QLatin1String("releases"))
            applyReleases(root);
        else
            continue;
        const qint64 at = query.value(1).toLongLong();
        oldest = oldest == 0 ? at : qMin(oldest, at);
        shown = true;
    }
    if (!shown)
        return false;
    composeShelves();
    m_savedAt = QLocale().toString(QDateTime::fromSecsSinceEpoch(oldest), QStringLiteral("d MMM, HH:mm"));
    qInfo("catalog: Home as it was saved on %s, until it answers afresh", qPrintable(m_savedAt));
    Q_EMIT homeChanged();
    return true;
}

void Catalog::saveAnswer(const QString &part, const QJsonObject &root)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO home_cache (part, region, saved_at, body) VALUES (?, ?, ?, ?)"));
    query.addBindValue(part);
    query.addBindValue(InnerTube::region());
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(qCompress(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    query.exec();
}

// Home is two requests, the home feed and new releases, run side by side;
// the view updates once both have answered.
void Catalog::refresh()
{
    ask(Feed | Releases);
}

void Catalog::ask(int parts)
{
    // A load is already out, asked from whatever country, and as whichever
    // account, was set when it started. Refusing this one would leave Home on
    // that country after a new one was picked mid-load, so it waits and runs
    // when the load ends.
    if (m_pendingHome > 0) {
        m_queuedParts |= parts;
        return;
    }
    m_retries = 0;   // asked for, so start counting again
    load(parts);
}

void Catalog::load(int parts)
{
    if (m_pendingHome > 0 || parts == 0)
        return;
    m_pendingHome = ((parts & Feed) ? 1 : 0) + ((parts & Releases) ? 1 : 0);
    m_error.clear();
    Q_EMIT homeChanged();

    if (parts & Feed) {
        // As the account while one is confirmed and Settings lets Home use
        // it. IfSignedIn is the anonymous call byte for byte otherwise, so
        // with the setting on and nobody signed in nothing changes.
        const bool useAccount = m_account && m_account->useForHome();
        const bool asAccount = m_account && m_account->accountForHome();
        m_feedAsAccount = asAccount;
        m_innerTube.browse(QStringLiteral("FEmusic_home"), [this, asAccount](const QJsonObject &root,
                                                                             const QString &error) {
            // Superseded while it was out: the queued load asks again, and
            // this answer is for what was asked before.
            if (m_queuedParts & Feed) {
                finishHome();
                return;
            }
            if (error.isEmpty()) {
                applyFeed(root, asAccount);
                // Kept for the next launch: the signed-out feed alone. The
                // account's is its own, and is asked for afresh each time.
                if (!asAccount && m_feedLoggedIn != QLatin1String("1") && !m_homeShelves.isEmpty())
                    saveAnswer(QStringLiteral("feed"), root);
            } else {
                m_error = error;
            }
            finishHome();
        }, useAccount ? InnerTube::Auth::IfSignedIn : InnerTube::Auth::Anonymous);
    }

    if (!(parts & Releases))
        return;
    // Never the account's: what came out this week is the same for everyone
    // in a country, and asking it signed out keeps it so.
    m_innerTube.browse(QStringLiteral("FEmusic_new_releases"), [this](const QJsonObject &root, const QString &error) {
        if (m_queuedParts & Releases) {
            finishHome();
            return;
        }
        if (error.isEmpty()) {
            applyReleases(root);
            if (!m_releaseShelves.isEmpty())
                saveAnswer(QStringLiteral("releases"), root);
        } else if (m_error.isEmpty()) {
            m_error = error;
        }
        finishHome();
    });
}

void Catalog::applyReleases(const QJsonObject &root)
{
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
}

// Whether a page linked from the account's feed is the account's own: a mix
// or radio made for it (RD…), or one of its personal shelves' pages.
static bool accountsOwn(const QString &browseId)
{
    if (browseId.startsWith(QLatin1String("VLRD")) || browseId.startsWith(QLatin1String("RD")))
        return true;
    if (!browseId.startsWith(QLatin1String("FEmusic_")))
        return false;
    for (const char *personal : { "for_you", "listen_again", "liked", "library", "history", "recap", "quick_picks" }) {
        if (browseId.contains(QLatin1String(personal)))
            return true;
    }
    return false;
}

QVariant ShelfModel::data(const QModelIndex &index, int role) const
{
    if (role != ShelfRole || !index.isValid() || index.row() >= m_shelves.size())
        return {};
    return m_shelves.at(index.row());
}

void ShelfModel::append(const QVariantList &shelves)
{
    if (shelves.isEmpty())
        return;
    beginInsertRows(QModelIndex(), int(m_shelves.size()), int(m_shelves.size() + shelves.size()) - 1);
    m_shelves += shelves;
    endInsertRows();
    Q_EMIT countChanged();
}

void ShelfModel::clear()
{
    if (m_shelves.isEmpty())
        return;
    beginResetModel();
    m_shelves.clear();
    endResetModel();
    Q_EMIT countChanged();
}

// Pages of the feed after its first, as the reader scrolls: a few, since the
// feed goes on a long way and each page is a request. The account's own feed
// fewer: its pages are the account's requests, which AccountGuard paces.
static constexpr int kMoreHomePages = 6;
static constexpr int kMoreHomePagesAsAccount = 3;

QVariantMap Catalog::feedShelfToMap(const InnerTube::Shelf &shelf, bool personal)
{
    if (!shelf.songs.isEmpty()) {
        InnerTube::Shelf asCards = shelf;
        asCards.songs.clear();
        for (const InnerTube::Track &track : shelf.songs) {
            InnerTube::Card card;
            card.type = track.isVideo ? QStringLiteral("video") : QStringLiteral("song");
            card.videoId = track.videoId;
            card.title = track.title;
            card.subtitle = track.artist;
            card.artist = track.artist;
            card.primaryArtist = track.primaryArtist;
            card.artwork = track.artwork;
            asCards.cards.append(card);
        }
        return shelfToMap(asCards);
    }
    if (personal) {
        // What the account's feed links to that is the account's alone
        // (a mix made for it, which answers nothing, or somebody else's,
        // signed out; a shelf of its own behind SHOW ALL) opens as the
        // account. A public album or playlist on the same feed does not:
        // it is the same for everyone, and opened from New releases or
        // Search it must stay the signed-out call it always was.
        for (const InnerTube::Card &card : shelf.cards) {
            if (accountsOwn(card.browseId))
                m_feedAccountIds.insert(card.browseId);
        }
        if (accountsOwn(shelf.more.browseId))
            m_feedAccountIds.insert(shelf.more.browseId);
    }
    return shelfToMap(shelf);
}

void Catalog::resetMoreHome()
{
    ++m_homeGeneration;
    m_homeNext.clear();
    m_homeLoadingMore = false;
    m_homePagesMore = 0;
    m_moreShelves.clear();
    Q_EMIT homeMoreChanged();
}

void Catalog::loadMoreHome()
{
    if (m_homeNext.isEmpty() || m_homeLoadingMore || m_pendingHome > 0)
        return;
    const bool personal = m_personalFeed;
    if (m_homePagesMore >= (personal ? kMoreHomePagesAsAccount : kMoreHomePages)) {
        m_homeNext.clear();
        Q_EMIT homeMoreChanged();
        return;
    }
    m_homeLoadingMore = true;
    Q_EMIT homeMoreChanged();
    const quint64 generation = m_homeGeneration;
    m_innerTube.continueBrowse(m_homeNext, [this, generation, personal](const QJsonObject &root, const QString &error) {
        if (generation != m_homeGeneration)
            return;   // Home was asked afresh meanwhile: this page was the old feed's
        m_homeLoadingMore = false;
        if (!error.isEmpty()) {
            // No more this time; Home asked afresh starts again.
            qWarning("catalog: Home's next page did not come: %s", qPrintable(error));
            m_homeNext.clear();
            Q_EMIT homeMoreChanged();
            return;
        }
        ++m_homePagesMore;
        const InnerTube::Continuation part = InnerTube::parseContinuation(root);
        QVariantList maps;
        for (const InnerTube::Shelf &shelf : part.shelves)
            maps.append(feedShelfToMap(shelf, personal));
        m_homeNext = part.next;
        qInfo("catalog: Home's page %d below the first: %d shelves%s", m_homePagesMore, int(maps.size()),
              m_homeNext.isEmpty() ? ", and no more" : "");
        m_moreShelves.append(maps);
        Q_EMIT homeMoreChanged();
    }, personal ? InnerTube::Auth::IfSignedIn : InnerTube::Auth::Anonymous, personal ? QString() : m_homeVisitor);
}

void Catalog::applyFeed(const QJsonObject &root, bool asAccount)
{
    // The one sure sign the feed is the account's: YouTube Music answers a
    // session it does not accept with the signed-out feed, 200 OK.
    // YtmSession's own checks decide on the session; this only says what
    // Home got.
    m_feedLoggedIn = InnerTube::parseLoggedIn(root);
    const bool personal = asAccount && m_feedLoggedIn == QLatin1String("1");
    m_homeShelves.clear();
    m_feedAccountIds.clear();
    m_feedQuickPicksAt = -1;
    resetMoreHome();
    m_homeNext = InnerTube::parseFeedContinuation(root);
    // Asked signed out, the next pages answer only as the visitor this one
    // was answered to; the account's are asked as the account.
    m_homeVisitor = personal ? QString() : InnerTube::parseVisitorData(root);
    Q_EMIT homeMoreChanged();
    bool songsTaken = false;
    for (const InnerTube::Shelf &shelf : InnerTube::parseShelves(root)) {
        if (!shelf.songs.isEmpty()) {
            // The first run of songs is Home's own list (Quick picks).
            if (!songsTaken) {
                m_quickPicks.replace(toItems(shelf.songs));
                m_quickPicksTitle = shelf.title;
                m_quickPicksStrapline = personal ? shelf.strapline : QString();
                m_feedQuickPicksAt = int(m_homeShelves.size());
                songsTaken = true;
                continue;
            }
            // Later ones are rare on the signed-out feed, and left out as
            // they always were. The account's feed has more of them (Listen
            // again, a mix of songs), and they are its own: kept, in place,
            // as cards that play.
            if (!personal)
                continue;
            m_homeShelves.append(feedShelfToMap(shelf, personal));
            continue;
        }
        m_homeShelves.append(feedShelfToMap(shelf, personal));
    }
    // A feed with no songs has no Quick picks: the last feed's, perhaps
    // another account's, do not stay under it.
    if (!songsTaken) {
        m_quickPicks.clear();
        m_quickPicksTitle.clear();
        m_quickPicksStrapline.clear();
    }
    m_personalFeed = personal;
    if (asAccount) {
        // Counts only: the straplines name the account.
        qInfo("catalog: Home's feed, asked as the account, answered logged_in=%s (%d songs, %d shelves)",
              m_feedLoggedIn.isEmpty() ? "(not said)" : qPrintable(m_feedLoggedIn), m_quickPicks.rowCount(),
              int(m_homeShelves.size()));
    }
}

void Catalog::composeShelves()
{
    if (m_personalFeed) {
        // The account's feed first, in its own order; this week's releases,
        // the same for everyone, after it.
        m_shelves = m_homeShelves + m_releaseShelves;
        m_quickPicksAt = m_quickPicks.rowCount() > 0 && m_feedQuickPicksAt < m_shelves.size() ? m_feedQuickPicksAt : -1;
    } else {
        m_shelves = m_releaseShelves + m_homeShelves;
        m_quickPicksAt = -1;
    }
}

InnerTube::Auth Catalog::authFor(const QString &browseId) const
{
    if (m_accountPages && m_accountPages(browseId))
        return InnerTube::Auth::IfSignedIn;
    if (m_personalFeed && m_feedAccountIds.contains(browseId))
        return InnerTube::Auth::IfSignedIn;
    return InnerTube::Auth::Anonymous;
}

void Catalog::finishHome()
{
    if (--m_pendingHome > 0)
        return;
    // Asked again while this load was out — another country picked in
    // Settings, the account confirmed. Load again rather than settle on, or
    // retry, the old one; Home stays "loading" throughout, since the next
    // load starts before anything is announced.
    if (const int queued = std::exchange(m_queuedParts, 0)) {
        ask(queued);
        return;
    }
    composeShelves();
    // Answered in full: what Home shows is fresh now, not the last launch's.
    const bool answered = m_error.isEmpty();
    if (answered)
        m_savedAt.clear();
    // One failed request out of two still leaves a page worth showing. The
    // last launch's page, shown while nothing fresh has come, is shown on,
    // and asked for again by itself as an empty one is.
    if ((!m_shelves.isEmpty() || m_quickPicks.rowCount() > 0) && (answered || m_savedAt.isEmpty())) {
        m_error.clear();
        m_retries = 0;
    } else if (m_retries < 2) {
        // Nothing at all: a request that dropped, most likely. Ask again by
        // itself rather than leaving Home empty until someone presses RETRY.
        ++m_retries;
        QTimer::singleShot(m_retries * 4000, this, [this]() { load(Feed | Releases); });
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
    const quint64 generation = ++m_pageGeneration;
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
    m_pageFetchingTold = false;
    m_pageWantsAll = false;
    m_pageFailedToken.clear();
    Q_EMIT pageChanged();
    Q_EMIT pageMoreChanged();

    m_pageAuth = authFor(browseId);
    m_innerTube.browse(browseId, [this, browseId, generation](const QJsonObject &root, const QString &error) {
        if (generation != m_pageGeneration)
            return;   // another page was opened meanwhile, or this one again
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
    }, m_pageAuth);
}

void Catalog::setAccountPages(std::function<bool(const QString &browseId)> isAccountPage)
{
    m_accountPages = std::move(isAccountPage);
}

void Catalog::accountPagesChanged()
{
    if (m_pageAuth != InnerTube::Auth::Anonymous && authFor(m_pageId) == InnerTube::Auth::Anonymous)
        dropAccountPage(QStringLiteral("This playlist is your YouTube Music account's, and your library is no longer "
                                       "read from it."));
    if (m_listingAuth != InnerTube::Auth::Anonymous
        && authFor(m_listingKey.section(QLatin1Char('|'), 0, 0)) == InnerTube::Auth::Anonymous)
        dropAccountListing(QStringLiteral("This shelf is your YouTube Music account's, and is no longer read from it."));
}

void Catalog::dropAccountPage(const QString &why)
{
    ++m_pageGeneration;
    m_pageAuth = InnerTube::Auth::Anonymous;
    m_pageTracks.clear();
    m_pageFeed.clear();
    m_pageNext.clear();
    m_pageKeys.clear();
    m_pageHeader = {};
    m_pageFailedToken.clear();
    m_pageLoading = false;
    m_pageRequestOut = false;
    m_pageWantsAll = false;
    m_pageFeedScheduled = false;
    m_page = { { QStringLiteral("browseId"), m_pageId }, { QStringLiteral("error"), why } };
    m_pageId.clear();
    updatePageLoadingMore();
    Q_EMIT pageChanged();
    Q_EMIT pageMoreChanged();
}

void Catalog::dropAccountListing(const QString &why)
{
    ++m_listingGeneration;
    m_listingAuth = InnerTube::Auth::Anonymous;
    m_listingLoading = false;
    m_listingLoadingMore = false;
    m_listingItemsNext.clear();
    m_listingSectionsNext.clear();
    m_listingFailedToken.clear();
    m_listingSongs.clear();
    m_listing = { { QStringLiteral("key"), m_listingKey }, { QStringLiteral("error"), why } };
    m_listingKey.clear();
    Q_EMIT listingChanged();
    Q_EMIT listingMoreChanged();
}

void Catalog::loadMorePage()
{
    if (m_pageNext.isEmpty() || m_pageLoading || m_pageLoadingMore)
        return;
    // Would not load a moment ago: scrolling does not ask again by itself.
    if (!m_pageFailedToken.isEmpty() && m_pageNext == m_pageFailedToken)
        return;
    fetchPagePart();
}

void Catalog::loadRestOfPage()
{
    // No token: whatever is still being added to the list is the rest, and
    // "loading more" ends when it is in.
    if (m_pageNext.isEmpty() || m_pageLoading)
        return;
    // Asked for by the user (Play, Shuffle, Download): a part that failed
    // is tried once more.
    m_pageFailedToken.clear();
    m_pageWantsAll = true;
    // A part already on its way carries on to the rest when it lands.
    if (!m_pageRequestOut)
        fetchPagePart();
}

void Catalog::updatePageLoadingMore()
{
    const bool loading = m_pageRequestOut || !m_pageFeed.isEmpty();
    if (loading == m_pageLoadingMore && m_pageRequestOut == m_pageFetchingTold)
        return;
    m_pageLoadingMore = loading;
    m_pageFetchingTold = m_pageRequestOut;
    Q_EMIT pageMoreChanged();
}

void Catalog::schedulePageFeed()
{
    if (m_pageFeedScheduled || m_pageFeed.isEmpty())
        return;
    m_pageFeedScheduled = true;
    const quint64 generation = m_pageGeneration;
    QTimer::singleShot(0, this, [this, generation]() {
        if (generation != m_pageGeneration)
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
    // Opened with the account, which may no longer be used for it (the
    // import turned off, the feed that showed it gone): not paged on with it.
    if (m_pageAuth != InnerTube::Auth::Anonymous && authFor(m_pageId) == InnerTube::Auth::Anonymous) {
        dropAccountPage(QStringLiteral("This playlist is your YouTube Music account's, and is no longer read from it."));
        return;
    }
    const quint64 generation = m_pageGeneration;
    const QString token = m_pageNext;
    m_pageRequestOut = true;
    updatePageLoadingMore();

    m_innerTube.continueBrowse(token, [this, generation, token](const QJsonObject &root, const QString &error) {
        // Another page opened meanwhile, or this one opened afresh: that
        // opening started with nothing on its way.
        if (generation != m_pageGeneration)
            return;
        m_pageRequestOut = false;
        // Not the part the page is waiting for. Should it ever happen, the
        // page is at least not left "loading" for good.
        if (token != m_pageNext) {
            updatePageLoadingMore();
            return;
        }
        if (!error.isEmpty()) {
            // The token is kept, so pressing Play again asks once more;
            // scrolling on does not (m_pageFailedToken), or a part that fails
            // would be asked for over and over while the page stays open.
            m_pageFailedToken = token;
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
    }, m_pageAuth);
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
    const quint64 generation = ++m_listingGeneration;
    m_listingLoading = true;
    m_listingLoadingMore = false;
    m_listingItemsNext.clear();
    m_listingSectionsNext.clear();
    m_listing = { { QStringLiteral("key"), key }, { QStringLiteral("title"), title } };
    m_listingSongs.clear();
    Q_EMIT listingChanged();
    Q_EMIT listingMoreChanged();

    m_listingOwner.clear();
    m_listingFailedToken.clear();
    m_listingAuth = authFor(browseId);
    m_innerTube.browse(browseId, params, [this, browseId, key, title, generation](const QJsonObject &root,
                                                                                 const QString &error) {
        if (generation != m_listingGeneration)
            return;   // another was opened meanwhile, or this one again
        m_listingLoading = false;
        if (!error.isEmpty()) {
            m_listing.insert(QStringLiteral("error"), error);
            Q_EMIT listingChanged();
            return;
        }
        const InnerTube::Listing listing = InnerTube::parseListing(root);
        // An artist's discography is "MPAD" and their channel: the artist
        // whose page it was opened from, by name, or else the page's own
        // heading, which is theirs.
        if (browseId.startsWith(QLatin1String("MPAD"))) {
            m_listingOwner = m_artist.value(QStringLiteral("browseId")).toString() == browseId.mid(4)
                             ? m_artist.value(QStringLiteral("name")).toString() : listing.title;
        }
        QVariantList sections;
        QList<InnerTube::Track> songs;
        for (const InnerTube::Shelf &shelf : listing.sections) {
            songs += shelf.songs;
            if (!shelf.cards.isEmpty())
                sections.append(shelfToMap(shelf));
        }
        markSectionsOwner(sections, m_listingOwner);
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
    }, m_listingAuth);
}

void Catalog::loadMoreListing()
{
    if (m_listingLoading || m_listingLoadingMore || !listingHasMore())
        return;
    const QString next = !m_listingItemsNext.isEmpty() ? m_listingItemsNext : m_listingSectionsNext;
    if (!m_listingFailedToken.isEmpty() && next == m_listingFailedToken)
        return;
    fetchListingPart();
}

void Catalog::fetchListingPart()
{
    // The last section's own items first; the sections after it once those
    // are all in, so the page grows downwards in the order it reads.
    const bool items = !m_listingItemsNext.isEmpty();
    const QString token = items ? m_listingItemsNext : m_listingSectionsNext;
    const quint64 generation = m_listingGeneration;
    m_listingLoadingMore = true;
    Q_EMIT listingMoreChanged();

    m_innerTube.continueBrowse(token, [this, generation, items, token](const QJsonObject &root,
                                                                     const QString &error) {
        if (generation != m_listingGeneration)
            return;   // another listing opened meanwhile, which started afresh
        m_listingLoadingMore = false;
        if (token != (items ? m_listingItemsNext : m_listingSectionsNext)) {
            Q_EMIT listingMoreChanged();
            return;
        }
        if (!error.isEmpty()) {
            m_listingFailedToken = token;
            Q_EMIT listingMoreChanged();
            Q_EMIT notice(QStringLiteral("The rest of this shelf would not load: %1").arg(error));
            return;
        }
        const InnerTube::Continuation part = InnerTube::parseContinuation(root);
        QVariantList sections = m_listing.value(QStringLiteral("sections")).toList();
        // More cards for the last grid, where there is one to add them to:
        // handed over as they are (listingAppended), so the cards already
        // shown stay, pictures and all. New sections, or a first grid, and
        // the listing is shown again whole.
        const bool extendsLast = !sections.isEmpty() && part.shelves.isEmpty();
        QVariantList added;
        if (!part.cards.isEmpty()) {
            QVariantMap last = sections.isEmpty() ? QVariantMap() : sections.takeLast().toMap();
            QVariantList cards = last.value(QStringLiteral("items")).toList();
            for (const InnerTube::Card &card : part.cards)
                added.append(InnerTube::cardToVariant(card));
            markOwner(added, m_listingOwner);
            cards += added;
            last.insert(QStringLiteral("items"), cards);
            sections.append(last);
        }
        for (const InnerTube::Shelf &shelf : part.shelves) {
            if (!shelf.cards.isEmpty()) {
                QVariantMap map = shelfToMap(shelf);
                QVariantList items = map.value(QStringLiteral("items")).toList();
                markOwner(items, m_listingOwner);
                map.insert(QStringLiteral("items"), items);
                sections.append(map);
            }
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
        if (extendsLast) {
            if (!added.isEmpty())
                Q_EMIT listingAppended(int(sections.size()) - 1, added);
        } else if (!empty) {
            Q_EMIT listingChanged();
        }
        Q_EMIT listingMoreChanged();
    }, m_listingAuth);
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
    }, authFor(browseId));
}

QVariantList Catalog::pageTrackList() const
{
    QVariantList list;
    list.reserve(m_pageTracks.rowCount() + m_pageFeed.size());
    for (int row = 0; row < m_pageTracks.rowCount(); ++row)
        list.append(m_pageTracks.get(row));
    // Arrived, and still waiting for their rows (feedPage): the table is a
    // few frames behind the songs, and a long playlist's many frames.
    for (const SearchResultModel::Item &item : m_pageFeed)
        list.append(SearchResultModel::toMap(item));
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
        // The albums and singles on their page are theirs.
        markSectionsOwner(shelves, artist.name);
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
