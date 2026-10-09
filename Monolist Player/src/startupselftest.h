#pragma once

// Opening at sign-in (Startup): the Run value written, read back and taken
// away, the minimised start, Windows' own switch read, and a scratch library
// never writing the real key — on a scratch key under HKEY_CURRENT_USER,
// removed again at the end. Prints a line per check; returns how many failed.
int runStartupSelfTest();
