#pragma once

class Library;

// --library-edit-test: what the song and queue menus change, on invented
// songs, with no network and no window. The queue's moves (the current song,
// the radio's heading and the shuffle's restored order kept true), a
// playlist's order as dragged and as read back, Remove from library, Remove
// from history, and Copy link. Reports on stderr, one line per check, and
// returns how many failed. Refuses to run without MONOLIST_DATA_DIR, since it
// writes playlists, likes and plays into the database there.
int runLibraryEditSelfTest(Library *library);
