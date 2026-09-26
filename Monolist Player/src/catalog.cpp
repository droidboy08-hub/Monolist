#include "catalog.h"
#include "appdatabase.h"
#include "artistlinks.h"

#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

#include <utility>

Catalog::Catalog(QObject *parent)
    : QObject(parent)
{
}

QVariantMap Catalog::cardToMap(const InnerTube::Card &card)
{
    return {
        { QStringLiteral("type"), card.type },
        { QStringLiteral("browseId"), card.browseId },
        { QStringLiteral("videoId"), card.videoId },
        { QStringLiteral("title"), card.title },
        { QStringLiteral("subtitle"), card.subtitle },
        { QStringLiteral("artwork"), card.artwork },
        { QStringLiteral("artist"), card.artist },
        { QStringLiteral("primaryArtist"), card.primaryArtist }
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
                QVariantList items;
                for (const InnerTube::Card &card : shelf.cards)
                    items.append(cardToMap(card));
                m_homeShelves.append(QVariantMap{
                    { QStringLiteral("title"), shelf.title },
                    { QStringLiteral("strapline"), shelf.strapline },
                    { QStringLiteral("items"), items } });
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
                QVariantList items;
                for (const InnerTube::Card &card : shelf.cards)
                    items.append(cardToMap(card));
                if (m_featured.isEmpty())
                    m_featured = items.first().toMap();
                // The country is part of what this shelf is: another one
                // lists different records. Settings picks it.
                m_releaseShelves.append(QVariantMap{
                    { QStringLiteral("title"), QStringLiteral("New releases") },
                    { QStringLiteral("strapline"), QStringLiteral("ALBUMS & SINGLES · %1").arg(InnerTube::region()) },
                    { QStringLiteral("items"), items } });
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
    Q_EMIT pageChanged();

    m_innerTube.browse(browseId, [this, browseId](const QJsonObject &root, const QString &error) {
        if (browseId != m_pageId)
            return;   // another page was opened meanwhile
        m_pageLoading = false;
        if (!error.isEmpty()) {
            m_page.insert(QStringLiteral("error"), error);
            Q_EMIT pageChanged();
            return;
        }
        const InnerTube::Collection collection = InnerTube::parseCollection(browseId, root);
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
        m_pageTracks.replace(toItems(collection.tracks));
        Q_EMIT pageChanged();
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
        for (const InnerTube::Shelf &shelf : artist.shelves) {
            QVariantList items;
            for (const InnerTube::Card &card : shelf.cards)
                items.append(cardToMap(card));
            shelves.append(QVariantMap{
                { QStringLiteral("title"), shelf.title },
                { QStringLiteral("strapline"), shelf.strapline },
                { QStringLiteral("items"), items } });
        }
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
        // As rows, in the shape every list hands the player.
        SearchResultModel rows;
        rows.replace(toItems(tracks));
        QVariantList list;
        list.reserve(rows.rowCount());
        for (int row = 0; row < rows.rowCount(); ++row)
            list.append(rows.get(row));
        Q_EMIT artistMixReady(kind, list);
    });
}
