#pragma once

#include <QString>
#include <QVector>

namespace Rec {

class Catalog;
struct TasteProfile;
struct PlayEvent;

// One suggestion. It is a name, not a track: the catalogue knows what a song
// is called and who by, and nothing else — no stream, no id, no artwork.
// Turning it into something playable costs a search, so that is left until
// somebody actually presses it (see Recommender::play).
struct Suggestion {
    QString title;
    QString artist;
    int row = -1;             // the catalogue row it came from; -1 for a graph track
    float score = 0.0f;
    // Graph recordings carry their MusicBrainz length, and pressing one picks
    // the search result within fifteen seconds of it. Catalogue rows have no
    // length and say -1.
    qint64 lengthMs = -1;
};

// A row of suggestions under a heading.
//
// `reason` is not decoration. A recommendation nobody can account for is
// indistinguishable from a mistake, and the one question a listener asks of a
// shelf is "why am I being shown this" — so every shelf carries its answer.
struct Shelf {
    QString title;            // "More like Kyoto"
    QString reason;           // "Because you played it on Tuesday"
    QString kind;             // song | artist | taste | recent | popular
    QVector<Suggestion> rows;
};

// Everything the Search tab shows when nobody is searching.
//
// Catalogue only: no network, no resolution, nothing that can fail slowly.
// The shelves are built in whatever order they can be — a listener with no
// history still gets the popular shelf, one with a few plays gets "more like
// this", and the personal rails appear once the profile has earned itself.
//
// `perShelf` is how many suggestions each shelf holds. `history` is newest
// first, as readPlayEvents returns it.
class Graph;

// `graph`, when given, is where "Because you like <artist>" finds the artists
// that listeners of theirs also play; without it that shelf falls back to the
// catalogue's "Sounds like <artist>". `region` is the country the listener
// browses as, which decides where an artist is looked up first.
QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf = 12,
                            const Graph *graph = nullptr,
                            const QString &region = QString());

// "From <country>" and "More from <country>", out of the regional graph shards.
//
// Not "Popular in", which is what the iOS app calls it, because the shards
// cannot support the claim. An artist's region is where MusicBrainz says they
// are from, not where they are listened to, and they are ranked by how many
// MusicBrainz ratings they have, not by plays — there is no date or listen
// count anywhere in the schema. Measured on the real data: Germany's list
// includes Bach and Beethoven, Britain's is the Beatles and Pink Floyd, and
// India's top ten includes Cliff Richard. "From Germany" is true of all of it.
//
// `catalogue` is required, and is the safety gate as well as a quality one:
// every artist shown must also be one the music catalogue knows. The shards
// carry non-musicians and worse — a banned neo-Nazi band and recordings of
// Hitler's speeches sit in the German shard with enough ratings and a
// confident enough attribution to pass every other filter. Without a catalogue
// to vet against, no regional shelf is built at all.
//
// A country whose data cannot fill a shelf with artists that pass is left out
// rather than padded with the worldwide list under its name.
QVector<Shelf> buildRegionShelves(const Graph &graph,
                                  const Catalog *catalogue,
                                  const QString &region,
                                  const QString &regionName,
                                  const QVector<PlayEvent> &history,
                                  int perShelf = 12);

} // namespace Rec
