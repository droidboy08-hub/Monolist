import QtQuick
import Monolist

// One band of the equaliser, standing: a 6px track from -12 dB at the foot to
// +12 at the head, a 2px tick at 0, and an ink fill from the tick to the
// value — never red, because a band that is lifted is not "on", it is just
// set. The value is printed above ("+4.5", "0") and the band's frequency
// below.
//
// It is driven like the progress line (ProgressSlider): press anywhere on it
// and drag, in 0.5 dB steps. A double-click puts it back to 0, and Up and
// Down move it by a step once Tab has brought the keyboard to it. Nothing
// moves on its own: the fill follows the hand, and only colour changes (the
// value turns to ink under the pointer at once and fades back over `quick`).
Item {
    id: root

    // In dB.
    property real value: 0
    // "31", "1K" … under the track.
    property string label: ""
    // The equaliser is off: grey instead of ink, still to be moved.
    property bool dimmed: false
    property real range: 12
    property real step: 0.5
    property int trackHeight: 140
    // Room above the track for the value; EffectsPanel draws the response
    // curve against the same numbers.
    property int trackTop: 20
    signal moved(real db)

    readonly property bool pointed: area.containsMouse || area.pressed
    readonly property color ink: dimmed ? Theme.neutral500 : Theme.text

    implicitWidth: 40
    implicitHeight: trackTop + trackHeight + Theme.space2 + bandLabel.implicitHeight

    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: label.endsWith("K") ? label.slice(0, -1) + " kHz" : label + " Hz"
    Accessible.description: format(value) + " dB"

    function snap(db) {
        return Math.max(-range, Math.min(range, Math.round(db / step) * step))
    }
    // Only a value that differs: a drag inside one step says nothing.
    function set(db) {
        const next = snap(db)
        if (Math.abs(next - value) > 0.001)
            moved(next)
    }
    function valueAt(y) {
        return (1 - y / trackHeight) * range * 2 - range
    }
    // "+4.5", "+12", "0", "−3" (a true minus, as print sets it).
    function format(db) {
        if (Math.abs(db) < 0.05)
            return "0"
        const size = Math.abs(db)
        return (db > 0 ? "+" : "−") + (Math.abs(size - Math.round(size)) < 0.05 ? size.toFixed(0) : size.toFixed(1))
    }

    Keys.onUpPressed: set(value + step)
    Keys.onDownPressed: set(value - step)

    Text {
        id: valueText
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.round((root.trackTop - Theme.space1 - implicitHeight))
        text: root.format(root.value)
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        color: root.pointed ? Theme.text : (root.dimmed ? Theme.neutral500 : Theme.neutral700)

        // Arrives at once, leaves over `quick` (DESIGN 2.6).
        Behavior on color {
            enabled: !root.pointed
            ColorAnimation { duration: Theme.quick }
        }
    }

    Rectangle {
        id: track
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.trackTop
        width: 6
        height: root.trackHeight
        color: Theme.neutral300

        readonly property real zero: Math.round(height / 2)
        readonly property real reach: Math.min(1, Math.abs(root.value) / root.range) * zero

        Rectangle {
            width: parent.width
            y: root.value >= 0 ? track.zero - track.reach : track.zero
            height: track.reach
            color: root.ink
        }

        // 0 dB, wider than the track so it reads with nothing above or below it.
        Rectangle {
            x: -4
            y: track.zero - 1
            width: 14
            height: 2
            color: root.ink
        }
    }

    // The keyboard's ring, as an icon button draws one: a pointer can see
    // what it is over, a keyboard cannot.
    Rectangle {
        visible: root.activeFocus
        x: track.x - 6
        y: track.y - 6
        width: track.width + 12
        height: track.height + 12
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
    }

    Text {
        id: bandLabel
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.trackTop + root.trackHeight + Theme.space2
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    // The whole column takes the hand, a little beyond the track's ends so
    // that +12 and -12 are easy to reach.
    MouseArea {
        id: area
        x: 0
        y: root.trackTop - 6
        width: root.width
        height: root.trackHeight + 12
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        onPressed: function(mouse) { root.set(root.valueAt(mouse.y - 6)) }
        onPositionChanged: function(mouse) {
            if (pressed)
                root.set(root.valueAt(mouse.y - 6))
        }
        onDoubleClicked: root.set(0)
    }
}
