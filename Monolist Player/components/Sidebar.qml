import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    property string currentView: "home"
    property bool showCloseButton: false
    signal viewRequested(string view)
    signal newPlaylistRequested()
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
            text: "V." + Qt.application.version
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
                active: root.currentView.indexOf("library") === 0
                onClicked: root.viewRequested("library")
            }
            NavItem {
                iconName: "download"
                label: "Downloads"
                active: root.currentView === "downloads"
                onClicked: root.viewRequested("downloads")
            }
            NavItem {
                iconName: "settings"
                label: "Settings"
                active: root.currentView === "settings"
                onClicked: root.viewRequested("settings")
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
            onClicked: root.newPlaylistRequested()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "New playlist"
        }
    }

    // — playlists: Liked songs first, then the user's own —
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

        header: PlaylistRow {
            width: ListView.view ? ListView.view.width : 0
            iconName: "heart-filled"
            name: "Liked songs"
            trackCount: Library.liked.count
            active: root.currentView === "playlist:liked"
            onActivated: root.viewRequested("playlist:liked")
        }

        // Roles through `model`: the row's own properties share their names.
        delegate: PlaylistRow {
            width: ListView.view ? ListView.view.width : 0
            number: model.number
            name: model.name
            trackCount: model.trackCount
            active: root.currentView === "playlist:" + model.playlistId
            onActivated: root.viewRequested("playlist:" + model.playlistId)
        }

        ScrollBar.vertical: MonoScrollBar {}
    }

    // — the user —
    // The name the system knows them by, and what they keep here. The name
    // can be changed in place: the pencil, then Enter.
    Item {
        id: userStrip
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: 68

        property bool editing: false

        function commit() {
            if (!editing)
                return
            editing = false
            Library.setUserName(nameField.text)
        }

        Rectangle {
            anchors.fill: parent
            color: userHover.hovered && !userStrip.editing ? Theme.surface : "transparent"

            Behavior on color {
                enabled: !userHover.hovered
                ColorAnimation { duration: Theme.quick }
            }
        }

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        HoverHandler { id: userHover; cursorShape: userStrip.editing ? Qt.ArrowCursor : Qt.PointingHandCursor }
        TapHandler {
            enabled: !userStrip.editing
            onTapped: root.viewRequested("library")
        }

        Rectangle {
            id: initials
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            color: Theme.text

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
            anchors.left: initials.right
            anchors.leftMargin: Theme.space3
            anchors.right: editButton.left
            anchors.rightMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            Item {
                width: parent.width
                height: 18

                Text {
                    visible: !userStrip.editing
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    text: Library.userName
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.Bold
                    color: Theme.text
                }

                TextInput {
                    id: nameField
                    visible: userStrip.editing
                    width: parent.width
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.Bold
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentForeground
                    maximumLength: 40
                    clip: true
                    onAccepted: userStrip.commit()
                    onActiveFocusChanged: if (!activeFocus) userStrip.commit()
                    Keys.onEscapePressed: {
                        userStrip.editing = false
                        focus = false
                    }

                    Rectangle {
                        anchors.top: parent.bottom
                        width: parent.width
                        height: Theme.ruleWidth
                        color: Theme.accent
                    }
                }
            }

            Text {
                width: parent.width
                text: Library.liked.count + " LIKED · " + Downloads.storedCount + " DOWNLOADED"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.neutral600
            }
        }

        IconButton {
            id: editButton
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6 - Theme.space1
            anchors.verticalCenter: parent.verticalCenter
            visible: userHover.hovered && !userStrip.editing
            side: 28
            iconName: "pencil"
            iconSize: 14
            iconColor: Theme.text
            onClicked: {
                nameField.text = Library.userName
                userStrip.editing = true
                nameField.forceActiveFocus()
                nameField.selectAll()
            }
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "Change the name shown here"
        }
    }
}
