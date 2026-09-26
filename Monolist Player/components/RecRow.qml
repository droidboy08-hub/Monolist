import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// One suggestion in a list: its place, the song, who by, and under the
// pointer the two answers a suggestion can be given without playing it —
// Not interested, and the song menu.
//
// Text rather than a card, because a catalogue row is a name and nothing
// else: there is no artwork to show without first looking every one of them
// up. Pressing the row is what finds the song, and while that search runs a
// short red rule runs along the row's foot, so a press that takes a second
// is seen to have been taken.
Item {
    id: row

    // title, artist and lengthMs, as Recs has them
    property var song: ({})
    property int number: 0

    signal activated()
    signal menuRequested()

    readonly property bool finding: Recs.pendingKey.length > 0
                                    && Recs.pendingKey === (song.title || "") + "\n" + (song.artist || "")

    height: 40

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.rowHover : "transparent"
    }

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        text: String(row.number).padStart(2, "0")
        font.family: Theme.fontFamily
        font.pixelSize: 12
        color: hover.hovered || row.finding ? Theme.accent : Theme.neutral700
    }

    Text {
        x: 28
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, parent.width * 0.55 - 28)
        text: row.song.title || ""
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 14
        font.weight: Theme.weightBlack
        color: Theme.text
    }

    // The catalogue keeps names only; Artists finds the page for one YouTube
    // Music has linked, and looks up the rest. It leaves room at the end for
    // the row's two buttons.
    ArtistLine {
        x: parent.width * 0.55
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, parent.width * 0.45 - 66)
        artist: row.song.artist || ""
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // Buttons, so a click on either is not also taken as a tap on the row.
    Row {
        visible: hover.hovered
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter

        IconButton {
            side: 30
            iconName: "x"
            iconSize: 15
            iconColor: Theme.neutral700
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "Not interested"
            onClicked: Recs.notInterested(row.song)
        }
        IconButton {
            side: 30
            iconName: "dots"
            iconSize: 16
            iconColor: Theme.neutral700
            onClicked: row.menuRequested()
        }
    }

    Rectangle {
        width: parent.width
        height: Theme.ruleWidth
        anchors.bottom: parent.bottom
        color: Theme.neutral300
    }

    // Being looked up: a quarter of the rule in red, running along it.
    // Position only (DESIGN 2.3), and only while it is true.
    Item {
        visible: row.finding
        width: parent.width
        height: Theme.ruleWidth
        anchors.bottom: parent.bottom
        clip: true

        Rectangle {
            id: runner
            width: parent.width / 4
            height: parent.height
            color: Theme.accent

            NumberAnimation on x {
                running: row.finding
                loops: Animation.Infinite
                from: -runner.width
                to: row.width
                duration: 900
                easing.type: Theme.moveCurve
            }
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: row.activated() }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: row.menuRequested()
    }
}
