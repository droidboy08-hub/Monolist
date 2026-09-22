import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// Home: what is new, what to play next, and what you played. The content is
// YouTube Music's own feed, set in the system's type — the newest release on
// the red poster, then numbered sections in reading order.
Flickable {
    id: root

    signal pageRequested(string browseId)
    signal searchRequested(string term)

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: ScrollBar {
        width: 10
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle { color: Theme.neutral300 }
    }

    // Sections are numbered in reading order, whichever of them have content.
    readonly property bool hasPicks: Catalog.quickPicks.count > 0
    readonly property bool hasRecent: Catalog.recent.count > 0
    readonly property int shelfBase: (hasPicks ? 1 : 0) + (hasRecent ? 1 : 0)

    function pad(n) { return n < 10 ? "0" + n : String(n) }

    function openCard(card) {
        if (card.type === "album" || card.type === "playlist")
            pageRequested(card.browseId)
        else if (card.type === "artist")
            searchRequested(card.title)
        else if (card.videoId)
            Player.playSource(card.videoId, card.title, card.subtitle, card.artwork)
    }

    // The number, the title and the table, with the rule under it.
    component TrackSection: Column {
        property string number: ""
        property string title: ""
        property var model: null

        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: Theme.space8
        spacing: Theme.space6

        SectionHeader {
            width: parent.width
            number: parent.number
            title: parent.title
        }

        TrackTable {
            width: parent.width
            model: parent.model
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(model, index) }
        }
    }

    Column {
        id: column
        width: root.width
        spacing: 0

        PosterHero {
            width: parent.width
            visible: Catalog.featured.title !== undefined
            kicker: "NEW RELEASE"
            titleLine1: Catalog.featured.title !== undefined ? Catalog.featured.title : ""
            meta: Catalog.featured.subtitle !== undefined ? Catalog.featured.subtitle : ""
            artwork: Catalog.featured.artwork !== undefined ? Catalog.featured.artwork : ""
            buttonText: "Open album"
            onPlayRequested: root.pageRequested(Catalog.featured.browseId)
        }

        // — while the feed loads, or when it cannot —
        Text {
            visible: Catalog.loading && Catalog.shelves.length === 0 && !root.hasPicks
            x: Theme.space8
            topPadding: Theme.space8
            text: "Loading YouTube Music…"
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        Row {
            visible: !Catalog.loading && Catalog.error.length > 0
            x: Theme.space8
            topPadding: Theme.space8
            spacing: Theme.space4

            Text {
                width: Math.min(implicitWidth, root.width - Theme.space8 * 2 - 80)
                text: "YouTube Music did not answer: " + Catalog.error
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.accent700
            }
            Text {
                text: "RETRY"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: retryHover.hovered ? Theme.accent700 : Theme.text

                HoverHandler { id: retryHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Catalog.refresh() }
            }
        }

        // — quick picks —
        TrackSection {
            visible: root.hasPicks
            number: "01"
            title: Catalog.quickPicksTitle.length > 0 ? Catalog.quickPicksTitle : "Quick picks"
            model: Catalog.quickPicks
        }

        HRule {
            visible: root.hasPicks && root.hasRecent
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }

        // — recently played —
        TrackSection {
            visible: root.hasRecent
            number: root.pad(root.hasPicks ? 2 : 1)
            title: "Recently played"
            model: Catalog.recent
        }

        // — shelves: new releases, then the feed's own —
        Repeater {
            model: Catalog.shelves

            delegate: Column {
                required property int index
                required property var modelData

                width: column.width

                HRule {
                    visible: parent.index > 0 || root.shelfBase > 0
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                }

                Item { width: 1; height: Theme.space8 }

                CardShelf {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                    number: root.pad(root.shelfBase + parent.index + 1)
                    title: parent.modelData.title
                    strapline: parent.modelData.strapline
                    items: parent.modelData.items
                    onCardActivated: function(card) { root.openCard(card) }
                }

                Item { width: 1; height: Theme.space8 }
            }
        }

        Item { width: 1; height: 64 }
    }
}
