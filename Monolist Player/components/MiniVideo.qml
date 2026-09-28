import QtQuick
import Monolist
import Monolist.Backend

// Picture in picture: a small picture at the bottom right, standing on the
// page above the player bar, so the song can be watched while the rest of the
// app is used. Only when asked for, from its button in Now Playing. It is
// framed by a 2px ink rule, as anything that stands on the page is. A click
// opens Now Playing, where the picture is large; a double-click, or its corner
// button, goes full screen; its other corner button puts it away, and the song
// carries on as sound.
Rectangle {
    id: root

    // The picture's place is here now (Main decides, one place at a time).
    property bool active: false
    signal openRequested()
    signal fullscreenRequested()
    signal closeRequested()

    readonly property bool videoShowing: Player.videoPlaying && surface.showing
    readonly property string artwork: Player.currentTrack.artwork !== undefined
                                      ? Player.currentTrack.artwork : ""

    width: 320 + Theme.ruleWidth * 2
    height: Math.round(320 / surface.aspectRatio) + Theme.ruleWidth * 2
    color: Theme.text

    // Appears and goes where it is (DESIGN 2.1, `normal`); it does not travel.
    opacity: active ? 1 : 0
    visible: opacity > 0
    Behavior on opacity {
        NumberAnimation {
            duration: root.active ? Theme.normal : Theme.leaving
            easing.type: root.active ? Theme.enterCurve : Theme.exitCurve
        }
    }

    Item {
        id: picture
        anchors.fill: parent
        anchors.margins: Theme.ruleWidth
        clip: true

        // Taken from `active`, not from the fade: the picture moves to its
        // next place the moment this one is left.
        VideoSurface {
            id: surface
            anchors.fill: parent
            visible: root.active && Player.videoPlaying
        }

        // The cover until the first frame, as in Now Playing.
        Artwork {
            anchors.fill: parent
            visible: !root.videoShowing
            source: root.artwork
            placeholder: ""
            colour: true
        }
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        onSingleTapped: root.openRequested()
        onDoubleTapped: root.fullscreenRequested()
    }

    // Arrive with the pointer, leave a moment after it (DESIGN 2.6).
    Row {
        anchors.top: picture.top
        anchors.right: picture.right
        anchors.margins: Theme.space2
        spacing: Theme.space1
        opacity: hover.hovered ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            enabled: !hover.hovered
            NumberAnimation { duration: Theme.quick }
        }

        PlateButton {
            iconName: "fullscreen"
            tip: "Full screen (F)"
            onClicked: root.fullscreenRequested()
        }
        PlateButton {
            iconName: "x"
            tip: "Close picture in picture"
            onClicked: root.closeRequested()
        }
    }
}
