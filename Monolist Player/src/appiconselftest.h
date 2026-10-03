#pragma once

class Library;

// The app icon choice (AppIcon): the two pictures, the setting, and the
// shortcuts pointed at the executable's icons — on shortcuts made for the
// test in MONOLIST_DATA_DIR, never the real Start menu or desktop. Prints a
// line per check; returns how many failed.
int runAppIconSelfTest(Library *library);
