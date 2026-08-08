#pragma once

#include <QObject>
#include <QVariantMap>

#include "albummodel.h"
#include "playlistmodel.h"
#include "trackmodel.h"

// Read side of the local store. Exposed to QML as the "Library" singleton.
class Library : public QObject
{
    Q_OBJECT
    Q_PROPERTY(PlaylistModel *playlists READ playlists CONSTANT)
    Q_PROPERTY(AlbumModel *albums READ albums CONSTANT)
    Q_PROPERTY(TrackModel *tracks READ tracks CONSTANT)
    Q_PROPERTY(QString userName READ userName CONSTANT)
    Q_PROPERTY(QString userInitials READ userInitials CONSTANT)
    Q_PROPERTY(QString userPlan READ userPlan CONSTANT)
    Q_PROPERTY(QVariantMap featured READ featured NOTIFY featuredChanged)
public:
    explicit Library(QObject *parent = nullptr);

    void load();

    PlaylistModel *playlists() { return &m_playlists; }
    AlbumModel *albums() { return &m_albums; }
    TrackModel *tracks() { return &m_tracks; }

    QString userName() const;
    QString userInitials() const;
    QString userPlan() const;
    QVariantMap featured() const { return m_featured; }

    Q_INVOKABLE QString settingValue(const QString &key, const QString &fallback = QString()) const;
    Q_INVOKABLE void setSetting(const QString &key, const QString &value);

Q_SIGNALS:
    void featuredChanged();

private:
    void reloadFeatured();

    QVariantMap m_featured;
    PlaylistModel m_playlists;
    AlbumModel m_albums;
    TrackModel m_tracks;
};
