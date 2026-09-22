import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The lyrics, set large. Synced lyrics follow the song: the line being sung in
// ink with the red mark beside it, the rest in grey, the view keeping it a
// third of the way down. A click on a line plays from there. Scrolling away
// stops the following for a few seconds, or until SYNC.
Item {
    id: root

    readonly property bool synced: Lyrics.state === "synced"
    readonly property bool plain: Lyrics.state === "plain"
    readonly property int size: width >= 720 ? 36 : (width >= 480 ? 30 : 24)
    // Room for the red mark to the left of the lines.
    readonly property int gutter: 28
    property bool following: true

    function reveal(animated) {
        if (!synced || !following)
            return
        var target = -scroller.topMargin
        var item = Lyrics.currentLine >= 0 ? lines.itemAt(Lyrics.currentLine) : null
        if (item)
            target = item.y + Math.min(item.height, root.size * 1.2) / 2 - scroller.height * 0.36
        target = Math.max(-scroller.topMargin,
                          Math.min(target, scroller.contentHeight - scroller.height + scroller.bottomMargin))
        if (animated && scroller.visible) {
            glide.to = target
            glide.restart()
        } else {
            glide.stop()
            scroller.contentY = target
        }
    }

    function pauseFollowing() {
        glide.stop()
        following = false
        resume.restart()
    }

    function follow() {
        resume.stop()
        following = true
        reveal(true)
    }

    Connections {
        target: Lyrics
        function onCurrentLineChanged() { root.reveal(true) }
        function onStateChanged() {
            root.following = true
            // After the new lines are laid out.
            Qt.callLater(root.reveal, false)
            if (!root.synced)
                scroller.contentY = -scroller.topMargin
        }
    }
    onVisibleChanged: if (visible) Qt.callLater(reveal, false)
    onHeightChanged: Qt.callLater(reveal, false)

    Timer {
        id: resume
        interval: 5000
        onTriggered: root.follow()
    }

    Flickable {
        id: scroller
        anchors.fill: parent
        visible: root.synced || root.plain
        contentWidth: width
        contentHeight: column.height
        // Synced lines start at the reading line and can scroll up to it.
        topMargin: root.synced ? height * 0.36 : 0
        bottomMargin: root.synced ? height * 0.6 : Theme.space8 * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onMovementStarted: if (root.synced) root.pauseFollowing()

        ScrollBar.vertical: MonoScrollBar { visible: root.plain && size < 1.0 }

        NumberAnimation {
            id: glide
            target: scroller
            property: "contentY"
            duration: 520
            easing.type: Easing.OutCubic
        }

        Column {
            id: column
            width: scroller.width
            spacing: root.synced ? Math.round(root.size * 0.42) : 0

            Repeater {
                id: lines
                model: Lyrics.lines

                delegate: Item {
                    id: line

                    required property int index
                    required property string text
                    required property real timeMs

                    readonly property bool current: root.synced && index === Lyrics.currentLine
                    readonly property bool past: root.synced && Lyrics.currentLine >= 0 && index < Lyrics.currentLine
                    // An empty line: a pause in synced lyrics, a stanza break in plain ones.
                    readonly property bool gap: text.length === 0

                    width: column.width
                    height: gap && !root.synced ? Math.round(root.size * 0.7) : label.implicitHeight

                    Rectangle {
                        visible: line.current
                        width: 10
                        height: 10
                        y: Math.round((Math.min(label.implicitHeight, label.font.pixelSize * 1.2) - height) / 2)
                        color: Theme.accent
                    }

                    Text {
                        id: label
                        x: root.gutter
                        width: parent.width - root.gutter - Theme.space4
                        visible: !line.gap || root.synced
                        text: line.gap ? "• • •" : line.text
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: root.synced ? root.size : Math.round(root.size * 0.62)
                        font.weight: root.synced ? Theme.weightBlack : Theme.weightMedium
                        font.letterSpacing: root.synced ? Theme.tracking(root.size, -0.02) : 0
                        lineHeight: root.synced ? 1.05 : 1.45
                        lineHeightMode: Text.ProportionalHeight
                        // What is still to come reads a shade darker than
                        // what has been sung.
                        color: !root.synced ? Theme.text
                             : line.current ? Theme.text
                             : lineHover.hovered ? Theme.neutral700
                             : line.past ? Theme.neutral500
                             : Theme.neutral600

                        Behavior on color {
                            ColorAnimation { duration: 220 }
                        }
                    }

                    HoverHandler {
                        id: lineHover
                        enabled: root.synced
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: root.synced
                        onTapped: Lyrics.seekToLine(line.index)
                    }
                }
            }
        }
    }

    // Lines leave and arrive through a fade to paper, not a hard cut.
    component Fade: Rectangle {
        id: fade
        property bool downward: true   // paper at the top, clearing downwards
        readonly property color clear: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0)
        width: parent.width
        height: Math.round(root.size * 1.4)
        gradient: Gradient {
            GradientStop { position: 0; color: fade.downward ? Theme.bg : fade.clear }
            GradientStop { position: 1; color: fade.downward ? fade.clear : Theme.bg }
        }
    }
    Fade {
        visible: scroller.visible
        downward: true
        anchors.top: parent.top
    }
    Fade {
        visible: scroller.visible
        downward: false
        anchors.bottom: parent.bottom
    }

    // — back to the line being sung —
    Rectangle {
        visible: root.synced && !root.following
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space6
        width: syncLabel.implicitWidth + Theme.space4 * 2
        height: 32
        color: syncHover.hovered ? Theme.accent600 : Theme.accent

        Text {
            id: syncLabel
            anchors.centerIn: parent
            text: "SYNC"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: Theme.accentForeground
        }

        HoverHandler { id: syncHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.follow() }
    }

    // — no lines to show —
    Column {
        visible: !root.synced && !root.plain
        anchors.left: parent.left
        anchors.leftMargin: root.gutter
        anchors.right: parent.right
        y: Math.round(parent.height * 0.3)
        spacing: Theme.space4

        Text {
            width: parent.width
            text: Lyrics.state === "instrumental" ? "Instrumental"
                : Lyrics.state === "none" ? "No lyrics for this song."
                : Lyrics.state === "error" ? "The lyrics could not be fetched."
                : Player.currentSourceId.length > 0 ? "Finding the lyrics…" : "Nothing playing."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: root.size
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(root.size, -0.02)
            color: Theme.neutral500
        }

        Text {
            visible: Lyrics.state === "none" || Lyrics.state === "error"
            text: Lyrics.state === "error" ? "TRY AGAIN" : "SEARCH AGAIN"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: againHover.hovered ? Theme.accent700 : Theme.text

            HoverHandler { id: againHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Lyrics.retry() }
        }
    }
}
