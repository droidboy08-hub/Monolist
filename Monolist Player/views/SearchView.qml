import QtQuick
import Monolist
import Monolist.Backend
import "../components"
import "../ImportText.js" as ImportText

// Same vocabulary as Home: a section header, the track table for songs and
// videos, and cards for albums, artists and playlists. Results come from
// YouTube Music's own API; songs and videos fall back to youtube.com and then
// yt-dlp.
ScrollPage {
    id: root

    property string term: ""

    // The empty page's way to the recommendation folders.
    signal settingsRequested()
    // A recent search picked on the empty page: typed into the field, as if
    // typed there.
    signal searchRequested(string term)

    // A song an import could not place, being looked for here (the review's
    // SEARCH, ImportReview): its row as the review has it, and the
    // playlist's id; null otherwise. Its results can then be used for it.
    property var pick: null
    signal pickUsed(var track)
    signal pickCancelled()

    // A link to a playlist or an album that can be imported: offered above
    // the results, which for a link are rarely what was meant. Read as it is
    // typed, without asking anything (Importer.parse).
    readonly property var importLink: term.trim().length > 8 ? Importer.parse(term) : ({})
    readonly property bool importOffered: importLink.ok === true

    // Albums, artists and playlists are cards; songs and videos are rows.
    readonly property bool cardMode: Extractor.filter === "albums" || Extractor.filter === "artists"
                                     || Extractor.filter === "playlists"
    readonly property int found: {
        if (!cardMode)
            return Extractor.results.count
        let count = 0
        const sections = Extractor.cardSections
        for (let i = 0; i < sections.length; ++i)
            count += sections[i].items.length
        return count
    }

    // A card opens what it is; its play plate plays it (ShelfCard). Either
    // way the search was used, and is kept among the recent ones.
    function openCard(card) {
        Library.rememberSearch(root.term)
        if (card.type === "artist")
            Nav.openArtist(card.title, card.browseId)
        else
            Nav.openPage(card.browseId)
    }

    // YouTube Music answers in a few hundred milliseconds, so results can
    // follow the typing; the pause only keeps one request per word or so.
    onTermChanged: debounce.restart()

    Timer {
        id: debounce
        interval: 250
        onTriggered: Extractor.search(root.term)
    }

    contentHeight: column.height

    // No cap: YouTube Music answers twenty at a time, and the next twenty
    // are asked for while the reader is still a screen away from the end, so
    // the list keeps ahead of the scrolling (as a long playlist does,
    // PageView). A page landing with the end still in sight asks for the one
    // after. For cards, this is the last section's; one above it has its own
    // "show more". Never while choosing a song for an import: only the first
    // few results are shown then, so the page never grows, and every page
    // landing would ask for the next one, all of them, one after another.
    function loadMoreIfNear() {
        if (visible && root.pick === null && root.term.length > 0 && Extractor.hasMore && !Extractor.loadingMore
                && !Extractor.busy && contentY + height * 2 >= contentHeight)
            Extractor.loadMore(-1)
    }
    onContentYChanged: loadMoreIfNear()
    onContentHeightChanged: loadMoreIfNear()
    onVisibleChanged: loadMoreIfNear()
    onPickChanged: loadMoreIfNear()

    Connections {
        target: Extractor
        function onMoreChanged() { root.loadMoreIfNear() }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        SectionHeader {
            width: parent.width
            number: "01"
            title: root.term.length > 0 ? "Results for " + root.term : "Search"
        }

        // — a link that can be imported —
        Rectangle {
            visible: root.importOffered && root.pick === null
            width: parent.width
            height: offerColumn.implicitHeight + Theme.space4 * 2
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: Theme.text

            Column {
                id: offerColumn
                x: Theme.space4
                y: Theme.space4
                width: parent.width - Theme.space4 * 2
                spacing: Theme.space3

                Text {
                    text: (root.importLink.label || "").toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.accent700
                }
                Text {
                    width: parent.width
                    text: "That's a link to " + (root.importLink.type === "album" ? "an album"
                                                 : root.importLink.type === "song" ? "a song" : "a playlist")
                          + ". Import it into a playlist of your own: Monolist finds the songs on YouTube Music."
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.text
                }
                ActionButton {
                    iconName: "import"
                    text: "IMPORT"
                    onClicked: Nav.openImport(root.term.trim())
                }
            }
        }

        // Negative spacing lets neighbouring segments share one 2px rule. A
        // Flow, so that in a narrow window the chips wrap rather than run off
        // the edge, and a wrapped row shares its rule too.
        Flow {
            width: parent.width
            spacing: -Theme.ruleWidth

            Repeater {
                model: [
                    { filter: "songs", label: "SONGS" },
                    { filter: "videos", label: "VIDEOS" },
                    { filter: "albums", label: "ALBUMS" },
                    { filter: "artists", label: "ARTISTS" },
                    { filter: "playlists", label: "PLAYLISTS" }
                ]

                delegate: ChoiceChip {
                    required property var modelData
                    label: modelData.label
                    selected: Extractor.filter === modelData.filter
                    onPicked: Extractor.filter = modelData.filter
                }
            }

            Text {
                height: 32
                leftPadding: Theme.space4 + Theme.ruleWidth
                verticalAlignment: Text.AlignVCenter
                visible: root.term.length > 0 && !Extractor.busy && root.found > 0
                text: Extractor.source === "yt-dlp"
                      ? "FROM YT-DLP · YOUTUBE MUSIC DID NOT ANSWER"
                      : "FROM " + Extractor.source.toUpperCase()
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.neutral700
            }
        }

        // — a song for an import, chosen here —
        Rectangle {
            visible: root.pick !== null
            width: parent.width
            height: pickColumn.implicitHeight + Theme.space4 * 2
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: Theme.text

            Column {
                id: pickColumn
                x: Theme.space4
                y: Theme.space4
                width: parent.width - Theme.space4 * 2
                spacing: Theme.space3

                Item {
                    width: parent.width
                    height: pickHeading.implicitHeight

                    Text {
                        id: pickHeading
                        width: parent.width - pickCancel.width - Theme.space4
                        text: "CHOOSING A SONG FOR YOUR IMPORT"
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        font.letterSpacing: Theme.tracking(11, 0.14)
                        color: Theme.accent700
                    }
                    Text {
                        id: pickCancel
                        anchors.right: parent.right
                        anchors.baseline: pickHeading.baseline
                        text: "CANCEL"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Font.Bold
                        font.letterSpacing: Theme.tracking(12, 0.12)
                        color: pickCancelHover.hovered ? Theme.accent700 : Theme.neutral700

                        HoverHandler { id: pickCancelHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.pickCancelled() }
                    }
                }
                Text {
                    width: parent.width
                    text: root.pick === null ? ""
                          : "Which of these is “" + root.pick.title + "”" + (root.pick.artist ? " by " + root.pick.artist : "")
                            + (root.pick.durationMs > 0 ? " (" + ImportText.length(root.pick.durationMs) + ")" : "")
                            + "? Choose it, and it goes into the playlist in that song's place. If it isn't here, "
                            + "change the search above, or look among the videos."
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.text
                }

                // The first few songs found, each to choose.
                Column {
                    width: parent.width

                    Repeater {
                        model: root.pick !== null ? Extractor.results : null

                        delegate: Item {
                            id: pickRow
                            required property int index
                            required property string title
                            required property string artist
                            required property string album
                            required property string artwork
                            required property string sourceId
                            required property string durationText

                            visible: index < 8
                            width: pickColumn.width
                            height: visible ? 56 : 0

                            HoverHandler { id: pickHover }
                            Rectangle {
                                anchors.fill: parent
                                color: pickHover.hovered ? Theme.rowHover : Theme.clear(Theme.rowHover)

                                Behavior on color {
                                    enabled: !pickHover.hovered
                                    ColorAnimation { duration: Theme.quick }
                                }
                            }
                            Rectangle {
                                width: parent.width
                                height: 1
                                color: Theme.hairline
                            }

                            TrackCover {
                                id: pickCover
                                anchors.verticalCenter: parent.verticalCenter
                                width: 40
                                height: 40
                                source: pickRow.artwork
                                sourceId: pickRow.sourceId
                                hovered: pickHover.hovered
                                active: pickRow.sourceId === Player.currentSourceId
                                covered: Nav.pagesCovered
                                onPlayRequested: Player.playTracks([Extractor.results.get(pickRow.index)], 0, "search")
                            }
                            Column {
                                anchors.left: pickCover.right
                                anchors.leftMargin: Theme.space3
                                anchors.right: pickUse.left
                                anchors.rightMargin: Theme.space4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    width: parent.width
                                    text: pickRow.title
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 14
                                    font.weight: Theme.weightMedium
                                    color: Theme.text
                                }
                                Text {
                                    width: parent.width
                                    text: pickRow.artist + (pickRow.album ? "  ·  " + pickRow.album : "")
                                          + (pickRow.durationText ? "  ·  " + pickRow.durationText : "")
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    color: Theme.neutral700
                                }
                            }
                            Text {
                                id: pickUse
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: "USE THIS"
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                font.weight: Font.Bold
                                font.letterSpacing: Theme.tracking(12, 0.12)
                                color: pickUseHover.hovered ? Theme.accent700 : Theme.neutral700

                                HoverHandler { id: pickUseHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.pickUsed(Extractor.results.get(pickRow.index)) }
                            }
                        }
                    }
                }

                Text {
                    visible: root.pick !== null && !Extractor.busy && Extractor.results.count === 0
                    text: root.term.length > 0 ? "Nothing found for that: change the search above." : "Type a search above."
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }
            }
        }

        // — with nothing typed, the page is suggestions rather than an
        // instruction nobody needs twice —
        Text {
            visible: root.term.length === 0 && !Recs.available && Recs.busy
            width: parent.width
            text: "Reading the catalogue…"
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        // Nothing to suggest from: said plainly, and the download offered
        // right here, rather than a blank page that leaves the listener to
        // guess why there is nothing on it.
        Column {
            visible: root.term.length === 0 && !Recs.available && !Recs.busy
            width: parent.width
            spacing: Theme.space4

            Text {
                width: parent.width
                // A folder set by hand that holds no catalogue says so, since
                // that is the thing to fix.
                text: Recs.dataDirectory.length > 0 && Recs.message.length > 0 && !RecData.busy
                      ? Recs.message
                      : "Nothing to suggest yet: suggestions come from the recommendation data, which is "
                        + "downloaded separately. The field above searches YouTube Music in the meantime."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 14
                color: Theme.neutral700
            }

            RecDataPanel {
                width: parent.width
            }

            // For a copy kept somewhere else.
            Text {
                text: "OR CHOOSE A FOLDER IN SETTINGS →"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: settingsLinkHover.hovered ? Theme.accent700 : Theme.neutral700

                HoverHandler { id: settingsLinkHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.settingsRequested() }
            }
        }

        Text {
            visible: root.term.length === 0 && Recs.available && Recs.busy
            width: parent.width
            text: "Working out what to suggest…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        // What the shelves are, and the way to a fresh set: the page draws
        // new rows by itself every 45 minutes, and REFRESH does it now.
        Item {
            visible: root.term.length === 0 && Recs.available && Recs.shelves.length > 0
            width: parent.width
            height: suggestionsLabel.implicitHeight

            Text {
                id: suggestionsLabel
                width: Math.max(0, parent.width - refreshLink.width - Theme.space4)
                text: Recs.personal ? "SUGGESTED FOR YOU" : "SUGGESTIONS"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }

            Text {
                id: refreshLink
                anchors.right: parent.right
                anchors.baseline: suggestionsLabel.baseline
                text: Recs.busy ? "REFRESHING…" : "REFRESH"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: refreshHover.hovered && !Recs.busy ? Theme.accent700 : Theme.neutral700

                HoverHandler { id: refreshHover; cursorShape: Recs.busy ? Qt.ArrowCursor : Qt.PointingHandCursor }
                TapHandler {
                    enabled: !Recs.busy
                    onTapped: Recs.rebuild()
                }
            }
        }

        Repeater {
            model: root.term.length === 0 && Recs.available ? Recs.shelves : []

            RecShelf {
                required property var modelData
                required property int index

                width: column.width
                title: modelData.title
                reason: modelData.reason
                rows: modelData.rows
                hasMore: modelData.more === true
                shelfIndex: index
            }
        }

        // — with nothing typed: the searches made before, each to search
        // again or to take away, and YouTube Music's Moods & genres —
        Column {
            visible: root.term.length === 0 && Library.recentSearches.length > 0
            width: parent.width
            spacing: Theme.space3

            Item {
                width: parent.width
                height: recentHeading.implicitHeight

                Text {
                    id: recentHeading
                    text: "RECENT SEARCHES"
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }
                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: recentHeading.verticalCenter
                    text: "CLEAR ALL"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.12)
                    color: clearHover.hovered ? Theme.accent700 : Theme.neutral700

                    HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Library.clearSearches() }
                }
            }

            Flow {
                width: parent.width
                spacing: Theme.space2

                Repeater {
                    model: Library.recentSearches.slice(0, 20)

                    delegate: Rectangle {
                        id: recent
                        required property string modelData

                        width: recentText.implicitWidth + forget.width + Theme.space3 * 2 + Theme.space1
                        height: 36
                        color: recentHover.hovered ? Theme.surface : "transparent"
                        border.width: Theme.ruleWidth
                        border.color: recentHover.hovered ? Theme.text : Theme.neutral300

                        HoverHandler { id: recentHover }

                        Text {
                            id: recentText
                            x: Theme.space3
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, 320)
                            text: recent.modelData
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.text

                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.searchRequested(recent.modelData) }
                        }
                        IconButton {
                            id: forget
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.space1
                            anchors.verticalCenter: parent.verticalCenter
                            side: 28
                            iconName: "x"
                            iconSize: 12
                            iconColor: Theme.neutral700
                            onClicked: Library.forgetSearch(recent.modelData)
                        }
                    }
                }
            }
        }

        Repeater {
            model: root.term.length === 0 ? Catalog.moods : []

            delegate: Column {
                id: moodGroup
                required property var modelData

                width: column.width
                spacing: Theme.space3

                Text {
                    text: (moodGroup.modelData.title || "").toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }

                Flow {
                    width: parent.width
                    spacing: Theme.space3

                    Repeater {
                        model: moodGroup.modelData.chips

                        delegate: MoodChip {
                            required property var modelData
                            width: 184
                            height: 56
                            chip: modelData
                            onActivated: Nav.openMore({ kind: "browse", browseId: modelData.browseId,
                                                        params: modelData.params }, modelData.title)
                        }
                    }
                }
            }
        }

        Text {
            visible: Extractor.busy
            width: parent.width
            text: "Searching…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.letterSpacing: Theme.tracking(13, 0.02)
            color: Theme.neutral700
        }

        // Only under a query: the search itself is only told of an emptied
        // field once the debounce has run, and until then the failure would
        // sit under the suggestions.
        Text {
            visible: !Extractor.busy && root.term.length > 0 && Extractor.lastError.length > 0
            width: parent.width
            text: Extractor.lastError
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        Text {
            visible: !Extractor.busy && root.term.length > 0
                     && root.found === 0
                     && Extractor.lastError.length === 0
                     && root.pick === null && !root.importOffered
            width: parent.width
            text: "No results."
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        // Choosing for an import, the results are the choice above instead.
        TrackTable {
            visible: Extractor.results.count > 0 && root.pick === null
            width: parent.width
            model: Extractor.results
            activeIndex: -1
            showDownloads: true
            // A result is one song asked for by name, not the start of a list,
            // so it plays on its own and autoplay's radio carries on from it
            // with songs like it, as YouTube Music does. The rest of the
            // results are other answers to the search, not what comes next.
            // With "Autoplay similar songs" turned off, the player respects
            // the switch: no radio is fetched, and the song plays alone.
            onTrackActivated: function(index) {
                Library.rememberSearch(root.term)
                Player.playTracks([Extractor.results.get(index)], 0, "search")
            }
        }

        // — albums, artists, playlists: cards, a section each where the
        // search had more than one kind to ask for —
        Repeater {
            model: root.term.length > 0 && root.cardMode ? Extractor.cardSections : []

            delegate: Column {
                id: cardSection

                required property var modelData
                required property int index

                width: column.width
                spacing: Theme.space4

                // This section's next page, added to the cards it has.
                Connections {
                    target: Extractor
                    function onCardsAppended(section, cards) {
                        if (section === cardSection.index)
                            grid.append(cards)
                    }
                }

                Text {
                    visible: text.length > 0
                    text: (cardSection.modelData.title || "").toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }

                CardGrid {
                    id: grid
                    width: parent.width
                    items: cardSection.modelData.items
                    origin: "search"
                    onCardActivated: function(card) { root.openCard(card) }
                }

                // A section with another below it: its next page on asking,
                // since the page's own end belongs to the last one.
                Text {
                    visible: cardSection.index < Extractor.cardSections.length - 1
                             && Extractor.cardMore[cardSection.index] === true
                    text: Extractor.loadingMore ? "LOADING…" : "SHOW MORE"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.12)
                    color: moreHover.hovered && !Extractor.loadingMore ? Theme.accent700 : Theme.neutral700

                    HoverHandler { id: moreHover; cursorShape: Extractor.loadingMore ? Qt.ArrowCursor : Qt.PointingHandCursor }
                    TapHandler {
                        enabled: !Extractor.loadingMore
                        onTapped: Extractor.loadMore(cardSection.index)
                    }
                }
            }
        }

        Text {
            visible: root.term.length > 0 && Extractor.loadingMore
            width: parent.width
            text: "Loading more…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.letterSpacing: Theme.tracking(13, 0.02)
            color: Theme.neutral700
        }
    }
}
