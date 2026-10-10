#pragma once

// The sleep timer (SleepTimer) on a stand-in player with no mpv: the clock
// running out, the fade and the volume put back, +10 minutes, the end of the
// song, cancelling, and a timer that finds the music already paused. Prints a
// line per check; returns how many failed.
int runSleepSelfTest();
