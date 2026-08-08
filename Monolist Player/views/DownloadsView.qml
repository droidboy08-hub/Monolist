import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// The offline set. Downloaded tracks are inserted into the library as real
// rows, so the table below is the library filtered to what exists on disk —
// the queue state above it is what the DownloadManager is doing right now.
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
            text: Downloads.storedCount + " items available offline"
                  + (Downloads.activeCount > 0
                     ? " · " + Downloads.activeCount + " downloading"
                     : "")
                  + (Downloads.queuedCount > 0
                     ? " · " + Downloads.queuedCount + " queued"
                     : "")
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.letterSpacing: Theme.tracking(13, 0.02)
            color: Theme.neutral700
        }

        Text {
            width: parent.width
            text: Downloads.downloadDirectory
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: 12
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
