import QtQuick
import Monolist
import Monolist.Backend

// A song's cover at the head of a list row.
//
// Grey at rest, like every cover in a list, and in colour under the pointer
// and while it is the song playing: colour means "this one" (DESIGN 1). A
// song with no cover of its own shows its video's thumbnail, so a row is
// never a blank plate where YouTube has a picture; with none at all, a note.
//
// Under the pointer it darkens and says what a click on it does: play the
// song, or for the one playing, pause it and go on. The song playing carries
// the moving bars (PlayingBars), held still while it is paused.
Item {
    id: root

    property string source: ""
    // The song's YouTube id, for its thumbnail when it has no cover.
    property string sourceId: ""
    // The row is under the pointer.
    property bool hovered: false
    // This is the song playing, or paused.
    property bool active: false
    // A click on it plays the song, as the row's own click does, and pauses
    // or goes on with the song playing. False: a picture only, the row's
    // click its click.
    property bool clickable: true
    // A list's covers are small and many: softened corners set them apart
    // from the square type around them.
    property real radius: 6

    signal playRequested()

    readonly property string shown: source.length > 0 ? source
                                   : sourceId.length > 0 ? Player.artworkForSource(sourceId) : ""
    readonly property bool marked: hovered && clickable

    implicitWidth: 40
    implicitHeight: 40

    Artwork {
        id: art
        anchors.fill: parent
        placeholder: ""
        source: root.shown
        colour: root.hovered || root.active
        radius: root.radius
    }

    Icon {
        visible: !art.ready
        anchors.centerIn: parent
        width: Math.round(parent.width * 0.4)
        height: width
        name: "music"
        color: Theme.neutral600
    }

    // Ink over the picture, so the paper-white marks on it read on any cover.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: Theme.text
        opacity: root.marked || root.active ? 0.45 : 0

        Behavior on opacity {
            NumberAnimation { duration: Theme.quick; easing.type: Theme.enterCurve }
        }
    }

    PlayingBars {
        visible: root.active && !root.marked
        anchors.centerIn: parent
        width: Math.round(parent.width * 0.4)
        height: Math.round(parent.width * 0.35)
        color: Theme.bg
        running: Player.playing
    }

    Icon {
        visible: root.marked
        anchors.centerIn: parent
        width: Math.round(parent.width * 0.4)
        height: width
        name: root.active && Player.playing ? "pause" : "play"
        color: Theme.bg
    }

    // Taken here, so a click on the cover is not also the row's.
    MouseArea {
        anchors.fill: parent
        enabled: root.clickable
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            if (root.active)
                Player.togglePlay()
            else
                root.playRequested()
        }
    }
}
