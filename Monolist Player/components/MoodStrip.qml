import QtQuick
import Monolist
import Monolist.Backend

// YouTube Music's Moods & genres as one row of chips that scrolls sideways,
// moods first, then genres, with arrows for a mouse that has no horizontal
// wheel. A chip opens the mood's shelves of playlists.
Item {
    id: root

    // Catalog.moods: [{ title, chips: [...] }].
    property var groups: []

    readonly property var chips: {
        const all = []
        for (let i = 0; i < root.groups.length; ++i) {
            const group = root.groups[i]
            for (let j = 0; j < group.chips.length; ++j)
                all.push(group.chips[j])
        }
        return all
    }

    implicitHeight: body.implicitHeight

    function page(direction) {
        const step = Math.max(200, list.width - 200)
        const max = Math.max(0, list.contentWidth - list.width)
        slide.to = Math.max(0, Math.min(max, list.contentX + direction * step))
        slide.restart()
    }

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Item {
            width: parent.width
            height: heading.implicitHeight

            Text {
                id: heading
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "MOODS & GENRES"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }

            Row {
                visible: list.contentWidth > list.width
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
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
            width: parent.width
            height: 44
            orientation: ListView.Horizontal
            spacing: Theme.space3
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.chips

            delegate: MoodChip {
                required property var modelData
                height: list.height
                width: implicitWidth
                chip: modelData
                onActivated: Nav.openMore({ kind: "browse", browseId: modelData.browseId, params: modelData.params },
                                          modelData.title)
            }

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
