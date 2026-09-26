import QtQuick
import Monolist
import Monolist.Backend

// One shelf of suggestions: a heading, the reason it exists, and the songs.
//
// Text rows rather than cards, because a catalogue row is a name and nothing
// else — there is no artwork to show without first resolving every one of
// them, which would cost a search each. The names are the honest thing to
// show, and pressing one is what turns it into a song.
Item {
    id: root

    property string title: ""
    property string reason: ""
    property var rows: []
    // Which of Recs.shelves this is, for the row menu's look-ups.
    property int shelfIndex: -1

    signal rowActivated(int index)

    implicitHeight: body.implicitHeight

    // The song menu, for a row that is still only a name: what needs the
    // song itself (Play next, Add to playlist, Copy link…) looks it up first,
    // as pressing the row does, and TrackMenu is handed the answer (Main).
    function openMenu(index) {
        const row = rows[index]
        if (!row)
            return
        Menus.openTrack({ title: row.title, artist: row.artist }, {
            resolve: function(action, argument) {
                Recs.resolve(root.shelfIndex, index, action + "|" + argument)
            }
        })
    }

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Text {
            width: parent.width
            text: root.title
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 19
            font.weight: Theme.weightBlack
            color: Theme.text
        }

        Text {
            width: parent.width
            visible: root.reason.length > 0
            text: root.reason
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }

        Column {
            id: list
            width: parent.width

            Repeater {
                model: root.rows

                Item {
                    id: row

                    required property var modelData
                    required property int index

                    width: list.width
                    height: 40

                    Rectangle {
                        anchors.fill: parent
                        color: hover.hovered ? Theme.rowHover : "transparent"
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        text: String(row.index + 1).padStart(2, "0")
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: hover.hovered ? Theme.accent : Theme.neutral700
                    }

                    Text {
                        x: 28
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, parent.width * 0.55 - 28)
                        text: row.modelData.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Theme.weightBlack
                        color: Theme.text
                    }

                    // The catalogue keeps names only; Artists finds the page
                    // for one YouTube Music has linked, and looks up the rest.
                    // It leaves room at the end for the row's dots.
                    ArtistLine {
                        x: parent.width * 0.55
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, parent.width * 0.45 - 36)
                        artist: row.modelData.artist
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.neutral700
                    }

                    // A button, so its click is not also taken as a tap on
                    // the row.
                    IconButton {
                        visible: hover.hovered
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        side: 30
                        iconName: "dots"
                        iconSize: 16
                        iconColor: Theme.neutral700
                        onClicked: root.openMenu(row.index)
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.ruleWidth
                        anchors.bottom: parent.bottom
                        color: Theme.neutral300
                    }

                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.rowActivated(row.index) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.openMenu(row.index)
                    }
                }
            }
        }
    }
}
