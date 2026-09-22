import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "components"
import "views"

ApplicationWindow {
    id: window

    width: 1512
    height: 945
    minimumWidth: 480
    minimumHeight: 560
    visible: true
    title: "Monolist"
    color: Theme.bg

    property string currentView: "home"
    property var viewHistory: []
    property var viewFuture: []
    // Set from the command line (--query), to open with a search typed in.
    property string initialQuery: ""
    // The queue docks at the right, like a second sidebar.
    property bool queueOpen: false

    Component.onCompleted: {
        if (initialQuery.length > 0)
            topBar.searchText = initialQuery
    }

    // Below this width the sidebar leaves the layout and becomes an overlay —
    // the only concession the design makes to narrow windows.
    readonly property bool sidebarDocked: width >= Theme.sidebarBreakpoint

    function navigate(view) {
        if (view === currentView)
            return;
        viewHistory.push(currentView);
        viewFuture = [];
        currentView = view;
        overlaySidebar.visible = false;
    }

    function goBack() {
        if (viewHistory.length === 0)
            return;
        viewFuture.push(currentView);
        currentView = viewHistory.pop();
    }

    function goForward() {
        if (viewFuture.length === 0)
            return;
        viewHistory.push(currentView);
        currentView = viewFuture.pop();
    }

    function breadcrumbText() {
        var label = currentView === "home" ? "HOME"
                  : currentView === "search" ? "SEARCH"
                  : currentView === "downloads" ? "DOWNLOADS" : "YOUR LIBRARY";
        return label + " / " + Qt.formatDate(new Date(), "dddd d MMMM yyyy").toUpperCase();
    }

    Item {
        id: shell
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: playerBar.top

        Sidebar {
            id: dockedSidebar
            visible: window.sidebarDocked
            width: visible ? Theme.sidebarWidth : 0
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            currentView: window.currentView
            onViewRequested: function(view) { window.navigate(view) }
        }

        QueuePanel {
            id: queuePanel
            visible: window.queueOpen
            width: visible ? Math.min(380, Math.max(300, window.width * 0.26)) : 0
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            onCloseRequested: window.queueOpen = false
        }

        Item {
            id: mainArea
            anchors.left: dockedSidebar.right
            anchors.right: queuePanel.visible ? queuePanel.left : parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            TopBar {
                id: topBar
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                breadcrumb: window.breadcrumbText()
                showMenuButton: !window.sidebarDocked
                onMenuRequested: overlaySidebar.visible = true
                onBackRequested: window.goBack()
                onForwardRequested: window.goForward()
                onSearchActivated: function(term) { window.navigate("search") }
            }

            Item {
                id: viewStack
                anchors.top: topBar.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                HomeView {
                    anchors.fill: parent
                    visible: window.currentView === "home"
                    onViewRequested: function(view) { window.navigate(view) }
                }

                SearchView {
                    anchors.fill: parent
                    visible: window.currentView === "search"
                    term: topBar.searchText
                }

                LibraryView {
                    anchors.fill: parent
                    visible: window.currentView === "library"
                }

                DownloadsView {
                    anchors.fill: parent
                    visible: window.currentView === "downloads"
                }
            }
        }
    }

    NowPlayingBar {
        id: playerBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        queueOpen: window.queueOpen
        onQueueToggled: window.queueOpen = !window.queueOpen
    }

    // — narrow-window sidebar —
    Rectangle {
        anchors.fill: parent
        color: "#66201e1d"
        visible: overlaySidebar.visible
        TapHandler { onTapped: overlaySidebar.visible = false }
    }

    Sidebar {
        id: overlaySidebar
        visible: false
        width: Math.min(Theme.sidebarWidth, window.width - 48)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        currentView: window.currentView
        showCloseButton: true
        onCloseRequested: visible = false
        onViewRequested: function(view) { window.navigate(view) }
    }

    // — keyboard —
    Shortcut {
        sequence: "Space"
        enabled: !topBar.searchFocused
        onActivated: Player.togglePlay()
    }
    Shortcut { sequence: "Ctrl+Right"; onActivated: Player.next() }
    Shortcut { sequence: "Ctrl+Left"; onActivated: Player.previous() }
    // StandardKey.Find maps to several sequences (Ctrl+F, F3); `sequences`
    // binds all of them, `sequence` would silently take only the first.
    Shortcut { sequences: [StandardKey.Find]; onActivated: window.navigate("search") }
}
