#pragma once

class Library;

// Self-tests for scrobbling, run from the command line (main.cpp). Each
// reports on stderr, one line per check, and returns how many failed.
//
// Every value is invented ("TESTAPIKEY...", "TESTSESSIONKEY...", user
// "monolist-test"), nothing is sent to Last.fm, and each refuses to run
// without MONOLIST_DATA_DIR, since they empty the scrobble queue and the
// Last.fm settings of the database they are given.

// --listen-test: the heard-time rule through the real PlaybackController,
// with the engine's part played by the test (positions, pauses, buffering,
// ends of file): 25 and 30 s tracks, half, four minutes, seeks, pauses,
// buffering, repeat-one, a video toggle, launch-paused Play, a failed
// resolve, Previous, an unknown length; and the primary artist.
int runListenSelfTest();

// --scrobble-test: the queue and the sending against canned answers: what is
// recorded and when, 120 rows as 50/50/20, and every error's handling.
int runScrobbleSelfTest(Library *library);

// --lastfm-connect-test: connecting and disconnecting against canned
// answers: the token, the browser page, polling, focus, the button, the
// 10-minute limit, an expired token, and the session kept in SecretStore.
int runLastFmConnectSelfTest(Library *library);

// --scrobble-send-test [rows] [--expect-kept]: `rows` queued scrobbles
// (default 120) sent over HTTP to the stand-in server MONOLIST_LASTFM_URL
// names, which must be on this computer. Reports each request's size and
// what is left; --expect-kept checks that a refusing server leaves the
// queue whole.
int runScrobbleSendTest(Library *library, int rows, bool expectKept);

// --scrobble-kill-test: two listens heard to their thresholds and queued,
// then it waits to be killed; --diag afterwards shows the rows survived.
// Returns false when it could not set up.
bool startScrobbleKillTest(Library *library);
