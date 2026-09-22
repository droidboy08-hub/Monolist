import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// One of the user's playlists, or Liked songs: the same page as an album's,
// with the cover a mosaic of the songs' own, and the title theirs to change.
Flickable {
    id: root

    // "liked", or a playlist's id.
    property string key: ""
    // Set for a playlist just made, so its name is ready to be typed.
    property bool renameOnOpen: false
    signal renameStarted()
    signal deleted()

    readonly property bool liked: key === "liked"
    readonly property int playlistId: liked ? 0 : parseInt(key)
    readonly property var info: Library.playlist
    readonly property bool ready: liked || (info.playlistId === playlistId)
    readonly property bool missing: !liked && ready && !info.exists
    readonly property var songs: liked ? Library.liked : Library.playlistTracks
    readonly property int songCount: songs ? songs.count : 0
    readonly property bool wide: width >= 900

    property bool renaming: false
    property bool confirmingDelete: false

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: MonoScrollBar {}

    onKeyChanged: {
        renaming = false
        confirmingDelete = false
        contentY = 0
    }
    onVisibleChanged: if (!visible) { renaming = false; confirmingDelete = false }

    // A new playlist opens with its name selected, the way a new file does.
    function startRenaming() {
        if (liked || missing)
            return
        titleField.text = info.name
        renaming = true
        titleField.forceActiveFocus()
        titleField.selectAll()
        renameStarted()
    }
    function commitRename() {
        if (!renaming)
            return
        renaming = false
        Library.renamePlaylist(playlistId, titleField.text)
    }

    onRenameOnOpenChanged: if (renameOnOpen && ready) Qt.callLater(startRenaming)
    onReadyChanged: if (renameOnOpen && ready) Qt.callLater(startRenaming)

    function trackList() {
        return liked ? Library.likedTrackList() : Library.playlistTrackList()
    }

    function downloadAll() {
        var tracks = trackList()
        for (var i = 0; i < tracks.length; ++i) {
            var track = tracks[i]
            Downloads.enqueue(track.sourceId, track.title, track.artist, track.artwork, track.durationMs)
        }
    }

    function songsLabel(n) { return n + (n === 1 ? " song" : " songs") }

    MonoMenu {
        id: playlistMenu

        Action {
            text: "Add all to queue"
            enabled: root.songCount > 0
            onTriggered: {
                var tracks = root.trackList()
                for (var i = 0; i < tracks.length; ++i)
                    Player.addToQueue(tracks[i])
            }
        }
        MonoMenuRule {}
        Action {
            text: "Rename"
            onTriggered: root.startRenaming()
        }
        Action {
            text: "Delete playlist"
            onTriggered: root.confirmingDelete = true
        }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        Text {
            visible: root.missing
            width: parent.width
            text: "This playlist no longer exists."
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        // — head —
        Item {
            visible: !root.missing
            width: parent.width
            height: Math.max(cover.height, details.implicitHeight)

            CollectionCover {
                id: cover
                width: root.wide ? 248 : 168
                height: width
                plate: root.liked ? "liked" : ""
                artworks: root.liked || !root.ready ? [] : root.info.artworks
                colour: true
            }

            Column {
                id: details
                anchors.left: cover.right
                anchors.leftMargin: Theme.space6
                anchors.right: parent.right
                anchors.bottom: cover.bottom
                spacing: Theme.space2

                Text {
                    text: "PLAYLIST"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.18)
                    color: Theme.accent700
                }

                Item {
                    width: parent.width
                    height: root.renaming ? titleField.implicitHeight + Theme.space2 : title.implicitHeight

                    Text {
                        id: title
                        visible: !root.renaming
                        width: parent.width
                        text: root.liked ? "Liked songs" : (root.ready ? root.info.name : "")
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

                        // Clicking the name of one's own playlist edits it.
                        HoverHandler {
                            enabled: !root.liked
                            cursorShape: Qt.IBeamCursor
                        }
                        TapHandler {
                            enabled: !root.liked
                            onTapped: root.startRenaming()
                        }
                    }

                    TextInput {
                        id: titleField
                        visible: root.renaming
                        width: parent.width
                        font: title.font
                        color: Theme.text
                        selectionColor: Theme.accent
                        selectedTextColor: Theme.accentForeground
                        maximumLength: 100
                        clip: true
                        onAccepted: root.commitRename()
                        onActiveFocusChanged: if (!activeFocus) root.commitRename()
                        Keys.onEscapePressed: {
                            root.renaming = false
                            focus = false
                        }

                        // The field's rule, where the name will sit.
                        Rectangle {
                            anchors.top: parent.bottom
                            anchors.topMargin: 2
                            width: parent.width
                            height: Theme.ruleWidth
                            color: Theme.accent
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: "By " + Library.userName
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Theme.weightMedium
                    color: Theme.text
                }

                Text {
                    text: root.songsLabel(root.songCount)
                          + (!root.liked && root.ready && root.info.durationText
                             ? " · " + root.info.durationText : "")
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
                        enabled: root.songCount > 0
                        onClicked: Player.playModel(root.songs, 0)
                    }
                    ActionButton {
                        iconName: "shuffle"
                        text: "Shuffle"
                        enabled: root.songCount > 1
                        onClicked: {
                            Player.shuffle = true
                            Player.playModel(root.songs, Math.floor(Math.random() * root.songCount))
                        }
                    }
                    ActionButton {
                        visible: Downloads.available
                        iconName: "download"
                        text: "Download all"
                        enabled: root.songCount > 0
                        onClicked: root.downloadAll()
                    }
                    ActionButton {
                        id: moreButton
                        visible: !root.liked
                        iconName: "dots"
                        onClicked: playlistMenu.popup(moreButton, 0, moreButton.height + Theme.space1)
                    }
                }
            }
        }

        // — deleting, confirmed —
        Rectangle {
            visible: root.confirmingDelete && !root.missing
            width: parent.width
            height: confirmRow.implicitHeight + Theme.space4 * 2
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: Theme.accent

            Row {
                id: confirmRow
                anchors.left: parent.left
                anchors.leftMargin: Theme.space4
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space4

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Delete “" + (root.ready ? root.info.name : "") + "”? "
                          + (root.songCount > 0 ? "Its " + root.songsLabel(root.songCount) + " stay wherever else they are." : "")
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.text
                }
                ActionButton {
                    primary: true
                    text: "Delete"
                    onClicked: {
                        root.confirmingDelete = false
                        Library.deletePlaylist(root.playlistId)
                        root.deleted()
                    }
                }
                ActionButton {
                    text: "Cancel"
                    onClicked: root.confirmingDelete = false
                }
            }
        }

        Text {
            visible: !root.missing && root.ready && root.songCount === 0
            width: Math.min(parent.width, 640)
            text: root.liked
                  ? "Songs you like show up here. Press the heart on any song, or in the player bar."
                  : "Nothing here yet. Add songs from any song's menu (the three dots, or a right click): Add to playlist."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        TrackTable {
            visible: root.songCount > 0 && !root.missing
            width: parent.width
            model: root.songs
            playlistId: root.playlistId
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(root.songs, index) }
        }
    }
}
