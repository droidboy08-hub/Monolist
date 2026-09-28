#pragma once

#include <QString>

class Lyrics;
class MediaExtractor;
class PlaybackController;

// --lyrics-query-test: the lyrics query cleaner and match scorer
// (lyricsquery.cpp), with no network and no database. The 18 fixed cases of
// checklist row C40, each fed as the app has the song (its artist credits as
// YouTube Music links them), the same lines without credits, and the scorer:
// "Anti-Hero" is not "Hero", an entry matching the artist alone is refused.
// Reports on stderr, one line per check, and returns how many failed.
int runLyricsQuerySelfTest();

// --lyrics-flow-test: how a lookup ends, against a stand-in for LRCLIB and
// YouTube Music on this computer. LRCLIB's exact lookup before its search,
// the search asked too when the exact one is slow, one deadline for LRCLIB;
// YouTube Music's text shown at the race's patience window while LRCLIB is
// silent, and called off when LRCLIB wins; an answer shown because a better
// source failed kept as provisional and replaced once that source answers;
// nothing stored when a source could not be asked; old rows read as final.
// Refuses to run without MONOLIST_DATA_DIR, since it writes lyrics rows there
// (its invented rows are removed at the end).
int runLyricsFlowSelfTest(PlaybackController *player);

// --lyrics-race-test: the race (lyrics/lyricsrace.*) with scripted providers
// and no network: answers taken in order, the winner shown at once and the
// losers called off, the patience window, upgrades, plain and timed answers
// kept apart, a failure never taken for "none", lazy providers, the serial
// switch, answers kept from before, the gate, stray and late answers; then
// the view's side of it (Lyrics), with its database rows: an earlier song's
// answer never shown, an answer on show replaced only as the upgrade rule
// allows, and a provider that answered before not asked again. Needs
// MONOLIST_DATA_DIR (its invented rows are removed at the end).
int runLyricsRaceSelfTest();

// --lyrics-prefetch-test: the queue's lookups (Lyrics::followQueue) against a
// stand-in LRCLIB and YouTube Music: two queue rows looked up with the lyrics
// closed, then opened and on show from the database (timed, with no request
// made); none for a song of unknown length; one lookup per song when the view
// opens while the queue's is out; none with the setting off, none for the
// next song on a metered connection, and no retry loop after a failure.
// Needs MONOLIST_DATA_DIR.
int runLyricsPrefetchSelfTest();

// --lyrics-pane "<query>" [--dwell <ms>]: what opening the lyrics costs, on
// the real services. The search's first two songs are queued and played; the
// lyrics are opened <dwell> ms (3000) after the first one's sound starts,
// closed, then Next, and opened again 1 s after the second's sound starts.
// One line per opening: "selftest: pane-open step=<current|next> ...", with
// the time from opening to the first lines on show and to the answer.
void startLyricsPaneTest(MediaExtractor *extractor, PlaybackController *player, Lyrics *lyrics,
                         const QString &query, int dwellMs);
