import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// One of the user's playlists, or Liked songs: the same page as an album's,
// with the cover a mosaic of the songs' own, and the title theirs to change.
// Also Liked on YouTube Music, the account's liked songs as its last sync
// read them (AccountLibrary): the same page, read-only, since nothing here is
// ever written back to the account.
ScrollPage {
    id: root

    // "liked", "ytliked", or a playlist's id.
    property string key: ""
    // Set for a playlist just made, so its name is ready to be typed.
    property bool renameOnOpen: false
    // Set when Delete was chosen from the playlist's menu elsewhere, so the
    // page opens asking to confirm it.
    property bool confirmDeleteOnOpen: false
    signal renameStarted()
    signal deleteAsked()
    signal deleted()

    readonly property bool liked: key === "liked"
    // The account's, read-only: no renaming, reordering or deleting.
    readonly property bool account: key === "ytliked"
    readonly property bool own: !liked && !account
    readonly property int playlistId: own ? parseInt(key) : 0
    readonly property var info: Library.playlist
    readonly property bool ready: !own || (info.playlistId === playlistId)
    // Signed out, or the import turned off: nothing of the account is left.
    readonly property bool missing: account ? !AccountLibrary.shown : (own && ready && !info.exists)
    readonly property var songs: liked ? Library.liked : account ? AccountLibrary.liked : Library.playlistTracks
    readonly property int songCount: songs ? songs.count : 0
    readonly property bool wide: width >= 900

    property bool renaming: false
    property bool confirmingDelete: false

    contentHeight: column.implicitHeight

    onKeyChanged: {
        renaming = false
        confirmingDelete = false
        contentY = 0
    }
    onVisibleChanged: if (!visible) { renaming = false; confirmingDelete = false }

    // A new playlist opens with its name selected, the way a new file does.
    function startRenaming() {
        if (!own || missing)
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
    onReadyChanged: {
        if (renameOnOpen && ready)
            Qt.callLater(startRenaming)
        if (confirmDeleteOnOpen && ready)
            Qt.callLater(askDelete)
    }

    // Later rather than at once, so it lands after onKeyChanged, which
    // clears the question for the playlist it is leaving.
    function askDelete() {
        if (!own || missing || !confirmDeleteOnOpen)
            return
        confirmingDelete = true
        deleteAsked()
    }
    onConfirmDeleteOnOpenChanged: if (confirmDeleteOnOpen && ready) Qt.callLater(askDelete)

    function trackList() {
        return liked ? Library.likedTrackList() : account ? AccountLibrary.likedTrackList() : Library.playlistTrackList()
    }

    // In one call, video flags and all (Downloads.enqueueAll).
    function downloadAll() {
        Downloads.enqueueAll(trackList())
    }

    // Asked again whenever a download starts, ends or fails, and whenever
    // the playlist changes.
    // The account's list fills a few rows a frame: counted once for the
    // list (likedCount), not again at every few rows.
    readonly property var downloadCounts: Downloads.revision >= 0 && Library.revision >= 0
                                          && (root.account ? AccountLibrary.likedCount >= 0 : root.songCount >= 0)
                                          ? Downloads.downloadCounts(trackList()) : ({})

    function songsLabel(n) { return n + (n === 1 ? " song" : " songs") }

    // The playlist's own menu, as on its card and in the sidebar; here the
    // renaming and the question are this page's own.
    PlaylistMenu {
        id: playlistMenu
        onRenameRequested: root.startRenaming()
        onDeleteRequested: root.confirmingDelete = true
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
            text: root.account
                  ? "Your YouTube Music library is shown here while you are signed in and importing it is on "
                    + "(Settings, Connections)."
                  : "This playlist no longer exists."
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
                plate: root.liked || root.account ? "liked" : ""
                artworks: !root.own || !root.ready ? [] : root.info.artworks
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
                    text: root.account ? "PLAYLIST · YOUTUBE MUSIC" : "PLAYLIST"
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
                        text: root.liked ? "Liked songs" : root.account ? "Liked on YouTube Music"
                                                                    : (root.ready ? root.info.name : "")
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
                            enabled: root.own
                            cursorShape: Qt.IBeamCursor
                        }
                        TapHandler {
                            enabled: root.own
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
                    text: "By " + (root.account ? (Account.accountName.length > 0 ? Account.accountName : "your account")
                                                : Library.userName)
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Theme.weightMedium
                    color: Theme.text
                }

                Text {
                    text: root.songsLabel(root.account ? AccountLibrary.likedCount : root.songCount)
                          + (root.own && root.ready && root.info.durationText
                             ? " · " + root.info.durationText : "")
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }

                // Where the list came from, and that it is read, never
                // written: a like changed on YouTube Music shows at the next
                // sync, and nothing done here goes back.
                Text {
                    visible: root.account
                    width: parent.width
                    text: AccountLibrary.status
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
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
                        onClicked: {
                            // The whole list, not only the rows made so far.
                            if (root.account)
                                Player.playTracks(root.trackList(), 0, "playlist")
                            else
                                Player.playModel(root.songs, 0, "playlist")
                        }
                    }
                    ActionButton {
                        iconName: "shuffle"
                        text: "Shuffle"
                        enabled: root.songCount > 1
                        onClicked: {
                            Player.shuffle = true
                            if (root.account) {
                                const all = root.trackList()
                                Player.playTracks(all, Math.floor(Math.random() * all.length), "playlist")
                            } else {
                                Player.playModel(root.songs, Math.floor(Math.random() * root.songCount), "playlist")
                            }
                        }
                    }
                    DownloadAllButton {
                        visible: Downloads.available
                        counts: root.downloadCounts
                        enabled: root.songCount > 0
                        onDownloadAllRequested: root.downloadAll()
                        onRetryRequested: Downloads.retryFailed(root.trackList())
                    }
                    ActionButton {
                        id: moreButton
                        visible: !root.missing
                        iconName: "dots"
                        onClicked: playlistMenu.show(root.key, moreButton)
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
                  : root.account
                    ? "No liked songs read from YouTube Music yet. They are read a minute or two after you sign in, "
                      + "or with SYNC NOW in Settings, Connections."
                    : "Nothing here yet. Add songs from any song's menu (the three dots, or a right click): Add to playlist."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        // A playlist's songs are in the order the user gives them: dragged by
        // the grip, or moved from a row's menu or with Alt+Up and Alt+Down.
        // Liked songs keeps the order they were liked in.
        TrackTable {
            visible: root.songCount > 0 && !root.missing
            width: parent.width
            model: root.songs
            playlistId: root.playlistId
            reorderable: root.own
            flickable: root
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(root.songs, index, "playlist") }
        }
    }
}
