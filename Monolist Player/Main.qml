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
    // Shown from C++ once WindowChrome has replaced the system title bar, so
    // the window is never drawn with one.
    visible: false
    title: "Monolist"
    color: Theme.bg

    property string currentView: "home"
    property var viewHistory: []
    property var viewFuture: []
    // Set from the command line (--query), to open with a search typed in.
    property string initialQuery: ""
    // The queue docks at the right, like a second sidebar.
    property bool queueOpen: false
    // A playlist just made, whose page opens with its name ready to type.
    property int pendingRename: 0

    Component.onCompleted: {
        if (initialQuery.length > 0)
            topBar.searchText = initialQuery
        openCurrentPage()
    }

    // Views are named: "home", "search", "downloads", "library[:tab]",
    // "page:<browse id>" for an album or a YouTube Music playlist, and
    // "playlist:<id>" or "playlist:liked" for the user's own. Back and
    // forward step through them.
    function openPage(browseId) {
        navigate("page:" + browseId)
    }

    function openCurrentPage() {
        if (currentView.indexOf("page:") === 0)
            Catalog.openPage(currentView.substring(5))
        else if (currentView.indexOf("playlist:") === 0 && currentView !== "playlist:liked")
            Library.openPlaylist(parseInt(currentView.substring(9)))
    }

    function createPlaylist() {
        var created = Library.createPlaylist("")
        if (created <= 0)
            return
        pendingRename = created
        navigate("playlist:" + created)
    }

    readonly property string libraryTab: currentView.indexOf("library:") === 0 ? currentView.substring(8) : "playlists"

    onCurrentViewChanged: openCurrentPage()

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
                  : currentView === "downloads" ? "DOWNLOADS"
                  : currentView.indexOf("page:") === 0 ? (Catalog.page.type === "playlist" ? "PLAYLIST" : "ALBUM")
                  : currentView === "playlist:liked" ? "YOUR LIBRARY / LIKED SONGS"
                  : currentView.indexOf("playlist:") === 0 ? "YOUR LIBRARY / PLAYLIST"
                  : "YOUR LIBRARY";
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
            onNewPlaylistRequested: window.createPlaylist()
        }

        // The top bar spans to the window's right edge, so its window buttons
        // keep the top-right corner whether or not the queue is open.
        TopBar {
            id: topBar
            anchors.top: parent.top
            anchors.left: dockedSidebar.right
            anchors.right: parent.right
            breadcrumb: window.breadcrumbText()
            showMenuButton: !window.sidebarDocked
            onMenuRequested: overlaySidebar.visible = true
            onBackRequested: window.goBack()
            onForwardRequested: window.goForward()
            onSearchActivated: function(term) { window.navigate("search") }
        }

        QueuePanel {
            id: queuePanel
            visible: window.queueOpen
            width: visible ? Math.min(380, Math.max(300, window.width * 0.26)) : 0
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            onCloseRequested: window.queueOpen = false
        }

        Item {
            id: mainArea
            anchors.left: dockedSidebar.right
            anchors.right: queuePanel.visible ? queuePanel.left : parent.right
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom

            Item {
                id: viewStack
                anchors.fill: parent

                HomeView {
                    anchors.fill: parent
                    visible: window.currentView === "home"
                    onPageRequested: function(browseId) { window.openPage(browseId) }
                    onSearchRequested: function(term) {
                        topBar.searchText = term
                        window.navigate("search")
                    }
                }

                PageView {
                    anchors.fill: parent
                    visible: window.currentView.indexOf("page:") === 0
                }

                SearchView {
                    anchors.fill: parent
                    visible: window.currentView === "search"
                    term: topBar.searchText
                }

                LibraryView {
                    anchors.fill: parent
                    visible: window.currentView.indexOf("library") === 0
                    tab: window.libraryTab
                    onTabRequested: function(tab) { window.navigate(tab === "playlists" ? "library" : "library:" + tab) }
                    onViewRequested: function(view) { window.navigate(view) }
                    onPageRequested: function(browseId) { window.openPage(browseId) }
                    onNewPlaylistRequested: window.createPlaylist()
                }

                PlaylistView {
                    anchors.fill: parent
                    visible: window.currentView.indexOf("playlist:") === 0
                    key: visible ? window.currentView.substring(9) : ""
                    renameOnOpen: window.pendingRename > 0 && key === String(window.pendingRename)
                    onRenameStarted: window.pendingRename = 0
                    onDeleted: window.goBack()
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
        onNewPlaylistRequested: window.createPlaylist()
    }

    // — confirmations —
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: playerBar.top
        anchors.bottomMargin: Theme.space6
        z: 900
    }

    Connections {
        target: Library
        function onNotice(text) { toast.show(text) }
    }

    // — resize edges —
    // Only where the platform frame is gone entirely (Linux): the edges resize
    // through the compositor, as a frame would. Windows and macOS keep their
    // own.
    Loader {
        anchors.fill: parent
        z: 1000
        active: Chrome.drawsResizeEdges && window.visibility !== Window.Maximized
                && window.visibility !== Window.FullScreen
        sourceComponent: Item {
            id: edges

            readonly property int grip: 6

            component Edge: MouseArea {
                property int edges: 0
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onPressed: window.startSystemResize(edges)
            }

            Edge { edges: Qt.LeftEdge; cursorShape: Qt.SizeHorCursor
                   x: 0; y: edges.grip; width: edges.grip; height: parent.height - edges.grip * 2 }
            Edge { edges: Qt.RightEdge; cursorShape: Qt.SizeHorCursor
                   x: parent.width - edges.grip; y: edges.grip; width: edges.grip; height: parent.height - edges.grip * 2 }
            Edge { edges: Qt.TopEdge; cursorShape: Qt.SizeVerCursor
                   x: edges.grip; y: 0; width: parent.width - edges.grip * 2; height: edges.grip }
            Edge { edges: Qt.BottomEdge; cursorShape: Qt.SizeVerCursor
                   x: edges.grip; y: parent.height - edges.grip; width: parent.width - edges.grip * 2; height: edges.grip }
            Edge { edges: Qt.TopEdge | Qt.LeftEdge; cursorShape: Qt.SizeFDiagCursor
                   x: 0; y: 0; width: edges.grip; height: edges.grip }
            Edge { edges: Qt.BottomEdge | Qt.RightEdge; cursorShape: Qt.SizeFDiagCursor
                   x: parent.width - edges.grip; y: parent.height - edges.grip; width: edges.grip; height: edges.grip }
            Edge { edges: Qt.TopEdge | Qt.RightEdge; cursorShape: Qt.SizeBDiagCursor
                   x: parent.width - edges.grip; y: 0; width: edges.grip; height: edges.grip }
            Edge { edges: Qt.BottomEdge | Qt.LeftEdge; cursorShape: Qt.SizeBDiagCursor
                   x: 0; y: parent.height - edges.grip; width: edges.grip; height: edges.grip }
        }
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
