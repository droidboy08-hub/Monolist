import QtQuick
import QtQuick.Controls.Basic
import Monolist

// One outside service the app can be connected to.
//
// A row is built or it is not. One that is not yet says so rather than
// offering a button that does nothing: pressing it opens the steps the
// sign-in will actually take, so the shape of the thing is visible and honest
// before any of it exists. A greyed-out control with no explanation tells you
// nothing; this tells you what is coming and what it will ask of you.
//
// One that is built says where it stands, and its button is the one thing to
// do next: connect, cancel, disconnect or reconnect. What the button does is
// the page's business; the row only asks, through its signals.
Item {
    id: root

    property string name: ""
    property string detail: ""
    // What connecting will involve, one step per line. Shown when opened.
    property var steps: []
    // Said before anything else in the panel, where there is something the
    // person should know before they start rather than after.
    property string caution: ""

    // Whether this build can actually sign in. Until it can, the button
    // shows the design, and the panel ends by saying it is not built yet.
    property bool built: false
    // off | waiting | connected | expired | error | unavailable. Not called
    // `state`: every Item already has one, for its States, and redefining it
    // would quietly break any transition someone puts on the row later.
    property string serviceState: "off"
    // Who is connected, shown while connected.
    property string accountName: ""
    // One line on where things stand: "12 scrobbles waiting", "This build has
    // no Last.fm key". Red when it is something to act on.
    property string statusLine: ""

    readonly property bool connected: serviceState === "connected"
    readonly property bool needsAttention: serviceState === "expired" || serviceState === "error"

    signal connectRequested()
    signal disconnectRequested()
    signal cancelRequested()

    // The steps, opened by hand from a row not built yet; a built one shows
    // them on its own while it waits on the browser, as what is happening.
    property bool stepsOpen: false

    implicitHeight: body.implicitHeight

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Item {
            width: parent.width
            height: Math.max(title.implicitHeight, open.visible ? open.implicitHeight : 0)

            Column {
                id: title
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - (open.visible ? open.width + Theme.space4 : 0)
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
                Text {
                    visible: root.connected && root.accountName.length > 0
                    width: parent.width
                    text: "Connected as " + root.accountName
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Theme.weightBlack
                    color: Theme.text
                    topPadding: Theme.space1
                }
                Text {
                    visible: root.statusLine.length > 0
                    width: parent.width
                    text: root.statusLine
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    // Red only when there is something to do about it; a fact
                    // about this build is not an alarm.
                    color: root.needsAttention ? Theme.accent : Theme.neutral700
                    topPadding: root.connected && root.accountName.length > 0 ? 0 : Theme.space1
                }
            }

            ActionButton {
                id: open
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // Built but unable to work here (no key in this build, say):
                // no button at all, and the status line says why, rather than
                // one that does nothing.
                visible: !root.built || root.serviceState !== "unavailable"
                text: !root.built ? (root.stepsOpen ? "CLOSE" : "HOW IT WILL WORK")
                      : root.serviceState === "connected" ? "DISCONNECT"
                      : root.serviceState === "waiting" ? "CANCEL"
                      : root.serviceState === "expired" ? "RECONNECT"
                      : root.serviceState === "error" ? "TRY AGAIN"
                      : "CONNECT"
                onClicked: {
                    if (!root.built)
                        root.stepsOpen = !root.stepsOpen
                    else if (root.serviceState === "connected")
                        root.disconnectRequested()
                    else if (root.serviceState === "waiting")
                        root.cancelRequested()
                    else
                        root.connectRequested()
                }
            }
        }

        // The flow, as text: the steps are the decision being shown, and a
        // mock of the finished dialog would suggest the dialog exists.
        Column {
            id: panel
            visible: root.built ? root.serviceState === "waiting" : root.stepsOpen
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
                visible: !root.built
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
