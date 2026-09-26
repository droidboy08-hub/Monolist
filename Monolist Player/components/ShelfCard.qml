import QtQuick
import Monolist
import Monolist.Backend

// One of YouTube Music's cards — an album, a playlist, an artist, a song or a
// video — as an AlbumCard, wherever a list of them is shown: Home's shelves,
// an artist's, a search for albums, a shelf's "show all". Pressing it is
// `activated` (the view decides what that opens); an album's or a playlist's
// play plate plays it from here, through Catalog.playCollection.
AlbumCard {
    id: root

    property var card: ({})
    // Which surface this is, for the player (see Player.playModel).
    property string origin: ""
    signal activated(var card)

    // "Album • Seth Ballad": the kind goes to the footer label, the rest
    // under the title, so neither is said twice.
    readonly property var label: {
        const parts = root.card.subtitle ? root.card.subtitle.split(" • ") : []
        const kinds = ["Album", "Single", "EP", "Playlist", "Song", "Video", "Artist"]
        if (parts.length > 1 && kinds.indexOf(parts[0]) >= 0)
            return { kind: parts[0].toUpperCase(), rest: parts.slice(1).join(" • ") }
        return { kind: root.card.type ? root.card.type.toUpperCase() : "", rest: root.card.subtitle || "" }
    }
    readonly property bool collection: card.type === "album" || card.type === "playlist"

    title: card.title || ""
    artist: label.rest
    year: ""
    format: label.kind
    artwork: card.artwork || ""
    playable: collection && (card.browseId || "").length > 0
    playLoading: playable && Catalog.collectionLoading === card.browseId
    // A song's or a video's is the song menu; anything else's, the card's.
    hasMenu: (card.videoId || "").length > 0 || (card.browseId || "").length > 0
    onPlayRequested: root.activated(root.card)
    onPlayClicked: Catalog.playCollection(root.card.browseId, root.card.title || "", root.origin)
    onMenuRequested: {
        if (!root.collection && card.type !== "artist" && (card.videoId || "").length > 0)
            Menus.openTrack(Menus.trackOfCard(root.card), {})
        else
            Menus.openCard(root.card, root.origin)
    }
}
