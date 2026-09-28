#pragma once

#include <QString>

// --saavn-test: JioSaavn's part checked with no network (saavnselftest.cpp):
// DES against the standard's known answers and another implementation's
// output, links decrypted and moved up to 320 kbps, both shapes of a details
// answer, HTML entities, and the matcher on invented rows — the same song, a
// remaster, live against studio, remixes, covers, credits, "&" in names,
// lengths two and five seconds off, other scripts. Returns how many failed.
int runSaavnSelfTest();

// --saavn "<title>" "<artist>" [seconds]: one real lookup, as playback makes
// it. Prints the row taken, its bitrate and its host (never the whole link),
// and why every other row was turned down. 0 for a match, 1 for none, 2 when
// JioSaavn could not be asked.
int runSaavnLookup(const QString &title, const QString &artist, qint64 durationMs, bool indiaHeaders);
