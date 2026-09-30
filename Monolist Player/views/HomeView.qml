import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// Home: what is new, what to play next, and what you played. The content is
// YouTube Music's own feed, set in the system's type — the newest release on
// the red poster, then numbered sections in reading order.
//
// Signed out, or with "Use my account for Home" off, that feed is the one
// anyone would see, and the listener's own suggestions from this computer
// (Recs.homeShelves: "Made for you", "Because you like …") come first, when
// the recommendation data is installed and there is listening to go on.
// With the YouTube Music account's own feed (Catalog.personalFeed), the page
// is that feed as YouTube Music lays it out, for the account: its shelves in
// their own order under their own headings, Quick picks among them, then new
// releases and Recently played.
ScrollPage {
    id: root

    signal pageRequested(string browseId)
    // Another of the app's views by name: Recently played's SHOW ALL.
    signal viewRequested(string view)

    contentHeight: column.implicitHeight

    readonly property bool personal: Catalog.personalFeed
    // The listener's own suggestions, never beside the account's feed.
    readonly property var recPicks: personal ? [] : Recs.homeShelves
    readonly property bool hasRecs: {
        for (let i = 0; i < recPicks.length; ++i) {
            const shelf = Recs.shelves[recPicks[i]]
            if (shelf && shelf.rows && shelf.rows.length > 0)
                return true
        }
        return false
    }
    readonly property bool hasPicks: Catalog.quickPicks.count > 0
    readonly property bool hasRecent: Catalog.recent.count > 0
    // Quick picks where the account's feed has them, or at the top.
    readonly property bool picksInline: personal && Catalog.quickPicksAt >= 0 && hasPicks
    readonly property bool picksOnTop: hasPicks && !picksInline

    // Sections are numbered in reading order, whichever of them have content.
    readonly property int recBase: hasRecs ? 1 : 0
    readonly property int shelfBase: personal ? (picksOnTop ? 1 : 0)
                                              : recBase + (picksOnTop ? 1 : 0) + (hasRecent ? 1 : 0)
    function shelfNumber(index) {
        return pad(shelfBase + index + 1 + (picksInline && index >= Catalog.quickPicksAt ? 1 : 0))
    }

    function pad(n) { return n < 10 ? "0" + n : String(n) }

    // A suggestion shelf's own map, or an empty one while the page is
    // rebuilt under it.
    function recShelf(index) {
        const shelf = Recs.shelves[index]
        return shelf ? shelf : ({ title: "", reason: "", rows: [], more: false })
    }

    function openCard(card) {
        if (card.type === "album" || card.type === "playlist")
            pageRequested(card.browseId)
        else if (card.type === "artist")
            Nav.openArtist(card.title, card.browseId)
        // The credit read from the card, not its whole subtitle, which also
        // holds a type label or a view count; the subtitle only when the card
        // names no one, and then Last.fm is told nothing (Scrobbler).
        else if (card.videoId)
            Player.playSource(card.videoId, card.title, card.artist ? card.artist : card.subtitle,
                              card.artwork, 0, "", card.type === "video", "home",
                              card.primaryArtist ? card.primaryArtist : "")
    }

    // Built as the page opens, and when it is opened again: a like or a
    // listen since changes what to suggest. Nothing is rebuilt when nothing
    // changed (Recs.refresh).
    onVisibleChanged: if (visible) Recs.refresh()

    // The number, the title and the table, with the rule under it, and the
    // section's one link at the header's end.
    component TrackSection: Column {
        id: section
        property string number: ""
        property string title: ""
        // The small line above the title, as a shelf's.
        property string strapline: ""
        property var model: null
        property string action: ""
        // Recently played is the history's latest: its rows' menus can take
        // a song out of it.
        property bool history: false
        signal actionTriggered()

        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: Theme.space8
        spacing: Theme.space6

        Column {
            width: parent.width
            spacing: Theme.space1

            Text {
                visible: section.strapline.length > 0
                width: parent.width
                text: section.strapline.toUpperCase()
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }
            SectionHeader {
                width: parent.width
                number: section.number
                title: section.title
                action: section.action
                onActionTriggered: section.actionTriggered()
            }
        }

        TrackTable {
            width: parent.width
            model: section.model
            history: section.history
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(section.model, index, "home") }
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

        // — whose feed this is —
        // Only for the account's own: the page is then theirs, not everyone's.
        Text {
            visible: root.personal
            x: Theme.space8
            width: root.width - Theme.space8 * 2
            topPadding: Theme.space8
            text: (Account.accountName.length > 0 ? "FOR " + Account.accountName.toUpperCase() : "FOR YOU")
                  + " · FROM YOUR YOUTUBE MUSIC"
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.18)
            color: Theme.accent700
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
                // Over the last launch's Home, still shown: when that one is from.
                text: (Catalog.savedAt.length > 0 ? "Home as it was on " + Catalog.savedAt + ". " : "")
                      + "YouTube Music did not answer: " + Catalog.error
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

        // — made for you: the listener's own, from this computer —
        // Signed out only. One or two shelves, a few rows each; SHOW ALL
        // goes on from there.
        Column {
            visible: root.hasRecs
            x: Theme.space8
            width: root.width - Theme.space8 * 2
            topPadding: Theme.space8
            bottomPadding: Theme.space8
            spacing: Theme.space6

            Column {
                width: parent.width
                spacing: Theme.space1

                Text {
                    width: parent.width
                    text: "FROM YOUR LISTENING ON THIS COMPUTER"
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
                    title: "Suggested for you"
                }
            }

            Grid {
                id: recGrid
                width: parent.width
                columns: width >= 960 ? 2 : 1
                columnSpacing: Theme.space8
                rowSpacing: Theme.space6

                Repeater {
                    model: root.recPicks

                    RecShelf {
                        required property var modelData

                        width: (recGrid.width - (recGrid.columns - 1) * recGrid.columnSpacing) / recGrid.columns
                        shelfIndex: modelData
                        title: root.recShelf(modelData).title
                        reason: root.recShelf(modelData).reason
                        rows: root.recShelf(modelData).rows
                        hasMore: root.recShelf(modelData).more === true
                        maxRows: 5
                    }
                }
            }
        }

        // Only before a section of its own: the shelves below carry their
        // own rule.
        HRule {
            visible: root.hasRecs && (root.picksOnTop || (!root.personal && root.hasRecent))
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }

        // — quick picks —
        // The whole list, in order; autoplay carries on after it. At the
        // top, unless the account's feed has it further down.
        TrackSection {
            visible: root.picksOnTop
            number: root.pad(root.recBase + 1)
            title: Catalog.quickPicksTitle.length > 0 ? Catalog.quickPicksTitle : "Quick picks"
            strapline: Catalog.quickPicksStrapline
            model: Catalog.quickPicks
            action: "PLAY ALL"
            onActionTriggered: Player.playModel(Catalog.quickPicks, 0, "home")
        }

        HRule {
            visible: root.picksOnTop && !root.personal && root.hasRecent
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }

        // — recently played, signed out: after Quick picks —
        // The last ten; everything played is the library's History.
        TrackSection {
            visible: root.hasRecent && !root.personal
            number: root.pad(root.recBase + (root.picksOnTop ? 1 : 0) + 1)
            title: "Recently played"
            model: Catalog.recent
            history: true
            action: "SHOW ALL"
            onActionTriggered: root.viewRequested("library:history")
        }

        // — shelves: new releases then the feed's own; or, the account's,
        // its feed in its own order then new releases —
        Repeater {
            model: Catalog.shelves

            delegate: Column {
                id: shelfColumn
                required property int index
                required property var modelData

                width: column.width

                // The account's Quick picks, where its feed puts them.
                Loader {
                    active: root.picksInline && shelfColumn.index === Catalog.quickPicksAt
                    visible: active
                    width: parent.width
                    sourceComponent: Column {
                        width: shelfColumn.width

                        HRule {
                            visible: shelfColumn.index > 0 || root.shelfBase > 0
                            x: Theme.space8
                            width: parent.width - Theme.space8 * 2
                        }
                        TrackSection {
                            number: root.pad(root.shelfBase + Catalog.quickPicksAt + 1)
                            title: Catalog.quickPicksTitle.length > 0 ? Catalog.quickPicksTitle : "Quick picks"
                            strapline: Catalog.quickPicksStrapline
                            model: Catalog.quickPicks
                            action: "PLAY ALL"
                            onActionTriggered: Player.playModel(Catalog.quickPicks, 0, "home")
                        }
                    }
                }

                HRule {
                    visible: shelfColumn.index > 0 || root.shelfBase > 0
                             || (root.picksInline && shelfColumn.index >= Catalog.quickPicksAt)
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                }

                Item { width: 1; height: Theme.space8 }

                CardShelf {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                    number: root.shelfNumber(shelfColumn.index)
                    title: shelfColumn.modelData.title
                    strapline: shelfColumn.modelData.strapline
                    items: shelfColumn.modelData.items
                    more: shelfColumn.modelData.more
                    origin: "home"
                    onCardActivated: function(card) { root.openCard(card) }
                }

                Item { width: 1; height: Theme.space8 }
            }
        }

        // — recently played, with the account's feed: after it —
        HRule {
            visible: root.hasRecent && root.personal
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }
        TrackSection {
            visible: root.hasRecent && root.personal
            number: root.pad(root.shelfBase + Catalog.shelves.length + (root.picksInline ? 1 : 0) + 1)
            title: "Recently played"
            model: Catalog.recent
            history: true
            action: "SHOW ALL"
            onActionTriggered: root.viewRequested("library:history")
        }

        Item { width: 1; height: 64 }
    }
}
