import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// Home: what to play now first, then what is new. The owner's choice of
// 2026-10-01 (a mix of this design and Spotify's quick start), with the
// songs just heard leading it (JustPlayed, while Settings has Home follow
// what you play):
//
//   - a row of filters, which keep to songs, albums, playlists or artists
//     everything below them;
//   - Jump back in: the places opened lately (Library.recentPlaces), Liked
//     songs first, as plates to open or play in one click;
//   - More like <the song just heard>: YouTube Music's radio for it;
//   - Based on your recent listening: the radios of the last few songs,
//     taken in turn;
//   - Recommended for today: YouTube Music's quick picks as cards, the
//     account's own when its feed is Home's;
//   - More like <a song of another kind heard lately>;
//   - Made for you: the suggestions from this computer (Recs) as mixes;
//   - More like …: the artists YouTube Music links with the one heard most
//     lately (Catalog.moreLike);
//   - More like <a song of the taste heard most in the last two weeks>, so
//     that it is not lost under today's;
//
// then the moods, the newest release on the red poster, Recently played and
// YouTube Music's feed, its shelves in their own order under their own
// headings. Sections are numbered in reading order while nothing is
// filtered out. A row with nothing to show is left out, saying nothing.
//
// The rows from the songs just heard are a copy (sessionShown), taken again
// on a visit, or while the pointer is off them, the page is still and no
// menu is open, so they never change under the reader; and when the reader
// is below them, the page keeps what is in view where it was.
ScrollPage {
    id: root

    signal pageRequested(string browseId)
    // Another of the app's views by name: Recently played's SHOW ALL, a
    // playlist from Jump back in.
    signal viewRequested(string view)

    contentHeight: column.implicitHeight

    readonly property bool personal: Catalog.personalFeed

    // — the songs just heard (JustPlayed) —
    // The rows as shown: JustPlayed.rows when last taken (commitSession).
    property var sessionShown: []
    // JustPlayed has rows not taken yet, held while the reader was on them.
    property bool sessionPending: false
    // A menu is open over the page (Main): nothing moves under it.
    property bool holdUpdates: false
    readonly property bool session: JustPlayed.enabled && sessionShown.length > 0
    function sessionRow(kind) {
        if (!JustPlayed.enabled)
            return null
        for (let i = 0; i < sessionShown.length; ++i) {
            if (sessionShown[i].kind === kind)
                return sessionShown[i]
        }
        return null
    }
    readonly property var s1Row: sessionRow("s1")
    readonly property var mixRow: sessionRow("mix")
    readonly property var s2Row: sessionRow("s2")
    readonly property var s3Row: sessionRow("s3")
    readonly property bool hasS1: s1Row !== null
    readonly property bool hasMix: mixRow !== null
    readonly property bool hasS2: s2Row !== null
    readonly property bool hasS3: s3Row !== null
    // The pointer is on one of those rows.
    readonly property bool sessionHovered: s1Slot.hovered || mixSlot.hovered || s2Slot.hovered || s3Slot.hovered

    // JustPlayed's rows taken as the ones shown, unless the reader is on
    // them (`force`: a visit, where nothing is being read yet). Below them,
    // the page moves by what changed above, in the same frame, so what is in
    // view stays where it is (keepStill).
    function commitSession(force) {
        if (!force && root.visible && (root.sessionHovered || root.scrolling || root.moving || root.holdUpdates)) {
            sessionPending = true
            return
        }
        sessionPending = false
        const rows = JustPlayed.enabled ? JustPlayed.rows : []
        if (rows.length === 0 && sessionShown.length === 0)
            return
        const anchor = contentY > s1Slot.y ? anchorAt(contentY) : null
        stillAnchor = anchor
        stillY = anchor ? anchor.y : 0
        sessionShown = rows
        mixGrid.forceLayout()
        column.forceLayout()
        if (anchor)
            stillRelease.restart()
    }
    function tryCommit() { commitSession(false) }

    // The part kept where it was while the page settles: a row's shelf
    // lays itself out a frame after it is shown, and Made for you can change
    // with the rows, so its place is followed for a moment, not read once.
    // Each move is made as the layout makes it, before the frame is drawn.
    property Item stillAnchor: null
    property real stillY: 0
    function keepStill() {
        if (!stillAnchor || stillAnchor.y === stillY)
            return
        const moved = stillAnchor.y - stillY
        stillY = stillAnchor.y
        contentY = Math.max(0, contentY + moved)
    }
    Connections {
        target: root.stillAnchor
        function onYChanged() { root.keepStill() }
    }
    Timer {
        id: stillRelease
        interval: 300
        onTriggered: root.stillAnchor = null
    }
    // What the page keeps still: the first of its parts, other than the
    // session rows, that reaches `y` or starts below it.
    function anchorAt(y) {
        const parts = column.children
        for (let i = 0; i < parts.length; ++i) {
            const part = parts[i]
            if (part === s1Slot || part === mixSlot || part === s2Slot || part === s3Slot)
                continue
            if (part.visible && part.height > 0 && part.y + part.height > y)
                return part
        }
        return null
    }
    Connections {
        target: JustPlayed
        function onRowsChanged() { Qt.callLater(root.tryCommit) }
        function onEnabledChanged() { Qt.callLater(root.tryCommit) }
    }
    // Tried again once the reader has left the rows or the page is still. A
    // moment after, so that the pointer is back over what it was over when
    // the wheel stopped (ScrollPage holds hover while it turns).
    function retryCommit() {
        if (sessionPending)
            sessionRetry.restart()
    }
    onSessionHoveredChanged: retryCommit()
    onScrollingChanged: retryCommit()
    onMovementEnded: retryCommit()
    onHoldUpdatesChanged: retryCommit()
    Timer {
        id: sessionRetry
        interval: 150
        onTriggered: root.tryCommit()
    }

    // A session card plays its row, as shown, from that card on.
    function playSessionRow(key, index) {
        for (let r = 0; r < sessionShown.length; ++r) {
            const row = sessionShown[r]
            if (row.key !== key)
                continue
            const tracks = []
            for (let i = 0; i < row.items.length; ++i) {
                const card = row.items[i]
                tracks.push({
                    sourceId: card.videoId,
                    title: card.title,
                    artist: card.artist ? card.artist : card.subtitle,
                    album: card.album ? card.album : "",
                    albumId: card.albumId ? card.albumId : "",
                    credits: card.credits ? card.credits : [],
                    artwork: card.artwork,
                    durationMs: card.durationMs ? card.durationMs : 0,
                    isVideo: card.type === "video",
                    primaryArtist: card.primaryArtist ? card.primaryArtist : ""
                })
            }
            if (tracks.length > 0)
                Player.playTracks(tracks, Math.max(0, Math.min(tracks.length - 1, index)), "home")
            return
        }
    }

    // — the filter —
    // "all", "songs", "albums", "playlists" or "artists".
    property string filter: "all"
    readonly property var filters: [
        { key: "all", label: "ALL" },
        { key: "songs", label: "SONGS" },
        { key: "albums", label: "ALBUMS" },
        { key: "playlists", label: "PLAYLISTS" },
        { key: "artists", label: "ARTISTS" }
    ]
    readonly property bool filtered: filter !== "all"
    function shows(kind) { return filter === "all" || filter === kind }
    // A shelf's cards of the kind the filter keeps.
    function cardsFor(items) {
        if (!items || filter === "all")
            return items ? items : []
        return items.filter(function(card) {
            const type = card.type || ""
            return filter === "songs" ? type === "song" || type === "video"
                 : filter === "albums" ? type === "album"
                 : filter === "playlists" ? type === "playlist"
                 : type === "artist"
        })
    }

    function pad(n) { return n < 10 ? "0" + n : String(n) }

    // — Jump back in —
    // Liked songs, then the places opened lately, then the user's own
    // playlists to fill the rows: eight at most, each once.
    readonly property var jumpTiles: {
        const revision = Library.revision + Library.playlists.count   // read again as they change
        const group = root.filter                                      // and as the filter does
        const places = Library.recentPlaces
        const out = []
        const seen = {}
        // The filter applies before the count, so a filtered grid is full too.
        function add(tile) {
            if (out.length >= 8 || seen[tile.key] || revision < 0)
                return
            if (group !== "all" && tile.group !== group)
                return
            seen[tile.key] = true
            out.push(tile)
        }
        add({ key: "playlist:liked", kind: "playlist", ref: "liked", name: "Liked songs", plate: "liked",
              artworks: [], group: "playlists", playable: Library.liked.count > 0 })
        for (let i = 0; i < places.length; ++i) {
            const place = places[i]
            if (place.kind === "playlist") {
                if (place.ref === "liked")
                    continue
                if (place.ref === "ytliked") {
                    if (AccountLibrary.shown)
                        add({ key: "playlist:ytliked", kind: "playlist", ref: "ytliked",
                              name: "Liked on YouTube Music", plate: "liked", artworks: [], group: "playlists",
                              playable: AccountLibrary.likedCount > 0 })
                    continue
                }
                const row = Library.playlistRow(Number(place.ref))
                if (row < 0)
                    continue   // deleted since
                const playlist = Library.playlists.get(row)
                add({ key: "playlist:" + place.ref, kind: "playlist", ref: place.ref, name: playlist.name,
                      plate: "", artworks: playlist.artworks || [], group: "playlists",
                      playable: playlist.trackCount > 0 })
            } else if (place.kind === "page") {
                add({ key: "page:" + place.ref, kind: "page", ref: place.ref, name: place.title, plate: "",
                      artworks: place.artwork ? [place.artwork] : [],
                      group: place.type === "album" ? "albums" : "playlists", playable: true })
            } else if (place.kind === "artist") {
                add({ key: "artist:" + place.ref, kind: "artist", ref: place.ref, name: place.title, plate: "",
                      artworks: place.artwork ? [place.artwork] : [], group: "artists", playable: false })
            }
        }
        if (AccountLibrary.shown)
            add({ key: "playlist:ytliked", kind: "playlist", ref: "ytliked", name: "Liked on YouTube Music",
                  plate: "liked", artworks: [], group: "playlists", playable: AccountLibrary.likedCount > 0 })
        for (let row = 0; row < Library.playlists.count && out.length < 8; ++row) {
            const playlist = Library.playlists.get(row)
            add({ key: "playlist:" + playlist.playlistId, kind: "playlist", ref: String(playlist.playlistId),
                  name: playlist.name, plate: "", artworks: playlist.artworks || [], group: "playlists",
                  playable: playlist.trackCount > 0 })
        }
        return out
    }
    readonly property var shownTiles: jumpTiles

    function openTile(tile) {
        if (tile.kind === "playlist")
            viewRequested("playlist:" + tile.ref)
        else if (tile.kind === "page")
            pageRequested(tile.ref)
        else if (tile.kind === "artist")
            Nav.openArtist(tile.name, tile.ref)
    }

    function playTile(tile) {
        if (tile.kind === "page") {
            Catalog.playCollection(tile.ref, tile.name, "home")
        } else if (tile.ref === "liked") {
            Player.playModel(Library.liked, 0, "library")
        } else if (tile.ref === "ytliked") {
            Player.playTracks(AccountLibrary.likedTrackList(), 0, "playlist")
        } else {
            const tracks = Library.playlistTracksFor(Number(tile.ref))
            if (tracks.length > 0)
                Player.playTracks(tracks, 0, "playlist")
        }
    }

    // — Recommended for today: the quick picks, as song cards —
    readonly property var quickCards: {
        const count = Catalog.quickPicks.count
        const cards = []
        for (let i = 0; i < count; ++i) {
            const song = Catalog.quickPicks.get(i)
            cards.push({
                type: song.isVideo ? "video" : "song",
                videoId: song.sourceId,
                title: song.title,
                subtitle: song.artist,
                artist: song.artist,
                artwork: song.artwork,
                album: song.album,
                albumId: song.albumId,
                credits: song.credits,
                durationMs: song.durationMs,
                primaryArtist: song.primaryArtist ? song.primaryArtist : "",
                quickIndex: i
            })
        }
        return cards
    }
    readonly property bool hasPicks: quickCards.length > 0

    // — Made for you: the suggestions' own shelves as mixes —
    // The listener's (from what they played) when there are any; for
    // someone who has played nothing yet, the ones to start from. Under the
    // rows from the songs just heard, not Recs' own "More like <song>": those
    // rows say it, from YouTube Music's radio.
    readonly property var personalKinds: root.session ? ["taste", "recent", "artist"]
                                                      : ["taste", "recent", "song", "artist"]
    readonly property var mixes: {
        const shelves = Recs.shelves
        let picked = []
        for (let i = 0; i < shelves.length; ++i) {
            const shelf = shelves[i]
            if (shelf.rows && shelf.rows.length > 0 && personalKinds.indexOf(shelf.kind) >= 0)
                picked.push({ index: i, shelf: shelf })
        }
        if (picked.length === 0) {
            for (let i = 0; i < shelves.length; ++i) {
                if (shelves[i].rows && shelves[i].rows.length > 0)
                    picked.push({ index: i, shelf: shelves[i] })
            }
        }
        return picked.slice(0, 5)
    }
    readonly property bool hasMixes: Recs.available && mixes.length > 0
    readonly property bool mixesPersonal: mixes.length > 0 && personalKinds.indexOf(mixes[0].shelf.kind) >= 0
    readonly property var mixBands: ["#ec3013", "#3c7a8a", "#6b4fa0", "#b5651d", "#2f6b3f"]

    // Who is on a mix: its first few names.
    function mixArtists(rows) {
        const names = []
        for (let i = 0; i < rows.length && names.length < 3; ++i) {
            const name = rows[i].artist || ""
            if (name.length > 0 && names.indexOf(name) < 0)
                names.push(name)
        }
        return names.length === 0 ? "" : names.join(", ") + " and more"
    }

    // Four covers for a mix, asked for (paced, signed out) where none is known.
    function mixCovers(rows, revision) {
        const covers = []
        for (let i = 0; i < rows.length && covers.length < 4 && revision >= 0; ++i) {
            const cover = Recs.coverOf(rows[i].title || "", rows[i].artist || "")
            if (cover.length > 0)
                covers.push(cover)
        }
        return covers
    }
    function askMixCovers() {
        for (let m = 0; m < mixes.length; ++m) {
            const rows = mixes[m].shelf.rows
            for (let i = 0; i < Math.min(6, rows.length); ++i)
                Recs.wantCover(rows[i])
        }
    }
    onMixesChanged: if (visible) askMixCovers()

    // — More like —
    readonly property var moreLikeCards: Catalog.moreLike.items !== undefined ? Catalog.moreLike.items : []
    readonly property bool hasMoreLike: moreLikeCards.length > 0

    // — numbers, in reading order, while nothing is filtered out —
    readonly property bool hasRecent: Catalog.recent.count > 0
    readonly property int s1Number: hasS1 ? 1 : 0
    readonly property int mixNumber: s1Number + (hasMix ? 1 : 0)
    readonly property int todayNumber: mixNumber + (hasPicks ? 1 : 0)
    readonly property int s2Number: todayNumber + (hasS2 ? 1 : 0)
    readonly property int mixesNumber: s2Number + (hasMixes ? 1 : 0)
    readonly property int moreLikeNumber: mixesNumber + (hasMoreLike ? 1 : 0)
    readonly property int s3Number: moreLikeNumber + (hasS3 ? 1 : 0)
    readonly property int recentNumber: s3Number + (hasRecent && !personal ? 1 : 0)
    readonly property int shelfBase: recentNumber
    function number(n) { return root.filtered ? "" : pad(n) }

    // The feed's next page, asked for before the reader reaches the end: on
    // a scroll that nears it, and when a page that came leaves the end still
    // in view. Only while Home is the page shown.
    function askForMore() {
        if (!visible || !Catalog.homeHasMore || Catalog.homeLoadingMore || Catalog.loading)
            return
        // The column places what has just come before the next frame, not
        // now: laid out first, or the page's height is still the one it had
        // before the shelves came, and every page would be asked for at once.
        column.forceLayout()
        if (contentY + height > contentHeight - 1500)
            Catalog.loadMoreHome()
    }
    onContentYChanged: askForMore()
    Connections {
        target: Catalog
        function onHomeMoreChanged() { Qt.callLater(root.askForMore) }
    }

    function openCard(card) {
        if (card.rowKey !== undefined)
            root.playSessionRow(card.rowKey, card.rowIndex)
        else if (card.quickIndex !== undefined)
            Player.playModel(Catalog.quickPicks, card.quickIndex, "home")
        else if (card.type === "album" || card.type === "playlist")
            pageRequested(card.browseId)
        else if (card.type === "artist")
            Nav.openArtist(card.title, card.browseId)
        // The credit read from the card, not its whole subtitle, which also
        // holds a type label or a view count; the subtitle only when the card
        // names no one, and then Last.fm is told nothing (Scrobbler).
        else if (card.videoId)
            Player.playSource(card.videoId, card.title, card.artist ? card.artist : card.subtitle,
                              card.artwork, 0, "", card.type === "video", "home",
                              card.primaryArtist ? card.primaryArtist : "")
    }

    // More like …'s artist: while Home follows what you play, the one heard
    // most lately whose page YouTube Music has (JustPlayed), a song counted
    // once a day; otherwise the one played most.
    function loadMoreLike() {
        if (JustPlayed.enabled)
            Catalog.loadMoreLikeFrom(JustPlayed.moreLikeCandidates.concat([{ name: Library.topArtist(true), browseId: "" }]))
        else
            Catalog.loadMoreLike(Library.topArtist())
    }

    // A visit: the page opened, made at launch, or Now Playing closed over
    // it (Main). The rows from the songs just heard are drawn again and
    // taken at once; the rest is built again, and nothing is rebuilt when
    // nothing changed (Recs.refresh), and More like asks nothing within the
    // day.
    function revisit() {
        JustPlayed.visit()
        commitSession(true)
        Recs.refresh()
        loadMoreLike()
        askMixCovers()
    }
    onVisibleChanged: {
        if (visible)
            revisit()
        askForMore()
    }
    // Home is the page a launch opens on, so it is not made visible: the
    // same, once it is made.
    Component.onCompleted: {
        if (visible)
            revisit()
    }

    // The number, the title and the table, with the rule under it, and the
    // section's one link at the header's end.
    component TrackSection: Column {
        id: section
        property string number: ""
        property string title: ""
        // The small line above the title, as a shelf's.
        property string strapline: ""
        property var model: null
        property string action: ""
        // Recently played is the history's latest: its rows' menus can take
        // a song out of it.
        property bool history: false
        signal actionTriggered()

        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: Theme.space8
        spacing: Theme.space6

        Column {
            width: parent.width
            spacing: Theme.space1

            Text {
                visible: section.strapline.length > 0
                width: parent.width
                text: section.strapline.toUpperCase()
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }
            SectionHeader {
                width: parent.width
                number: section.number
                title: section.title
                action: section.action
                onActionTriggered: section.actionTriggered()
            }
        }

        TrackTable {
            width: parent.width
            model: section.model
            history: section.history
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(section.model, index, "home") }
        }
    }

    // One of the rows from the songs just heard (JustPlayed), from the copy
    // shown: a shelf of song cards, under a rule when a section is above it.
    // No line above the title, and nothing at all while it has no row.
    component SessionShelf: Item {
        id: slot
        property var row: null
        property int number: 0
        property bool ruled: false
        readonly property bool hovered: slotHover.hovered

        visible: row !== null && root.shows("songs")
        width: column.width
        height: visible ? sessionShelf.implicitHeight + Theme.space8 * 2 + (ruled ? Theme.ruleWidth : 0) : 0

        HRule {
            visible: slot.ruled
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }

        CardShelf {
            id: sessionShelf
            x: Theme.space8
            y: Theme.space8 + (slot.ruled ? Theme.ruleWidth : 0)
            width: parent.width - Theme.space8 * 2
            number: root.number(slot.number)
            title: slot.row !== null ? slot.row.title : ""
            items: slot.row !== null ? root.cardsFor(slot.row.items) : []
            origin: "home"
            onCardActivated: function(card) { root.openCard(card) }
        }

        // Only to know that the reader is here (commitSession): it takes
        // nothing from what is under it.
        HoverHandler { id: slotHover }
    }

    Column {
        id: column
        width: root.width
        spacing: 0

        // — the filter —
        Row {
            x: Theme.space8
            topPadding: Theme.space8
            spacing: -Theme.ruleWidth

            Repeater {
                model: root.filters

                ChoiceChip {
                    required property var modelData
                    label: modelData.label
                    selected: root.filter === modelData.key
                    onPicked: root.filter = modelData.key
                }
            }
        }

        // — Jump back in —
        Grid {
            id: jumpGrid
            visible: root.shownTiles.length > 0 && root.filter !== "songs"
            x: Theme.space8
            width: root.width - Theme.space8 * 2
            topPadding: Theme.space6
            columns: width >= 1000 ? 4 : width >= 560 ? 2 : 1
            columnSpacing: Theme.space3
            rowSpacing: Theme.space3

            Repeater {
                model: root.shownTiles

                QuickTile {
                    required property var modelData
                    width: (jumpGrid.width - (jumpGrid.columns - 1) * jumpGrid.columnSpacing) / jumpGrid.columns
                    name: modelData.name
                    artworks: modelData.artworks
                    plate: modelData.plate
                    playable: modelData.playable
                    loading: modelData.kind === "page" && Catalog.collectionLoading === modelData.ref
                    onOpened: root.openTile(modelData)
                    onPlayRequested: root.playTile(modelData)
                    onMenuRequested: {
                        if (modelData.kind === "playlist")
                            Menus.openPlaylist(modelData.ref === "liked" || modelData.ref === "ytliked"
                                               ? modelData.ref : Number(modelData.ref))
                    }
                }
            }
        }

        // — while the feed loads, or when it cannot —
        Text {
            visible: Catalog.loading && Catalog.shelves.length === 0 && !root.hasPicks
            x: Theme.space8
            topPadding: Theme.space8
            text: "Loading YouTube Music…"
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        Row {
            visible: !Catalog.loading && Catalog.error.length > 0
            x: Theme.space8
            topPadding: Theme.space8
            spacing: Theme.space4

            Text {
                width: Math.min(implicitWidth, root.width - Theme.space8 * 2 - 80)
                // Over the last launch's Home, still shown: when that one is from.
                text: (Catalog.savedAt.length > 0 ? "Home as it was on " + Catalog.savedAt + ". " : "")
                      + "YouTube Music did not answer: " + Catalog.error
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.accent700
            }
            Text {
                text: "RETRY"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.12)
                color: retryHover.hovered ? Theme.accent700 : Theme.text

                HoverHandler { id: retryHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Catalog.refresh() }
            }
        }

        // — More like <the song just heard>, and Based on your recent
        // listening —
        SessionShelf {
            id: s1Slot
            row: root.s1Row
            number: root.s1Number
        }
        SessionShelf {
            id: mixSlot
            row: root.mixRow
            number: root.mixNumber
            ruled: root.hasS1
        }

        // — Recommended for today —
        Item {
            id: todaySection
            readonly property bool ruled: (root.hasS1 || root.hasMix) && root.shows("songs")

            visible: root.hasPicks && root.shows("songs")
            width: parent.width
            height: visible ? todayShelf.implicitHeight + Theme.space8 * 2 + (ruled ? Theme.ruleWidth : 0) : 0

            HRule {
                visible: todaySection.ruled
                x: Theme.space8
                width: parent.width - Theme.space8 * 2
            }

            CardShelf {
                id: todayShelf
                x: Theme.space8
                y: Theme.space8 + (todaySection.ruled ? Theme.ruleWidth : 0)
                width: parent.width - Theme.space8 * 2
                number: root.number(root.todayNumber)
                title: "Recommended for today"
                strapline: root.personal ? "Inspired by what you played · from your YouTube Music"
                                         : (Catalog.quickPicksStrapline.length > 0 ? Catalog.quickPicksStrapline
                                                                                   : "Picked by YouTube Music")
                items: root.quickCards
                origin: "home"
                onCardActivated: function(card) { root.openCard(card) }
            }
        }

        // — More like <a song of another kind heard lately> —
        SessionShelf {
            id: s2Slot
            row: root.s2Row
            number: root.s2Number
            ruled: root.hasS1 || root.hasMix || root.hasPicks
        }

        // — Made for you —
        Column {
            id: mixesSection
            // A row of songs is above it, whose rule is its gap.
            readonly property bool ruled: (root.hasS1 || root.hasMix || root.hasPicks || root.hasS2)
                                          && root.shows("songs")

            visible: root.hasMixes && (root.shows("songs") || root.filter === "playlists")
            x: Theme.space8
            width: root.width - Theme.space8 * 2
            // Under a row of songs its rule is the gap; first, a gap of its own.
            topPadding: ruled ? 0 : Theme.space8
            bottomPadding: Theme.space8
            spacing: Theme.space6

            HRule {
                visible: mixesSection.ruled
                width: parent.width
            }

            Column {
                width: parent.width
                spacing: Theme.space1

                Text {
                    width: parent.width
                    text: root.mixesPersonal ? "MIXES FROM WHAT YOU PLAY · ON THIS COMPUTER" : "MIXES TO START YOU OFF"
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }
                SectionHeader {
                    width: parent.width
                    number: root.number(root.mixesNumber)
                    title: "Made for you"
                }
            }

            Grid {
                id: mixGrid
                width: parent.width
                columns: width >= 1000 ? 5 : width >= 640 ? 3 : 2
                columnSpacing: Theme.space6
                rowSpacing: Theme.space6

                Repeater {
                    model: root.mixes

                    MixTile {
                        required property var modelData
                        required property int index
                        width: (mixGrid.width - (mixGrid.columns - 1) * mixGrid.columnSpacing) / mixGrid.columns
                        number: index + 1
                        title: modelData.shelf.title
                        subtitle: root.mixArtists(modelData.shelf.rows)
                        covers: root.mixCovers(modelData.shelf.rows, Recs.coversRevision)
                        band: root.mixBands[index % root.mixBands.length]
                        onOpened: Nav.openSuggestions(modelData.index)
                        onPlayRequested: Recs.playAll(modelData.index)
                    }
                }
            }
        }

        // — More like … —
        Item {
            visible: root.hasMoreLike && root.shows("artists")
            width: parent.width
            height: visible ? moreLikeShelf.implicitHeight + Theme.space8 * 2 + Theme.ruleWidth : 0

            HRule {
                x: Theme.space8
                width: parent.width - Theme.space8 * 2
            }

            CardShelf {
                id: moreLikeShelf
                x: Theme.space8
                y: Theme.space8 + Theme.ruleWidth
                width: parent.width - Theme.space8 * 2
                number: root.number(root.moreLikeNumber)
                title: "More like " + (Catalog.moreLike.name !== undefined ? Catalog.moreLike.name : "")
                strapline: JustPlayed.enabled ? "" : "Artists YouTube Music links with the one you play most"
                items: root.moreLikeCards
                origin: "home"
                onCardActivated: function(card) { root.openCard(card) }
            }
        }

        // — More like <a song of the taste heard most in two weeks> —
        SessionShelf {
            id: s3Slot
            row: root.s3Row
            number: root.s3Number
            ruled: true
        }

        // — Moods & genres: a way in rather than a section, so not numbered —
        Item {
            visible: Catalog.moods.length > 0 && !root.filtered
            width: parent.width
            height: visible ? moodStrip.implicitHeight + Theme.space8 : 0

            MoodStrip {
                id: moodStrip
                x: Theme.space8
                width: parent.width - Theme.space8 * 2
                groups: Catalog.moods
            }
        }

        // — the newest release, on the red poster —
        Item {
            visible: Catalog.featured.title !== undefined && root.shows("albums")
            width: parent.width
            height: visible ? hero.implicitHeight + Theme.space8 : 0

            PosterHero {
                id: hero
                width: parent.width
                kicker: "NEW RELEASE"
                titleLine1: Catalog.featured.title !== undefined ? Catalog.featured.title : ""
                meta: Catalog.featured.subtitle !== undefined ? Catalog.featured.subtitle : ""
                artwork: Catalog.featured.artwork !== undefined ? Catalog.featured.artwork : ""
                // It opens the release's page, so an arrow, and the button says
                // what kind of release it is ("Single • Horror Skunx").
                buttonText: {
                    const kind = (Catalog.featured.subtitle || "").split(" • ")[0]
                    return "Open " + (kind === "Single" ? "single" : kind === "EP" ? "EP" : "album")
                }
                buttonIcon: "arrow-right"
                onPlayRequested: root.pageRequested(Catalog.featured.browseId)
            }
        }

        // — recently played, signed out: before the feed —
        // The last ten; everything played is the library's History.
        TrackSection {
            visible: root.hasRecent && !root.personal && root.shows("songs")
            number: root.number(root.recentNumber)
            title: "Recently played"
            model: Catalog.recent
            history: true
            action: "SHOW ALL"
            onActionTriggered: root.viewRequested("library:history")
        }

        // — shelves: new releases then the feed's own; or, the account's,
        // its feed in its own order then new releases —
        Repeater {
            model: Catalog.shelves

            delegate: Column {
                id: shelfColumn
                required property int index
                required property var modelData
                readonly property var cards: root.cardsFor(modelData.items)

                visible: cards.length > 0
                width: column.width

                HRule {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                }

                Item { width: 1; height: Theme.space8 }

                CardShelf {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                    number: root.number(root.shelfBase + shelfColumn.index + 1)
                    title: shelfColumn.modelData.title
                    strapline: shelfColumn.modelData.strapline
                    items: shelfColumn.cards
                    more: shelfColumn.modelData.more
                    origin: "home"
                    onCardActivated: function(card) { root.openCard(card) }
                }

                Item { width: 1; height: Theme.space8 }
            }
        }

        // — the feed below its first page, a page more as the reader nears
        // the end (Catalog.loadMoreHome) —
        Repeater {
            model: Catalog.moreShelves

            delegate: Column {
                id: moreColumn
                required property int index
                required property var shelf
                readonly property var cards: root.cardsFor(shelf.items)

                visible: cards.length > 0
                width: column.width

                HRule {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                }

                Item { width: 1; height: Theme.space8 }

                CardShelf {
                    x: Theme.space8
                    width: parent.width - Theme.space8 * 2
                    number: root.number(root.shelfBase + Catalog.shelves.length + moreColumn.index + 1)
                    title: moreColumn.shelf.title
                    strapline: moreColumn.shelf.strapline
                    items: moreColumn.cards
                    more: moreColumn.shelf.more
                    origin: "home"
                    onCardActivated: function(card) { root.openCard(card) }
                }

                Item { width: 1; height: Theme.space8 }
            }
        }

        Text {
            visible: Catalog.homeLoadingMore
            x: Theme.space8
            topPadding: Theme.space4
            bottomPadding: Theme.space4
            text: "Loading more…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        // — recently played, with the account's feed: after it —
        HRule {
            visible: root.hasRecent && root.personal && root.shows("songs")
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
        }
        TrackSection {
            visible: root.hasRecent && root.personal && root.shows("songs")
            number: root.number(root.shelfBase + Catalog.shelves.length + Catalog.moreShelves.count + 1)
            title: "Recently played"
            model: Catalog.recent
            history: true
            action: "SHOW ALL"
            onActionTriggered: root.viewRequested("library:history")
        }

        Item { width: 1; height: 64 }
    }
}
