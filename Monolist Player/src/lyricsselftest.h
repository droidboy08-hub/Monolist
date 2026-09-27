#pragma once

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
// an answer shown because a better source failed kept as provisional and
// replaced once that source answers; nothing stored when a source could not
// be asked; old rows read as final. Refuses to run without MONOLIST_DATA_DIR,
// since it writes lyrics rows there (its invented rows are removed at the end).
int runLyricsFlowSelfTest(PlaybackController *player);
