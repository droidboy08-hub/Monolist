#pragma once

#include <QStringList>

class Library;

// Self-tests for the YouTube Music sign-in, run from the command line
// (main.cpp). Each reports on stderr, one line per check, and returns how many
// failed.
//
// Every cookie is invented ("TESTSAPISID123", "TESTVAL-..."): nothing here
// reads, prints or writes a real cookie, and nothing is ever sent to YouTube.

// --cookie-test: the import parser on synthetic fixtures (cookies.txt with LF
// and CRLF, "#HttpOnly_" lines, spaces where the tabs go, duplicate names
// across domains, cookies for www.youtube.com, s.youtube.com and other paths
// kept, other parts of YouTube left out, a missing LOGIN_INFO; a Cookie
// header; cURL in bash and cmd.exe quoting, with x-goog-authuser and
// x-goog-visitor-id read beside the cookies), the Cookie header for each of
// music, www and s.youtube.com byte for byte, the jar as it is stored
// (versions 1 and 2), the jar as a cookies.txt file for yt-dlp (never a
// google.com cookie, read back the same), and the SAPISIDHASH known answers.
// No network and no data folder.
int runCookieImportSelfTest();

// --ytm-session-test: YtmSession and InnerTube's account path against a
// stand-in server on this computer: signed-out requests byte for byte as
// before (their headers and context as the build before the three-host jar
// sent them, and every later anonymous call identical to the first), the
// account's headers only where asked for and music.youtube.com's cookies
// alone on its calls, the check (logged_in, the account menu, twice-zero, no
// answer), the session's visitor id and a brand channel learned from a
// signed-in answer and sent as X-Goog-Visitor-Id and context.user.
// onBehalfOfUser, X-Goog-AuthUser from a copied request (0 when not known),
// Set-Cookie rotation kept out of the anonymous jar and refused for another
// host, a 400 that never touches the country, 401/403, a restart, sign-out,
// the offer to delete an imported file, and no cookie value or session id in
// the log. Refuses to run without MONOLIST_DATA_DIR, since it replaces the
// stored session and the account setting.
int runYtmSessionSelfTest(Library *library);

// --home-account-test: Home's feed as the account (Catalog::followAccount),
// against the same stand-in: signed out, the feed and new releases byte for
// byte the anonymous calls Home made before; once a session is confirmed the
// feed alone asked again with the account (logged_in=1) and new releases
// never; "Use my account for Home" off and on again, kept in settings; a
// sign-out, and a 403, with the account's feed on its way, leaving nothing of
// it on screen; a launch with a stored session (signed out first, then the
// account's); no cookie value in the log. Refuses to run without
// MONOLIST_DATA_DIR, as --ytm-session-test does.
int runHomeAccountSelfTest(Library *library);

// --account-play-test: the account where a song needs it, and listens
// reported to its history, against the same stand-in, with this program
// standing in for yt-dlp (runFakeYtDlp). Signed out, a song YouTube refuses
// signed out goes down the anonymous ladder as before and the account's rung
// is never tried; signed in, it is asked with the account next (never before
// an anonymous refusal that names a reason an account answers, never ahead
// of time, never with the switch off or the hour's limit reached), through
// a cookies.txt file that holds youtube.com's cookies alone, is read back for
// what yt-dlp rotated and deleted, with tv_downgraded,web_embedded, and its
// link goes to mpv without a Cookie header; leftovers are swept at start;
// "cookies are no longer valid" has the session checked; a refused signed-in
// link is not asked for again unless the walk lists it; signing out forgets
// every signed-in link and stops a lookup under way. A qualified listen is
// reported as YouTube Music's player does (the account's WEB_REMIX /player,
// then a GET of its videostatsPlaybackUrl with ver=2, c=WEB_REMIX and a cpn,
// with s.youtube.com's own cookies and the SID hashes), and never to another
// host or path, after a redirect, from a signed-out answer, with the switch
// off, or without a confirmed session. No cookie value, info JSON or link in
// the log. Refuses to run without MONOLIST_DATA_DIR.
int runAccountPlaySelfTest(Library *library);
// --fake-yt-dlp <folder> <yt-dlp's arguments>: this program in yt-dlp's
// place, for the test above. It writes each run to <folder>/calls.jsonl (the
// arguments, and the cookies file as it was given), sleeps
// <id>.<mode>.delay ms, writes <folder>/writeback.txt over the cookies file
// as yt-dlp writes its jar back at exit, prints <id>.<mode>.warn to stderr,
// then answers <id>.<mode>.json on stdout, or <id>.<mode>.err (else "Video
// unavailable") on stderr with exit code 1. <mode> is "signed" with
// --cookies, "muxed" for -f 18/b, "anon" otherwise.
int runFakeYtDlp(const QStringList &arguments);

// --visitor-test: the one visitor id every InnerTube shares, against the same
// stand-in: one sw.js_data fetch however many ask, a stored id used with no
// fetch at all, the home page when sw.js_data fails, the 30-day limit, the
// renewal after the first play, LOGIN_REQUIRED renewed and asked once more
// (and only once), the jar shared, Clear history, the switch back, the
// account's id kept apart, and no id or cookie value in the log. Its store
// is in memory: no data folder is touched.
int runVisitorSelfTest();

// --player-client-test: /player's second client (VISIONOS 0.1) against the
// same stand-in: asked only when VISIONOS 1.02's answer holds no plain
// stream (refused, or ciphered formats only), at once and as the same app;
// never after no answer at all; after the stored id's renewal; no retry and
// a 3 s limit of its own; both refusals in the error and the log; the switch
// (youtube.player_client first|second); no id or cookie value in the log.
int runPlayerClientSelfTest();

// --format-test: which of /player's formats is played, against the same
// stand-in. Two real answers with their URLs replaced (IPeJ7iM55hc: Opus 251
// although AAC 140 peaks higher; CmThpha4Hoo: 140, with no Opus offered) and
// invented ones: DRC copies marked by isDrc or by xtags passed over, 774 >
// 251 > 141 > 140 > the rest by bitrate, a dubbed track, a ciphered format,
// formats[] alone giving itag 18, the same choice whatever the listed order,
// the itag, codec and kbps in the log (never a URL), and the switch back
// (youtube.format=bitrate).
int runFormatSelfTest();
