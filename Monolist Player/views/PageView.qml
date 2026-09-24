import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// An album or playlist: its cover — the one photograph in the system allowed
// its colour, since the whole page is about it — the title set large, the
// page's actions, and its songs.
Flickable {
    id: root

    readonly property var page: Catalog.page
    readonly property bool wide: width >= 900

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: MonoScrollBar {}

    function downloadAll() {
        var tracks = Catalog.pageTrackList()
        for (var i = 0; i < tracks.length; ++i) {
            var track = tracks[i]
            Downloads.enqueue(track.sourceId, track.title, track.artist, track.artwork, track.durationMs)
        }
    }

    readonly property bool saved: Library.revision >= 0 && Library.isSaved(page.browseId !== undefined ? page.browseId : "")

    MonoMenu {
        id: pageMenu

        Action {
            text: "Add all to queue"
            enabled: Catalog.pageTracks.count > 0
            onTriggered: {
                var tracks = Catalog.pageTrackList()
                for (var i = 0; i < tracks.length; ++i)
                    Player.addToQueue(tracks[i])
            }
        }
        PlaylistSubmenu {
            title: "Add all to playlist"
            enabled: Catalog.pageTracks.count > 0
            onPicked: function(playlistId) { Library.addAllToPlaylist(playlistId, Catalog.pageTrackList()) }
        }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        // — head —
        Item {
            width: parent.width
            height: Math.max(cover.height, info.implicitHeight)

            Artwork {
                id: cover
                width: root.wide ? 248 : 168
                height: width
                placeholder: ""
                colour: true
                source: root.page.artwork !== undefined ? root.page.artwork : ""
            }

            Column {
                id: info
                anchors.left: cover.right
                anchors.leftMargin: Theme.space6
                anchors.right: parent.right
                anchors.bottom: cover.bottom
                spacing: Theme.space2

                Text {
                    text: (root.page.subtitle !== undefined ? root.page.subtitle : "").toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.18)
                    color: Theme.accent700
                }

                Text {
                    width: parent.width
                    text: root.page.title !== undefined ? root.page.title
                          : (Catalog.pageLoading ? "Loading…" : "")
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: root.wide ? 56 : 40
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(root.wide ? 56 : 40, -0.03)
                    lineHeight: 0.95
                    lineHeightMode: Text.ProportionalHeight
                    color: Theme.text
                }

                Text {
                    visible: text.length > 0
                    width: parent.width
                    text: root.page.artist !== undefined ? root.page.artist : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Theme.weightMedium
                    color: Theme.text
                }

                Text {
                    visible: text.length > 0
                    text: root.page.details !== undefined ? root.page.details : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }

                Flow {
                    width: parent.width
                    topPadding: Theme.space3
                    spacing: Theme.space3

                    ActionButton {
                        primary: true
                        iconName: "play"
                        text: "Play"
                        enabled: Catalog.pageTracks.count > 0
                        onClicked: Player.playModel(Catalog.pageTracks, 0, "playlist")
                    }
                    ActionButton {
                        iconName: "shuffle"
                        text: "Shuffle"
                        enabled: Catalog.pageTracks.count > 1
                        onClicked: {
                            Player.shuffle = true
                            Player.playModel(Catalog.pageTracks, Math.floor(Math.random() * Catalog.pageTracks.count), "playlist")
                        }
                    }
                    ActionButton {
                        iconName: root.saved ? "check" : "plus"
                        text: root.saved ? "Saved" : "Save"
                        enabled: root.page.title !== undefined && root.page.error === undefined
                        onClicked: Library.setSaved(root.page, !root.saved)
                    }
                    ActionButton {
                        visible: Downloads.available
                        iconName: "download"
                        text: "Download all"
                        enabled: Catalog.pageTracks.count > 0
                        onClicked: root.downloadAll()
                    }
                    ActionButton {
                        id: moreButton
                        iconName: "dots"
                        enabled: Catalog.pageTracks.count > 0
                        onClicked: pageMenu.popup(moreButton, 0, moreButton.height + Theme.space1)
                    }
                }
            }
        }

        Text {
            visible: text.length > 0
            width: Math.min(parent.width, 760)
            text: root.page.description !== undefined ? root.page.description : ""
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 13
            lineHeight: 1.3
            color: Theme.neutral700
        }

        Text {
            visible: root.page.error !== undefined
            width: parent.width
            text: root.page.error !== undefined ? "This page could not be loaded: " + root.page.error : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        TrackTable {
            visible: Catalog.pageTracks.count > 0
            width: parent.width
            model: Catalog.pageTracks
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(Catalog.pageTracks, index, "playlist") }
        }
    }
}
