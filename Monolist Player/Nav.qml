pragma Singleton
import QtQuick

// Where a link deep inside a list asks to go. An artist's name is a link in
// every row, bar and page that shows one, and threading a signal up through
// each of them to the window would be a dozen relays for one sentence: "open
// this". Main listens here and does the navigating.
QtObject {
    // `browseId` may be empty: the name alone is then looked up.
    signal artistRequested(string name, string browseId)
    // An album's or a YouTube Music playlist's page.
    signal pageRequested(string browseId)
    // A shelf's "show all" page of cards, headed `title` while it loads.
    signal listingRequested(string browseId, string params, string title)
    // A suggestion shelf's SHOW ALL, by its index in Recs.shelves.
    signal suggestionsRequested(int shelf)
    // Importing a playlist (Importer), from wherever it is offered: its
    // window, on a link to read at once (Search's offer) or empty; or on an
    // import already started, by its id (the sidebar's progress, a page's).
    signal importRequested(string text)
    signal importJobRequested(int jobId)
    // A song an import could not place, looked for in Search, whose results
    // can then go into the playlist in its place. `row` is the review's
    // (Importer.review).
    signal importSongPickRequested(var row, int playlistId)

    // How much of the bottom of every page something stands over — the mini
    // video panel, while it is up — so each page (ScrollPage) leaves that
    // much room below its end and its last rows can scroll out from under
    // it. Main sets it.
    property real pageClearance: 0

    // Now Playing, or the video full screen, stands over every page: what
    // moves on them (the playing song's bars) can hold still. Main sets it.
    property bool pagesCovered: false

    // The rounded corners of a list's covers (Artwork's radius), drawn once
    // for all of them rather than once a cover. Main makes it. The player
    // bar's cover has the same 6px corners at 56px, so it has its own: a
    // list cover's stretched to 56 would round it by 8.4.
    property Item coverMask: null
    property Item barCoverMask: null

    function openSuggestions(shelf) {
        if (shelf >= 0)
            suggestionsRequested(shelf)
    }

    function openImport(text) {
        importRequested(text ? text : "")
    }

    function openImportJob(jobId) {
        if (jobId > 0)
            importJobRequested(jobId)
    }

    function pickImportSong(row, playlistId) {
        if (row && row.rowId > 0)
            importSongPickRequested(row, playlistId)
    }

    function openArtist(name, browseId) {
        artistRequested(name ? name : "", browseId ? browseId : "")
    }

    function openPage(browseId) {
        if (browseId)
            pageRequested(browseId)
    }

    // A shelf's SHOW ALL, by where its `more` says it goes (Catalog.shelves):
    // a playlist's or an album's page, an artist's, or a page of cards.
    function openMore(more, title) {
        if (!more || !more.browseId)
            return
        if (more.kind === "page")
            pageRequested(more.browseId)
        else if (more.kind === "artist")
            artistRequested("", more.browseId)
        else if (more.kind === "browse")
            listingRequested(more.browseId, more.params ? more.params : "", title ? title : "")
    }
}
