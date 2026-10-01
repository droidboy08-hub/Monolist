import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// One suggestion in a list: its place, its cover, the song, who by, and
// under the pointer the two answers a suggestion can be given without
// playing it — Not interested, and the song menu.
//
// A catalogue row is a name and nothing else, so its cover is looked for
// once the row is shown (Recs.wantCover: paced, signed out, and kept for
// good), and until it is found the plate shows a note. Pressing the row is
// what finds the song, and while that search runs a short red rule runs
// along the row's foot, so a press that takes a second is seen to have been
// taken.
Item {
    id: row

    // title, artist and lengthMs, as Recs has them
    property var song: ({})
    property int number: 0

    signal activated()
    signal menuRequested()

    readonly property bool finding: Recs.pendingKey.length > 0
                                    && Recs.pendingKey === (song.title || "") + "\n" + (song.artist || "")
    // Where the title starts: after the number and the cover.
    readonly property int titleX: 28 + 40 + Theme.space3
    // Narrow, the title takes the line and the artist goes under it.
    readonly property bool stacked: width < 480

    height: 56

    // Asked for only while the row is shown: a page not open (Search, built
    // at launch behind Home) asks for nothing. A row hidden does not take
    // its turn back, since the same name may be showing elsewhere.
    function askCover() {
        if (row.visible)
            Recs.wantCover(song)
    }
    Component.onCompleted: askCover()
    Component.onDestruction: Recs.dropCover(song)
    onSongChanged: askCover()
    onVisibleChanged: askCover()

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
        color: hover.hovered || row.finding ? Theme.accent700 : Theme.neutral700
    }

    TrackCover {
        x: 28
        width: 40
        height: 40
        anchors.verticalCenter: parent.verticalCenter
        source: Recs.coversRevision >= 0 ? Recs.coverOf(row.song.title || "", row.song.artist || "") : ""
        hovered: hover.hovered
        onPlayRequested: row.activated()
    }

    Column {
        x: row.titleX
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, row.stacked ? parent.width - row.titleX - 66 : parent.width * 0.55 - row.titleX)
        spacing: 2

        Text {
            width: parent.width
            text: row.song.title || ""
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 14
            font.weight: Theme.weightBlack
            color: Theme.text
        }
        ArtistLine {
            visible: row.stacked
            width: parent.width
            artist: row.song.artist || ""
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }
    }

    // The catalogue keeps names only; Artists finds the page for one YouTube
    // Music has linked, and looks up the rest. It leaves room at the end for
    // the row's two buttons.
    ArtistLine {
        visible: !row.stacked
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
