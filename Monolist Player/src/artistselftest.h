#pragma once

// --artist-links-test: artist links, with no network. Credits read from
// canned YouTube Music answers (each name with its page, the pieces adding up
// to the artist line), ArtistLinks splitting a line it only has the name of
// into names it knows — and refusing to split one it does not — and keeping
// them across a restart; and the artist page's and artist search's parsers.
// Reports on stderr, one line per check, and returns how many failed.
// Refuses to run without MONOLIST_DATA_DIR, since it writes to the database's
// artist_links table there (its invented rows are removed at the end).
int runArtistLinksSelfTest();
