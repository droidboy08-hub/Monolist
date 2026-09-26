import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The play queue, docked at the right. What has already played is left out:
// the list opens on the song playing now, then what will play, in order, with
// the point where autoplay's radio takes over marked.
Rectangle {
    id: root

    // Off where the panel sits inside another view that has its own.
    property bool showHead: true
    property bool showRule: true
    signal closeRequested()

    color: Theme.bg

    // — a drag under way —
    // What is still to come can be put in another order: a grip at the row's
    // left edge under the pointer, dragged; or Alt+Up and Alt+Down once the
    // grip has been pressed; or Move up and Move down in the row's menu.
    // The row held (-1 when none), the gap it would drop into (a row number:
    // before that row, or the count for after the last), how far the row has
    // been carried, where it was picked up and where its row began in the
    // list's content, the pointer in the window, and whether it has been
    // carried at all rather than the grip only pressed.
    property int dragFrom: -1
    property int dragGap: -1
    property real dragOffset: 0
    property real grabY: 0
    property real heldTop: 0
    property point dragPointer: Qt.point(0, 0)
    property bool dragMoved: false
    readonly property int firstUpcoming: Player.queue.currentIndex + 1
    readonly property int dropIndex: dragGap > dragFrom ? dragGap - 1 : dragGap
    readonly property bool dropMoves: dragFrom >= 0 && dropIndex !== dragFrom
    readonly property int rowHeight: 56

    function openMenu(index, anchor) {
        Menus.openTrack(Player.queue.get(index), { queueIndex: index, anchor: anchor ? anchor : null })
    }

    function moveEntry(from, to) {
        if (from >= root.firstUpcoming && to >= root.firstUpcoming && to < Player.queue.count)
            Player.moveInQueue(from, to)
    }

    // Where a row's own part starts in the list's content, under whatever
    // caption heads it; NaN for a row the list has not made.
    function rowTopOf(index) {
        const item = list.itemAtIndex(index)
        return item ? item.y + item.height - rowHeight : NaN
    }

    function beginDrag(index, sceneX, sceneY) {
        grabY = list.contentItem.mapFromItem(null, sceneX, sceneY).y
        heldTop = rowTopOf(index)
        dragOffset = 0
        dragMoved = false
        dragFrom = index
        dragGap = index
        updateDrag(sceneX, sceneY)
    }

    // The place is the entry under the middle of the row carried, its
    // caption included; never among what has played, nor before the song
    // playing. The rows between make room for it there (shiftFor), so the
    // gap is open and nothing is drawn across the row carried.
    function updateDrag(sceneX, sceneY) {
        if (dragFrom < 0)
            return
        dragPointer = Qt.point(sceneX, sceneY)
        const y = list.contentItem.mapFromItem(null, sceneX, sceneY).y
        dragOffset = y - grabY
        if (Math.abs(dragOffset) >= Theme.space1)
            dragMoved = true
        const middle = heldTop + rowHeight / 2 + dragOffset
        const count = Player.queue.count
        let target = list.indexAt(1, middle)
        if (target < 0) {
            const first = list.itemAtIndex(root.firstUpcoming)
            target = first && middle < first.y ? root.firstUpcoming : count - 1
        }
        target = Math.max(root.firstUpcoming, Math.min(count - 1, target))
        dragGap = target > dragFrom ? target + 1 : target
    }

    // How far a row that is not held moves aside while one is carried
    // across it: into the slot of the row before it (up, into the one the
    // held row left) or after it (down, out of the one it is going to).
    // Captions stay where they are; a row steps to the next row's slot.
    function shiftFor(index) {
        if (dragFrom < 0 || index === dragFrom)
            return 0
        let other = -1
        if (dropIndex > dragFrom && index > dragFrom && index <= dropIndex)
            other = index - 1
        else if (dropIndex < dragFrom && index >= dropIndex && index < dragFrom)
            other = index + 1
        if (other < 0)
            return 0
        const from = rowTopOf(index)
        const to = rowTopOf(other)
        if (isNaN(from) || isNaN(to))
            return other < index ? -rowHeight : rowHeight
        return to - from
    }

    function endDrag(drop) {
        const from = dragFrom
        const to = dropIndex
        const moves = dropMoves
        dragFrom = -1
        dragGap = -1
        dragOffset = 0
        if (drop && moves)
            moveEntry(from, to)
    }

    component Caption: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.12)
        color: Theme.neutral700
    }

    // — head, level with the top bar —
    Item {
        id: head
        visible: root.showHead
        width: parent.width
        height: visible ? 64 : 0

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            text: "Queue"
            font.family: Theme.fontFamily
            font.pixelSize: 20
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(20, -0.01)
            color: Theme.text
        }

        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            iconName: "x"
            iconColor: Theme.neutral700
            iconSize: 14
            onClicked: root.closeRequested()
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    ListView {
        id: list
        anchors.top: head.bottom
        anchors.bottom: foot.top
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Player.queue

        ScrollBar.vertical: MonoScrollBar { width: 8 }
        SmoothWheel { flickable: list }

        // A drag held near the list's top or bottom edge scrolls it.
        Timer {
            interval: 16
            repeat: true
            running: root.dragFrom >= 0
            onTriggered: {
                const at = list.mapFromItem(null, root.dragPointer.x, root.dragPointer.y).y
                const edge = 48
                let step = 0
                if (at < edge)
                    step = -Math.ceil((edge - Math.max(0, at)) / edge * 12)
                else if (at > list.height - edge)
                    step = Math.ceil((Math.min(list.height, at) - (list.height - edge)) / edge * 12)
                if (step === 0)
                    return
                const most = Math.max(0, list.contentHeight - list.height)
                const target = Math.max(0, Math.min(most, list.contentY + step))
                if (target === list.contentY)
                    return
                list.contentY = target
                root.updateDrag(root.dragPointer.x, root.dragPointer.y)
            }
        }

        delegate: Column {
            id: entry

            required property int index
            required property string title
            required property string artist
            required property string artwork
            required property string durationText
            required property real durationMs
            required property bool isCurrent
            required property bool isPast
            required property var credits

            readonly property bool upcoming: !isCurrent && !isPast
            readonly property bool held: root.dragFrom === index
            readonly property bool gripShown: upcoming && (entryHover.hovered || activeFocus || held)

            width: ListView.view ? ListView.view.width : 0
            visible: !isPast
            height: visible ? implicitHeight : 0
            z: held ? 2 : 0

            // From the keyboard, once the grip has been pressed: Alt+Up and
            // Alt+Down move the song, the menu key opens its menu, Esc lets go.
            Keys.onPressed: function(event) {
                if (entry.upcoming && (event.modifiers & Qt.AltModifier)
                        && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    event.accepted = true
                    root.moveEntry(entry.index, entry.index + (event.key === Qt.Key_Up ? -1 : 1))
                } else if (event.key === Qt.Key_Menu
                           || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                    event.accepted = true
                    root.openMenu(entry.index, rowPart)
                } else if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    entry.focus = false
                }
            }

            Caption {
                visible: entry.isCurrent
                leftPadding: Theme.space6
                topPadding: Theme.space6
                bottomPadding: Theme.space2
                text: "NOW PLAYING"
            }
            Caption {
                visible: entry.index === Player.queue.currentIndex + 1
                leftPadding: Theme.space6
                topPadding: Theme.space6
                bottomPadding: Theme.space2
                text: "UP NEXT · " + Player.queue.upcomingCount
            }
            Caption {
                visible: entry.index === Player.queue.radioStartIndex
                leftPadding: Theme.space6
                topPadding: entry.index === Player.queue.currentIndex + 1 ? 0 : Theme.space6
                bottomPadding: Theme.space2
                text: "AUTOPLAY · SONGS LIKE WHAT YOU PLAYED"
                color: Theme.accent700
            }

            Item {
                id: rowPart
                width: entry.width
                height: root.rowHeight
                // Carried with the pointer; the caption above it stays. The
                // rows it passes step aside to open the gap it would go into.
                transform: Translate { y: entry.held ? root.dragOffset : root.shiftFor(entry.index) }

                // A row being carried is a plate of paper in an ink frame.
                Rectangle {
                    anchors.fill: parent
                    color: entry.held ? Theme.bg
                         : entryHover.hovered && !entry.isCurrent ? Theme.rowHover : "transparent"
                    border.width: entry.held ? Theme.ruleWidth : 0
                    border.color: Theme.text

                    Behavior on color {
                        enabled: !entryHover.hovered && !entry.held
                        ColorAnimation { duration: Theme.quick }
                    }
                }

                // The row that has the keyboard.
                Rectangle {
                    visible: entry.activeFocus && !entry.held
                    width: Theme.ruleWidth
                    height: parent.height
                    color: Theme.accent
                }

                // The grip, in the margin at the row's left edge. Taken
                // rather than stolen, so the list does not scroll instead.
                Icon {
                    visible: entry.gripShown
                    x: (Theme.space6 - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    name: "grip"
                    color: grip.containsMouse || grip.pressed ? Theme.text : Theme.neutral700
                }
                MouseArea {
                    id: grip
                    enabled: entry.upcoming
                    width: Theme.space6
                    height: parent.height
                    hoverEnabled: true
                    preventStealing: true
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    onPressed: function(mouse) {
                        entry.forceActiveFocus()
                        const at = mapToItem(null, mouse.x, mouse.y)
                        root.beginDrag(entry.index, at.x, at.y)
                    }
                    onPositionChanged: function(mouse) {
                        const at = mapToItem(null, mouse.x, mouse.y)
                        root.updateDrag(at.x, at.y)
                    }
                    // As in a track table: a press alone keeps the keyboard
                    // for Alt+Up and Alt+Down, a drag leaves no focus mark.
                    onReleased: {
                        if (root.dragMoved)
                            entry.focus = false
                        root.endDrag(true)
                    }
                    onCanceled: root.endDrag(false)
                }

                Artwork {
                    id: art
                    x: Theme.space6
                    width: 40
                    height: 40
                    anchors.verticalCenter: parent.verticalCenter
                    placeholder: ""
                    source: entry.artwork
                }

                Column {
                    anchors.left: art.right
                    anchors.leftMargin: Theme.space3
                    anchors.right: tail.left
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        width: parent.width
                        text: entry.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: entry.isCurrent ? Font.Bold : Theme.weightRegular
                        color: entry.isCurrent ? Theme.accent700 : Theme.text
                    }
                    ArtistLine {
                        width: parent.width
                        artist: entry.artist
                        credits: entry.credits
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                }

                // The length at rest; under the pointer, the row's menu and,
                // for what is still to come, taking it out.
                Item {
                    id: tail
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 64
                    height: 30

                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !entryHover.hovered
                        text: entry.durationMs > 0 ? entry.durationText : ""
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: entry.isCurrent ? Theme.accent700 : Theme.neutral700
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        visible: entryHover.hovered
                        spacing: Theme.space1

                        IconButton {
                            side: 30
                            iconName: "dots"
                            iconColor: Theme.neutral700
                            iconSize: 15
                            onClicked: root.openMenu(entry.index, null)
                        }
                        IconButton {
                            visible: !entry.isCurrent
                            side: 30
                            iconName: "x"
                            iconColor: Theme.neutral700
                            iconSize: 12
                            onClicked: Player.removeFromQueue(entry.index)
                        }
                    }
                }

                HoverHandler { id: entryHover; cursorShape: entry.isCurrent ? Qt.ArrowCursor : Qt.PointingHandCursor }
                TapHandler {
                    enabled: !entry.isCurrent
                    onTapped: Player.playIndex(entry.index)
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.openMenu(entry.index, null)
                }
            }
        }
    }

    Text {
        anchors.centerIn: list
        visible: Player.queue.count === 0
        width: list.width - Theme.space6 * 2
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "Nothing queued. Play something from Search, Home or your Downloads."
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // — foot: autoplay and clearing —
    Item {
        id: foot
        anchors.bottom: parent.bottom
        width: parent.width
        height: 56

        Rectangle {
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            Rectangle {
                width: 18
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                color: Player.autoplay ? Theme.accent : "transparent"
                border.width: Theme.ruleWidth
                border.color: Player.autoplay ? Theme.accent : Theme.text

                Icon {
                    anchors.centerIn: parent
                    width: 12
                    height: 12
                    name: "check"
                    thickness: 3
                    visible: Player.autoplay
                    color: Theme.accentForeground
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Autoplay similar songs"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.text
            }

            TapHandler { onTapped: Player.autoplay = !Player.autoplay }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            visible: Player.queue.upcomingCount > 0
            text: "CLEAR"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: clearHover.hovered ? Theme.accent700 : Theme.neutral700

            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Player.clearUpcoming() }
        }
    }

    // The system's divider: a 2px rule down the left edge.
    Rectangle {
        visible: root.showRule
        width: Theme.ruleWidth
        height: parent.height
        color: Theme.divider
    }
}
