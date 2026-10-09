#pragma once

// Opening at sign-in (Startup): the Run value written, read back and taken
// away, the minimised start, Windows' own switch read, a value naming a
// Monolist gone made this one's and one naming another copy left and named,
// and a scratch library's run read-only — on a scratch key under
// HKEY_CURRENT_USER, removed again at the end, Windows' own never written to.
// And a start at sign-in finding Monolist open leaving its window alone
// (InstanceGuard), in the scratch library. Prints a line per check; returns
// how many failed.
int runStartupSelfTest();
