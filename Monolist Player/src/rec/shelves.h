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
    int row = -1;             // the catalogue row it came from
    float score = 0.0f;
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
QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf = 12);

} // namespace Rec
