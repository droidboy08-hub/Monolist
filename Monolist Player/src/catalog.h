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

public Q_SLOTS:
    void refresh();
    void reloadRecent();
    void openPage(const QString &browseId);
    // The open page's songs as maps, for "Download all".
    QVariantList pageTrackList() const;

Q_SIGNALS:
    void homeChanged();
    void pageChanged();

private:
    void finishHome();
    static QVariantMap cardToMap(const InnerTube::Card &card);
    static QList<SearchResultModel::Item> toItems(const QList<InnerTube::Track> &tracks);

    InnerTube m_innerTube;
    SearchResultModel m_quickPicks;
    SearchResultModel m_recent;
    SearchResultModel m_pageTracks;

    int m_pendingHome = 0;
    QString m_error;
    QString m_quickPicksTitle;
    QVariantList m_homeShelves;       // from the home feed
    QVariantList m_releaseShelves;    // from new releases
    QVariantList m_shelves;           // both, in the order Home shows them
    QVariantMap m_featured;

    QString m_pageId;                 // the page whose answer is still wanted
    QVariantMap m_page;
    bool m_pageLoading = false;
};
