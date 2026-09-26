import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// A shelf's "show all": everything the shelf showed a few of — an artist's
// albums, the week's new releases — as a grid of cards under the shelf's own
// title, with the page's name (the artist's, say) above it. More comes as the
// reader nears the end, where YouTube Music has more to give.
ScrollPage {
    id: root

    readonly property var listing: Catalog.listing
    readonly property var sections: listing.sections !== undefined ? listing.sections : []

    // The listing it was showing, so a new one starts at the top.
    property string shownKey: ""

    contentHeight: column.implicitHeight

    function openCard(card) {
        if (card.type === "album" || card.type === "playlist")
            Nav.openPage(card.browseId)
        else if (card.type === "artist")
            Nav.openArtist(card.title, card.browseId)
        else if (card.videoId)
            Player.playSource(card.videoId, card.title, card.artist ? card.artist : card.subtitle,
                              card.artwork, 0, "", card.type === "video", "explore",
                              card.primaryArtist ? card.primaryArtist : "")
    }

    // As a long playlist does (PageView): the next part while the end is
    // still a screen away.
    function loadMoreIfNear() {
        if (visible && Catalog.listingHasMore && !Catalog.listingLoadingMore && !Catalog.listingLoading
                && contentY + height * 2 >= contentHeight)
            Catalog.loadMoreListing()
    }
    onContentYChanged: loadMoreIfNear()
    onContentHeightChanged: loadMoreIfNear()
    onVisibleChanged: loadMoreIfNear()

    Connections {
        target: Catalog
        function onListingChanged() {
            const key = Catalog.listing.key !== undefined ? Catalog.listing.key : ""
            if (key === root.shownKey)
                return
            root.shownKey = key
            root.contentY = 0
        }
        // A part just in, with the end still in sight: the next.
        function onListingMoreChanged() { root.loadMoreIfNear() }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        Column {
            width: parent.width
            spacing: Theme.space1

            Text {
                visible: text.length > 0
                width: parent.width
                text: (root.listing.kicker !== undefined ? root.listing.kicker : "").toUpperCase()
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }

            SectionHeader {
                width: parent.width
                number: "01"
                title: root.listing.title !== undefined && root.listing.title.length > 0
                       ? root.listing.title : (Catalog.listingLoading ? "Loading…" : "")
                // Its songs, where it lists songs, in order.
                action: Catalog.listingSongs.count > 0 ? "PLAY ALL" : ""
                onActionTriggered: Player.playModel(Catalog.listingSongs, 0, "explore")
            }
        }

        Text {
            visible: Catalog.listingLoading
            width: parent.width
            text: "Loading YouTube Music…"
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        Text {
            visible: root.listing.error !== undefined
            width: parent.width
            text: root.listing.error !== undefined ? "This shelf could not be loaded: " + root.listing.error : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        TrackTable {
            visible: Catalog.listingSongs.count > 0
            width: parent.width
            model: Catalog.listingSongs
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(Catalog.listingSongs, index, "explore") }
        }

        Repeater {
            model: root.sections

            delegate: Column {
                id: section

                required property var modelData
                required property int index

                width: column.width
                spacing: Theme.space4

                // More cards for this grid: added to it, rather than every
                // card made again (and every cover fetched again) for each
                // part that arrives.
                Connections {
                    target: Catalog
                    function onListingAppended(at, cards) {
                        if (at === section.index)
                            grid.append(cards)
                    }
                }

                // A page of several shelves names each; a grid alone is the
                // page's title already.
                Text {
                    visible: root.sections.length > 1 && text.length > 0
                    width: parent.width
                    text: section.modelData.title || ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 19
                    font.weight: Theme.weightBlack
                    color: Theme.text
                }

                CardGrid {
                    id: grid
                    width: parent.width
                    items: section.modelData.items
                    origin: "explore"
                    onCardActivated: function(card) { root.openCard(card) }
                }
            }
        }

        Text {
            visible: Catalog.listingLoadingMore
            width: parent.width
            text: "Loading more…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }
    }
}
