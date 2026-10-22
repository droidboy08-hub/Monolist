import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The lyrics, set large. Synced lyrics follow the song: the line being sung in
// ink (with the red mark beside it, where `marked`), what has been sung faded,
// what is still to come half lit, the view keeping the line being sung near
// the top third. A click on a line plays from there. Scrolling away stops the
// following for a few seconds, or until SYNC.
Item {
    id: root

    readonly property bool synced: Lyrics.state === "synced"
    readonly property bool plain: Lyrics.state === "plain"
    readonly property int size: width >= 560 ? 36 : (width >= 400 ? 30 : 24)
    // The red mark beside the line being sung. Now Playing's colour field
    // has none: the line being sung is the one in full ink.
    property bool marked: true
    // Room for the red mark to the left of the lines.
    readonly property int gutter: marked ? 28 : 0
    // Lines centred across the pane rather than set from its left edge. Now
    // Playing's wide stage wants them in the middle of the space beside the
    // cover, not leaning against it (the owner's word: too close).
    property bool centered: false
    // How far down the pane the line being sung is held.
    property real readingLine: 0.36
    // What it is printed on and in: paper and the page's greys, or the tints
    // of a colour field (Now Playing). `paper` is what the lines fade into
    // at the top and bottom.
    property color paper: Theme.bg
    property color ink: Theme.text
    property color pastInk: Theme.neutral500
    property color nextInk: Theme.neutral600
    property color hoverInk: Theme.neutral700
    property bool following: true

    function reveal(animated) {
        if (!synced || !following)
            return
        // Laid out now rather than at the next frame, so the line's place is
        // known even in a window that is not drawing (behind others, or with
        // the screen off), where the lines would all still be at the top.
        column.forceLayout()
        var target = -scroller.topMargin
        var item = Lyrics.currentLine >= 0 ? lines.itemAt(Lyrics.currentLine) : null
        if (item)
            target = item.y + Math.min(item.height, root.size * 1.2) / 2 - scroller.height * root.readingLine
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
        topMargin: root.synced ? height * root.readingLine : 0
        bottomMargin: root.synced ? height * 0.6 : Theme.space8 * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onMovementStarted: if (root.synced) root.pauseFollowing()

        ScrollBar.vertical: MonoScrollBar { visible: root.plain && size < 1.0 }

        // The longest movement in the app, and the one nobody is waiting on:
        // it has to be slow enough to read through.
        NumberAnimation {
            id: glide
            target: scroller
            property: "contentY"
            duration: Theme.slow
            easing.type: Theme.enterCurve
        }

        Column {
            id: column
            width: scroller.width
            spacing: root.synced ? Math.round(root.size * 0.6) : 0

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
                        visible: line.current && root.marked
                        width: 10
                        height: 10
                        y: Math.round((Math.min(label.implicitHeight, label.font.pixelSize * 1.2) - height) / 2)
                        color: Theme.accent
                    }

                    Text {
                        id: label
                        x: root.centered ? Theme.space4 : root.gutter
                        width: root.centered ? parent.width - Theme.space4 * 2
                                             : parent.width - root.gutter - Theme.space4
                        horizontalAlignment: root.centered ? Text.AlignHCenter : Text.AlignLeft
                        visible: !line.gap || root.synced
                        text: line.gap ? "• • •" : line.text
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: root.synced ? root.size : Math.round(root.size * 0.62)
                        font.weight: root.synced ? Theme.weightBlack : Theme.weightMedium
                        font.letterSpacing: root.synced ? Theme.tracking(root.size, -0.02) : 0
                        lineHeight: root.synced ? 1.1 : 1.45
                        lineHeightMode: Text.ProportionalHeight
                        // What is still to come reads brighter than what
                        // has been sung.
                        color: !root.synced ? root.ink
                             : line.current ? root.ink
                             : lineHover.hovered ? root.hoverInk
                             : line.past ? root.pastInk
                             : root.nextInk

                        // The line lights as it is sung; the change has to be
                        // visible without flashing.
                        Behavior on color {
                            ColorAnimation { duration: Theme.normal }
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
        property color paper: Theme.bg
        readonly property color clear: Qt.rgba(paper.r, paper.g, paper.b, 0)
        width: parent.width
        height: Math.round(root.size * 1.4)
        // Solid for its first third: a line cut off by the pane's edge shows
        // only its descenders there, which read as stray marks under a soft
        // start, so they are covered before the fade begins.
        gradient: Gradient {
            GradientStop { position: 0; color: fade.downward ? fade.paper : fade.clear }
            GradientStop { position: 0.35; color: fade.downward ? fade.paper : Qt.rgba(fade.paper.r, fade.paper.g, fade.paper.b, 0.65) }
            GradientStop { position: 0.65; color: fade.downward ? Qt.rgba(fade.paper.r, fade.paper.g, fade.paper.b, 0.65) : fade.paper }
            GradientStop { position: 1; color: fade.downward ? fade.clear : fade.paper }
        }
    }
    Fade {
        visible: scroller.visible
        downward: true
        paper: root.paper
        anchors.top: parent.top
    }
    Fade {
        visible: scroller.visible
        downward: false
        paper: root.paper
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
        // Takes the press: a plain tap also reached the lyric line under the
        // button, and the song jumped to it.
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: root.follow()
        }
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
            horizontalAlignment: root.centered ? Text.AlignHCenter : Text.AlignLeft
            text: Lyrics.state === "instrumental" ? "Instrumental"
                : Lyrics.state === "none" ? "No lyrics for this song."
                : Lyrics.state === "error" ? "The lyrics could not be fetched."
                : Player.currentSourceId.length > 0 ? "Finding the lyrics…" : "Nothing playing."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: root.size
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(root.size, -0.02)
            color: root.nextInk
        }

        Text {
            visible: Lyrics.state === "none" || Lyrics.state === "error"
            x: root.centered ? Math.round((parent.width - width) / 2) : 0
            text: Lyrics.state === "error" ? "TRY AGAIN" : "SEARCH AGAIN"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: againHover.hovered ? Theme.accent700 : root.ink

            HoverHandler { id: againHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Lyrics.retry() }
        }
    }
}
