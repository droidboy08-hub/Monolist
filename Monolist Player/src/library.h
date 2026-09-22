#pragma once

#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include "albummodel.h"
#include "mediaextractor.h"
#include "playlistmodel.h"
#include "trackmodel.h"

// The user's own things, kept in the local database: their playlists, the
// songs they like, the albums and playlists they saved from YouTube Music, and
// what they played. Exposed to QML as the "Library" singleton.
//
// Songs are stored by video id rather than as references to the tracks table:
// a playlist can hold a song that was never liked or downloaded, the way any
// streaming app's can.
class Library : public QObject
{
    Q_OBJECT
    Q_PROPERTY(PlaylistModel *playlists READ playlists CONSTANT)
    // Saved albums, singles and EPs, and saved YouTube Music playlists.
    Q_PROPERTY(AlbumModel *albums READ albums CONSTANT)
    Q_PROPERTY(AlbumModel *savedPlaylists READ savedPlaylists CONSTANT)
    Q_PROPERTY(TrackModel *tracks READ tracks CONSTANT)
    // Liked songs, the latest first.
    Q_PROPERTY(SearchResultModel *liked READ liked CONSTANT)
    // Every song played, the latest first.
    Q_PROPERTY(SearchResultModel *history READ history CONSTANT)
    // The playlist open now: { playlistId, name, trackCount, durationText, artworks }.
    Q_PROPERTY(QVariantMap playlist READ playlist NOTIFY playlistChanged)
    Q_PROPERTY(SearchResultModel *playlistTracks READ playlistTracks CONSTANT)
    // Moves whenever a like or a save changes, so bindings that call isLiked()
    // or isSaved() know to ask again.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY userChanged)
    Q_PROPERTY(QString userInitials READ userInitials NOTIFY userChanged)
    // The country YouTube Music is asked from; empty follows the system.
    Q_PROPERTY(QString region READ region WRITE setRegion NOTIFY regionChanged)
    Q_PROPERTY(QString regionInUse READ regionInUse NOTIFY regionChanged)
    Q_PROPERTY(QString regionInUseName READ regionInUseName NOTIFY regionChanged)
    Q_PROPERTY(QString systemRegionName READ systemRegionName CONSTANT)
public:
    explicit Library(QObject *parent = nullptr);

    void load();

    PlaylistModel *playlists() { return &m_playlists; }
    AlbumModel *albums() { return &m_albums; }
    AlbumModel *savedPlaylists() { return &m_savedPlaylists; }
    TrackModel *tracks() { return &m_tracks; }
    SearchResultModel *liked() { return &m_liked; }
    SearchResultModel *history() { return &m_history; }
    QVariantMap playlist() const { return m_playlist; }
    SearchResultModel *playlistTracks() { return &m_playlistTracks; }
    int revision() const { return m_revision; }

    QString userName() const;
    QString userInitials() const;

    Q_INVOKABLE QString settingValue(const QString &key, const QString &fallback = QString()) const;
    Q_INVOKABLE void setSetting(const QString &key, const QString &value);

    // — playlists —
    // Returns the new playlist's id. An empty name gives "New playlist", or
    // "New playlist 2" and so on when that is taken.
    Q_INVOKABLE int createPlaylist(const QString &name = QString());
    Q_INVOKABLE void renamePlaylist(int playlistId, const QString &name);
    Q_INVOKABLE void deletePlaylist(int playlistId);
    // A track is a map with the app's track roles (sourceId, title, artist,
    // album, artwork, durationMs). False when the song is there already.
    Q_INVOKABLE bool addToPlaylist(int playlistId, const QVariantMap &track);
    Q_INVOKABLE int addAllToPlaylist(int playlistId, const QVariantList &tracks);
    Q_INVOKABLE void removeFromPlaylist(int playlistId, int entryId);
    Q_INVOKABLE void openPlaylist(int playlistId);
    Q_INVOKABLE QString playlistName(int playlistId) const;
    Q_INVOKABLE QVariantList playlistTrackList() const;
    Q_INVOKABLE QVariantList likedTrackList() const;

    // — likes —
    Q_INVOKABLE bool isLiked(const QString &videoId) const;
    Q_INVOKABLE void setLiked(const QVariantMap &track, bool liked);

    // — albums and playlists saved from YouTube Music —
    Q_INVOKABLE bool isSaved(const QString &browseId) const;
    // `page` is Catalog.page: browseId, type, title, subtitle, artist, artwork.
    Q_INVOKABLE void setSaved(const QVariantMap &page, bool saved);

    // — history —
    Q_INVOKABLE void reloadHistory();
    Q_INVOKABLE void clearHistory();

    // An empty name goes back to the one the system knows the user by.
    Q_INVOKABLE void setUserName(const QString &name);

    QString region() const;
    void setRegion(const QString &code);
    QString regionInUse() const;
    QString regionInUseName() const;
    QString systemRegionName() const;
    // The countries YouTube Music serves, as [{ code, name }], by name.
    Q_INVOKABLE QVariantList countries() const;
    Q_INVOKABLE QString countryName(const QString &code) const;
    // YouTube Music refused this country: forget it and say so.
    void dropRegion(const QString &code);

Q_SIGNALS:
    // A short confirmation of what just happened, for the toast.
    void notice(const QString &text);
    void likesChanged();
    void revisionChanged();
    void playlistChanged();
    void historyCleared();
    void userChanged();
    void regionChanged();

private:
    void reloadLiked();
    void reloadSaved();
    void reloadOpenPlaylist();
    void touch();
    static QString systemUserName();
    static QString trackVideoId(const QVariantMap &track);

    PlaylistModel m_playlists;
    AlbumModel m_albums{ AlbumModel::Albums };
    AlbumModel m_savedPlaylists{ AlbumModel::Playlists };
    TrackModel m_tracks;
    SearchResultModel m_liked;
    SearchResultModel m_history;
    SearchResultModel m_playlistTracks;
    QVariantMap m_playlist;
    int m_openPlaylistId = 0;
    QSet<QString> m_likedIds;
    QSet<QString> m_savedIds;
    int m_revision = 0;
};
