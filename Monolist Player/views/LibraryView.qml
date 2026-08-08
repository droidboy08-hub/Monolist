import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

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
            title: "Your Library"
        }

        Grid {
            id: grid
            width: parent.width
            columns: Math.max(1, Math.min(4, Math.floor((width + Theme.space6) / (200 + Theme.space6))))
            columnSpacing: Theme.space6
            rowSpacing: Theme.space6

            Repeater {
                model: Library.albums

                delegate: AlbumCard {
                    width: (grid.width - (grid.columns - 1) * Theme.space6) / grid.columns
                    title: model.title
                    artist: model.artist
                    year: model.year
                    format: model.format
                    artwork: model.artwork
                    onPlayRequested: Player.playIndex(Math.min(model.index, Library.tracks.count - 1))
                }
            }
        }
    }
}
