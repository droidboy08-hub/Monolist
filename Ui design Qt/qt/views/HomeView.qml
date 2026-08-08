import QtQuick
import QtQuick.Controls.Basic
import Phono
import Phono.Backend
import "../components"

Flickable {
    id: root

    signal viewRequested(string view)

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: ScrollBar {
        width: 10
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle { color: Theme.neutral300 }
    }

    Column {
        id: column
        width: root.width
        spacing: 0

        PosterHero {
            width: parent.width
            visible: Library.featured.titleLine1 !== undefined
            kicker: Library.featured.kicker !== undefined ? Library.featured.kicker : ""
            titleLine1: Library.featured.titleLine1 !== undefined ? Library.featured.titleLine1 : ""
            titleLine2: Library.featured.titleLine2 !== undefined ? Library.featured.titleLine2 : ""
            meta: Library.featured.meta !== undefined ? Library.featured.meta : ""
            onPlayRequested: Player.playIndex(0)
        }

        // — 01 recently played —
        Item {
            width: parent.width
            height: albumSection.height + Theme.space8 * 2

            Column {
                id: albumSection
                x: Theme.space8
                y: Theme.space8
                width: parent.width - Theme.space8 * 2
                spacing: Theme.space6

                SectionHeader {
                    width: parent.width
                    number: "01"
                    title: "Recently played"
                    action: "SHOW ALL →"
                    onActionTriggered: root.viewRequested("library")
                }

                Grid {
                    id: albumGrid
                    width: parent.width
                    columns: Math.max(1, Math.min(4, Math.floor((width + Theme.space6) / (200 + Theme.space6))))
                    columnSpacing: Theme.space6
                    rowSpacing: Theme.space6

                    Repeater {
                        model: Library.albums

                        delegate: AlbumCard {
                            width: (albumGrid.width - (albumGrid.columns - 1) * Theme.space6) / albumGrid.columns
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

        HRule {
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }

        // — 02 in rotation —
        Item {
            width: parent.width
            height: trackSection.height + Theme.space8 * 2 + 64

            Column {
                id: trackSection
                x: Theme.space8
                y: Theme.space8
                width: parent.width - Theme.space8 * 2
                spacing: Theme.space6

                SectionHeader {
                    width: parent.width
                    number: "02"
                    title: "In rotation this week"
                }

                TrackTable {
                    width: parent.width
                    model: Library.tracks
                    activeIndex: Player.currentIndex
                    onTrackActivated: function(index) { Player.playIndex(index) }
                }
            }
        }
    }
}
