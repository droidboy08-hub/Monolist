import QtQuick
import Monolist

// A card: the cover, a title, a line under it, and a footer label with the
// play mark. The cover blooms into colour under the pointer.
//
// A card that opens a page (an album, a playlist) can also be played from
// where it stands: with `playable` set, a play plate comes onto the cover's
// corner under the pointer, and pressing it plays instead of opening.
Rectangle {
    id: root

    property string title: ""
    property string artist: ""
    property string year: ""
    property string format: "LP"
    // Overrides the year and format in the footer when set.
    property string footer: ""
    property string artwork: ""
    // A playlist's mosaic; takes the place of `artwork` when it has any.
    property var artworks: []
    // "liked" or "new": see CollectionCover.
    property string plate: ""
    // Whether a press opens a page (an album, a playlist, an artist) rather
    // than playing: the footer's mark says which, an arrow or the triangle.
    property bool opensPage: true
    // The play plate, and three dots on it while the songs are fetched.
    property bool playable: false
    property bool playLoading: false
    // A menu, from three dots in the footer under the pointer or a right
    // click anywhere on the card: what the owner opens (Menus).
    property bool hasMenu: false
    // The card itself pressed: open it (or, for a song, play it).
    signal playRequested()
    // The play plate pressed.
    signal playClicked()
    signal menuRequested()

    color: hover.hovered ? Theme.surface : Theme.bg
    border.width: Theme.ruleWidth
    border.color: Theme.text
    implicitHeight: width + 100

    Behavior on color {
        enabled: !hover.hovered
        ColorAnimation { duration: Theme.quick }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space3
        spacing: Theme.space3

        CollectionCover {
            width: parent.width
            height: width
            artworks: root.artworks && root.artworks.length > 0 ? root.artworks
                    : root.artwork.length > 0 ? [root.artwork] : []
            plate: root.plate
            colour: hover.hovered

            // On the picture, so a plate rather than a bare glyph (see
            // PlateButton). It arrives with the pointer and leaves over
            // `quick`, like every hover; it stays while its songs are on
            // their way, so the dots are seen.
            PlateButton {
                id: playPlate
                readonly property bool shown: root.playable && (hover.hovered || root.playLoading)
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space2
                iconName: root.playLoading ? "dots" : "play"
                tip: "Play"
                opacity: shown ? 1 : 0
                visible: opacity > 0
                onClicked: root.playClicked()

                Behavior on opacity {
                    enabled: !playPlate.shown
                    NumberAnimation { duration: Theme.quick }
                }
            }
        }

        Column {
            width: parent.width
            spacing: 2

            Text {
                width: parent.width
                text: root.title
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Theme.weightBlack
                color: Theme.text
            }
            Text {
                width: parent.width
                text: root.artist
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.neutral700
            }
        }
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space3
        height: 26

        Rectangle {
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Text {
            anchors.left: parent.left
            anchors.right: moreButton.visible ? moreButton.left : playMark.left
            anchors.rightMargin: Theme.space2
            anchors.bottom: parent.bottom
            text: root.footer.length > 0 ? root.footer
                  : [root.year, root.format].filter(function(part) { return part.length > 0 }).join(" · ")
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.1)
            color: Theme.neutral600
        }

        // On paper, so a bare glyph (DESIGN 2.6a), beside the mark that says
        // what a click on the card does. A button, so its press is not also
        // taken for one on the card.
        IconButton {
            id: moreButton
            visible: root.hasMenu && hover.hovered
            anchors.right: playMark.left
            anchors.rightMargin: Theme.space1
            anchors.verticalCenter: playMark.verticalCenter
            side: 24
            iconName: "dots"
            iconSize: 14
            iconColor: Theme.neutral700
            onClicked: root.menuRequested()
        }

        Icon {
            id: playMark
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            name: root.plate === "new" ? "plus" : root.opensPage ? "arrow-right" : "play"
            width: 14
            height: 14
            color: Theme.accent
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    // Not when the press was the play plate's: that one plays, and opening
    // the page as well would take the reader away from where they pressed.
    TapHandler {
        onTapped: {
            if (!playPlate.visible || !playPlate.hovered)
                root.playRequested()
        }
    }
    TapHandler {
        enabled: root.hasMenu
        acceptedButtons: Qt.RightButton
        onTapped: root.menuRequested()
    }
}
