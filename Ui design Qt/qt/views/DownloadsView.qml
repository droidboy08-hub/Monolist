import QtQuick
import QtQuick.Controls.Basic
import Phono
import Phono.Backend
import "../components"

// Offline items. Until yt-dlp downloads are wired up, the local library
// stands in for the downloaded set; the table and states are final.
Flickable {
    id: root

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
            title: "Downloads"
        }

        Text {
            width: parent.width
            text: Library.tracks.count + " items available offline"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.letterSpacing: Theme.tracking(13, 0.02)
            color: Theme.neutral700
        }

        TrackTable {
            width: parent.width
            model: Library.tracks
            activeIndex: Player.currentIndex
            onTrackActivated: function(index) { Player.playIndex(index) }
        }
    }
}
