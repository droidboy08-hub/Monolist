#pragma once

#include <QString>

class QJsonObject;

// How loud YouTube measured each song, kept so that the song is levelled the
// same whichever copy of it plays: a stream, a download, a replay from the
// link cache, JioSaavn's copy.
//
// YouTube's own normalisation reads the same number: loudnessDb, how many dB
// a format is above its reference (positive is louder). YouTube only ever
// turns a song down by it, never up, and so does Monolist (gainFor): a boost
// would need a limiter to keep it from clipping.
namespace Loudness {

// The song's loudness from its /player answer: the chosen format's own
// loudnessDb (`formatDb`, NaN when the format had none), or failing that the
// answer's perceptualLoudnessDb, which is on another scale (LUFS-like) and is
// moved onto YouTube's by +14. NaN when the answer says neither.
double fromAnswer(double formatDb, const QJsonObject &player);

// Kept in memory and in the database, so a download or a later launch knows
// it too. Main thread only.
void remember(const QString &videoId, double db);
// NaN when it was never measured.
double of(const QString &videoId);

// What mpv is told to apply: -db, only ever down, and never by more than
// 15 dB. 0 when the loudness is unknown.
double gainFor(double db);

} // namespace Loudness
