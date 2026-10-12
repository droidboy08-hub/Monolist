#pragma once

#include <QString>

class Library;

// --import-test: importing a playlist (ROADMAP X03), against stand-ins on
// this computer for YouTube Music, Spotify, JioSaavn, Deezer and Apple
// Music, with a YouTube Music session imported that must never be carried:
//  - every shape of link, read or refused with a plain reason; a pasted
//    list and CSV files by their headers' names;
//  - the matcher's rule: an exact match, a cover ("Originally By", or a
//    tribute album's plain title) refused, a live take refused, one live
//    take or remix for another refused, a dub its album names refused,
//    "(Movie: X)" read as "(From X)", 20 s apart refused, another artist's
//    exact title, no length to compare, the ISRC search's first result
//    alone; and the key a match is kept by, its version in it;
//  - each source's pages: Spotify's 100 of 150 and its warning, an album's
//    cover from its visualIdentity, JioSaavn's pages ending empty, Deezer's
//    next links (never off its own host), Apple Music's "first 300" and an
//    album's count read past its ℗ line, YouTube's browse and its
//    continuation;
//  - the job: the playlist made at once and its songs in the source's
//    order (a YouTube list's first page found at once), the second search
//    by album, a video with no length left to check, songs in the library
//    and matched before not searched for (another version of one is),
//    not-found rows kept, songs with no artist kept, the review (Not this,
//    an ISRC's first result said Not this to, a song chosen, put back in
//    its place, a stop's songs apart), a 429 rested on and a second paused
//    on, that rest holding every import (the one stopped, one started
//    after), the hour's cap, the gap between searches, Spotify's 100
//    counted as 100, how long a long list is said to take, and a job going
//    on after a restart (a second Importer on the same library);
//  - no Cookie, Authorization or X-Goog-AuthUser header on anything the
//    import sends, with the session imported and confirmed.
// One line per check; returns how many failed. Refuses to run without
// MONOLIST_DATA_DIR: it writes playlists, settings and a session there.
int runImportSelfTest(Library *library);

// --import-preview <link>: the real source read as an import reads it, its
// first page and the next (two requests, 2-3 s apart), printed. 0 when it
// read.
int runImportPreview(const QString &link);

// --import-match "<title>" "<artist>" <seconds> ["<album>"] ["<ISRC>"]: one
// song looked for on the real YouTube Music as an import looks for it,
// signed out, its searches 2-3 s apart, every result's verdict printed.
int runImportMatch(const QString &title, const QString &artist, int seconds, const QString &album,
                   const QString &isrc);

// --import-demo <stage>: the app with stand-ins on this computer for
// YouTube Music and Spotify, for a look at the import's screens (Main.qml's
// runImportDemo walks them to the stage). They hold an invented playlist of
// fourteen songs that comes out found, likely, to check and not found, and
// one of 434 songs of which Spotify shows 100, for the warning; their covers
// are drawn here. From now on every request InnerTube and the import's
// reader make goes to them, so nothing is asked of the real services. At
// the stages "resting" and "held" they refuse every search (429), for the
// import's rest and its pause after a second refusal. Null when they could
// not listen; the caller owns them.
QObject *startImportDemo(const QString &stage);
