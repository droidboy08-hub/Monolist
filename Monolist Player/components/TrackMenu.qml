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

    // — the offline copy —
    // What can be done depends on where the download has got to: fetch it,
    // try it again, or, once the file is here, find it or delete it.
    MonoMenuItem {
        visible: menu.downloadState !== "failed" && menu.downloadState !== "done"
        text: menu.downloadState.length > 0 ? "Downloading…" : "Download"
        enabled: Downloads.available && menu.sourceId.length > 0 && menu.downloadState.length === 0
        onTriggered: Downloads.enqueue(menu.sourceId, menu.track.title, menu.track.artist,
                                       menu.track.artwork, menu.track.durationMs)
    }
    MonoMenuItem {
        visible: menu.downloadState === "failed"
        text: "Retry download"
        enabled: Downloads.available
        onTriggered: Downloads.retry(menu.sourceId)
    }
    MonoMenuItem {
        visible: menu.downloadState === "done"
        text: "Show in folder"
        onTriggered: Downloads.revealFile(menu.sourceId)
    }
    MonoMenuItem {
        id: removeDownload

        // Deleting takes a second click, as it does in Downloads, so a stray
        // one cannot lose a file. The first only arms it, and must leave the
        // menu open for the second; a click that reached the item would close
        // the menu, so both are taken here instead.
        property bool armed: false

        function activate() {
            if (armed) {
                Downloads.remove(menu.sourceId)
                menu.dismiss()
            } else {
                armed = true
                disarm.restart()
            }
        }

        visible: menu.downloadState === "done"
        text: armed ? "Click again to delete the file" : "Remove download"
        textColor: armed ? Theme.accent700 : Theme.text

        MouseArea {
            anchors.fill: parent
            z: 1
            onClicked: removeDownload.activate()
        }
        // The keys that would click it. Held down, a key repeats, and the
        // repeat must not be taken for the second press.
        Keys.onPressed: function(event) {
            if (event.key !== Qt.Key_Space && event.key !== Qt.Key_Return
                    && event.key !== Qt.Key_Enter && event.key !== Qt.Key_Select)
                return
            event.accepted = true
            if (!event.isAutoRepeat)
                removeDownload.activate()
        }

        Timer {
            id: disarm
            interval: 3000
            onTriggered: removeDownload.armed = false
        }
    }

    onClosed: removeDownload.armed = false
}
