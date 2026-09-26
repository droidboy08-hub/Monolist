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
    // A playlist just made, whose page opens with its name ready to type; or
    // one whose Rename was chosen from its menu away from its page.
    property int pendingRename: 0
    // A playlist whose Delete was chosen from its menu, whose page opens
    // asking to confirm it.
    property int pendingDelete: 0
    // Now Playing covers everything above the player bar.
    property bool nowPlayingOpen: false
    // The part of Settings a link asked for, until Settings has scrolled to it.
    property string settingsSection: ""

    // — the picture —
    // Where the video goes while there is one: full screen when asked for,
    // Now Playing while that is open, and the mini panel above the player bar
    // otherwise. One place at a time, and always one: a picture nobody can see
    // is not decoded, so closing Now Playing moves it rather than hiding it,
    // and the mini panel's close button is the switch turning off.
    property bool videoFullscreen: false
    readonly property string videoPlace: !Player.videoWanted && !Player.videoPlaying ? ""
                                       : videoFullscreen ? "fullscreen"
                                       : nowPlayingOpen ? "nowplaying"
                                       : "mini"
    // How the window was before full screen, to go back to.
    property int visibilityBeforeFullscreen: Window.Windowed

    // Asked for with nothing playing yet, it is asked for with the picture
    // too: F on a music video means "watch it", not "turn the switch on first".
    function enterVideoFullscreen() {
        if (videoFullscreen || !Player.videoAvailable)
            return
        if (!Player.videoWanted)
            Player.videoWanted = true
        visibilityBeforeFullscreen = window.visibility
        videoFullscreen = true
        if (window.visibility !== Window.FullScreen)
            window.showFullScreen()
    }

    // Back to how the window was. One that was full screen already — a Mac
    // window in a Space of its own, from its green button — stays so: only
    // the picture leaves.
    function leaveVideoFullscreen() {
        if (!videoFullscreen)
            return
        videoFullscreen = false
        if (visibilityBeforeFullscreen === Window.FullScreen)
            return
        if (visibilityBeforeFullscreen === Window.Maximized)
            window.showMaximized()
        else
            window.showNormal()
    }

    // Left some other way — the system's own full-screen control, say.
    onVisibilityChanged: {
        if (window.videoFullscreen && window.visibility !== Window.FullScreen)
            window.videoFullscreen = false
    }

    Component.onCompleted: {
        if (initialQuery.length > 0)
            topBar.searchText = initialQuery
        openCurrentPage()
    }

    // Views are named: "home", "search", "downloads", "library[:tab]",
    // "page:<browse id>" for an album or a YouTube Music playlist,
    // "artist:<channel id>" for an artist, "artistname:<name>" for one known
    // only by name until it is looked up, "shelf:<browse id>[|<params>]" for
    // a shelf's "show all", "recs:<key>" for a suggestion shelf's (the page
    // it was on, its place there, its kind and title: Recs.moreKey), and
    // "playlist:<id>" or "playlist:liked" for the user's own. Back and forward
    // step through them.
    function openPage(browseId) {
        nowPlayingOpen = false
        navigate("page:" + browseId)
    }

    function openSuggestions(shelf) {
        const key = Recs.moreKey(shelf)
        if (key.length === 0)
            return
        nowPlayingOpen = false
        navigate("recs:" + key)
    }

    // The shelf's own title, which heads its page while the page loads. Read
    // as the view changes and not kept, as pendingArtistName is.
    property string pendingListingTitle: ""

    function openListing(browseId, params, title) {
        nowPlayingOpen = false
        pendingListingTitle = title ? title : ""
        navigate("shelf:" + browseId + (params ? "|" + params : ""))
        pendingListingTitle = ""
    }

    // The name a link was showing, so the page it opens is headed with it
    // before the rest has loaded.
    property string pendingArtistName: ""

    // An artist's name, clicked anywhere. Its page when the name came with
    // one, or when an earlier answer linked that name; otherwise the page
    // opens on the name and looks it up. Now Playing covers the page, so it
    // closes, as it does for Search.
    function openArtist(name, browseId) {
        nowPlayingOpen = false
        var id = browseId ? browseId : Artists.idFor(name)
        // Read as the view changes (openCurrentPage), and not kept past
        // this call: a link to the page already open changes nothing, and
        // its name must not head the next artist opened some other way.
        pendingArtistName = name ? name : ""
        if (id)
            navigate("artist:" + id)
        else if (name)
            navigate("artistname:" + name)
        pendingArtistName = ""
    }

    // Swaps the entry being shown for another without adding a step to the
    // history: a name that has been looked up becomes its page, so Back does
    // not walk into the lookup again.
    function replaceView(view) {
        currentView = view
    }

    function openCurrentPage() {
        if (currentView.indexOf("page:") === 0)
            Catalog.openPage(currentView.substring(5))
        else if (currentView.indexOf("artist:") === 0)
            Catalog.openArtist(currentView.substring(7), pendingArtistName)
        else if (currentView.indexOf("artistname:") === 0)
            Catalog.openArtistNamed(currentView.substring(11))
        else if (currentView.indexOf("shelf:") === 0) {
            const key = currentView.substring(6)
            const bar = key.indexOf("|")
            Catalog.openListing(bar < 0 ? key : key.substring(0, bar), bar < 0 ? "" : key.substring(bar + 1),
                                pendingListingTitle)
        }
        else if (currentView.indexOf("playlist:") === 0 && currentView !== "playlist:liked")
            Library.openPlaylist(parseInt(currentView.substring(9)))
        else if (currentView.indexOf("recs:") === 0)
            Recs.openMore(currentView.substring(5))
    }

    function createPlaylist() {
        var created = Library.createPlaylist("")
        if (created <= 0)
            return
        pendingRename = created
        navigate("playlist:" + created)
    }

    // Rename and Delete from a playlist's menu in the sidebar or on its card:
    // both are done on its page, so they go there first. Now Playing covers
    // the page, so it closes.
    function renamePlaylist(playlistId) {
        nowPlayingOpen = false
        pendingRename = playlistId
        navigate("playlist:" + playlistId)
    }
    function deletePlaylist(playlistId) {
        nowPlayingOpen = false
        pendingDelete = playlistId
        navigate("playlist:" + playlistId)
    }

    readonly property string libraryTab: currentView.indexOf("library:") === 0 ? currentView.substring(8) : "playlists"

    onCurrentViewChanged: {
        openCurrentPage()
        // A like or a new listen since Search was last open changes what it
        // should suggest. This rebuilds only when something did change.
        if (currentView === "search")
            Recs.refresh()
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
        sidebarOverlayOpen = false;
    }

    // Search asked for by name, from the sidebar or with Ctrl+F: the page, and
    // the cursor in the field ready to type. Not what navigate("search")
    // does for typing, which is already in the field and must not have its
    // text selected under it. Now Playing covers the field, so it closes.
    function openSearch() {
        nowPlayingOpen = false
        navigate("search")
        topBar.focusSearch()
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
                  : currentView === "settings" ? "SETTINGS"
                  : currentView.indexOf("page:") === 0 ? (Catalog.page.type === "playlist" ? "PLAYLIST" : "ALBUM")
                  : currentView.indexOf("artist") === 0 ? "ARTIST"
                  : currentView.indexOf("shelf:") === 0 ? "SHOW ALL"
                  : currentView.indexOf("recs:") === 0 ? "SEARCH / SHOW ALL"
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

        // Every `ToolTip.text` in the app shows the same single tool tip, which
        // Qt makes in the Basic style: the system's font, on the style's own
        // colours. Printed like the toast instead — paper type on ink, in
        // Archivo, square; its 1px frame takes the fill's colour, so it is one
        // flat block. Asked for here rather than on the window because the
        // attached ToolTip only attaches to an Item.
        Component.onCompleted: {
            const tip = ToolTip.toolTip
            if (!tip)
                return
            tip.font = Qt.font({ family: Theme.fontFamily, pixelSize: 12, weight: Theme.weightMedium })
            tip.palette.toolTipBase = Theme.text
            tip.palette.toolTipText = Theme.bg
            tip.palette.dark = Theme.text
            tip.horizontalPadding = Theme.space2
        }

        Sidebar {
            id: dockedSidebar
            visible: window.sidebarDocked
            width: visible ? Theme.sidebarWidth : 0
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            currentView: window.currentView
            onViewRequested: function(view) {
                if (view === "search")
                    window.openSearch()
                else
                    window.navigate(view)
            }
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
            onMenuRequested: window.sidebarOverlayOpen = true
            onBackRequested: window.goBack()
            onForwardRequested: window.goForward()
            onSearchActivated: function(term) { window.navigate("search") }
        }

        // The queue pushes the content aside rather than covering it: what it
        // is next to is the point. Its width is the animation, so the view
        // beside it reflows with it.
        QueuePanel {
            id: queuePanel
            readonly property int openWidth: Math.min(380, Math.max(300, window.width * 0.26))

            visible: width > 0
            width: window.queueOpen ? openWidth : 0
            clip: true
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            onCloseRequested: window.queueOpen = false

            Behavior on width {
                NumberAnimation {
                    duration: window.queueOpen ? Theme.page : Theme.leaving
                    easing.type: window.queueOpen ? Theme.enterCurve : Theme.exitCurve
                }
            }
        }

        Item {
            id: mainArea
            anchors.left: dockedSidebar.right
            anchors.right: queuePanel.visible ? queuePanel.left : parent.right
            anchors.top: topBar.bottom
            anchors.bottom: parent.bottom

            // Changing page replaces the content and fades the new one in:
            // pages are not laid out side by side, so sliding would be a lie,
            // and it would cost a third of a second on every navigation.
            component ViewFade: Item {
                property bool shown: false
                anchors.fill: parent
                visible: opacity > 0
                opacity: shown ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Theme.quick }
                }
            }

            Item {
                id: viewStack
                anchors.fill: parent

                ViewFade {
                    shown: window.currentView === "home"

                    HomeView {
                        anchors.fill: parent
                        onPageRequested: function(browseId) { window.openPage(browseId) }
                        onViewRequested: function(view) { window.navigate(view) }
                    }
                }

                ViewFade {
                    shown: window.currentView.indexOf("page:") === 0

                    PageView {
                        anchors.fill: parent
                    }
                }

                ViewFade {
                    shown: window.currentView.indexOf("artist:") === 0
                           || window.currentView.indexOf("artistname:") === 0

                    ArtistView {
                        anchors.fill: parent
                        onPageRequested: function(browseId) { window.openPage(browseId) }
                    }
                }

                ViewFade {
                    shown: window.currentView.indexOf("shelf:") === 0

                    ShelfView {
                        anchors.fill: parent
                    }
                }

                ViewFade {
                    shown: window.currentView.indexOf("recs:") === 0

                    RecsView {
                        anchors.fill: parent
                    }
                }

                ViewFade {
                    shown: window.currentView === "search"

                    SearchView {
                        anchors.fill: parent
                        term: topBar.searchText
                        // Its link names the catalogue folder, so Settings
                        // opens on the section that holds it.
                        onSettingsRequested: {
                            window.settingsSection = "recommendations"
                            window.navigate("settings")
                        }
                    }
                }

                ViewFade {
                    shown: window.currentView.indexOf("library") === 0

                    LibraryView {
                        anchors.fill: parent
                        tab: window.libraryTab
                        onTabRequested: function(tab) { window.navigate(tab === "playlists" ? "library" : "library:" + tab) }
                        onViewRequested: function(view) { window.navigate(view) }
                        onPageRequested: function(browseId) { window.openPage(browseId) }
                        onNewPlaylistRequested: window.createPlaylist()
                    }
                }

                ViewFade {
                    id: playlistFade
                    shown: window.currentView.indexOf("playlist:") === 0

                    PlaylistView {
                        anchors.fill: parent
                        key: playlistFade.shown ? window.currentView.substring(9) : ""
                        renameOnOpen: window.pendingRename > 0 && key === String(window.pendingRename)
                        onRenameStarted: window.pendingRename = 0
                        confirmDeleteOnOpen: window.pendingDelete > 0 && key === String(window.pendingDelete)
                        onDeleteAsked: window.pendingDelete = 0
                        onDeleted: window.goBack()
                    }
                }

                ViewFade {
                    shown: window.currentView === "downloads"

                    DownloadsView {
                        anchors.fill: parent
                    }
                }

                ViewFade {
                    id: settingsFade
                    // Built the first time it is opened, then kept. Settings
                    // asks each bundled tool for its version when it is made,
                    // and a page nobody has opened has no business starting
                    // processes while the window is coming up.
                    property bool opened: false
                    shown: window.currentView === "settings"
                    // Any change of `shown` means the page is on screen now
                    // or was a moment ago. Latched here rather than from the
                    // Loader's own onLoaded, which would feed `active` while
                    // it is still being set.
                    onShownChanged: opened = true

                    Loader {
                        anchors.fill: parent
                        active: settingsFade.shown || settingsFade.opened
                        sourceComponent: Component {
                            SettingsView {
                                section: window.settingsSection
                                onSectionRevealed: window.settingsSection = ""
                            }
                        }
                    }
                }
            }
        }
    }

    // Above Now Playing, so that view slides out from behind the bar and
    // tucks back behind it: it is the bar enlarged, not a page over it.
    NowPlayingBar {
        id: playerBar
        z: 850
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // Over the picture in full screen, and gone with the rest of its
        // chrome once the pointer has been still a moment.
        readonly property bool tucked: window.videoFullscreen && !fullscreenVideo.chromeShown
        opacity: tucked ? 0 : 1
        visible: opacity > 0
        Behavior on opacity {
            NumberAnimation {
                duration: playerBar.tucked ? Theme.normal : Theme.quick
                easing.type: playerBar.tucked ? Theme.exitCurve : Theme.enterCurve
            }
        }
        HoverHandler { id: barHover }
        // Now Playing covers the docked queue, so there the button shows the
        // queue where it can be seen: the view's own UP NEXT pane.
        queueOpen: window.nowPlayingOpen ? nowPlaying.pane === "queue" : window.queueOpen
        nowPlayingOpen: window.nowPlayingOpen
        onQueueToggled: {
            if (window.nowPlayingOpen)
                nowPlaying.pane = nowPlaying.pane === "queue" ? "lyrics" : "queue"
            else
                window.queueOpen = !window.queueOpen
        }
        onNowPlayingToggled: window.nowPlayingOpen = !window.nowPlayingOpen
    }

    // — Now Playing —
    // It rises from the player bar, because it is that bar enlarged: it comes
    // from where the bar is, and goes back there, a little faster.
    NowPlayingView {
        id: nowPlaying
        width: parent.width
        height: playerBar.y
        y: window.nowPlayingOpen ? 0 : height
        visible: y < height
        z: 800
        videoHere: window.videoPlace === "nowplaying"
        onCloseRequested: window.nowPlayingOpen = false
        onFullscreenRequested: window.enterVideoFullscreen()

        Behavior on y {
            NumberAnimation {
                duration: window.nowPlayingOpen ? Theme.page : Theme.leaving
                easing.type: window.nowPlayingOpen ? Theme.enterCurve : Theme.exitCurve
            }
        }
    }

    // — the picture, while Now Playing is closed —
    // Under Now Playing, which rises over it and takes the picture as it
    // comes, and over the page and the queue, beside which it stands.
    MiniVideo {
        id: miniVideo
        z: 790
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6 + (queuePanel.visible ? queuePanel.width : 0)
        anchors.bottom: playerBar.top
        anchors.bottomMargin: Theme.space6
        active: window.videoPlace === "mini" && Player.videoPlaying
        onOpenRequested: window.nowPlayingOpen = true
        onFullscreenRequested: window.enterVideoFullscreen()
    }

    // The pages leave room for it below their ends while it is up, so the
    // rows it stands over can be scrolled clear (Nav.pageClearance).
    Binding {
        target: Nav
        property: "pageClearance"
        value: miniVideo.active ? miniVideo.height + miniVideo.anchors.bottomMargin : 0
    }

    // — the picture, full screen —
    // Over everything but the player bar, which stands on it while the
    // pointer moves, and the app's own answers (the toast).
    FullscreenVideo {
        id: fullscreenVideo
        anchors.fill: parent
        z: 840
        active: window.videoPlace === "fullscreen"
        barHovered: barHover.hovered
        onLeaveRequested: window.leaveVideoFullscreen()
    }

    // Lyrics are looked up only while they are on screen.
    Binding {
        target: Lyrics
        property: "active"
        value: window.nowPlayingOpen && nowPlaying.pane === "lyrics"
    }

    // — narrow-window sidebar —
    // It comes in from the left edge, where the docked sidebar lives, over a
    // dimmed page: it is the same sidebar, arriving rather than appearing.
    property bool sidebarOverlayOpen: false

    Rectangle {
        anchors.fill: parent
        color: "#66201e1d"
        visible: opacity > 0
        opacity: window.sidebarOverlayOpen ? 1 : 0
        // Over the player bar as well: the sidebar is asked for, and covers
        // the window until it is answered.
        z: 870
        TapHandler { onTapped: window.sidebarOverlayOpen = false }

        Behavior on opacity {
            NumberAnimation { duration: Theme.quick }
        }
    }

    Sidebar {
        id: overlaySidebar
        width: Math.min(Theme.sidebarWidth, window.width - 48)
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        x: window.sidebarOverlayOpen ? 0 : -width
        visible: x > -width
        z: 871
        currentView: window.currentView
        showCloseButton: true
        onCloseRequested: window.sidebarOverlayOpen = false
        onViewRequested: function(view) {
            window.sidebarOverlayOpen = false
            if (view === "search")
                window.openSearch()
            else
                window.navigate(view)
        }
        onNewPlaylistRequested: {
            window.sidebarOverlayOpen = false
            window.createPlaylist()
        }

        Behavior on x {
            NumberAnimation {
                duration: window.sidebarOverlayOpen ? Theme.page : Theme.leaving
                easing.type: window.sidebarOverlayOpen ? Theme.enterCurve : Theme.exitCurve
            }
        }
    }

    // — confirmations —
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: playerBar.top
        anchors.bottomMargin: Theme.space6
        // Over everything: it is the app answering, and it leaves by itself.
        z: 950
    }

    Connections {
        target: Library
        function onNotice(text) { toast.show(text) }
    }

    // — menus —
    // One of each for the whole window, opened through Menus from wherever a
    // song, a card or a playlist was right-clicked or its dots pressed.
    TrackMenu { id: trackMenu }
    CardMenu { id: cardMenu }
    PlaylistMenu {
        id: playlistMenu
        onRenameRequested: function(playlistId) { window.renamePlaylist(playlistId) }
        onDeleteRequested: function(playlistId) { window.deletePlaylist(playlistId) }
    }

    Connections {
        target: Menus
        function onTrackRequested(track, context) { trackMenu.show(track, context) }
        function onCardRequested(card, origin) { cardMenu.show(card, origin) }
        function onPlaylistRequested(key) { playlistMenu.show(key, null) }
    }

    // — links —
    // An artist's name or an album's title, clicked in any list, bar or page.
    Connections {
        target: Nav
        function onArtistRequested(name, browseId) { window.openArtist(name, browseId) }
        function onPageRequested(browseId) { window.openPage(browseId) }
        function onListingRequested(browseId, params, title) { window.openListing(browseId, params, title) }
        function onSuggestionsRequested(shelf) { window.openSuggestions(shelf) }
    }

    Connections {
        target: Catalog
        function onNotice(text) { toast.show(text) }
        // A card's play button: its album or playlist, fetched, played from
        // the top.
        function onCollectionReady(origin, tracks) {
            if (tracks.length > 0)
                Player.playTracks(tracks, 0, origin)
        }
        // A name looked up: its page takes the lookup's place in the history.
        function onArtistResolved(name, browseId) {
            if (window.currentView === "artistname:" + name)
                window.replaceView("artist:" + browseId)
        }
        // YouTube Music has no artist of that name: the search for it is the
        // next best thing, in the lookup's place.
        function onArtistNotFound(name) {
            if (window.currentView !== "artistname:" + name)
                return
            topBar.searchText = name
            window.replaceView("search")
        }
    }

    Connections {
        target: Recs
        function onNotice(text) { toast.show(text) }
        // "Not interested" and "Don't suggest", which can be taken back.
        function onUndoable(text) {
            toast.show(text, "UNDO", function() { Recs.undoNotInterested() })
        }
        // A suggestion's menu entry, once the name has been found as a song:
        // "<action>|<argument>", as TrackMenu asked (RecShelf).
        function onResolved(purpose, track) {
            const bar = purpose.indexOf("|")
            trackMenu.run(bar < 0 ? purpose : purpose.substring(0, bar),
                          bar < 0 ? "" : purpose.substring(bar + 1), track)
        }
    }

    // The YouTube Music sign-in's answers: a file deleted or kept.
    Connections {
        target: Account
        function onNotice(text) { toast.show(text) }
    }

    Connections {
        target: Player
        function onNotice(text) { toast.show(text) }
        // A track that will not play used to fail in complete silence: the
        // status line is only shown while a track is resolving, and failing is
        // the moment that stops. Say it out loud.
        function onPlaybackError(reason) { toast.show(reason) }
        // The picture gone — the switch turned off, the next song begun, a
        // video that would not play — takes full screen with it.
        function onVideoChanged() {
            if (!Player.videoWanted && !Player.videoPlaying)
                window.leaveVideoFullscreen()
        }
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
    Shortcut {
        sequences: [StandardKey.Find]
        onActivated: window.openSearch()
    }
    // Full screen leaves first; Now Playing, under it, stays open.
    Shortcut {
        sequence: "Esc"
        enabled: window.nowPlayingOpen || window.videoFullscreen
        onActivated: {
            if (window.videoFullscreen)
                window.leaveVideoFullscreen()
            else
                window.nowPlayingOpen = false
        }
    }
    // The picture full screen, and back; with no picture on yet, the picture
    // too. Not while a search is being typed.
    Shortcut {
        sequence: "F"
        enabled: !topBar.searchFocused && (window.videoFullscreen || Player.videoAvailable)
        onActivated: {
            if (window.videoFullscreen)
                window.leaveVideoFullscreen()
            else
                window.enterVideoFullscreen()
        }
    }
    // The cover / video switch in Now Playing, from the keyboard.
    Shortcut {
        sequence: "V"
        enabled: window.nowPlayingOpen && Player.videoAvailable && !topBar.searchFocused
        onActivated: Player.videoWanted = !Player.videoWanted
    }
}
