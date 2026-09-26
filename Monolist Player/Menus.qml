pragma Singleton
import QtQuick

// Where a right click, or a "more" button, asks for a menu. A song, a card or
// a playlist can be shown almost anywhere — a table, a shelf, the queue,
// Downloads, the player bar — and a menu in each of those would be dozens of
// copies of one menu, most never opened. Main keeps one of each and opens it
// here, filled in with what was clicked, as Nav does for links.
QtObject {
    // `context` says where the song was shown, for the entries that only
    // apply there (TrackMenu reads it).
    signal trackRequested(var track, var context)
    // An album, a playlist or an artist from YouTube Music, or one saved.
    signal cardRequested(var card, string origin)
    // One of the user's playlists, by id, or "liked".
    signal playlistRequested(string key)

    function openTrack(track, context) {
        trackRequested(track ? track : ({}), context ? context : ({}))
    }

    function openCard(card, origin) {
        if (card)
            cardRequested(card, origin ? origin : "")
    }

    function openPlaylist(key) {
        playlistRequested(String(key))
    }

    // A card that is a song or a video, as the player and TrackMenu take a
    // song: the credit read from the card, not its whole subtitle (see
    // HomeView.openCard).
    function trackOfCard(card) {
        return {
            sourceId: card.videoId,
            title: card.title,
            artist: card.artist ? card.artist : card.subtitle,
            artwork: card.artwork,
            durationMs: 0,
            isVideo: card.type === "video",
            primaryArtist: card.primaryArtist ? card.primaryArtist : ""
        }
    }

    // — links, as YouTube Music and YouTube print them —
    function songLink(videoId) {
        return videoId ? "https://music.youtube.com/watch?v=" + videoId : ""
    }
    function watchLink(videoId) {
        return videoId ? "https://www.youtube.com/watch?v=" + videoId : ""
    }
    // An album's page is its browse id; a playlist's is its list id, which
    // is the browse id without YouTube Music's "VL"; an artist's is their
    // channel.
    function pageLink(browseId, type) {
        if (!browseId)
            return ""
        if (type === "artist" || browseId.indexOf("UC") === 0)
            return "https://music.youtube.com/channel/" + browseId
        if (type === "playlist" || browseId.indexOf("VL") === 0)
            return "https://music.youtube.com/playlist?list="
                   + (browseId.indexOf("VL") === 0 ? browseId.substring(2) : browseId)
        return "https://music.youtube.com/browse/" + browseId
    }
}
