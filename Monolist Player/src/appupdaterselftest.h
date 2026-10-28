#pragma once

// --update-test: the in-app update (AppUpdater, About's UPDATE / CANCEL /
// RESTART) on stand-ins on this computer and scratch copies of the app, never
// GitHub, never the real app or its folder. The rules: the feed's release
// read (its page only ever the project's), the file chosen for each kind of
// copy (a Mac's Developer ID or ad hoc image, Windows' setup or zip), the
// origin rule (HTTPS from the project's release files on github.com, and
// GitHub's two download hosts as redirects; plain HTTP on this computer only
// for a feed on it and a scratch library), checksum files, the Windows
// folder rules (the user's folders inside a portable copy, a folder the
// setup made). On a stand-in: the check and download through About (a
// double-click's CANCEL let go, TRY AGAIN checking again), a matching
// download kept, a mismatch, a missing checksum, a link and a redirect
// elsewhere refused, more than the feed promised, a cut, a cancel and a quit
// mid-download leaving no half file, a launch's cleanup leaving a download
// alone, a download reused, and what a launch says of the last RESTART (and
// that it waits for the helper's word). On a Mac: scratch app bundles,
// signed ad hoc, in disk images, got ready and checked (another app, no
// newer build, a broken seal, not the Developer ID a Developer ID copy
// needs; refused ones deleted), a copy put in place by hand shown only a
// checked, quarantined image, an image a crash left attached let go of, and
// the helper (this program, --update-swap): the swap, its way back when the
// new app cannot be moved in, will not open, or opens and does not start,
// its second check of the signature, a path a shell would misread, a second
// RESTART, and its wait. On Windows: a portable copy's zip unpacked and its
// folder swapped with the helper started from inside it, an old folder with
// the user's files kept, a copy holding the downloads left for the user, the
// way back while the new folder is held open and when the new copy will not
// start, and the setup started with its silent arguments, never once it has
// changed. Reports on stderr, one line per check, and returns how many
// failed. Refuses to run without MONOLIST_DATA_DIR.
int runUpdateSelfTest();

// --update-run: About's check, UPDATE and RESTART, start to finish with no
// window, against update.feed; the end-to-end test of the in-app update.
// Returns when this copy has handed over and quit (0), or failed (1).
int runUpdateRun();
