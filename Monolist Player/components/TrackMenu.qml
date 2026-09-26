import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// What can be done with one song, from a row's "more" button or a right
// click. One instance per list: set `track` (and in a playlist `playlistId`
// and `entryId`), then popup().
MonoMenu {
    id: menu

    // sourceId, title, artist, album, artwork, durationMs; credits and
    // albumId where the list kept them
    property var track: ({})
    // The playlist the list is, for "Remove from this playlist"; 0 elsewhere.
    property int playlistId: 0
    property int entryId: 0

    readonly property string sourceId: track && track.sourceId ? track.sourceId : ""
    readonly property string downloadState: Downloads.revision >= 0 ? Downloads.stateFor(sourceId) : ""
    readonly property bool liked: Library.revision >= 0 && Library.isLiked(sourceId)
    // The names in the artist line, each with its page where known, and the
    // album's page: what "Go to" can open.
    readonly property var artists: track && track.artist
                                   ? Artists.credits(track.artist, track.credits).filter(function(piece) { return piece.link })
                                   : []
    readonly property string albumId: track && track.albumId ? track.albumId : ""

    // One entry for one artist; for a joint credit, one per name, so each
    // can be reached, up to three.
    function goToText(index) {
        if (artists.length === 1)
            return "Go to artist"
        return index < artists.length ? "Go to " + artists[index].text : ""
    }
    function goTo(index) {
        if (index < artists.length)
            Nav.openArtist(artists[index].text, artists[index].id)
    }

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

    // A hidden entry is disabled as well, here and below. The arrow keys pass
    // over disabled entries but not hidden ones, so an entry that was only
    // hidden took the highlight out of sight, and Enter ran it.
    MonoMenuItem {
        visible: menu.playlistId > 0 && menu.entryId > 0
        enabled: menu.playlistId > 0 && menu.entryId > 0
        text: "Remove from this playlist"
        onTriggered: Library.removeFromPlaylist(menu.playlistId, menu.entryId)
    }

    // — where the song comes from —
    MonoMenuRule { visible: menu.artists.length > 0 || menu.albumId.length > 0 }

    MonoMenuItem {
        visible: menu.artists.length > 0
        enabled: menu.artists.length > 0
        text: menu.goToText(0)
        onTriggered: menu.goTo(0)
    }
    MonoMenuItem {
        visible: menu.artists.length > 1
        enabled: menu.artists.length > 1
        text: menu.goToText(1)
        onTriggered: menu.goTo(1)
    }
    MonoMenuItem {
        visible: menu.artists.length > 2
        enabled: menu.artists.length > 2
        text: menu.goToText(2)
        onTriggered: menu.goTo(2)
    }
    MonoMenuItem {
        visible: menu.albumId.length > 0
        enabled: menu.albumId.length > 0
        text: "Go to album"
        onTriggered: Nav.openPage(menu.albumId)
    }

    MonoMenuRule {}

    // — the offline copy —
    // What can be done depends on where the download has got to: fetch it,
    // call it off, try it again, or, once the file is here, find it or
    // delete it.
    MonoMenuItem {
        visible: menu.downloadState.length === 0
        text: "Download"
        enabled: Downloads.available && menu.sourceId.length > 0 && menu.downloadState.length === 0
        onTriggered: Downloads.enqueue(menu.sourceId, menu.track.title, menu.track.artist,
                                       menu.track.artwork, menu.track.durationMs,
                                       menu.track.isVideo === true)
    }
    // Waiting its turn, downloading, or being finished by FFmpeg: any of the
    // three can be stopped, as in Downloads, and nothing is left behind.
    MonoMenuItem {
        readonly property bool underWay: menu.downloadState === "queued"
                                         || menu.downloadState === "downloading"
                                         || menu.downloadState === "processing"
        visible: underWay
        enabled: underWay
        text: "Cancel download"
        onTriggered: Downloads.cancel(menu.sourceId)
    }
    MonoMenuItem {
        visible: menu.downloadState === "failed"
        text: "Retry download"
        enabled: menu.downloadState === "failed" && Downloads.available
        onTriggered: Downloads.retry(menu.sourceId)
    }
    MonoMenuItem {
        visible: menu.downloadState === "done"
        enabled: menu.downloadState === "done"
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
        enabled: menu.downloadState === "done"
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
