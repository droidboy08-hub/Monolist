import QtQuick
import QtQuick.Controls.Basic
import Monolist

// A numbered row of cards that scrolls sideways, with arrows in its header for
// a mouse that has no horizontal wheel.
Item {
    id: root

    property string number: ""
    property string title: ""
    property string strapline: ""
    property var items: []
    signal cardActivated(var card)

    implicitHeight: body.implicitHeight

    // "Album • Seth Ballad": the kind goes to the card's footer label, the rest
    // under the title, so neither is said twice.
    function splitKind(card) {
        var parts = card.subtitle ? card.subtitle.split(" • ") : []
        var kinds = ["Album", "Single", "EP", "Playlist", "Song", "Video"]
        if (parts.length > 1 && kinds.indexOf(parts[0]) >= 0)
            return { kind: parts[0].toUpperCase(), rest: parts.slice(1).join(" • ") }
        return { kind: card.type ? card.type.toUpperCase() : "", rest: card.subtitle || "" }
    }

    function page(direction) {
        var step = Math.max(list.cardWidth, list.width - list.cardWidth)
        var target = list.contentX + direction * step
        var max = Math.max(0, list.contentWidth - list.width)
        slide.to = Math.max(0, Math.min(max, target))
        slide.restart()
    }

    Column {
        id: body
        width: parent.width
        spacing: Theme.space6

        Item {
            width: parent.width
            height: heading.implicitHeight

            Column {
                id: heading
                spacing: Theme.space1

                Text {
                    visible: root.strapline.length > 0
                    text: root.strapline.toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }

                Row {
                    spacing: Theme.space4
                    baselineOffset: shelfTitle.y + shelfTitle.baselineOffset

                    Text {
                        text: root.number
                        anchors.baseline: shelfTitle.baseline
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Theme.weightBlack
                        color: Theme.accent700
                    }
                    Text {
                        id: shelfTitle
                        text: root.title
                        font.family: Theme.fontFamily
                        font.pixelSize: 28
                        font.weight: Theme.weightBlack
                        font.letterSpacing: Theme.tracking(28, -0.02)
                        color: Theme.text
                    }
                }
            }

            Row {
                visible: list.contentWidth > list.width
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: Theme.space1

                IconButton {
                    iconName: "arrow-left"
                    iconSize: 15
                    enabled: list.contentX > 0
                    opacity: enabled ? 1 : 0.3
                    onClicked: root.page(-1)
                }
                IconButton {
                    iconName: "arrow-right"
                    iconSize: 15
                    enabled: list.contentX < list.contentWidth - list.width - 1
                    opacity: enabled ? 1 : 0.3
                    onClicked: root.page(1)
                }
            }
        }

        ListView {
            id: list

            readonly property real cardWidth: 188

            width: parent.width
            height: cardWidth + 100   // AlbumCard: the square, then its text
            orientation: ListView.Horizontal
            spacing: Theme.space6
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.items

            delegate: AlbumCard {
                required property var modelData
                readonly property var label: root.splitKind(modelData)

                width: list.cardWidth
                height: list.height
                title: modelData.title
                artist: label.rest
                year: ""
                format: label.kind
                artwork: modelData.artwork
                onPlayRequested: root.cardActivated(modelData)
            }

            // Paging moves the shelf under a still pointer, so it is eased at
            // both ends rather than thrown.
            NumberAnimation {
                id: slide
                target: list
                property: "contentX"
                duration: Theme.page
                easing.type: Theme.moveCurve
            }
        }
    }
}
