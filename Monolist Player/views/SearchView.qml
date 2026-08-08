import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// Same vocabulary as Home: a section header and the track table. Results now
// come from the extractor (yt-dlp) rather than standing in for it with the
// local library.
Flickable {
    id: root

    property string term: ""

    // A yt-dlp search spawns a process, so wait for typing to settle rather
    // than firing one per keystroke.
    onTermChanged: debounce.restart()

    Timer {
        id: debounce
        interval: 350
        onTriggered: {
            if (root.term.trim().length > 0)
                Extractor.search(root.term)
        }
    }

    contentWidth: width
    contentHeight: column.height
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: ScrollBar {
        width: 10
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle { color: Theme.neutral300 }
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

        Text {
            visible: root.term.length === 0
            width: parent.width
            text: Extractor.available
                  ? "Type in the field above to search."
                  : "Search needs yt-dlp on the system path. Install it with: pip install yt-dlp"
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
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

        Text {
            visible: !Extractor.busy && Extractor.lastError.length > 0
            width: parent.width
            text: Extractor.lastError
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        Text {
            visible: !Extractor.busy && root.term.length > 0
                     && Extractor.results.count === 0
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
            onTrackActivated: function(index) {
                var item = Extractor.results.get(index)
                Player.playSource(item.sourceId, item.title, item.artist)
            }
        }
    }
}
