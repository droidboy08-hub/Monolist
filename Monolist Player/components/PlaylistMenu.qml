import QtQuick
import Monolist
import Monolist.Backend

// What can be done with one of the user's playlists, or with Liked songs, as
// a whole: play it, queue it, copy it into another, and for a playlist,
// rename or delete it. The same menu on its page, its card and its line in
// the sidebar; renaming and deleting are asked of whoever opened it, since
// both happen on the playlist's page (the name is edited in place there, and
// deleting is confirmed there, with its songs in view).
MonoMenu {
    id: menu

    // "liked", or a playlist's id.
    property string key: ""
    signal renameRequested(int playlistId)
    signal deleteRequested(int playlistId)

    readonly property bool liked: key === "liked"
    readonly property int playlistId: liked ? 0 : (parseInt(key) || 0)
    // Counted as the menu opens: a playlist that is not open has no model.
    property int songCount: 0

    function songs() {
        return liked ? Library.likedTrackList() : Library.playlistTracksFor(playlistId)
    }

    function show(newKey, anchor) {
        key = newKey
        songCount = songs().length
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

    MonoMenuItem {
        visible: !menu.liked
        enabled: !menu.liked
        text: "Rename"
        onTriggered: menu.renameRequested(menu.playlistId)
    }
    MonoMenuItem {
        visible: !menu.liked
        enabled: !menu.liked
        text: "Delete playlist…"
        onTriggered: menu.deleteRequested(menu.playlistId)
    }
}
