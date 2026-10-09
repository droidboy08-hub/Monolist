#pragma once

class Library;

// Closing to the system tray (Tray): the switch and its setting, the menu
// as built, and what it does — with no window, in MONOLIST_DATA_DIR only.
// Prints a line per check; returns how many failed.
int runTraySelfTest(Library *library);
