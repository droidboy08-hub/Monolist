import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The video, full screen.
//
// The one place the page is ink rather than paper. A moving picture set on
// paper reads as a photograph pasted onto the page, and the margins either
// side of it as bars; on ink the margins are the frame, and the picture is
// the page. The strip along the top and the player bar stay while the pointer
// moves, and fade once it has been still a moment, taking the pointer with
// them; moving it brings everything back. Esc, F, a double-click or the
// strip's own button leave.
Rectangle {
    id: root

    // Full screen is on, and the picture's place is here.
    property bool active: false
    // The player bar, which stands over the picture, is under the pointer.
    property bool barHovered: false
    signal leaveRequested()

    // Everything but the picture is showing.
    readonly property bool chromeShown: pointerMoved || barHovered || stripHover.hovered
    readonly property bool videoShowing: Player.videoPlaying && surface.showing
    readonly property string artwork: Player.currentTrack.artwork !== undefined
                                      ? Player.currentTrack.artwork : ""

    // Kept apart from the timer's own `running`, which restart() turns off
    // and on again: read directly, every nudge would hide the chrome for an
    // instant and start its fade.
    property bool pointerMoved: false
    // Where the pointer was when last seen. Qt Quick sends a hover again
    // after every frame, and a playing video is a new frame dozens of times a
    // second, so only a real change of place counts as the pointer moving.
    property point lastPointer: Qt.point(-1, -1)

    // The pointer moved: show everything, and start counting again.
    function poke() {
        pointerMoved = true
        stillness.restart()
    }

    // Full screen is ink in either appearance.
    color: Theme.ink
    visible: active
    onActiveChanged: if (active) poke()

    // Long enough to reach for a control after moving to it; short enough
    // that the picture is soon the only thing on the screen.
    Timer {
        id: stillness
        interval: 2500
        onTriggered: root.pointerMoved = false
    }

    // As large as the screen allows, at the picture's own shape.
    Item {
        id: frame
        anchors.centerIn: parent
        width: Math.min(root.width, root.height * surface.aspectRatio)
        height: Math.min(root.height, root.width / surface.aspectRatio)

        VideoSurface {
            id: surface
            anchors.fill: parent
            visible: root.active && Player.videoPlaying
        }

        // The cover until the first frame, as everywhere else the picture is.
        Artwork {
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height)
            height: width
            visible: !root.videoShowing
            source: root.artwork
            placeholder: ""
            colour: true
        }
    }

    HoverHandler {
        id: pointer
        cursorShape: root.chromeShown ? Qt.ArrowCursor : Qt.BlankCursor
        onPointChanged: {
            const at = pointer.point.scenePosition
            if (Math.abs(at.x - root.lastPointer.x) < 2 && Math.abs(at.y - root.lastPointer.y) < 2)
                return
            root.lastPointer = at
            root.poke()
        }
    }
    TapHandler {
        onTapped: root.poke()
        onDoubleTapped: root.leaveRequested()
    }

    // — the strip: which song, and the way out —
    Rectangle {
        id: strip
        width: parent.width
        height: Theme.titleBarHeight
        color: Theme.ink
        opacity: root.chromeShown ? 1 : 0
        visible: opacity > 0

        // In with the pointer, out at the pace of anything leaving.
        Behavior on opacity {
            NumberAnimation {
                duration: root.chromeShown ? Theme.quick : Theme.normal
                easing.type: root.chromeShown ? Theme.enterCurve : Theme.exitCurve
            }
        }

        HoverHandler { id: stripHover }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            // On ink the ladder runs the other way: grey at rest, paper under
            // the pointer, since ink is what is already behind it.
            IconButton {
                id: leave
                anchors.verticalCenter: parent.verticalCenter
                iconName: "fullscreen-exit"
                iconSize: 18
                iconColor: Theme.inkGrey
                hoverColor: Theme.paper
                onClicked: root.leaveRequested()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Leave full screen (Esc)"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - leave.width - Theme.space3)
                text: Player.currentTrack.title !== undefined
                      ? Player.currentTrack.title
                        + (Player.currentTrack.artist ? "  ·  " + Player.currentTrack.artist : "")
                      : ""
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Theme.weightBlack
                color: Theme.paper
            }
        }
    }
}
