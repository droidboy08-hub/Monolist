#pragma once

// Self-tests for the connections' plumbing, run from the command line with no
// network and no window (main.cpp). Each reports on stderr, one line per
// check, and returns how many checks failed.
//
// Every value they use is invented ("TESTSECRET...", "TESTSESSIONKEY..."):
// nothing here reads, prints or writes a real key, session or cookie.

// --secret-test: SecretStore round trips, tamper detection and delete, in
// the data folder (MONOLIST_DATA_DIR), under names starting "selftest.".
int runSecretStoreSelfTest();

// --lastfm-test: api_sig known-answer vectors, the form body, and the reply
// reader on canned answers for every error and ignored code.
int runLastFmSelfTest();
