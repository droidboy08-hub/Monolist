import QtQuick
import Monolist
import Monolist.Backend

// What can be done with one of the user's playlists, or with Liked songs, as
// a whole: play it, queue it, copy it into another, and for a playlist,
// rename or delete it. For Liked on YouTube Music ("ytliked"), the account's
// liked songs as they were read, the same less renaming and deleting (it is
// read-only), and Sync now instead. The same menu on its page, its card and its line in
// the sidebar; renaming and deleting are asked of whoever opened it, since
// both happen on the playlist's page (the name is edited in place there, and
// deleting is confirmed there, with its songs in view).
MonoMenu {
    id: menu

    // "liked", "ytliked", or a playlist's id.
    property string key: ""
    signal renameRequested(int playlistId)
    signal deleteRequested(int playlistId)

    readonly property bool liked: key === "liked"
    readonly property bool account: key === "ytliked"
    readonly property bool own: !liked && !account
    readonly property int playlistId: own ? (parseInt(key) || 0) : 0
    // Counted as the menu opens: a playlist that is not open has no model.
    property int songCount: 0
    // Where it is among the playlists, and whether it is pinned, as it opens.
    property int row: -1
    property bool pinned: false

    function songs() {
        return liked ? Library.likedTrackList() : account ? AccountLibrary.likedTrackList()
                                                          : Library.playlistTracksFor(playlistId)
    }

    function show(newKey, anchor) {
        key = newKey
        songCount = account ? AccountLibrary.likedCount : songs().length
        row = own ? Library.playlistRow(playlistId) : -1
        pinned = own && Library.isPlaylistPinned(playlistId)
        if (anchor)
            popup(anchor, 0, anchor.height + Theme.space1)
        else
            popup()
    }

    MonoMenuItem {
        text: "Play"
        enabled: menu.songCount > 0
        onTriggered: Player.playTracks(menu.songs(), 0, "playlist")
    }
    MonoMenuItem {
        text: "Shuffle"
        enabled: menu.songCount > 1
        onTriggered: {
            Player.shuffle = true
            Player.playTracks(menu.songs(), Math.floor(Math.random() * menu.songCount), "playlist")
        }
    }
    MonoMenuItem {
        text: "Add all to queue"
        enabled: menu.songCount > 0
        onTriggered: {
            const tracks = menu.songs()
            for (let i = 0; i < tracks.length; ++i)
                Player.addToQueue(tracks[i])
        }
    }
    PlaylistSubmenu {
        title: "Add all to playlist"
        enabled: menu.songCount > 0
        onPicked: function(playlistId) { Library.addAllToPlaylist(playlistId, menu.songs()) }
    }

    MonoMenuRule { visible: !menu.liked }

    // Where it sits among the playlists: pinned above the rest, or moved
    // within them (the sidebar's rows can be dragged too).
    MonoMenuItem {
        visible: menu.own
        enabled: menu.own
        text: menu.pinned ? "Unpin" : "Pin to the top"
        onTriggered: Library.setPlaylistPinned(menu.playlistId, !menu.pinned)
    }
    MonoMenuItem {
        visible: menu.own
        enabled: menu.own && menu.row > 0
        text: "Move up"
        onTriggered: Library.movePlaylist(menu.playlistId, menu.row - 1)
    }
    MonoMenuItem {
        visible: menu.own
        enabled: menu.own && menu.row >= 0 && menu.row < Library.playlists.count - 1
        text: "Move down"
        onTriggered: Library.movePlaylist(menu.playlistId, menu.row + 1)
    }

    MonoMenuItem {
        visible: menu.own
        enabled: menu.own
        text: "Rename"
        onTriggered: menu.renameRequested(menu.playlistId)
    }
    MonoMenuItem {
        visible: menu.own
        enabled: menu.own
        text: "Delete playlist…"
        onTriggered: menu.deleteRequested(menu.playlistId)
    }
    // Read again from YouTube Music, as Settings' SYNC NOW; at most once in
    // a quarter of an hour, to go easy on the account.
    MonoMenuItem {
        visible: menu.account
        enabled: menu.account && AccountLibrary.canSyncNow
        text: "Sync now"
        onTriggered: AccountLibrary.syncNow()
    }
}
