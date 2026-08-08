import QtQuick
import QtQuick.Controls.Basic
import Phono
import Phono.Backend
import "../components"

// Same vocabulary as Home: a section header and the track table.
// Results come from the local library until the extractor backend
// (yt-dlp / NewPipe) is connected.
Flickable {
    id: root

    property string term: ""

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
            text: "Type in the field above to search the local library. Streaming sources connect through the extractor interface."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        TrackTable {
            visible: root.term.length > 0
            width: parent.width
            model: Library.tracks
            activeIndex: Player.currentIndex
            onTrackActivated: function(index) { Player.playIndex(index) }
        }
    }
}
