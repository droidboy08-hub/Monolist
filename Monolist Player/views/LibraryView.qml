import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// Your Library: what you made, what you saved, what you played, one tab each.
// The tab is part of the view's name ("library:albums"), so back and forward
// step through tabs like pages.
ScrollPage {
    id: root

    // "playlists", "songs", "artists", "albums", "history", or "history-ytm" for the YouTube
    // Music account's history as its last sync read it (AccountLibrary)
    property string tab: "playlists"
    readonly property bool historyTab: tab === "history" || tab === "history-ytm"
    // Every song the library holds, and their artists: read when one of the
    // two tabs is shown, and again as the library changes under it.
    readonly property bool songsTab: tab === "songs" || tab === "artists"
    property var artists: []
    function refreshSongs() {
        if (!songsTab)
            return
        Library.reloadSongs()
        artists = Library.songArtists()
    }
    Connections {
        target: Library
        function onRevisionChanged() { root.refreshSongs() }
        function onLikesChanged() { root.refreshSongs() }
    }
    Connections {
        target: Downloads
        function onLibraryChanged() { root.refreshSongs() }
    }
    onVisibleChanged: if (visible) refreshSongs()

    TrackFilterModel {
        id: songsView
        sourceModel: Library.songs
    }
    readonly property bool accountHistory: tab === "history-ytm" && AccountLibrary.shown
    signal tabRequested(string tab)
    signal viewRequested(string view)
    signal pageRequested(string browseId)
    signal newPlaylistRequested()

    // Clearing the history takes a second click, so a stray one cannot.
    property bool clearArmed: false

    readonly property int columns: Math.max(1, Math.min(5, Math.floor((column.width + Theme.space6) / (200 + Theme.space6))))
    readonly property int cardWidth: Math.floor((column.width - (columns - 1) * Theme.space6) / columns)

    contentHeight: column.implicitHeight

    onTabChanged: {
        clearArmed = false
        contentY = 0
        refreshSongs()
    }

    Timer {
        id: disarm
        interval: 3000
        onTriggered: root.clearArmed = false
    }

    function songs(n) { return n + (n === 1 ? " SONG" : " SONGS") }

    // A saved album or playlist in the shape of a YouTube Music card, for
    // the card menu: Play, Open, Remove from library, its link.
    function savedCard(type, saved) {
        return { type: type, browseId: saved.browseId, title: saved.title,
                 artist: saved.artist, artwork: saved.artwork }
    }

    component Note: Text {
        width: column.width
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 14
        color: Theme.neutral700
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        SectionHeader {
            width: parent.width
            number: "01"
            title: "Your Library"
            action: root.historyTab && !root.accountHistory && Library.history.count > 0
                    ? (root.clearArmed ? "CLICK AGAIN TO CLEAR" : "CLEAR HISTORY") : ""
            onActionTriggered: {
                if (root.clearArmed) {
                    root.clearArmed = false
                    Library.clearHistory()
                } else {
                    root.clearArmed = true
                    disarm.restart()
                }
            }
        }

        // Negative spacing lets neighbouring segments share one 2px rule.
        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "PLAYLISTS"
                selected: root.tab === "playlists"
                onPicked: root.tabRequested("playlists")
            }
            ChoiceChip {
                label: "SONGS"
                selected: root.tab === "songs"
                onPicked: root.tabRequested("songs")
            }
            ChoiceChip {
                label: "ARTISTS"
                selected: root.tab === "artists"
                onPicked: root.tabRequested("artists")
            }
            ChoiceChip {
                label: "ALBUMS"
                selected: root.tab === "albums"
                onPicked: root.tabRequested("albums")
            }
            ChoiceChip {
                label: "HISTORY"
                selected: root.historyTab
                onPicked: root.tabRequested("history")
            }
        }

        // The history played here, and the account's on YouTube Music, side
        // by side and never mixed: one is this computer's, the other what
        // YouTube Music keeps, as its last sync read it.
        Row {
            visible: root.historyTab && AccountLibrary.shown
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "ON MONOLIST"
                selected: root.tab === "history"
                onPicked: root.tabRequested("history")
            }
            ChoiceChip {
                label: "ON YOUTUBE MUSIC"
                selected: root.tab === "history-ytm"
                onPicked: root.tabRequested("history-ytm")
            }
        }

        // — playlists: liked songs, yours, a new one, then the saved —
        Flow {
            visible: root.tab === "playlists"
            width: parent.width
            spacing: Theme.space6

            // Each card has its menu (Menus): a playlist's own for Liked
            // songs and the user's playlists, the card menu for the saved.
            AlbumCard {
                width: root.cardWidth
                plate: "liked"
                title: "Liked songs"
                artist: "Everything you liked, latest first"
                footer: root.songs(Library.liked.count)
                hasMenu: true
                onPlayRequested: root.viewRequested("playlist:liked")
                onMenuRequested: Menus.openPlaylist("liked")
            }

            // The account's liked songs, as read: its own list, never mixed
            // into the one above.
            AlbumCard {
                visible: AccountLibrary.shown
                width: root.cardWidth
                plate: "liked"
                title: "Liked on YouTube Music"
                artist: AccountLibrary.stale ? "As last read from your account" : "Read from your YouTube Music account"
                footer: root.songs(AccountLibrary.likedCount)
                hasMenu: true
                onPlayRequested: root.viewRequested("playlist:ytliked")
                onMenuRequested: Menus.openPlaylist("ytliked")
            }

            Repeater {
                model: Library.playlists

                delegate: AlbumCard {
                    width: root.cardWidth
                    artworks: model.artworks
                    title: model.name
                    artist: "By " + Library.userName
                    footer: "PLAYLIST · " + root.songs(model.trackCount)
                    hasMenu: true
                    onPlayRequested: root.viewRequested("playlist:" + model.playlistId)
                    onMenuRequested: Menus.openPlaylist(model.playlistId)
                }
            }

            AlbumCard {
                width: root.cardWidth
                plate: "new"
                title: "New playlist"
                artist: "Name it, then fill it"
                footer: "CREATE"
                onPlayRequested: root.newPlaylistRequested()
            }

            Repeater {
                model: Library.savedPlaylists

                delegate: AlbumCard {
                    width: root.cardWidth
                    artwork: model.artwork
                    title: model.title
                    artist: model.artist
                    footer: "PLAYLIST · SAVED"
                    hasMenu: model.browseId.length > 0
                    onPlayRequested: root.pageRequested(model.browseId)
                    onMenuRequested: Menus.openCard(root.savedCard("playlist", model), "library")
                }
            }

            // The account's own playlists, private ones among them, opened
            // with the account; one also saved here shows once, above.
            Repeater {
                model: AccountLibrary.shown ? AccountLibrary.playlists : null

                delegate: AlbumCard {
                    width: root.cardWidth
                    artwork: model.artwork
                    title: model.title
                    artist: model.artist
                    footer: "PLAYLIST · YOUTUBE MUSIC"
                    hasMenu: model.browseId.length > 0
                    onPlayRequested: root.pageRequested(model.browseId)
                    onMenuRequested: Menus.openCard(root.savedCard("playlist", model), "library")
                }
            }
        }

        // — albums —
        Note {
            visible: root.tab === "albums" && Library.albums.count === 0
            text: "Albums you save show up here. Open any album and press Save."
        }

        Flow {
            visible: root.tab === "albums" && Library.albums.count > 0
            width: parent.width
            spacing: Theme.space6

            Repeater {
                model: Library.albums

                delegate: AlbumCard {
                    width: root.cardWidth
                    artwork: model.artwork
                    title: model.title
                    artist: model.artist
                    year: model.year
                    format: model.format
                    hasMenu: model.browseId.length > 0
                    onPlayRequested: root.pageRequested(model.browseId)
                    onMenuRequested: Menus.openCard(root.savedCard("album", model), "library")
                }
            }
        }

        // — songs: every one the library holds, once —
        Item {
            visible: root.tab === "songs"
            width: parent.width
            height: Math.max(songsFilter.implicitHeight, songsActions.implicitHeight)

            ListFilter {
                id: songsFilter
                width: parent.width - songsActions.width - Theme.space4
                model: songsView
                settingKey: "sort.library.songs"
            }
            Row {
                id: songsActions
                anchors.right: parent.right
                spacing: Theme.space3

                ActionButton {
                    primary: true
                    iconName: "play"
                    text: "Play"
                    enabled: songsView.count > 0
                    onClicked: Player.playModel(songsView, 0, "library")
                }
                ActionButton {
                    iconName: "shuffle"
                    text: "Shuffle"
                    enabled: songsView.count > 1
                    onClicked: {
                        Player.shuffle = true
                        Player.playModel(songsView, Math.floor(Math.random() * songsView.count), "library")
                    }
                }
            }
        }

        // One artist's songs, from the Artists tab: back to all of them, or
        // on to the artist's page.
        Row {
            visible: root.tab === "songs" && songsView.artist.length > 0
            spacing: Theme.space4

            Text {
                text: "SONGS BY " + songsView.artist.toUpperCase()
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: Theme.text
            }
            Text {
                text: "ALL SONGS"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: allHover.hovered ? Theme.accent700 : Theme.neutral700
                HoverHandler { id: allHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: songsView.artist = "" }
            }
            Text {
                text: "ARTIST PAGE →"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: pageHover.hovered ? Theme.accent700 : Theme.neutral700
                HoverHandler { id: pageHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Nav.openArtist(songsView.artist, "") }
            }
        }

        Note {
            visible: root.tab === "songs" && Library.songs.count === 0
            text: "No songs yet. Like a song, download one or add it to a playlist, and it shows up here."
        }
        Note {
            visible: root.tab === "songs" && Library.songs.count > 0 && songsView.count === 0
            text: "Nothing here matches."
        }

        TrackTable {
            visible: root.tab === "songs" && songsView.count > 0
            width: parent.width
            model: songsView
            sortModel: songsView
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(songsView, index, "library") }
        }

        // — artists: the songs' artists, the most songs first —
        Note {
            visible: root.tab === "artists" && root.artists.length === 0
            text: "No artists yet: they come with the songs you like, download or add to playlists."
        }
        Flow {
            visible: root.tab === "artists"
            width: parent.width
            spacing: Theme.space6

            Repeater {
                model: root.tab === "artists" ? root.artists : []

                delegate: Item {
                    id: artistTile
                    required property var modelData

                    width: root.cardWidth
                    height: artistColumn.implicitHeight

                    Column {
                        id: artistColumn
                        width: parent.width
                        spacing: Theme.space2

                        Artwork {
                            width: parent.width
                            height: width
                            source: artistTile.modelData.artwork
                            placeholder: artistTile.modelData.name
                            colour: tileHover.hovered
                        }
                        Text {
                            width: parent.width
                            text: artistTile.modelData.name
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            color: tileHover.hovered ? Theme.accent700 : Theme.text
                        }
                        Text {
                            text: root.songs(artistTile.modelData.count)
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            font.letterSpacing: Theme.tracking(11, 0.08)
                            color: Theme.neutral700
                        }
                    }

                    HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            songsView.artist = artistTile.modelData.name
                            root.tabRequested("songs")
                        }
                    }
                }
            }
        }

        // — history —
        Note {
            visible: root.historyTab && !root.accountHistory && Library.history.count === 0
            text: "Nothing played yet. Songs you play show up here, the latest first."
        }
        Note {
            visible: root.accountHistory
            textFormat: Text.PlainText
            text: AccountLibrary.historyCount === 0
                  ? "No history read from YouTube Music yet. " + AccountLibrary.status
                  : "What YouTube Music keeps of your listening, the latest first, as read from your account. "
                    + AccountLibrary.status
        }

        TrackTable {
            visible: root.historyTab && !root.accountHistory && Library.history.count > 0
            width: parent.width
            model: Library.history
            history: true
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(Library.history, index, "library") }
        }

        // Made only while shown: the account's history is not rebuilt with
        // every play, as the local one is.
        Loader {
            active: root.accountHistory && AccountLibrary.historyCount > 0
            visible: active
            width: parent.width
            sourceComponent: TrackTable {
                width: parent ? parent.width : 0
                model: AccountLibrary.history
                showDownloads: true
                onTrackActivated: function(index) { Player.playModel(AccountLibrary.history, index, "library") }
            }
        }
    }
}
