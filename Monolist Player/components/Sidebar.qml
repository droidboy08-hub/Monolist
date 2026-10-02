import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    property string currentView: "home"
    property bool showCloseButton: false
    // The name folds the sidebar to a rail (SidebarRail), where it is docked.
    property bool collapsible: false
    signal collapseRequested()
    signal viewRequested(string view)
    signal newPlaylistRequested()
    signal closeRequested()
    // The account corner: signing in, and the signed-in account's settings.
    signal signInRequested()
    signal accountSettingsRequested()

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

        // Where the desktop keeps the window buttons on the left, they are
        // here, in the window's corner, ahead of the name.
        WindowButtons {
            id: brandButtons
            visible: Chrome.buttonsOnLeft && !Chrome.nativeButtons
            anchors.top: parent.top
            anchors.left: parent.left
            height: parent.height - Theme.ruleWidth
        }

        Row {
            id: brandName
            anchors.left: brandButtons.visible ? brandButtons.right : parent.left
            // macOS keeps its traffic lights in the corner above the name, so
            // the name keeps its place: set in beside them, it ran into the
            // version at the sidebar's other end.
            anchors.leftMargin: brandButtons.visible ? Theme.space2 : Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space2

            Text {
                text: "MONOLIST"
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                font.letterSpacing: Theme.tracking(28, -0.02)
                color: nameHover.hovered ? Theme.accent700 : Theme.text
            }
            Text {
                text: "."
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                color: Theme.accent
            }
        }

        // A click on the name folds the sidebar; a drag on it still moves
        // the window, and a double-click still maximises it (the handlers
        // only watch, and the title bar's take over past the threshold).
        Item {
            enabled: root.collapsible
            x: brandName.x - Theme.space1
            y: brandName.y - Theme.space1
            width: brandName.width + Theme.space2
            height: brandName.height + Theme.space2

            HoverHandler { id: nameHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.collapseRequested() }
        }
        ToolTip.visible: nameHover.hovered && root.collapsible
        ToolTip.delay: 600
        ToolTip.text: "Fold the sidebar"

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
        // Scrolled by the wheel alone: a drag on a row moves the playlist
        // (below), and the list must not take it for a flick.
        interactive: false
        bottomMargin: Theme.space6

        // Liked songs, and under it, while an account's library is
        // imported, its liked songs as they were read (never mixed in).
        header: Column {
            width: ListView.view ? ListView.view.width : 0

            PlaylistRow {
                width: parent.width
                iconName: "heart-filled"
                name: "Liked songs"
                trackCount: Library.liked.count
                active: root.currentView === "playlist:liked"
                onActivated: root.viewRequested("playlist:liked")
                onMenuRequested: Menus.openPlaylist("liked")
            }
            PlaylistRow {
                visible: AccountLibrary.shown
                width: parent.width
                iconName: "heart"
                name: "Liked on YouTube Music"
                trackCount: AccountLibrary.likedCount
                active: root.currentView === "playlist:ytliked"
                onActivated: root.viewRequested("playlist:ytliked")
                onMenuRequested: Menus.openPlaylist("ytliked")
            }
        }

        // Roles through `model`: the row's own properties share their names.
        // A row dragged up or down goes where it is dropped, among the
        // pinned if it is pinned, among the rest if not.
        delegate: PlaylistRow {
            id: playlistRow
            // The row it is at (the delegate's own index; no required
            // properties here, which would take away `model`).
            readonly property int row: index
            width: ListView.view ? ListView.view.width : 0
            number: model.pinned ? "▲" : model.number
            pinned: model.pinned
            name: model.name
            trackCount: model.trackCount
            active: root.currentView === "playlist:" + model.playlistId
            onActivated: root.viewRequested("playlist:" + model.playlistId)
            onMenuRequested: Menus.openPlaylist(model.playlistId)

            z: rowDrag.active ? 2 : 0
            transform: Translate { y: rowDrag.active ? rowDrag.activeTranslation.y : 0 }

            DragHandler {
                id: rowDrag
                // How far it has been carried, kept as it goes: the handler's
                // own translation is back at nothing by the time the release
                // is told.
                property real carried: 0
                target: null
                xAxis.enabled: false
                onActiveTranslationChanged: if (active) carried = activeTranslation.y
                onActiveChanged: {
                    if (active) {
                        carried = 0
                        return
                    }
                    const to = playlistRow.row + Math.round(carried / playlistRow.height)
                    if (to !== playlistRow.row)
                        Library.movePlaylist(model.playlistId, to)
                }
            }
        }

        ScrollBar.vertical: MonoScrollBar {}
        SmoothWheel { flickable: playlistList }
    }

    // — the account —
    // The YouTube Music account, in the corner where the app's own name for
    // the user used to be. Signed out it is the way in: Sign in, which opens
    // the sign-in. Signed in, the account's initials and name, and a click
    // opens the library, as it always did; the gear opens the account's
    // settings. A session that ended asks, in red, to be signed in again.
    Item {
        id: userStrip
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.rightMargin: Theme.ruleWidth
        height: 68

        readonly property string accountState: Account.state
        // Checking and unreachable keep the session, so they count as in.
        readonly property bool signedIn: accountState === "active" || accountState === "checking"
                                         || accountState === "unreachable"
        readonly property bool ended: accountState === "rejected"
        readonly property string name: Account.accountName
        readonly property string initials: {
            const parts = name.trim().split(/\s+/).filter(function(part) { return part.length > 0 })
            if (parts.length === 0)
                return ""
            const first = parts[0].charAt(0).toUpperCase()
            return parts.length > 1 ? first + parts[parts.length - 1].charAt(0).toUpperCase() : first
        }

        Rectangle {
            anchors.fill: parent
            color: userHover.hovered ? Theme.surface : "transparent"

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

        HoverHandler { id: userHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: {
                if (userStrip.signedIn)
                    root.viewRequested("library")
                else
                    root.signInRequested()
            }
        }

        // The account's initials in ink, signed in; signed out, a figure in
        // an ink frame, which the pointer fills red: the way in.
        Rectangle {
            id: mark
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            color: userStrip.signedIn ? Theme.text
                   : userHover.hovered ? Theme.accent : "transparent"
            border.width: userStrip.signedIn ? 0 : Theme.ruleWidth
            border.color: userHover.hovered || userStrip.ended ? Theme.accent : Theme.text

            Text {
                visible: userStrip.signedIn && userStrip.initials.length > 0
                anchors.centerIn: parent
                text: userStrip.initials
                color: Theme.bg
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Theme.weightBlack
            }
            Icon {
                visible: !(userStrip.signedIn && userStrip.initials.length > 0)
                anchors.centerIn: parent
                width: 16
                height: 16
                name: "user"
                color: userStrip.signedIn ? Theme.bg
                       : userHover.hovered ? Theme.accentForeground
                       : userStrip.ended ? Theme.accent700 : Theme.text
            }
        }

        Column {
            anchors.left: mark.right
            anchors.leftMargin: Theme.space3
            anchors.right: tail.left
            anchors.rightMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            spacing: 1

            Text {
                width: parent.width
                text: userStrip.signedIn ? (userStrip.name.length > 0 ? userStrip.name : "YouTube Music")
                      : userStrip.ended ? "Sign in again"
                      : "Sign in to YouTube Music"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Bold
                color: userStrip.ended || (!userStrip.signedIn && userHover.hovered) ? Theme.accent700 : Theme.text
            }

            Text {
                width: parent.width
                text: userStrip.accountState === "checking" ? "CHECKING YOUR SIGN-IN…"
                      : userStrip.accountState === "unreachable" ? "YOUTUBE MUSIC · OFFLINE"
                      : userStrip.signedIn ? "YOUTUBE MUSIC · SIGNED IN"
                      : userStrip.ended ? "YOUR SESSION ENDED"
                      : "YOUR PLAYLISTS AND LIKES"
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.neutral600
            }
        }

        // Signed in, the account's settings under the pointer; signed out,
        // the arrow in.
        Item {
            id: tail
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6 - Theme.space1
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28

            IconButton {
                anchors.fill: parent
                visible: userStrip.signedIn && userHover.hovered
                side: 28
                iconName: "settings"
                iconSize: 14
                iconColor: Theme.neutral700
                onClicked: root.accountSettingsRequested()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Your YouTube Music account"
            }
            Icon {
                visible: !userStrip.signedIn
                anchors.centerIn: parent
                width: 16
                height: 16
                name: "log-in"
                color: userHover.hovered || userStrip.ended ? Theme.accent : Theme.neutral700
            }
        }
    }
}
