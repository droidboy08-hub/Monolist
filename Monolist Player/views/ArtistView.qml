import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// An artist: their portrait, the name set large, the page's three ways to
// listen — their top songs in order, YouTube Music's shuffle of everything
// they made, and a radio of them and artists like them — then the top songs
// and the shelves YouTube Music keeps for them, in its order and under its
// titles: albums, singles and EPs, videos, what they are featured on, and
// the artists their fans also play, which open their own pages.
//
// The portrait prints black and white. A page about one record shows that
// record's cover in colour because the cover is its subject; an artist's
// page is about a person and many records, and no one photograph is what it
// is about. The records on it come to colour under the pointer, as cards do
// everywhere.
ScrollPage {
    id: root

    signal pageRequested(string browseId)

    readonly property var artist: Catalog.artist
    readonly property bool wide: width >= 900
    readonly property string name: artist.name !== undefined ? artist.name : ""
    readonly property bool lookingUp: artist.lookingUp === true
    readonly property var shelves: artist.shelves !== undefined ? artist.shelves : []
    readonly property int songCount: Catalog.artistSongs.count
    readonly property string songsId: artist.songsId !== undefined ? artist.songsId : ""
    property bool aboutOpen: false

    // The page it was showing, so a new artist starts at the top and a page
    // finishing loading does not throw the reader back up.
    property string shownId: ""

    contentHeight: column.implicitHeight

    function pad(n) { return n < 10 ? "0" + n : String(n) }

    // Opened, once it is known: one of Home's Jump back in places.
    function rememberArtist() {
        // Once the page has come (it has its shelves then), with its picture.
        if (!visible || lookingUp || artist.error !== undefined || name.length === 0
                || artist.shelves === undefined
                || artist.browseId === undefined || artist.browseId.length === 0)
            return
        Library.rememberPlace({ kind: "artist", ref: artist.browseId, title: name,
                                artwork: artist.artwork !== undefined ? artist.artwork : "" })
    }
    onVisibleChanged: rememberArtist()
    Connections {
        target: Catalog
        function onArtistChanged() { root.rememberArtist() }
    }

    function openCard(card) {
        if (card.type === "album" || card.type === "playlist")
            pageRequested(card.browseId)
        else if (card.type === "artist")
            Nav.openArtist(card.title, card.browseId)
        else if (card.videoId)
            Player.playSource(card.videoId, card.title, card.artist ? card.artist : root.name,
                              card.artwork, 0, "", card.type === "video", "explore",
                              card.primaryArtist ? card.primaryArtist : "")
    }

    Connections {
        target: Catalog
        function onArtistChanged() {
            var id = Catalog.artist.browseId !== undefined ? Catalog.artist.browseId
                                                           : "name:" + root.name
            if (id === root.shownId)
                return
            root.shownId = id
            root.contentY = 0
            root.aboutOpen = false
        }
        // The Shuffle or the Mix, fetched: played as YouTube Music would.
        function onArtistMixReady(kind, tracks) {
            if (tracks.length === 0)
                return
            if (kind === "shuffle")
                Player.shuffle = true
            Player.playTracks(tracks, 0, "explore")
        }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        // — head —
        Item {
            width: parent.width
            height: Math.max(portrait.height, info.implicitHeight)

            Artwork {
                id: portrait
                width: root.wide ? 248 : 168
                height: width
                placeholder: ""
                source: root.artist.artwork !== undefined ? root.artist.artwork : ""
            }

            Column {
                id: info
                anchors.left: portrait.right
                anchors.leftMargin: Theme.space6
                anchors.right: parent.right
                anchors.bottom: portrait.bottom
                spacing: Theme.space2

                Text {
                    width: parent.width
                    text: (root.lookingUp ? "ARTIST · FINDING THEIR PAGE"
                          : (root.artist.channel === true ? "CHANNEL" : "ARTIST")
                            + (root.artist.audience ? " · " + root.artist.audience : "")).toUpperCase()
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.18)
                    color: Theme.accent700
                }

                Text {
                    width: parent.width
                    text: root.name.length > 0 ? root.name : (Catalog.artistLoading ? "Loading…" : "")
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: root.wide ? 56 : 40
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(root.wide ? 56 : 40, -0.03)
                    lineHeight: 0.95
                    lineHeightMode: Text.ProportionalHeight
                    color: Theme.text
                }

                Flow {
                    width: parent.width
                    topPadding: Theme.space3
                    spacing: Theme.space3

                    // Their top songs, in order; autoplay carries on after.
                    ActionButton {
                        primary: true
                        iconName: "play"
                        text: "Play"
                        enabled: root.songCount > 0
                        onClicked: Player.playModel(Catalog.artistSongs, 0, "explore")
                    }
                    // Everything they made, shuffled by YouTube Music. Three
                    // dots while it is being fetched.
                    ActionButton {
                        iconName: Catalog.artistMixLoading === "shuffle" ? "dots" : "shuffle"
                        text: "Shuffle"
                        enabled: root.artist.canShuffle === true
                        onClicked: Catalog.loadArtistMix("shuffle")
                    }
                    // Them and artists like them.
                    ActionButton {
                        iconName: Catalog.artistMixLoading === "radio" ? "dots" : "radio"
                        text: "Radio"
                        enabled: root.artist.canRadio === true
                        onClicked: Catalog.loadArtistMix("radio")
                    }
                }
            }
        }

        // Long, as a rule (the first paragraphs of an encyclopaedia): three
        // lines, and the rest on a click.
        Text {
            visible: text.length > 0
            width: Math.min(parent.width, 760)
            text: root.artist.description !== undefined ? root.artist.description : ""
            wrapMode: Text.WordWrap
            maximumLineCount: root.aboutOpen ? 1000 : 3
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 13
            lineHeight: 1.3
            color: Theme.neutral700

            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.aboutOpen = !root.aboutOpen }
        }

        Text {
            visible: root.artist.error !== undefined
            width: parent.width
            text: root.artist.error !== undefined ? "This artist could not be loaded: " + root.artist.error : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        // — top songs —
        Column {
            visible: root.songCount > 0
            width: parent.width
            topPadding: Theme.space4
            spacing: Theme.space6

            SectionHeader {
                width: parent.width
                number: "01"
                title: root.artist.songsTitle ? root.artist.songsTitle : "Top songs"
                // Every song of theirs, as YouTube Music's own playlist.
                action: root.songsId.length > 0 ? "SHOW ALL" : ""
                onActionTriggered: root.pageRequested(root.songsId)
            }

            TrackTable {
                width: parent.width
                model: Catalog.artistSongs
                showDownloads: true
                onTrackActivated: function(index) { Player.playModel(Catalog.artistSongs, index, "explore") }
            }
        }

        // — albums, singles, videos, featured on, fans also like —
        Repeater {
            model: root.shelves

            delegate: Column {
                id: shelf

                required property int index
                required property var modelData

                width: column.width
                spacing: Theme.space8

                HRule {
                    visible: shelf.index > 0 || root.songCount > 0
                    width: parent.width
                }

                CardShelf {
                    width: parent.width
                    number: root.pad(shelf.index + (root.songCount > 0 ? 2 : 1))
                    title: shelf.modelData.title
                    strapline: shelf.modelData.strapline
                    items: shelf.modelData.items
                    more: shelf.modelData.more
                    origin: "explore"
                    onCardActivated: function(card) { root.openCard(card) }
                }
            }
        }
    }
}
