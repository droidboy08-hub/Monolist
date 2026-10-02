import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The sidebar folded to a rail: the owner's choice (2026-10-01), from the
// Spotify-like design. The brand's first letter, which opens the sidebar
// again; the views as icons; the playlists as their covers; and the account
// at the foot. Each is named under the pointer.
Rectangle {
    id: root

    property string currentView: "home"
    signal viewRequested(string view)
    signal expandRequested()
    signal newPlaylistRequested()
    signal signInRequested()

    color: Theme.bg
    implicitWidth: 72

    readonly property int railWidth: width - Theme.ruleWidth
    readonly property int coverSize: 44

    Rectangle {                       // right rule
        anchors.right: parent.right
        width: Theme.ruleWidth
        height: parent.height
        color: Theme.divider
    }

    // — brand —
    // Part of the window's title bar, as the sidebar's is; the mark opens
    // the sidebar again.
    Item {
        id: brand
        anchors.top: parent.top
        anchors.left: parent.left
        width: root.railWidth
        height: Theme.titleBarHeight

        WindowDragArea {
            anchors.fill: parent
        }

        Row {
            id: mark
            anchors.centerIn: parent

            Text {
                text: "M"
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                color: markHover.hovered ? Theme.accent700 : Theme.text
            }
            Text {
                text: "."
                font.family: Theme.fontFamily
                font.pixelSize: 28
                font.weight: Theme.weightBlack
                color: Theme.accent
            }
        }

        // A click on the mark opens the sidebar; a drag still moves the
        // window (the handlers only watch).
        Item {
            anchors.fill: mark
            anchors.margins: -Theme.space2

            HoverHandler { id: markHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.expandRequested() }
        }
        ToolTip.visible: markHover.hovered
        ToolTip.delay: 600
        ToolTip.text: "Open the sidebar"

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    // A square place in the rail: its plate under the pointer, its name in
    // a tooltip.
    component RailButton: Item {
        id: button
        property string label: ""
        signal clicked()
        signal menuRequested()

        width: root.railWidth
        height: 44

        Rectangle {
            anchors.centerIn: parent
            width: 44
            height: 44
            color: buttonHover.hovered ? Theme.surface : "transparent"

            Behavior on color {
                enabled: !buttonHover.hovered
                ColorAnimation { duration: Theme.quick }
            }
        }

        HoverHandler { id: buttonHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: button.clicked() }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: button.menuRequested()
        }
        ToolTip.visible: buttonHover.hovered
        ToolTip.delay: 400
        ToolTip.text: button.label
    }

    // — the views —
    Column {
        id: nav
        anchors.top: brand.bottom
        anchors.left: parent.left
        topPadding: Theme.space3
        bottomPadding: Theme.space3
        spacing: Theme.space1

        Repeater {
            model: [
                { icon: "home", view: "home", label: "Home" },
                { icon: "search", view: "search", label: "Search" },
                { icon: "library", view: "library", label: "Your Library" },
                { icon: "download", view: "downloads", label: "Downloads" },
                { icon: "settings", view: "settings", label: "Settings" }
            ]

            RailButton {
                id: navButton
                required property var modelData
                readonly property bool active: modelData.view === "library"
                                               ? root.currentView.indexOf("library") === 0
                                               : root.currentView === modelData.view
                label: modelData.label
                onClicked: root.viewRequested(modelData.view)

                Icon {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    name: navButton.modelData.icon
                    color: navButton.active ? Theme.accent700 : Theme.text
                }
            }
        }
    }

    Rectangle {
        id: navRule
        anchors.top: nav.bottom
        x: (root.railWidth - width) / 2
        width: 32
        height: Theme.ruleWidth
        color: Theme.divider
    }

    // — the playlists, as covers —
    // A cover in an ink frame is the one open.
    component CoverButton: RailButton {
        id: coverButton
        property var artworks: []
        property string plate: ""
        property bool active: false
        height: root.coverSize + Theme.space2

        Rectangle {
            visible: coverButton.active
            anchors.centerIn: parent
            width: root.coverSize + Theme.ruleWidth * 4
            height: width
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: Theme.text
        }
        CollectionCover {
            anchors.centerIn: parent
            width: root.coverSize
            height: root.coverSize
            artworks: coverButton.artworks
            plate: coverButton.plate
        }
    }

    ListView {
        id: covers
        anchors.top: navRule.bottom
        anchors.bottom: account.top
        anchors.left: parent.left
        width: root.railWidth
        topMargin: Theme.space3
        bottomMargin: Theme.space3
        spacing: Theme.space1
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Library.playlists

        header: Column {
            width: covers.width
            spacing: Theme.space1
            bottomPadding: Theme.space1

            CoverButton {
                label: "Liked songs"
                plate: "liked"
                active: root.currentView === "playlist:liked"
                onClicked: root.viewRequested("playlist:liked")
                onMenuRequested: Menus.openPlaylist("liked")
            }
            CoverButton {
                visible: AccountLibrary.shown
                label: "Liked on YouTube Music"
                plate: "liked"
                active: root.currentView === "playlist:ytliked"
                onClicked: root.viewRequested("playlist:ytliked")
                onMenuRequested: Menus.openPlaylist("ytliked")
            }
        }

        delegate: CoverButton {
            label: model.name
            artworks: model.artworks
            active: root.currentView === "playlist:" + model.playlistId
            onClicked: root.viewRequested("playlist:" + model.playlistId)
            onMenuRequested: Menus.openPlaylist(model.playlistId)
        }

        footer: CoverButton {
            label: "New playlist"
            plate: "new"
            onClicked: root.newPlaylistRequested()
        }

        SmoothWheel { flickable: covers }
    }

    // — the account —
    Item {
        id: account
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: root.railWidth
        height: 68

        readonly property string accountState: Account.state
        readonly property bool signedIn: accountState === "active" || accountState === "checking"
                                         || accountState === "unreachable"
        readonly property bool ended: accountState === "rejected"
        readonly property string initials: {
            const parts = Account.accountName.trim().split(/\s+/).filter(function(part) { return part.length > 0 })
            if (parts.length === 0)
                return ""
            const first = parts[0].charAt(0).toUpperCase()
            return parts.length > 1 ? first + parts[parts.length - 1].charAt(0).toUpperCase() : first
        }

        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        Rectangle {
            anchors.centerIn: parent
            width: 32
            height: 32
            color: account.signedIn ? Theme.text : accountHover.hovered ? Theme.accent : "transparent"
            border.width: account.signedIn ? 0 : Theme.ruleWidth
            border.color: accountHover.hovered || account.ended ? Theme.accent : Theme.text

            Text {
                visible: account.signedIn && account.initials.length > 0
                anchors.centerIn: parent
                text: account.initials
                color: Theme.bg
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Theme.weightBlack
            }
            Icon {
                visible: !(account.signedIn && account.initials.length > 0)
                anchors.centerIn: parent
                width: 16
                height: 16
                name: "user"
                color: account.signedIn ? Theme.bg
                       : accountHover.hovered ? Theme.accentForeground
                       : account.ended ? Theme.accent700 : Theme.text
            }
        }

        HoverHandler { id: accountHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: {
                if (account.signedIn)
                    root.viewRequested("library")
                else
                    root.signInRequested()
            }
        }
        ToolTip.visible: accountHover.hovered
        ToolTip.delay: 400
        ToolTip.text: account.signedIn ? (Account.accountName.length > 0 ? Account.accountName : "YouTube Music")
                      : account.ended ? "Sign in again" : "Sign in to YouTube Music"
    }
}
