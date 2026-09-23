import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One outside service the app can be connected to.
//
// None of them are wired up yet, and the row says so rather than offering a
// button that does nothing: pressing it opens the steps the sign-in will
// actually take, so the shape of the thing is visible and honest before any
// of it exists. A greyed-out control with no explanation tells you nothing;
// this tells you what is coming and what it will ask of you.
Item {
    id: root

    property string name: ""
    property string detail: ""
    // What connecting will involve, one step per line. Shown when opened.
    property var steps: []
    // Said before anything else in the panel, where there is something the
    // person should know before they start rather than after.
    property string caution: ""
    property bool connected: false

    implicitHeight: body.implicitHeight

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Item {
            width: parent.width
            height: Math.max(title.implicitHeight, open.implicitHeight)

            Column {
                id: title
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - open.width - Theme.space4
                spacing: 2

                Text {
                    text: root.name
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Theme.weightBlack
                    color: Theme.text
                }
                Text {
                    width: parent.width
                    text: root.detail
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }
            }

            ActionButton {
                id: open
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.connected ? "CONNECTED"
                                     : (panel.visible ? "CLOSE" : "HOW IT WILL WORK")
                enabled: !root.connected
                onClicked: panel.visible = !panel.visible
            }
        }

        // The designed flow. It is deliberately text: the steps are the
        // decision being shown, and a mock of the finished dialog would
        // suggest the dialog exists.
        Column {
            id: panel
            visible: false
            width: parent.width
            spacing: Theme.space2
            leftPadding: Theme.space4

            Rectangle {
                width: parent.width - Theme.space4
                height: Theme.ruleWidth
                color: Theme.text
            }

            Text {
                visible: root.caution.length > 0
                width: parent.width - Theme.space4
                text: root.caution
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Theme.weightBlack
                color: Theme.accent
                topPadding: Theme.space2
            }

            Repeater {
                model: root.steps

                Row {
                    required property int index
                    required property string modelData

                    spacing: Theme.space3

                    Text {
                        text: String(parent.index + 1).padStart(2, "0")
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Theme.weightBlack
                        color: Theme.accent
                    }
                    Text {
                        width: panel.width - Theme.space4 - Theme.space3 - 20
                        text: modelData
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.text
                    }
                }
            }

            Text {
                width: parent.width - Theme.space4
                text: "Not built yet — this is the design, not a working sign-in."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
                topPadding: Theme.space2
                bottomPadding: Theme.space2
            }
        }
    }
}
