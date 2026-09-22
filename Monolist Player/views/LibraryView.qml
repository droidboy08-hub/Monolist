import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// Your Library: what you made, what you saved, what you played, one tab each.
// The tab is part of the view's name ("library:albums"), so back and forward
// step through tabs like pages.
Flickable {
    id: root

    // "playlists", "albums" or "history"
    property string tab: "playlists"
    signal tabRequested(string tab)
    signal viewRequested(string view)
    signal pageRequested(string browseId)
    signal newPlaylistRequested()

    // Clearing the history takes a second click, so a stray one cannot.
    property bool clearArmed: false

    readonly property int columns: Math.max(1, Math.min(5, Math.floor((column.width + Theme.space6) / (200 + Theme.space6))))
    readonly property int cardWidth: Math.floor((column.width - (columns - 1) * Theme.space6) / columns)

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    onTabChanged: {
        clearArmed = false
        contentY = 0
    }

    ScrollBar.vertical: MonoScrollBar {}

    Timer {
        id: disarm
        interval: 3000
        onTriggered: root.clearArmed = false
    }

    function songs(n) { return n + (n === 1 ? " SONG" : " SONGS") }

    component Note: Text {
        width: column.width
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 14
        color: Theme.neutral700
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
            action: root.tab === "history" && Library.history.count > 0
                    ? (root.clearArmed ? "CLICK AGAIN TO CLEAR" : "CLEAR HISTORY") : ""
            onActionTriggered: {
                if (root.clearArmed) {
                    root.clearArmed = false
                    Library.clearHistory()
                } else {
                    root.clearArmed = true
                    disarm.restart()
                }
            }
        }

        // Negative spacing lets neighbouring segments share one 2px rule.
        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "PLAYLISTS"
                selected: root.tab === "playlists"
                onPicked: root.tabRequested("playlists")
            }
            ChoiceChip {
                label: "ALBUMS"
                selected: root.tab === "albums"
                onPicked: root.tabRequested("albums")
            }
            ChoiceChip {
                label: "HISTORY"
                selected: root.tab === "history"
                onPicked: root.tabRequested("history")
            }
        }

        // — playlists: liked songs, yours, a new one, then the saved —
        Flow {
            visible: root.tab === "playlists"
            width: parent.width
            spacing: Theme.space6

            AlbumCard {
                width: root.cardWidth
                plate: "liked"
                title: "Liked songs"
                artist: "Everything you liked, latest first"
                footer: root.songs(Library.liked.count)
                onPlayRequested: root.viewRequested("playlist:liked")
            }

            Repeater {
                model: Library.playlists

                delegate: AlbumCard {
                    width: root.cardWidth
                    artworks: model.artworks
                    title: model.name
                    artist: "By " + Library.userName
                    footer: "PLAYLIST · " + root.songs(model.trackCount)
                    onPlayRequested: root.viewRequested("playlist:" + model.playlistId)
                }
            }

            AlbumCard {
                width: root.cardWidth
                plate: "new"
                title: "New playlist"
                artist: "Name it, then fill it"
                footer: "CREATE"
                onPlayRequested: root.newPlaylistRequested()
            }

            Repeater {
                model: Library.savedPlaylists

                delegate: AlbumCard {
                    width: root.cardWidth
                    artwork: model.artwork
                    title: model.title
                    artist: model.artist
                    footer: "PLAYLIST · SAVED"
                    onPlayRequested: root.pageRequested(model.browseId)
                }
            }
        }

        // — albums —
        Note {
            visible: root.tab === "albums" && Library.albums.count === 0
            text: "Albums you save show up here. Open any album and press Save."
        }

        Flow {
            visible: root.tab === "albums" && Library.albums.count > 0
            width: parent.width
            spacing: Theme.space6

            Repeater {
                model: Library.albums

                delegate: AlbumCard {
                    width: root.cardWidth
                    artwork: model.artwork
                    title: model.title
                    artist: model.artist
                    year: model.year
                    format: model.format
                    onPlayRequested: root.pageRequested(model.browseId)
                }
            }
        }

        // — history —
        Note {
            visible: root.tab === "history" && Library.history.count === 0
            text: "Nothing played yet. Songs you play show up here, the latest first."
        }

        TrackTable {
            visible: root.tab === "history" && Library.history.count > 0
            width: parent.width
            model: Library.history
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(Library.history, index) }
        }
    }
}
