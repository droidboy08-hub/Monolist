#pragma once

class Library;

// --just-played-test: Home's just-played rows (JustPlayed, rec/sessionrows),
// with no network: a stand-in YouTube Music on this computer, a fake
// signed-in account that must never be reached, a real ListenTracker, fake
// clocks and short timings.
//  - what is sent: one cookieless /next for a song heard 45 s (RDAMVM,
//    wAEB), no Cookie, Authorization, X-Goog-AuthUser or visitor id, and the
//    account's hook never asked; nothing for a skip or a seek to the end; an
//    autoplay song at half the track; a repeat-one loop asked once; the
//    newer of two seeds first, the gap, one at a time; the hour's and the
//    day's caps across a restart; every failure tried once and rested on
//    (15 min, 1 h, 3 h; a stored rest clamped to a day); empty answers kept
//    and rested on after three; autoplay's radio kept and not asked again,
//    and never cancelled; the holds; switched off; quitting mid-answer; a
//    second launch on the same library;
//  - the rows (Rec::buildSessionRows): what they leave out, no song twice,
//    three of an artist at most, the minimums, the rotation, the clusters,
//    the live seed first, cleanSeedTitle;
//  - Clear history and Remove from history, Library::topArtist(true),
//    Catalog::loadMoreLikeFrom, ListenTracker::listenSettled, and
//    watchPlaylist's retries.
// One line per check; returns how many failed. Refuses to run without
// MONOLIST_DATA_DIR: it writes plays, history and settings there.
int runJustPlayedSelfTest(Library *library);
