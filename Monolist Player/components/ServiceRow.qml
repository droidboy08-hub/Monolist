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
// the page's business; the row only asks, through its signals. One built but
// unable to work here (no key in this build) keeps its CONNECT, greyed, with
// the status line saying why: the control is where it will be, and the reason
// it is dead is written beside it.
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
    // no Last.fm key". Red when it is something to act on. Styled text: it
    // may carry a link (where to revoke access, a page to open by hand).
    property string statusLine: ""
    // A small line of credit the service asks for, styled text with links.
    property string credit: ""
    // While waiting on the browser: a second button, for the person who has
    // done what the browser asked ("I'VE APPROVED IT"). None when empty.
    property string confirmText: ""

    readonly property bool connected: serviceState === "connected"
    readonly property bool needsAttention: serviceState === "expired" || serviceState === "error"

    signal connectRequested()
    signal disconnectRequested()
    signal cancelRequested()
    signal confirmRequested()

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
                    id: status
                    visible: root.statusLine.length > 0
                    width: parent.width
                    text: root.statusLine
                    textFormat: Text.StyledText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    // Red only when there is something to do about it; a fact
                    // about this build is not an alarm.
                    color: root.needsAttention ? Theme.accent : Theme.neutral700
                    linkColor: Theme.text
                    topPadding: root.connected && root.accountName.length > 0 ? 0 : Theme.space1
                    onLinkActivated: function(link) { Qt.openUrlExternally(link) }

                    HoverHandler {
                        cursorShape: status.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }
                Text {
                    id: creditLine
                    visible: root.credit.length > 0
                    width: parent.width
                    text: root.credit
                    textFormat: Text.StyledText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.neutral700
                    linkColor: Theme.text
                    topPadding: Theme.space1
                    onLinkActivated: function(link) { Qt.openUrlExternally(link) }

                    HoverHandler {
                        cursorShape: creditLine.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }
            }

            ActionButton {
                id: open
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // Built but unable to work here (no key in this build, say):
                // CONNECT, greyed, and the status line says why.
                enabled: !root.built || root.serviceState !== "unavailable"
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

            // Done in the browser: ask now rather than wait for the next look.
            Item {
                visible: root.built && root.serviceState === "waiting" && root.confirmText.length > 0
                width: parent.width - Theme.space4
                height: confirm.implicitHeight + Theme.space2 * 2

                ActionButton {
                    id: confirm
                    y: Theme.space2
                    primary: true
                    text: root.confirmText
                    onClicked: root.confirmRequested()
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
