import QtQuick
import Phono

// The flat 6px bar: track, accent fill, drag and click to seek.
Item {
    id: root

    property real value: 0            // 0..1
    property color fillColor: Theme.accent
    property bool interactive: true
    signal moved(real value)

    implicitHeight: 6
    implicitWidth: 120

    Rectangle {
        id: track
        anchors.fill: parent
        color: Theme.neutral300

        Rectangle {
            width: Math.max(0, Math.min(1, root.value)) * parent.width
            height: parent.height
            color: root.fillColor
        }
    }

    MouseArea {
        anchors.fill: parent
        anchors.topMargin: -6
        anchors.bottomMargin: -6
        enabled: root.interactive
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        onPressed: function(mouse) { root.moved(Math.max(0, Math.min(1, mouse.x / width))) }
        onPositionChanged: function(mouse) {
            if (pressed)
                root.moved(Math.max(0, Math.min(1, mouse.x / width)))
        }
    }
}
