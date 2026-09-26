#pragma once

class Library;

// Self-tests for the YouTube Music sign-in, run from the command line
// (main.cpp). Each reports on stderr, one line per check, and returns how many
// failed.
//
// Every cookie is invented ("TESTSAPISID123", "TESTVAL-..."): nothing here
// reads, prints or writes a real cookie, and nothing is ever sent to YouTube.

// --cookie-test: the import parser on synthetic fixtures (cookies.txt with LF
// and CRLF, "#HttpOnly_" lines, spaces where the tabs go, duplicate names
// across domains, a missing LOGIN_INFO; a Cookie header; cURL in bash and
// cmd.exe quoting), the jar as it is stored, and the SAPISIDHASH known
// answers. No network and no data folder.
int runCookieImportSelfTest();

// --ytm-session-test: YtmSession and InnerTube's account path against a
// stand-in server on this computer: signed-out requests byte for byte as
// before, the account's headers only where asked for, the check (logged_in,
// the account menu, twice-zero, no answer), Set-Cookie rotation kept out of
// the anonymous jar, a 400 that never touches the country, 401/403, a
// restart, sign-out, the offer to delete an imported file, and no cookie
// value in the log. Refuses to run without MONOLIST_DATA_DIR, since it
// replaces the stored session and the account setting.
int runYtmSessionSelfTest(Library *library);
