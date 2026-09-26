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

    // How much of the bottom of every page something stands over — the mini
    // video panel, while it is up — so each page (ScrollPage) leaves that
    // much room below its end and its last rows can scroll out from under
    // it. Main sets it.
    property real pageClearance: 0

    function openSuggestions(shelf) {
        if (shelf >= 0)
            suggestionsRequested(shelf)
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
