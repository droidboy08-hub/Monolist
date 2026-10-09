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

    // Closing the window to the system tray, the music playing on (Tray),
    // unless Settings turned that off or Monolist is ending (Quit, or Windows
    // signing out). Full screen is left first, so the window comes back as a
    // window.
    onClosing: function(close) {
        if (!Tray.available || !Tray.closeToTray || Tray.ending)
            return
        close.accepted = false
        if (window.videoFullscreen)
            window.leaveVideoFullscreen()
        window.hide()
        Tray.hidden()
    }

    property string currentView: "home"
    // Replaced, never changed in place, so what reads their length (the top
    // bar's arrows) hears of every step.
    property var viewHistory: []
    property var viewFuture: []
    readonly property bool canGoBack: viewHistory.length > 0
    readonly property bool canGoForward: viewFuture.length > 0
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
    // Opening Now Playing takes the picture back from the small panel; closing
    // it again leaves the picture off until the panel is asked for once more.
    onNowPlayingOpenChanged: if (nowPlayingOpen) videoPip = false
    // The part of Settings a link asked for, until Settings has scrolled to it.
    property string settingsSection: ""
    // Set from the command line (--effects-popup): the player bar's effects
    // popup open once the window is up, for a look at it.
    property bool effectsPopupAtStart: false

    // — ambient colour —
    // The playing song's colour, measured from its cover (CoverPalette),
    // eased from one song's to the next, and handed to the theme, which
    // washes the paper and the surfaces with it and glows it at the top of
    // the page (Settings > Appearance turns it off). A grey cover, or none,
    // gives none.
    readonly property string ambientCover: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""
    property color ambientColour: "transparent"
    onAmbientCoverChanged: measureAmbient()
    function measureAmbient() {
        if (ambientCover.length > 0)
            CoverPalette.request(ambientCover)
        else
            ambientColour = "transparent"
    }
    // The cover's hue at an even strength: never glaring, never mud.
    function ambientFrom(c) {
        if (c.hslSaturation < 0.08)
            return Qt.rgba(0, 0, 0, 0)
        return Qt.hsla(c.hslHue < 0 ? 0 : c.hslHue, Math.max(0.35, Math.min(0.85, c.hslSaturation)), 0.5, 1)
    }
    Connections {
        target: CoverPalette
        function onColourReady(source, colour) {
            if (source === window.ambientCover)
                window.ambientColour = window.ambientFrom(colour)
        }
    }
    // Atmosphere: nobody waits on it, so it takes its time.
    Behavior on ambientColour {
        ColorAnimation { duration: Theme.slow; easing.type: Theme.enterCurve }
    }
    Binding {
        target: Theme
        property: "ambient"
        value: Theme.ambientEnabled ? window.ambientColour : Qt.rgba(0, 0, 0, 0)
    }

    // — the picture —
    // Where the video goes while there is one: full screen when asked for,
    // Now Playing while that is open, and picture in picture only when its
    // button in Now Playing was pressed. Otherwise nowhere: the song carries
    // on as sound, and a picture nobody can see is not decoded. Keeping it
    // out of picture in picture unless asked keeps a second picture from
    // being drawn while Now Playing comes and goes.
    //
    // Picture in picture is the system's own window where there is one
    // (macOS), which is drawn outside this window and so costs it nothing;
    // the app's own panel (MiniVideo) otherwise, or if the system's would
    // not open.
    //
    // The video switch stays on from song to song (Player.videoPreferred),
    // and so does picture in picture: a song with no video shows its cover
    // there until one with a video comes, which plays in it.
    property bool videoFullscreen: false
    property bool videoPip: false
    property bool systemPipFailed: false
    readonly property bool useSystemPip: SystemPip.supported && !systemPipFailed
    readonly property bool videoOn: Player.videoWanted || Player.videoPlaying
    readonly property string videoPlace: videoFullscreen && videoOn ? "fullscreen"
                                       : nowPlayingOpen ? (videoOn ? "nowplaying" : "")
                                       : videoPip && (videoOn || Player.videoPreferred)
                                         ? (useSystemPip ? "system" : "mini")
                                       : ""
    // The system's window opens with picture in picture and closes as soon as
    // the picture goes anywhere else. Between songs it stays, on the cover.
    readonly property bool systemPipWanted: videoPlace === "system"
    onSystemPipWantedChanged: systemPipWanted ? SystemPip.start() : SystemPip.stop()

    Connections {
        target: SystemPip
        // Its own close button: the picture off, the song on as sound.
        function onStopped() { window.videoPip = false }
        // Its button back to the app: Now Playing, which takes the picture.
        function onRestoreRequested() { window.nowPlayingOpen = true }
        function onFailed(reason) {
            window.systemPipFailed = true
            window.say(reason + " — using the app's own")
        }
    }

    // Now Playing's picture to the small panel, and Now Playing away.
    function enterVideoPip() {
        videoPip = true
        nowPlayingOpen = false
    }
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
        // Before the first frame, so the window never shows the wrong one.
        Theme.mode = Library.settingValue("appearance", "system")
        Theme.ambientEnabled = Library.settingValue("ambient", "1") !== "0"
        Theme.monochrome = Library.settingValue("monochrome", "1") !== "0"
        sidebarCollapsed = Library.settingValue("sidebar.collapsed", "0") === "1"
        measureAmbient()
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
    // "playlist:<id>" or "playlist:liked" for the user's own, and
    // "playlist:ytliked" for the YouTube Music account's liked songs as its
    // last sync read them (AccountLibrary). Back and forward step through them.
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
        else if (currentView.indexOf("playlist:") === 0 && currentView !== "playlist:liked"
                 && currentView !== "playlist:ytliked")
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
        if (currentView === "search" || currentView === "home")
            Recs.refresh()
    }

    // Below this width the sidebar leaves the layout and becomes an overlay —
    // the only concession the design makes to narrow windows.
    readonly property bool sidebarDocked: width >= Theme.sidebarBreakpoint

    function navigate(view) {
        if (view === currentView)
            return;
        viewHistory = viewHistory.concat([currentView]);
        viewFuture = [];
        currentView = view;
        sidebarOverlayOpen = false;
    }

    // Search asked for by name, from the sidebar or with Ctrl+F: the page, and
    // the cursor in the field ready to type. Not what navigate("search")
    // does for typing, which is already in the field and must not have its
    // text selected under it. Now Playing covers the field, so it closes.
    // The docked sidebar folded to its rail (SidebarRail), as last left.
    property bool sidebarCollapsed: false
    property double sidebarToggledAt: 0
    function setSidebarCollapsed(collapsed) {
        // A double-click on the name is one fold, not a fold and an unfold.
        const now = Date.now()
        if (now - sidebarToggledAt < Application.styleHints.mouseDoubleClickInterval)
            return
        sidebarToggledAt = now
        sidebarCollapsed = collapsed
        Library.setSetting("sidebar.collapsed", collapsed ? "1" : "0")
    }
    function sideNavigate(view) {
        if (view === "search")
            openSearch()
        else
            navigate(view)
    }

    // Signing in to YouTube Music, from the sidebar's corner: Google's own
    // page in a window of Monolist's (SignIn); where there is none, or
    // Google will not sign in there, Settings, at the account's row, with
    // the import open.
    function signIn() {
        if (SignIn.available && !SignIn.refused)
            signInCaution.open()
        else
            openSignInImport()
    }

    function openSignInImport() {
        window.settingsSection = "signin"
        window.navigate("settings")
    }

    // The signed-in account's row and switches.
    function openAccountSettings() {
        window.settingsSection = "ytmusic"
        window.navigate("settings")
    }

    // Settings > Playback's EFFECTS block, from the player bar's popup. Now
    // Playing covers the page, so it goes down first.
    function openEffectsSettings() {
        window.nowPlayingOpen = false
        window.settingsSection = "effects"
        window.navigate("settings")
    }

    function openSearch() {
        nowPlayingOpen = false
        navigate("search")
        topBar.focusSearch()
    }

    function goBack() {
        if (viewHistory.length === 0)
            return;
        const back = viewHistory[viewHistory.length - 1];
        viewFuture = viewFuture.concat([currentView]);
        viewHistory = viewHistory.slice(0, -1);
        currentView = back;
    }

    function goForward() {
        if (viewFuture.length === 0)
            return;
        const ahead = viewFuture[viewFuture.length - 1];
        viewHistory = viewHistory.concat([currentView]);
        viewFuture = viewFuture.slice(0, -1);
        currentView = ahead;
    }

    // A playlist deleted on its page: back to the page before it, and the
    // playlist gone from the history both ways, so no arrow leads to a page
    // that is not there. Two steps that became one page are one step.
    function leaveDeletedPage() {
        const gone = currentView
        const tidy = function(views) {
            return views.filter(function(view, i) {
                return view !== gone && (i === 0 || view !== views[i - 1])
            })
        }
        // Twice: taking the playlist out can leave two of the same side by side.
        let history = tidy(tidy(viewHistory))
        viewFuture = tidy(tidy(viewFuture))
        const back = history.length > 0 ? history[history.length - 1] : "home"
        viewHistory = history.slice(0, -1)
        currentView = back
    }

    // Back from the keyboard or the mouse's side button: out of what covers
    // the page first, as Esc is (the picture full screen, then Now Playing),
    // and only then to the page before.
    function stepBack() {
        if (videoFullscreen)
            leaveVideoFullscreen()
        else if (nowPlayingOpen)
            nowPlayingOpen = false
        else
            goBack()
    }

    // Forward, the same way: a page reached with it is in front, as any page
    // opened is. Nothing moves when there is nowhere to go.
    function stepForward() {
        if (!canGoForward)
            return
        if (videoFullscreen)
            leaveVideoFullscreen()
        nowPlayingOpen = false
        goForward()
    }

    function breadcrumbText() {
        var label = currentView === "home" ? "HOME"
                  : currentView === "search" ? "SEARCH"
                  : currentView === "downloads" ? "DOWNLOADS"
                  : currentView === "settings" ? "SETTINGS"
                  : currentView.indexOf("page:") === 0 ? (Catalog.page.type === "playlist" ? "PLAYLIST" : "ALBUM")
                  : currentView.indexOf("artist") === 0 ? "ARTIST"
                  : currentView.indexOf("shelf:") === 0 ? "SHOW ALL"
                  : currentView.indexOf("recs:") === 0 ? "SUGGESTIONS / SHOW ALL"
                  : currentView === "playlist:liked" ? "YOUR LIBRARY / LIKED SONGS"
                  : currentView === "playlist:ytliked" ? "YOUR LIBRARY / LIKED ON YOUTUBE MUSIC"
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
            // Bound, so a change of appearance reprints it.
            tip.palette.toolTipBase = Qt.binding(function() { return Theme.text })
            tip.palette.toolTipText = Qt.binding(function() { return Theme.bg })
            tip.palette.dark = Qt.binding(function() { return Theme.text })
            tip.horizontalPadding = Theme.space2
        }

        // The docked sidebar, or folded to its rail (the name folds it, the
        // rail's mark opens it again; kept in the settings). Its width is
        // the animation, so the page beside it reflows with it. Not folded
        // where the window's buttons sit in its corner (a Mac's).
        Item {
            id: dockedSidebar
            readonly property bool folded: window.sidebarCollapsed && !Chrome.buttonsOnLeft
            // Only folding animates; crossing the docking breakpoint snaps.
            property real dockWidth: folded ? 72 : Theme.sidebarWidth
            visible: window.sidebarDocked
            width: visible ? dockWidth : 0
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            clip: true

            // Not at launch, where it opens as it was left.
            Behavior on dockWidth {
                enabled: window.visible
                NumberAnimation { duration: Theme.page; easing.type: Theme.moveCurve }
            }

            Sidebar {
                width: Theme.sidebarWidth
                height: parent.height
                visible: opacity > 0
                opacity: dockedSidebar.folded ? 0 : 1
                collapsible: !Chrome.buttonsOnLeft
                currentView: window.currentView
                onViewRequested: function(view) { window.sideNavigate(view) }
                onNewPlaylistRequested: window.createPlaylist()
                onSignInRequested: window.signIn()
                onAccountSettingsRequested: window.openAccountSettings()
                onCollapseRequested: window.setSidebarCollapsed(true)

                Behavior on opacity {
                    enabled: window.visible
                    NumberAnimation { duration: Theme.quick }
                }
            }

            SidebarRail {
                width: 72
                height: parent.height
                visible: opacity > 0
                opacity: dockedSidebar.folded ? 1 : 0
                currentView: window.currentView
                onViewRequested: function(view) { window.sideNavigate(view) }
                onNewPlaylistRequested: window.createPlaylist()
                onSignInRequested: window.signIn()
                onExpandRequested: window.setSidebarCollapsed(false)

                Behavior on opacity {
                    enabled: window.visible
                    NumberAnimation { duration: Theme.quick }
                }
            }
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
            canGoBack: window.canGoBack
            canGoForward: window.canGoForward
            onBackRequested: window.goBack()
            onForwardRequested: window.goForward()
            onSearchActivated: function(term) { window.navigate("search") }
            onSearchCommitted: function(term) { Library.rememberSearch(term) }
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
            covered: nowPlaying.settled

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

            // Ambient colour's glow: the song's colour at the head of the
            // page, gone a little way down, under every page.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: Math.min(parent.height * 0.6, 460)
                visible: Theme.ambient.a > 0
                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: Qt.rgba(Theme.ambient.r, Theme.ambient.g, Theme.ambient.b,
                                       Theme.ambient.a * (Theme.dark ? 0.24 : 0.17))
                    }
                    GradientStop {
                        position: 1
                        color: Qt.rgba(Theme.ambient.r, Theme.ambient.g, Theme.ambient.b, 0)
                    }
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
                        onSearchRequested: function(term) {
                            topBar.searchText = term
                            Library.rememberSearch(term)
                        }
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
                        onDeleted: window.leaveDeletedPage()
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
                                onSignInRequested: window.signIn()
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
        onEffectsSettingsRequested: window.openEffectsSettings()
        onMiniPlayerRequested: window.enterMini()
    }

    // — the mini player: in place of this window, never beside it —
    property bool miniMode: false

    MiniPlayer {
        id: miniPlayer
        onFullRequested: window.leaveMini()
        onCloseRequested: window.closeMini()
        onSoundSettingsRequested: {
            window.leaveMini()
            window.openEffectsSettings()
        }
    }

    function enterMini() {
        if (window.videoFullscreen)
            window.leaveVideoFullscreen()
        if (!window.miniMode)
            miniPlayer.place()
        window.miniMode = true
        miniPlayer.show()
        miniPlayer.raise()
        miniPlayer.requestActivate()
        window.hide()
    }

    function leaveMini() {
        window.miniMode = false
        miniPlayer.hide()
        window.bringBack(false)
    }

    // The mini player's close, its X or Windows' (Alt+F4, the taskbar): as
    // this window's (onClosing), the tray while that is on, else Monolist
    // quits.
    function closeMini() {
        if (Tray.available && Tray.closeToTray) {
            miniPlayer.hide()
            Tray.hidden()
        } else {
            Qt.quit()
        }
    }

    // Whichever is in use, shown and in front: the tray's icon, a second
    // start of Monolist (C++). `full`: the full window whatever was in use
    // (the tray's Open Monolist). A window hidden while maximised comes back
    // maximised; only a minimised one is restored.
    function bringBack(full) {
        if (full && window.miniMode) {
            window.leaveMini()
            return
        }
        const target = window.miniMode ? miniPlayer : window
        if (target.visibility === Window.Minimized)
            target.showNormal()
        else if (!target.visible)
            target.show()
        target.raise()
        target.requestActivate()
    }

    // The mini player; its own Shortcut of the same keys comes back.
    Shortcut {
        sequence: "Ctrl+Shift+M"
        onActivated: window.enterMini()
    }

    // --effects-popup: once the window has been laid out, so that the popup
    // stands where the button really is.
    Timer {
        interval: 800
        running: window.effectsPopupAtStart
        onTriggered: playerBar.showEffects()
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
        // The picture only once Now Playing is all the way up, and not from
        // the moment it starts closing: drawing video frames into a view that
        // is sliding is what made it stutter. The cover stands in meanwhile.
        readonly property bool settled: window.nowPlayingOpen && y === 0
        videoHere: window.videoPlace === "nowplaying" && settled
        onCloseRequested: window.nowPlayingOpen = false
        onFullscreenRequested: window.enterVideoFullscreen()
        onPipRequested: window.enterVideoPip()

        Behavior on y {
            NumberAnimation {
                duration: window.nowPlayingOpen ? Theme.page : Theme.leaving
                easing.type: window.nowPlayingOpen ? Theme.enterCurve : Theme.exitCurve
            }
        }
    }

    // — the picture, while Now Playing is closed —
    // Only when asked for, from its button in Now Playing. Under Now Playing,
    // and over the page and the queue, beside which it stands. It waits for
    // Now Playing to have gone down before it takes the picture.
    MiniVideo {
        id: miniVideo
        z: 790
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6 + (queuePanel.visible ? queuePanel.width : 0)
        anchors.bottom: playerBar.top
        anchors.bottomMargin: Theme.space6
        active: window.videoPlace === "mini" && !nowPlaying.visible
        onOpenRequested: window.nowPlayingOpen = true
        onFullscreenRequested: window.enterVideoFullscreen()
        onCloseRequested: window.videoPip = false
    }

    // The pages leave room for it below their ends while it is up, so the
    // rows it stands over can be scrolled clear (Nav.pageClearance).
    Binding {
        target: Nav
        property: "pageClearance"
        value: miniVideo.active ? miniVideo.height + miniVideo.anchors.bottomMargin : 0
    }
    Binding {
        target: Nav
        property: "pagesCovered"
        value: nowPlaying.settled || fullscreenVideo.active
    }

    // The corner mask every list cover shares (Nav.coverMask): one small
    // layer, stretched to each cover by its effect, rather than one each.
    Item {
        id: coverMask
        width: 40
        height: 40
        visible: false
        layer.enabled: true
        layer.smooth: true

        Rectangle {
            anchors.fill: parent
            radius: 6
            antialiasing: true
        }

        Component.onCompleted: Nav.coverMask = coverMask
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

    // Lyrics are put on show only while they are on screen; the lookups
    // themselves also run in the background (Lyrics.background).
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
        // Darker than the page in either appearance.
        color: Theme.dark ? Qt.rgba(0, 0, 0, 0.6) : "#66201e1d"
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
        onSignInRequested: {
            window.sidebarOverlayOpen = false
            window.signIn()
        }
        onAccountSettingsRequested: {
            window.sidebarOverlayOpen = false
            window.openAccountSettings()
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

    // The app's answers, on whichever window is in use: the mini player has
    // a toast of its own, as this window and its toast are hidden behind it.
    function say(message, actionLabel, onAction, sticky) {
        if (window.miniMode)
            miniPlayer.notice(message, actionLabel, onAction, sticky)
        else
            toast.show(message, actionLabel, onAction, sticky)
    }

    Connections {
        target: Library
        function onNotice(text) { window.say(text) }
    }

    Connections {
        target: About
        function onRecovered(text, folder) {
            window.say(text, folder.length > 0 ? "OPEN FOLDER" : "OK",
                       function() { if (folder.length > 0) Qt.openUrlExternally("file:///" + folder.replace(/\\/g, "/")) },
                       true)
        }
    }

    Connections {
        target: SignIn
        function onNotice(text) { window.say(text) }
        function onFallbackRequested() { window.openSignInImport() }
    }

    // Before Google's page opens, from wherever it is asked for: whose
    // account to use, and where the password goes. Paper in a 2px ink frame,
    // like a menu, over a dimmed window.
    Popup {
        id: signInCaution
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, window.width - Theme.space8 * 2)
        modal: true
        focus: true
        padding: Theme.space6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        Overlay.modal: Rectangle { color: Theme.dark ? Qt.rgba(0, 0, 0, 0.6) : "#66201e1d" }
        background: Rectangle {
            color: Theme.bg
            border.width: Theme.ruleWidth
            border.color: Theme.text
        }

        contentItem: Column {
            spacing: Theme.space4

            Text {
                width: parent.width
                text: "Sign in to YouTube Music"
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 22
                font.weight: Theme.weightBlack
                font.letterSpacing: Theme.tracking(22, -0.02)
                color: Theme.text
            }
            Text {
                width: parent.width
                text: "Google's own sign-in page opens in a window of Monolist's. Your password goes to Google "
                      + "alone; Monolist keeps only the session Google gives back, encrypted on this PC."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 14
                lineHeight: 1.3
                color: Theme.text
            }
            Text {
                width: parent.width
                text: "Use an account you can afford to lose: Google restricts accounts used by outside players, "
                      + "and that would take the account with it. Google may also refuse to sign in from inside an "
                      + "app; Monolist then offers the other way, copying a sign-in from your own browser."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                lineHeight: 1.3
                color: Theme.accent700
            }
            Row {
                spacing: Theme.space3
                topPadding: Theme.space2

                ActionButton {
                    text: "CONTINUE TO GOOGLE"
                    primary: true
                    onClicked: {
                        signInCaution.close()
                        SignIn.start()
                    }
                }
                ActionButton {
                    text: "CANCEL"
                    onClicked: signInCaution.close()
                }
            }
        }
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
        function onNotice(text) { window.say(text) }
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
        function onNotice(text) { window.say(text) }
        // "Not interested" and "Don't suggest", which can be taken back.
        function onUndoable(text) {
            window.say(text, "UNDO", function() { Recs.undoNotInterested() })
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
        function onNotice(text) { window.say(text) }
    }

    // The account's library: a sync asked for, and how it went.
    Connections {
        target: AccountLibrary
        function onNotice(text) { window.say(text) }
    }

    Connections {
        target: About
        function onNotice(text) { window.say(text) }
    }

    // The sound effects' answers: that they would not start, and are off
    // until the next launch.
    Connections {
        target: Sound
        function onNotice(text) { window.say(text) }
    }

    Connections {
        target: Player
        function onNotice(text) { window.say(text) }
        // A track that will not play used to fail in complete silence: the
        // status line is only shown while a track is resolving, and failing is
        // the moment that stops. Say it out loud.
        function onPlaybackError(reason) { window.say(reason) }
        // The picture gone — the switch turned off, the next song begun, a
        // video that would not play — takes full screen with it.
        function onVideoChanged() {
            if (!Player.videoWanted && !Player.videoPlaying) {
                window.leaveVideoFullscreen()
                // Picture in picture outlasts a song with no video; only the
                // switch turned off puts it away.
                if (!Player.videoPreferred)
                    window.videoPip = false
            }
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

    // The mouse's side buttons, anywhere in the window. Over everything, and
    // taking those two buttons alone: every other press, the wheel and the
    // pointer's hover go on to what is under it.
    MouseArea {
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        hoverEnabled: false
        onPressed: function(mouse) {
            if (mouse.button === Qt.BackButton)
                window.stepBack()
            else
                window.stepForward()
        }
    }

    // — keyboard —
    // Back and forward as a browser has them: Alt+Left and Alt+Right, Cmd+[
    // and Cmd+] on a Mac, and a keyboard's own Back and Forward keys. Not
    // Backspace, which Windows also counts as Back: a stray one would leave
    // the page.
    Shortcut {
        sequences: ["Alt+Left", "Ctrl+[", "Back"]
        onActivated: window.stepBack()
    }
    Shortcut {
        sequences: ["Alt+Right", "Ctrl+]", "Forward"]
        onActivated: window.stepForward()
    }
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

    // The Mac's own window keys, which a Mac app has whether or not it has a
    // Window menu to show them in. Closing leaves the app, and the music,
    // running; the Dock icon brings the window back.
    readonly property bool mac: Qt.platform.os === "osx"
    Shortcut { sequences: [StandardKey.Close]; enabled: window.mac; onActivated: window.close() }
    Shortcut { sequence: "Ctrl+M"; enabled: window.mac; onActivated: window.showMinimized() }
    Shortcut {
        sequences: [StandardKey.FullScreen]
        enabled: window.mac
        onActivated: window.visibility === Window.FullScreen ? window.showNormal() : window.showFullScreen()
    }
}
