import QtQuick
import Monolist
import Monolist.Backend

// What can be done with an album, a playlist or an artist from YouTube Music,
// on a card or saved in the library: play it where it stands, open it, keep
// it or let it go, and its link. Main keeps the one instance and opens it
// through Menus.openCard(card, origin). A card that is a song opens
// TrackMenu instead.
MonoMenu {
    id: menu

    // type, browseId, title, subtitle, artist, artwork: a card as Catalog
    // gives one (InnerTube::cardToVariant), or a saved row in that shape.
    property var card: ({})
    // Which surface this is, for the player (see Player.playModel).
    property string origin: ""

    readonly property string browseId: card && card.browseId ? card.browseId : ""
    readonly property string type: card && card.type ? card.type : ""
    readonly property bool collection: type === "album" || type === "playlist"
    readonly property bool saved: Library.revision >= 0 && Library.isSaved(browseId)

    function show(newCard, newOrigin) {
        card = newCard ? newCard : ({})
        origin = newOrigin ? newOrigin : ""
        popup()
    }

    function openCard() {
        if (type === "artist")
            Nav.openArtist(card.title, browseId)
        else
            Nav.openPage(browseId)
    }

    MonoMenuItem {
        visible: menu.collection
        enabled: menu.collection && menu.browseId.length > 0
        text: "Play"
        onTriggered: Catalog.playCollection(menu.browseId, menu.card.title || "", menu.origin)
    }
    MonoMenuItem {
        enabled: menu.browseId.length > 0
        text: menu.type === "artist" ? "Go to artist"
            : menu.type === "playlist" ? "Open playlist" : "Open album"
        onTriggered: menu.openCard()
    }
    MonoMenuItem {
        visible: menu.collection
        enabled: menu.collection && menu.browseId.length > 0
        text: menu.saved ? "Remove from library" : "Save to library"
        onTriggered: Library.setSaved(menu.card, !menu.saved)
    }

    MonoMenuRule {}

    MonoMenuItem {
        enabled: menu.browseId.length > 0
        text: "Copy link"
        onTriggered: Library.copyLink(Menus.pageLink(menu.browseId, menu.type))
    }
    MonoMenuItem {
        enabled: menu.browseId.length > 0
        text: "Open on YouTube Music"
        onTriggered: Qt.openUrlExternally(Menus.pageLink(menu.browseId, menu.type))
    }
}
