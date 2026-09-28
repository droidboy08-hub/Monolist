#pragma once

#include <QHash>
#include <QString>

#include "lyricsprovider.h"

// What is kept of lyrics, in the database.
//
// Two tables. `lyrics` holds the answer for each song, the one the pane shows
// (or that none was found, and when); `lyrics_results` holds each provider's
// own answer, found or none, so a provider that has answered is not asked
// again while its answer is fresh: when an answer is checked again because a
// better provider failed, only that provider is asked. Failures are kept in
// neither: they stay in memory, and the next lookup asks again.
namespace LyricsStore {

// What is kept for a song: nothing (or "none" long enough ago to ask again),
// an answer to show, or one to show and ask for again behind it (provisional,
// or found long enough ago to check).
enum class Stored { Nothing, Show, ShowAndCheck };
Stored read(const QString &videoId, LyricsAnswer *answer);
void write(const QString &videoId, const LyricsAnswer &answer);
// Kept as it is, and looked at again only after the refresh age.
void touch(const QString &videoId);

// Each provider's answer still fresh, by provider id: found for 60 days, none
// for 3, as the song's own answer.
QHash<QString, LyricsOutcome> results(const QString &videoId);
void writeResult(const QString &videoId, const QString &provider, const LyricsOutcome &outcome);

} // namespace LyricsStore
