import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// What can be done with one song, from a row's "more" button or a right
// click. One instance per list: set `track` (and in a playlist `playlistId`
// and `entryId`), then popup().
MonoMenu {
    id: menu

    // sourceId, title, artist, album, artwork, durationMs
    property var track: ({})
    // The playlist the list is, for "Remove from this playlist"; 0 elsewhere.
    property int playlistId: 0
    property int entryId: 0

    readonly property string sourceId: track && track.sourceId ? track.sourceId : ""
    readonly property string downloadState: Downloads.revision >= 0 ? Downloads.stateFor(sourceId) : ""
    readonly property bool liked: Library.revision >= 0 && Library.isLiked(sourceId)

    Action {
        text: "Play next"
        onTriggered: Player.playNext(menu.track)
    }
    Action {
        text: "Add to queue"
        onTriggered: Player.addToQueue(menu.track)
    }

    MonoMenuRule {}

    Action {
        text: menu.liked ? "Remove from Liked songs" : "Add to Liked songs"
        enabled: menu.sourceId.length > 0
        onTriggered: Library.setLiked(menu.track, !menu.liked)
    }

    PlaylistSubmenu {
        onPicked: function(playlistId) { Library.addToPlaylist(playlistId, menu.track) }
    }

    MonoMenuItem {
        visible: menu.playlistId > 0 && menu.entryId > 0
        text: "Remove from this playlist"
        onTriggered: Library.removeFromPlaylist(menu.playlistId, menu.entryId)
    }

    MonoMenuRule {}

    Action {
        text: menu.downloadState === "done" ? "Downloaded"
            : menu.downloadState.length > 0 ? "Downloading…"
            : "Download"
        enabled: Downloads.available && menu.sourceId.length > 0 && menu.downloadState.length === 0
        onTriggered: Downloads.enqueue(menu.sourceId, menu.track.title, menu.track.artist,
                                       menu.track.artwork, menu.track.durationMs)
    }
}
