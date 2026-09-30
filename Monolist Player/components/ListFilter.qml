import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// A list's own filter field and order, above its song table: words to find
// in the titles, artists and albums, and the list's own order, A–Z or by
// artist. The order is kept per list (settingKey); the words are not.
Item {
    id: root

    property TrackFilterModel model: null
    // Where the order is kept, "sort.<list>"; empty keeps nothing.
    property string settingKey: ""
    // What the list's own order is called: RECENT for the latest first,
    // PLAYLIST ORDER for a playlist's.
    property string ownOrder: "RECENT"

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    // The order kept as "<key>" or "<key>:desc".
    Component.onCompleted: {
        if (!model || settingKey.length === 0)
            return
        const kept = Library.settingValue(settingKey)
        if (kept.length === 0)
            return
        const parts = kept.split(":")
        model.descending = parts.length > 1 && parts[1] === "desc"
        model.sortKey = parts[0] === "own" ? "" : parts[0]
    }
    Connections {
        target: root.model
        function onSortChanged() {
            if (root.settingKey.length === 0)
                return
            const key = root.model.sortKey.length > 0 ? root.model.sortKey : "own"
            Library.setSetting(root.settingKey, key + (root.model.descending ? ":desc" : ""))
        }
    }

    function order(key) {
        if (!model)
            return
        model.descending = false
        model.sortKey = key
    }

    Flow {
        id: layout
        width: root.width
        spacing: Theme.space4

        Rectangle {
            width: 280
            height: 36
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: field.activeFocus ? Theme.accent : Theme.neutral300

            Icon {
                id: glass
                name: "search"
                width: 13
                height: 13
                x: Theme.space3
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.neutral600
            }

            TextField {
                id: field
                anchors.left: glass.right
                anchors.leftMargin: Theme.space2
                anchors.right: clear.left
                anchors.verticalCenter: parent.verticalCenter
                placeholderText: "Filter"
                placeholderTextColor: Theme.neutral500
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentForeground
                background: null
                leftPadding: 0
                rightPadding: 0
                onTextChanged: if (root.model) root.model.filterText = text
                Keys.onEscapePressed: text = ""
            }

            IconButton {
                id: clear
                visible: field.text.length > 0
                anchors.right: parent.right
                anchors.rightMargin: Theme.space1
                anchors.verticalCenter: parent.verticalCenter
                side: 28
                iconName: "x"
                iconSize: 12
                iconColor: Theme.neutral700
                onClicked: field.text = ""
            }
        }

        // Negative spacing lets neighbouring segments share one 2px rule.
        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: root.ownOrder
                selected: root.model !== null && root.model.sortKey === ""
                onPicked: root.order("")
            }
            ChoiceChip {
                label: "A–Z"
                selected: root.model !== null && root.model.sortKey === "title"
                onPicked: root.order("title")
            }
            ChoiceChip {
                label: "ARTIST"
                selected: root.model !== null && root.model.sortKey === "artist"
                onPicked: root.order("artist")
            }
        }
    }
}
