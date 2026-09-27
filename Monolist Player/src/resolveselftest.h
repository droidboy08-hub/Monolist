#pragma once

#include <QString>
#include <QStringList>

// Resolving, measured on the real thing (main.cpp runs these). Each reports on
// stderr, one line per check or measurement, and returns how many checks
// failed.

// --cancel-test [videoId] [rounds]: what a skip does to a track still being
// resolved by yt-dlp, timed. Each round starts the yt-dlp tier for the id
// (LrM_Y39Gmhk, royalty-free, unless one is given) and cancels it the way
// PlaybackController does on a skip, 0.3, 0.9 or 1.8 s in, while a 5 ms
// timer on this, the interface's thread, records the longest gap between two
// of its ticks. Rounds alternate between the cancel as it is and as the
// switch back has it (ytdlp.cancel=wait), `rounds` of each (6 unless given),
// and a cancel that does not wait must hold the thread under 100 ms and
// still see every process it started end. Needs yt-dlp and the network.
int runCancelSelfTest(const QString &videoId, int rounds);

// --player-canary [videoId...]: each /player client alone, on real YouTube,
// for each id (LrM_Y39Gmhk unless given): what it answered, the itag, how
// long it took, whether the link carries n= or pot=, and whether the link
// serves both its first and its second megabyte (a link that serves only
// the first is how a client that needs a PO token shows itself). Never the
// link itself, which carries the machine's address. Each client is a check:
// VISIONOS 1.02 failing is what playback feels at once (every track then
// costs the second client's round trip, or yt-dlp's seconds); VISIONOS 0.1
// failing is the rung under it gone.
int runPlayerCanary(const QStringList &videoIds);
