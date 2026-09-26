import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// Same vocabulary as Home: a section header, the track table for songs and
// videos, and cards for albums, artists and playlists. Results come from
// YouTube Music's own API; songs and videos fall back to youtube.com and then
// yt-dlp.
ScrollPage {
    id: root

    property string term: ""

    // The empty page's way to the recommendation folders.
    signal settingsRequested()

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

    // A card opens what it is; its play plate plays it (ShelfCard).
    function openCard(card) {
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
            width: parent.width
            text: "No results."
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        TrackTable {
            visible: Extractor.results.count > 0
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

                width: column.width
                spacing: Theme.space4

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
                    width: parent.width
                    items: cardSection.modelData.items
                    origin: "search"
                    onCardActivated: function(card) { root.openCard(card) }
                }
            }
        }
    }
}
