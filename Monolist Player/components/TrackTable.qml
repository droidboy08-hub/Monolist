import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Column {
    id: root

    property var model: null
    // Highlights a row by position, for rows without a source id. Rows with
    // one are matched to the playing song by identity.
    property int activeIndex: -1
    // A download control per row, for rows that have a source id to fetch.
    property bool showDownloads: false
    // The playlist this list is, so a row's menu can take it out; 0 otherwise.
    property int playlistId: 0
    // The rows can be put in another order (one of the user's playlists): a
    // grip takes the number's place under the pointer, and dragging it moves
    // the song; so do Alt+Up and Alt+Down once the grip has been pressed, and
    // Move up and Move down in the row's menu.
    property bool reorderable: false
    // The list is the history, so a row's menu can take a song out of it.
    property bool history: false
    // The page this list scrolls in: a drag held near its top or bottom edge
    // scrolls it, so a song can be carried further than the window shows.
    // Found by itself when not given.
    property Flickable flickable: null
    // The list's filter and order (TrackFilterModel), when its headers put
    // it in order: a click sorts by that column, a second the other way, a
    // third back to the list's own order.
    property TrackFilterModel sortModel: null
    // Each song's cover at the head of its row (TrackCover), on every list,
    // an album's too (the owner's choice); off, the rows are numbered.
    property bool showArtwork: true
    // And only where the table is wide enough that the title keeps its
    // room: narrower, the rows are numbered.
    readonly property bool coversShown: showArtwork && width >= 420
    signal trackActivated(int index)

    function sortBy(key) {
        if (!sortModel)
            return
        if (sortModel.sortKey !== key) {
            sortModel.descending = false
            sortModel.sortKey = key
        } else if (!sortModel.descending) {
            sortModel.descending = true
        } else {
            sortModel.descending = false
            sortModel.sortKey = ""
        }
    }

    // A column's heading, which puts the list in its order where it can.
    component HeadLabel: Text {
        id: head
        property string key: ""
        readonly property bool sorted: root.sortModel !== null && root.sortModel.sortKey === key
        text: label + (sorted ? (root.sortModel.descending ? "  ↓" : "  ↑") : "")
        property string label: ""
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.letterSpacing: Theme.tracking(11, 0.08)
        font.weight: sorted ? Font.Bold : Font.Normal
        color: sorted || headHover.hovered ? Theme.text : Theme.neutral700

        HoverHandler {
            id: headHover
            enabled: root.sortModel !== null
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            enabled: root.sortModel !== null
            onTapped: root.sortBy(head.key)
        }
    }

    // Only the rows near what the page shows are made (ROADMAP F28): a
    // thousand-song playlist would otherwise hold a thousand rows, each with
    // its buttons, made before the page could show. The rows the list has
    // as a window: from `windowTop`, `windowHeight` tall, a screen beyond
    // the page's view each way, and always the row being carried.
    readonly property Flickable page: flickable ? flickable : enclosingPage()
    property real windowTop: 0
    property real windowHeight: 0

    function enclosingPage() {
        for (let p = root.parent; p; p = p.parent) {
            if (p instanceof Flickable)
                return p
        }
        return null
    }

    function updateWindow() {
        const total = rows.count * rowHeight
        // Not shown, no rows: a table on a page not in view, or on a tab not
        // picked, costs nothing when its list changes (the History a play
        // rebuilds, ROADMAP F35).
        if (!visible) {
            windowTop = 0
            windowHeight = 0
        } else if (!page) {
            windowTop = 0
            windowHeight = total
        } else {
            const at = rowsArea.mapToItem(page.contentItem, 0, 0).y
            const margin = Math.max(page.height, 400)
            let top = Math.max(0, page.contentY - at - margin)
            let bottom = Math.min(total, page.contentY + page.height - at + margin)
            if (dragFrom >= 0) {
                top = Math.min(top, dragFrom * rowHeight)
                bottom = Math.max(bottom, (dragFrom + 1) * rowHeight)
            }
            top = Math.floor(Math.min(top, total) / rowHeight) * rowHeight
            windowTop = top
            windowHeight = Math.max(0, bottom - top)
        }
        rows.contentY = windowTop
    }

    Connections {
        target: root.page
        function onContentYChanged() { root.updateWindow() }
        function onHeightChanged() { root.updateWindow() }
        // Something above the list grew or shrank, which moves it on the page.
        function onContentHeightChanged() { root.updateWindow() }
    }
    onYChanged: updateWindow()
    onVisibleChanged: updateWindow()
    onDragFromChanged: updateWindow()
    Component.onCompleted: updateWindow()

    // Column visibility follows the window: metadata drops before the title does.
    readonly property bool showAlbum: width >= 900
    readonly property bool showArtist: width >= 700
    readonly property int timeWidth: 64
    readonly property int indexWidth: 48
    readonly property int moreWidth: 36
    readonly property int likeWidth: 36
    readonly property int downloadWidth: showDownloads && Downloads.available ? 40 : 0
    readonly property int coverSize: 40
    // The cover and the gap after it, before the title.
    readonly property int coverWidth: coversShown ? coverSize + Theme.space3 : 0
    readonly property int titleX: indexWidth + coverWidth
    readonly property int freeWidth: width - titleX - timeWidth - likeWidth - downloadWidth - moreWidth
    readonly property int albumColumnWidth: showAlbum ? Math.round(freeWidth * 0.30) : 0
    readonly property int artistColumnWidth: showArtist ? Math.round(freeWidth * 0.28) : 0
    readonly property int titleColumnWidth: freeWidth - albumColumnWidth - artistColumnWidth
    readonly property int headHeight: 32
    // As tall as a queue row where there are covers, so the two lists keep
    // one rhythm; a row of type alone keeps to the type.
    readonly property int rowHeight: coversShown ? 56 : 40

    // — a drag under way —
    // The row held (-1 when none), the gap between rows it would drop into
    // (0 before the first, count after the last), how far it has been
    // carried, and where it was picked up; the pointer in window coordinates,
    // for scrolling while it is held still at an edge; and whether it has
    // been carried at all, rather than the grip only pressed.
    property int dragFrom: -1
    property int dragGap: -1
    property real dragOffset: 0
    property real grabY: 0
    property point dragPointer: Qt.point(0, 0)
    property bool dragMoved: false
    // Where the row held would end up, as a row number.
    readonly property int dropIndex: dragGap > dragFrom ? dragGap - 1 : dragGap
    readonly property bool dropMoves: dragFrom >= 0 && dropIndex !== dragFrom

    function primaryArtistOf(index) {
        if (!model || typeof model.get !== "function")
            return ""
        const map = model.get(index)
        return map && map.primaryArtist ? map.primaryArtist : ""
    }

    function trackOf(row) {
        return {
            sourceId: row.sourceId,
            title: row.title,
            artist: row.artist,
            album: row.album,
            artwork: row.artwork,
            durationMs: row.durationMs,
            // Carried into a like, a playlist and the queue, so a music video
            // keeps its picture wherever it is played from next.
            isVideo: row.isVideo,
            // And into the queue and the menu, so the names still open
            // their pages from the player bar and "Go to artist".
            credits: row.credits,
            albumId: row.albumId,
            // The first credit alone, which Last.fm is sent: read from the
            // row's map, since not every list has the role to require.
            primaryArtist: root.primaryArtistOf(row.index)
        }
    }

    // The window's one TrackMenu (Menus), told where the row is. `anchor`,
    // for a menu asked for from the keyboard, opens it under the row rather
    // than at the pointer.
    function openMenu(row, anchor) {
        Menus.openTrack(trackOf(row), {
            playlistId: root.playlistId,
            entryId: row.entryId,
            index: root.reorderable ? row.index : -1,
            count: root.reorderable ? rows.count : 0,
            history: root.history,
            anchor: anchor ? anchor : null
        })
    }

    // One of the user's playlists: the song to row `to`, kept.
    function moveRow(from, to) {
        if (!reorderable || playlistId <= 0 || !model || from === to
                || from < 0 || to < 0 || from >= rows.count || to >= rows.count)
            return
        Library.movePlaylistEntry(playlistId, model.get(from).entryId, to)
    }

    function beginDrag(index, sceneX, sceneY) {
        grabY = mapFromItem(null, sceneX, sceneY).y
        dragOffset = 0
        dragMoved = false
        dragFrom = index
        dragGap = index
        dragPointer = Qt.point(sceneX, sceneY)
    }

    // The place is the row slot nearest the middle of the row carried: once
    // that is past the middle of the next row, the song goes beyond it. The
    // rows between make room for it there (shiftFor), so the gap it would
    // drop into is open, and nothing is drawn across the row it carries.
    function updateDrag(sceneX, sceneY) {
        if (dragFrom < 0)
            return
        dragPointer = Qt.point(sceneX, sceneY)
        const y = mapFromItem(null, sceneX, sceneY).y
        dragOffset = y - grabY
        if (Math.abs(dragOffset) >= Theme.space1)
            dragMoved = true
        const target = Math.max(0, Math.min(rows.count - 1, Math.round(dragFrom + dragOffset / rowHeight)))
        dragGap = target > dragFrom ? target + 1 : target
    }

    // How far a row that is not held moves aside, while one is carried
    // across it: up into the slot the held one left, or down out of the
    // slot it is going to.
    function shiftFor(index) {
        if (dragFrom < 0 || index === dragFrom)
            return 0
        if (dropIndex > dragFrom && index > dragFrom && index <= dropIndex)
            return -rowHeight
        if (dropIndex < dragFrom && index >= dropIndex && index < dragFrom)
            return rowHeight
        return 0
    }

    function endDrag(drop) {
        const from = dragFrom
        const to = dropIndex
        const moves = dropMoves
        dragFrom = -1
        dragGap = -1
        dragOffset = 0
        if (drop && moves)
            moveRow(from, to)
    }

    // A drag held within this distance of the page's top or bottom edge
    // scrolls it, faster the closer it is.
    Timer {
        interval: 16
        repeat: true
        running: root.dragFrom >= 0 && root.flickable !== null
        onTriggered: {
            const page = root.flickable
            const at = page.mapFromItem(null, root.dragPointer.x, root.dragPointer.y).y
            const edge = 56
            let step = 0
            if (at < edge)
                step = -Math.ceil((edge - Math.max(0, at)) / edge * 14)
            else if (at > page.height - edge)
                step = Math.ceil((Math.min(page.height, at) - (page.height - edge)) / edge * 14)
            if (step === 0)
                return
            const most = Math.max(0, page.contentHeight - page.height)
            const target = Math.max(0, Math.min(most, page.contentY + step))
            if (target === page.contentY)
                return
            page.contentY = target
            root.updateDrag(root.dragPointer.x, root.dragPointer.y)
        }
    }

    spacing: 0

    // — head —
    Item {
        width: root.width
        height: root.headHeight

        Text {
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            text: "#"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
        HeadLabel {
            x: root.titleX
            anchors.verticalCenter: parent.verticalCenter
            label: "TITLE"
            key: "title"
        }
        HeadLabel {
            visible: root.showArtist
            x: root.titleX + root.titleColumnWidth
            anchors.verticalCenter: parent.verticalCenter
            label: "ARTIST"
            key: "artist"
        }
        HeadLabel {
            visible: root.showAlbum
            x: root.titleX + root.titleColumnWidth + root.artistColumnWidth
            anchors.verticalCenter: parent.verticalCenter
            label: "ALBUM"
            key: "album"
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: root.moreWidth
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1

            Text {
                visible: root.sortModel !== null && root.sortModel.sortKey === "duration"
                anchors.verticalCenter: parent.verticalCenter
                text: root.sortModel !== null && root.sortModel.descending ? "↓" : "↑"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                color: Theme.text
            }
            Icon {
                name: "clock"
                width: 14
                height: 14
                anchors.verticalCenter: parent.verticalCenter
                color: durationHover.hovered || (root.sortModel !== null && root.sortModel.sortKey === "duration")
                       ? Theme.text : Theme.neutral700

                HoverHandler {
                    id: durationHover
                    enabled: root.sortModel !== null
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    enabled: root.sortModel !== null
                    onTapped: root.sortBy("duration")
                }
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    // — rows —
    // An item as tall as every row, holding a list the height of the window
    // over it, which the page's scroll moves along (updateWindow).
    Item {
        id: rowsArea
        width: root.width
        height: rows.count * root.rowHeight

        ListView {
            id: rows
            model: root.model
            y: root.windowTop
            width: parent.width
            height: root.windowHeight
            interactive: false
            currentIndex: -1
            // The row carried is drawn over the rows it passes, out of the window
            // if it goes there.
            clip: false
            onCountChanged: Qt.callLater(root.updateWindow)

            delegate: Item {
                id: row

                required property int index
                required property string title
                required property string artist
                required property string album
                required property string durationText
                required property string sourceId
                required property string artwork
                required property real durationMs
                required property int entryId
                required property bool isVideo
                // The artist line with each name's page, and the album's page,
                // where the list kept them (see ArtistLine).
                required property var credits
                required property string albumId

                width: root.width
                height: root.rowHeight
                readonly property bool isActive: sourceId.length > 0 ? sourceId === Player.currentSourceId
                                                                     : index === root.activeIndex
                readonly property bool liked: Library.revision >= 0 && Library.isLiked(sourceId)
                readonly property bool held: root.dragFrom === index
                // The grip shows under the pointer, and stays while the row has
                // the keyboard, so Alt+Up and Alt+Down are seen to apply to it.
                readonly property bool gripShown: root.reorderable && (rowHover.hovered || activeFocus || held)

                // Carried with the pointer, over the rows it passes, which step
                // aside to open the gap it would drop into: where the song goes
                // is the one empty slot in the list.
                z: held ? 2 : 0
                transform: Translate { y: row.held ? root.dragOffset : root.shiftFor(row.index) }

                // Moving a song from the keyboard: Alt+Up and Alt+Down, once the
                // grip has been pressed (or the row moved) gives the row the
                // keyboard. The menu key opens its menu under it; Esc lets go.
                Keys.onPressed: function(event) {
                    if (root.reorderable && (event.modifiers & Qt.AltModifier)
                            && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                        event.accepted = true
                        root.moveRow(row.index, row.index + (event.key === Qt.Key_Up ? -1 : 1))
                    } else if (event.key === Qt.Key_Menu
                               || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                        event.accepted = true
                        root.openMenu(row, row)
                    } else if (event.key === Qt.Key_Escape) {
                        event.accepted = true
                        row.focus = false
                    }
                }

                // Hover appears at once — that is what makes a list feel quick —
                // and leaves over a moment, so dragging down the rows does not
                // flicker at every boundary. A row being carried is a plate of
                // paper in an ink frame, over the rows it passes.
                Rectangle {
                    anchors.fill: parent
                    color: row.held ? Theme.bg : rowHover.hovered ? Theme.rowHover : "transparent"
                    border.width: row.held ? Theme.ruleWidth : 0
                    border.color: Theme.text

                    Behavior on color {
                        enabled: !rowHover.hovered && !row.held
                        ColorAnimation { duration: Theme.quick }
                    }
                }

                // The row that has the keyboard: a 2px rule down its left edge,
                // the width of every other focus mark in the app.
                Rectangle {
                    visible: row.activeFocus && !row.held
                    width: Theme.ruleWidth
                    height: parent.height
                    color: Theme.accent
                }

                // The number; for the song playing, in red, and where there
                // is no cover to carry its bars, the bars in its place.
                Text {
                    x: 0
                    width: root.indexWidth
                    visible: !row.gripShown && !(row.isActive && !root.coversShown)
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(row.index + 1)
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: row.isActive ? Theme.accent700 : Theme.text
                }
                PlayingBars {
                    visible: !row.gripShown && row.isActive && !root.coversShown
                    x: 1
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 11
                    color: Theme.accent700
                    running: Player.playing
                    covered: Nav.pagesCovered
                }

                // The grip, in the number's place. Pressed, it holds the row;
                // taken rather than stolen, so the page does not scroll instead.
                Item {
                    visible: root.reorderable
                    width: root.indexWidth
                    height: parent.height

                    Icon {
                        visible: row.gripShown
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        name: "grip"
                        color: grip.containsMouse || grip.pressed ? Theme.text : Theme.neutral700
                    }

                    MouseArea {
                        id: grip
                        // The glyph's own column, not the whole number cell, so a
                        // press beside it still plays the song.
                        width: 24
                        height: parent.height
                        enabled: root.reorderable
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        onPressed: function(mouse) {
                            row.forceActiveFocus()
                            const at = mapToItem(null, mouse.x, mouse.y)
                            root.beginDrag(row.index, at.x, at.y)
                        }
                        onPositionChanged: function(mouse) {
                            const at = mapToItem(null, mouse.x, mouse.y)
                            root.updateDrag(at.x, at.y)
                        }
                        // A press alone gives the row the keyboard, for Alt+Up and
                        // Alt+Down; a drag was the mouse's move, and leaves no red
                        // focus mark behind on the row it moved.
                        onReleased: {
                            if (root.dragMoved)
                                row.focus = false
                            root.endDrag(true)
                        }
                        onCanceled: root.endDrag(false)
                    }
                }

                // Not shown, it asks for no picture.
                TrackCover {
                    visible: root.coversShown
                    x: root.indexWidth
                    width: root.coverSize
                    height: root.coverSize
                    anchors.verticalCenter: parent.verticalCenter
                    source: root.coversShown ? row.artwork : ""
                    sourceId: root.coversShown ? row.sourceId : ""
                    covered: Nav.pagesCovered
                    hovered: rowHover.hovered && !row.held
                    active: row.isActive
                    onPlayRequested: root.trackActivated(row.index)
                }

                // The title; and where the window is too narrow for the
                // artist's column, the artist under it.
                Column {
                    x: root.titleX
                    width: Math.max(0, root.titleColumnWidth - Theme.space4)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        width: parent.width
                        text: row.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: row.isActive ? Font.Bold : Theme.weightRegular
                        color: row.isActive ? Theme.accent700 : Theme.text
                    }
                    ArtistLine {
                        visible: !root.showArtist && row.artist.length > 0
                        width: parent.width
                        artist: row.artist
                        credits: row.credits
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                }

                // Each name opens its artist's page; a click anywhere else in
                // the row still plays it.
                ArtistLine {
                    visible: root.showArtist
                    x: root.titleX + root.titleColumnWidth
                    width: Math.max(0, root.artistColumnWidth - Theme.space4)
                    anchors.verticalCenter: parent.verticalCenter
                    artist: row.artist
                    credits: row.credits
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    font.weight: row.isActive ? Font.Bold : Theme.weightRegular
                    color: row.isActive ? Theme.accent700 : Theme.text
                }

                // And the album its page, where the list knows which it is.
                ArtistLine {
                    visible: root.showAlbum
                    x: root.titleX + root.titleColumnWidth + root.artistColumnWidth
                    width: Math.max(0, root.albumColumnWidth - Theme.space4)
                    anchors.verticalCenter: parent.verticalCenter
                    artist: row.album
                    opens: "page"
                    pageId: row.albumId
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.neutral700
                }

                // Buttons, so their clicks are not also taken as a tap on the row.
                // The heart stays when the song is liked, and shows on hover.
                LikeButton {
                    visible: row.sourceId.length > 0 && (row.liked || rowHover.hovered)
                    x: root.width - root.moreWidth - root.timeWidth - root.downloadWidth - root.likeWidth
                       + (root.likeWidth - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    side: 30
                    liked: row.liked
                    iconSize: 15
                    onClicked: Library.setLiked(root.trackOf(row), !row.liked)
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: row.liked ? "Remove from Liked songs" : "Add to Liked songs"
                }

                DownloadButton {
                    visible: root.downloadWidth > 0 && row.sourceId.length > 0
                    x: root.width - root.moreWidth - root.timeWidth - root.downloadWidth
                       + (root.downloadWidth - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    side: 30
                    iconSize: 15
                    videoId: row.sourceId
                    title: row.title
                    artist: row.artist
                    artwork: row.artwork
                    durationMs: row.durationMs
                    isVideo: row.isVideo
                    album: row.album
                }

                // Blank rather than "0:00" where the list gave no length, as an
                // artist's top songs do not.
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: root.moreWidth
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.durationMs > 0 ? row.durationText : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: row.isActive ? Theme.accent700 : Theme.text
                }

                IconButton {
                    visible: rowHover.hovered
                    x: root.width - root.moreWidth + (root.moreWidth - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    side: 30
                    iconName: "dots"
                    iconSize: 16
                    iconColor: Theme.neutral700
                    onClicked: root.openMenu(row, null)
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.hairline
                }

                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.trackActivated(row.index) }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.openMenu(row, null)
                }
            }
        }
    }
}
