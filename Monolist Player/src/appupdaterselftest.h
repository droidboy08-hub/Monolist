#pragma once

// --update-test: the in-app update (AppUpdater, About's UPDATE / CANCEL /
// RESTART) on stand-ins on this computer and scratch copies of the app, never
// GitHub, never the real app or its folder. The rules: the feed's release
// read, the file chosen for each kind of copy (a Mac's Developer ID or ad hoc
// image, Windows' setup or zip), the origin rule (HTTPS from github.com and
// *.githubusercontent.com only; plain HTTP on this computer only for a feed
// on it), checksum files, the Windows folder rules. On a stand-in: the
// check and download through About, a matching download kept, a mismatch,
// a missing checksum, a link and a redirect elsewhere refused, a cut, a
// cancel and a quit mid-download leaving no half file, a download reused,
// and what a launch says of the last RESTART. On a Mac: scratch app bundles,
// signed ad hoc, in disk images, got ready and checked (another app, no
// newer build, a broken seal, not the Developer ID a Developer ID copy
// needs), why a copy cannot be replaced in place, and the helper's swap,
// its way back when the new app cannot be moved in or will not open, and its
// wait. On Windows: a portable copy's zip unpacked and its folder swapped,
// the way back while the new folder is held open, and the setup started
// with its silent arguments. Reports on stderr, one line per check, and
// returns how many failed. Refuses to run without MONOLIST_DATA_DIR.
int runUpdateSelfTest();

// --update-run: About's check, UPDATE and RESTART, start to finish with no
// window, against update.feed; the end-to-end test of the in-app update.
// Returns when this copy has handed over and quit (0), or failed (1).
int runUpdateRun();
