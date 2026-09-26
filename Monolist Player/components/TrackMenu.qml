import QtQuick
import Monolist
import Monolist.Backend

// What can be done with one song, from a row's "more" button or a right
// click, wherever the song is shown. Main keeps the one instance and opens it
// through Menus.openTrack(track, context); show() fills it in.
MonoMenu {
    id: menu

    // sourceId, title, artist, album, artwork, durationMs; credits and
    // albumId where the list kept them
    property var track: ({})
    // Where the song was shown, for the entries that only apply there. Each
    // may be left out:
    //   playlistId, entryId  one of the user's playlists: Remove from this
    //                        playlist
    //   index, count         its row in that playlist, for Move up and down
    //   queueIndex           its place in the play queue
    //   history              shown in History or Recently played
    //   resolve              function(action, argument): for a suggestion
    //                        that is only a name, looked up before any
    //                        action that needs the song itself (Recs.resolve)
    //   anchor               an item to open under, for a menu asked for
    //                        from the keyboard
    property var context: ({})

    readonly property string sourceId: track && track.sourceId ? track.sourceId : ""
    readonly property string downloadState: Downloads.revision >= 0 ? Downloads.stateFor(sourceId) : ""
    readonly property bool liked: Library.revision >= 0 && Library.isLiked(sourceId)
    readonly property bool inLibrary: Library.revision >= 0 && Library.isInLibrary(sourceId)
    // The names in the artist line, each with its page where known, and the
    // album's page: what "Go to" can open.
    readonly property var artists: track && track.artist
                                   ? Artists.credits(track.artist, track.credits).filter(function(piece) { return piece.link })
                                   : []
    readonly property string albumId: track && track.albumId ? track.albumId : ""

    readonly property int playlistId: context.playlistId > 0 ? context.playlistId : 0
    readonly property int entryId: context.entryId > 0 ? context.entryId : 0
    readonly property int rowIndex: context.index >= 0 ? context.index : -1
    readonly property int rowCount: context.count > 0 ? context.count : 0
    readonly property int queueIndex: context.queueIndex >= 0 ? context.queueIndex : -1
    readonly property bool inQueue: queueIndex >= 0
    readonly property bool inHistory: context.history === true
    // A name to be looked up before it is a song.
    readonly property bool unresolved: sourceId.length === 0 && typeof context.resolve === "function"
    // Whether the entries that need the song itself can be offered.
    readonly property bool songKnown: sourceId.length > 0 || unresolved

    // Up and down: in the queue among what is still to come, in a playlist
    // anywhere in it.
    readonly property int firstUpcoming: Player.queue.currentIndex + 1
    readonly property bool inPlaylist: playlistId > 0 && entryId > 0 && rowIndex >= 0
    readonly property bool canMoveUp: inQueue ? queueIndex > firstUpcoming
                                              : inPlaylist && rowIndex > 0
    readonly property bool canMoveDown: inQueue ? queueIndex >= firstUpcoming && queueIndex < Player.queue.count - 1
                                                : inPlaylist && rowIndex < rowCount - 1
    readonly property bool canLeaveQueue: inQueue && queueIndex !== Player.queue.currentIndex

    // The groups the rules sit between; see the rules below.
    readonly property bool playGroup: !inQueue
    readonly property bool orderGroup: canMoveUp || canMoveDown || canLeaveQueue

    function show(newTrack, newContext) {
        track = newTrack ? newTrack : ({})
        context = newContext ? newContext : ({})
        const anchor = context.anchor
        if (anchor)
            popup(anchor, 0, anchor.height)
        else
            popup()
    }

    // One artist: "Go to artist". A joint credit: one entry per name, so each
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

    function move(delta) {
        if (inQueue)
            Player.moveInQueue(queueIndex, queueIndex + delta)
        else if (inPlaylist)
            Library.movePlaylistEntry(playlistId, entryId, rowIndex + delta)
    }

    // An entry that needs the song: done at once, or once a suggestion has
    // been found (Main hands the answer to run()).
    function act(action, argument) {
        if (sourceId.length > 0)
            run(action, argument, track)
        else if (unresolved)
            context.resolve(action, argument === undefined ? "" : String(argument))
    }

    function run(action, argument, song) {
        if (!song || !song.sourceId)
            return
        if (action === "next")
            Player.playNext(song)
        else if (action === "queue")
            Player.addToQueue(song)
        else if (action === "like")
            Library.setLiked(song, true)
        else if (action === "unlike")
            Library.setLiked(song, false)
        else if (action === "playlist")
            Library.addToPlaylist(parseInt(argument), song)
        else if (action === "download")
            Downloads.enqueue(song.sourceId, song.title, song.artist, song.artwork, song.durationMs,
                              song.isVideo === true)
        else if (action === "copy")
            Library.copyLink(Menus.songLink(song.sourceId))
        else if (action === "open")
            Qt.openUrlExternally(Menus.watchLink(song.sourceId))
    }

    // A hidden entry is disabled as well, throughout. The arrow keys pass
    // over disabled entries but not hidden ones, so an entry that was only
    // hidden took the highlight out of sight, and Enter ran it.

    // — playing it —
    MonoMenuItem {
        visible: menu.playGroup
        enabled: menu.playGroup && menu.songKnown
        text: "Play next"
        onTriggered: menu.act("next")
    }
    MonoMenuItem {
        visible: menu.playGroup
        enabled: menu.playGroup && menu.songKnown
        text: "Add to queue"
        onTriggered: menu.act("queue")
    }

    MonoMenuRule { visible: menu.playGroup && menu.orderGroup }

    // — its place in the list —
    MonoMenuItem {
        visible: menu.canMoveUp
        enabled: menu.canMoveUp
        text: "Move up"
        onTriggered: menu.move(-1)
    }
    MonoMenuItem {
        visible: menu.canMoveDown
        enabled: menu.canMoveDown
        text: "Move down"
        onTriggered: menu.move(1)
    }
    MonoMenuItem {
        visible: menu.canLeaveQueue
        enabled: menu.canLeaveQueue
        text: "Remove from queue"
        onTriggered: Player.removeFromQueue(menu.queueIndex)
    }

    MonoMenuRule { visible: menu.playGroup || menu.orderGroup }

    // — keeping it —
    MonoMenuItem {
        text: menu.liked ? "Remove from Liked songs" : "Add to Liked songs"
        enabled: menu.songKnown
        onTriggered: menu.act(menu.liked ? "unlike" : "like")
    }

    PlaylistSubmenu {
        enabled: menu.songKnown
        onPicked: function(playlistId) { menu.act("playlist", playlistId) }
    }

    MonoMenuItem {
        visible: menu.playlistId > 0 && menu.entryId > 0
        enabled: menu.playlistId > 0 && menu.entryId > 0
        text: "Remove from this playlist"
        onTriggered: Library.removeFromPlaylist(menu.playlistId, menu.entryId)
    }
    MonoMenuItem {
        visible: menu.inHistory && menu.sourceId.length > 0
        enabled: menu.inHistory && menu.sourceId.length > 0
        text: "Remove from history"
        onTriggered: Library.removeFromHistory(menu.sourceId)
    }
    // Every playlist at once, and the like: a second click, as for a file.
    ConfirmMenuItem {
        visible: menu.inLibrary
        enabled: menu.inLibrary
        text: "Remove from library"
        armedText: "Click again to remove it"
        onConfirmed: Library.removeFromLibrary(menu.track)
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

    // — its link —
    MonoMenuRule { visible: menu.songKnown }

    MonoMenuItem {
        visible: menu.songKnown
        enabled: menu.songKnown
        text: "Copy link"
        onTriggered: menu.act("copy")
    }
    MonoMenuItem {
        visible: menu.songKnown
        enabled: menu.songKnown
        text: "Open on YouTube"
        onTriggered: menu.act("open")
    }

    MonoMenuRule {}

    // — the offline copy —
    // What can be done depends on where the download has got to: fetch it,
    // call it off, try it again, or, once the file is here, find it or
    // delete it.
    MonoMenuItem {
        visible: menu.downloadState.length === 0
        text: "Download"
        enabled: Downloads.available && menu.songKnown && menu.downloadState.length === 0
        onTriggered: menu.act("download")
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
    // Deleting takes a second click, as it does in Downloads, so a stray one
    // cannot lose a file.
    ConfirmMenuItem {
        visible: menu.downloadState === "done"
        enabled: menu.downloadState === "done"
        text: "Remove download"
        armedText: "Click again to delete the file"
        onConfirmed: Downloads.remove(menu.sourceId)
    }
}
