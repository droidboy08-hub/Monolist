#pragma once

#include <QObject>
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
    // [{ title, strapline, items: [{ type, browseId, videoId, title, subtitle, artwork }] }]
    Q_PROPERTY(QVariantList shelves READ shelves NOTIFY homeChanged)
    // The newest release, for the poster: { browseId, title, subtitle, artwork }.
    Q_PROPERTY(QVariantMap featured READ featured NOTIFY homeChanged)
    Q_PROPERTY(SearchResultModel *recent READ recent CONSTANT)
    // The album or playlist page open now:
    // { browseId, type, title, subtitle, artist, details, description, artwork, error }
    Q_PROPERTY(QVariantMap page READ page NOTIFY pageChanged)
    Q_PROPERTY(SearchResultModel *pageTracks READ pageTracks CONSTANT)
    Q_PROPERTY(bool pageLoading READ pageLoading NOTIFY pageChanged)
    // The artist page open now:
    // { browseId, channel, name, description, audience, artwork, songsTitle,
    //   songsId, canShuffle, canRadio, shelves: [{ title, items: [card] }], error }
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
    static QVariantMap cardToMap(const InnerTube::Card &card);
    static QList<SearchResultModel::Item> toItems(const QList<InnerTube::Track> &tracks);

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
