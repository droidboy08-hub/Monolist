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

    function openArtist(name, browseId) {
        artistRequested(name ? name : "", browseId ? browseId : "")
    }

    function openPage(browseId) {
        if (browseId)
            pageRequested(browseId)
    }
}
