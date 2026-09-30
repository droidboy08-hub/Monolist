#pragma once

class Library;

// --session-test: a launch opens on the queue the last one left, the song in
// it and the place in that song (PlaybackController::restoreSession), on the
// real mpv against a stand-in server on this computer, with no network:
//  - the queue's snapshot through JSON and back: every field, the row
//    playing, the order shuffle hides, at most 500 rows around the one
//    playing, and nothing put back from a snapshot that does not read;
//  - a streamed song left 12 s in opens paused there and plays on from
//    there; a file the same;
//  - the queue is not shuffled again when shuffle was left on, and is
//    written again a moment after it changes;
//  - a place that is another song's, or too near either end, starts the
//    song from the top; a queue that does not read is not put back;
//  - offline at launch, the song that will not load stays, at its place,
//    and Play begins it from there once it can load.
// One line per check; returns how many failed. Refuses to run without
// MONOLIST_DATA_DIR: it writes the player's settings and a tone there.
int runSessionSelfTest(Library *library);
