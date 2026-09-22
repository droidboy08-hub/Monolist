import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    property string currentView: "home"
    property bool showCloseButton: false
    signal viewRequested(string view)
    signal closeRequested()

    color: Theme.bg
    implicitWidth: Theme.sidebarWidth

    Rectangle {                       // right rule
        anchors.right: parent.right
        width: Theme.ruleWidth
        height: parent.height
        color: Theme.divider
    }

    // — brand —
    // Level with the top bar, and like it a part of the window's title bar.
    Item {
        id: brand
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: Theme.titleBarHeight

        WindowDragArea {
            anchors.fill: parent
        }

        Row {
            anchors.left: parent.left
            // macOS keeps its traffic lights here.
            anchors.leftMargin: Theme.space6 + Chrome.nativeButtonsInset
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space2

            Text {
                text: "MONOLIST"
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                font.letterSpacing: Theme.tracking(28, -0.02)
                color: Theme.text
            }
            Text {
                text: "."
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                color: Theme.accent
            }
        }

        Text {
            visible: !root.showCloseButton
            text: "V.2.6"
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.letterSpacing: Theme.tracking(11, 0.12)
            color: Theme.neutral600
        }

        IconButton {
            visible: root.showCloseButton
            iconName: "x"
            side: 28
            iconSize: 16
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6 - Theme.space1
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.closeRequested()
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    // — navigation —
    Item {
        id: nav
        anchors.top: brand.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: navColumn.height + Theme.space4 * 2 + Theme.ruleWidth

        Column {
            id: navColumn
            y: Theme.space4
            width: parent.width

            NavItem {
                iconName: "home"
                label: "Home"
                active: root.currentView === "home"
                onClicked: root.viewRequested("home")
            }
            NavItem {
                iconName: "search"
                label: "Search"
                active: root.currentView === "search"
                onClicked: root.viewRequested("search")
            }
            NavItem {
                iconName: "library"
                label: "Your Library"
                active: root.currentView === "library"
                onClicked: root.viewRequested("library")
            }
            NavItem {
                iconName: "download"
                label: "Downloads"
                active: root.currentView === "downloads"
                onClicked: root.viewRequested("downloads")
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    // — playlists header —
    Item {
        id: playlistHeader
        anchors.top: nav.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: 52

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            text: "PLAYLISTS — " + ("0" + Library.playlists.count).slice(-2)
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.14)
            color: Theme.neutral600
        }

        IconButton {
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6 - Theme.space1
            anchors.verticalCenter: parent.verticalCenter
            iconName: "plus"
            side: 28
            iconSize: 16
            onClicked: root.viewRequested("library")
        }
    }

    // — playlists —
    ListView {
        id: playlistList
        anchors.top: playlistHeader.bottom
        anchors.bottom: userStrip.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        clip: true
        model: Library.playlists
        boundsBehavior: Flickable.StopAtBounds
        bottomMargin: Theme.space6

        delegate: PlaylistRow {
            width: ListView.view.width
            number: model.number
            name: model.name
            trackCount: model.trackCount
            onActivated: root.viewRequested("library")
        }

        ScrollBar.vertical: ScrollBar {
            width: 10
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle { color: Theme.neutral300 }
        }
    }

    // — account —
    Item {
        id: userStrip
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: 68

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            Rectangle {
                width: 32
                height: 32
                color: Theme.text
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: Library.userInitials
                    color: Theme.bg
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Theme.weightBlack
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    text: Library.userName
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.Bold
                    color: Theme.text
                }
                Text {
                    text: Library.userPlan
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.letterSpacing: Theme.tracking(11, 0.08)
                    color: Theme.neutral600
                }
            }
        }
    }
}
