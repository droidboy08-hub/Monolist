#include "catalog.h"
#include "appdatabase.h"

#include <QSqlQuery>
#include <QTimer>
#include <QVariant>

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
        { QStringLiteral("artwork"), card.artwork }
    };
}

QList<SearchResultModel::Item> Catalog::toItems(const QList<InnerTube::Track> &tracks)
{
    QList<SearchResultModel::Item> items;
    items.reserve(tracks.size());
    for (const InnerTube::Track &track : tracks)
        items.append({ track.videoId, track.title, track.artist, track.album, track.artwork,
                       track.durationMs, 0, track.isVideo });
    return items;
}

// Home is two requests, the home feed and new releases, run side by side;
// the view updates once both have answered.
void Catalog::refresh()
{
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
        "SELECT video_id, title, artist, album, artwork, duration_ms FROM recent"
        " ORDER BY played_at DESC, rowid DESC LIMIT 10"));
    while (query.next()) {
        items.append({ query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
                       query.value(3).toString(), query.value(4).toString(), query.value(5).toLongLong() });
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
