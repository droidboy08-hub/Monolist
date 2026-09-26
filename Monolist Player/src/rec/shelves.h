#pragma once

#include <QMetaType>
#include <QSet>
#include <QString>
#include <QVector>

namespace Rec {

class Catalog;
class Graph;
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

// Where a shelf's rows came from, kept so See all can carry on from the same
// place rather than guess at it from the heading: the direction the rows were
// ranked by, or the artist or the country the shelf is about. Filled in by the
// builders below; only moreFrom() reads it.
struct Anchor {
    enum Kind { None, Vector, Neighbours, Country, Popular };
    Kind kind = None;
    // Vector: what the rows were ranked against, the song a "More like" shelf
    // is about (never one of its own rows), the artist a "Sounds like" shelf
    // leaves out (by artistKey), the score below which a row is not "like" it,
    // and how many rows one artist may have.
    QVector<float> query;
    int seedRow = -1;
    QString skipArtist;
    float floor = 0.0f;
    int perArtist = 2;
    // Neighbours: the credit as the listener has it, and where their plays of
    // it sit, which picks the right one of several people sharing the name.
    QString artist;
    QVector<float> heardSound;
    // Neighbours and Country: the country the listener browses as.
    QString region;
};

// A row of suggestions under a heading.
//
// `reason` is not decoration. A recommendation nobody can account for is
// indistinguishable from a mistake, and the one question a listener asks of a
// shelf is "why am I being shown this" — so every shelf carries its answer.
struct Shelf {
    QString title;            // "More like Kyoto"
    QString reason;           // "Because you played it on Tuesday"
    QString kind;             // song | artist | taste | recent | popular | region
    QVector<Suggestion> rows;
    Anchor anchor;
};

// What no shelf may offer, whatever would otherwise put it there.
//
// `owned` are the songs the listener already keeps — liked, in a playlist,
// downloaded — by strict text key (matchkey.h), as the catalogue has no ids to
// compare. A suggestion is something new, and a song already in the library is
// not one.
//
// `turnedDown` are the songs they said "Not interested" to, and `artists` the
// artists they asked never to be suggested, by suggestionArtistKey(). Both
// hold for good: a song turned down is never offered again, on any shelf, in
// See all, or by autoplay's radio. It is still theirs to search for and play.
//
// The history already names what was turned down lately, as a notInterested
// event; this is the whole list, however long ago, which the history's 4,000
// newest events would in time forget.
struct Exclusions {
    QSet<quint64> owned;
    QSet<quint64> turnedDown;
    QSet<QString> artists;
};

// The key an artist is turned down by, and compared by: the lead credit, as
// the shelves compare artists everywhere ("Bad Bunny & Drake" is Bad Bunny),
// without YouTube's " - Topic".
QString suggestionArtistKey(const QString &credit);

// Which version of the page to build. The page is a ranking, and the same
// history ranked the same way gives the same page for ever — so a listener who
// comes back finds exactly what they already passed over. A rotation keeps
// each shelf near the top of its ranking but draws its rows from a few times
// as many as it shows, the best-ranked the likeliest; and it picks which of
// the recent songs and liked artists seed the shelves after the first.
//
// Deterministic: the same rotation and the same history build the same page.
// The draw is keyed on each song rather than its place, so one more listen,
// which shifts places a little, still leaves most rows where they were.
//
// 0 is the page exactly as ranked, row for row — what the self-tests compare
// with. The app never asks for 0 (rotationFor never returns it).
//
// `period` is which stretch of time it is (the app uses 45-minute stretches of
// the clock) and `turn` how many times the listener has asked for a fresh page
// since the app started.
quint64 rotationFor(qint64 period, int turn);

// Whether the thread doing the building has been asked to stop, which the
// recommender does when the app quits (QThread::requestInterruption). Both
// builders below check it between scans and return what they have so far,
// for the caller to throw away. On a thread nobody asks, it is always false.
bool stopRequested();

// Everything the Search tab shows when nobody is searching.
//
// Catalogue only: no network, no resolution, nothing that can fail slowly.
// The shelves are built in whatever order they can be — a listener with no
// history still gets the popular shelf, one with a few plays gets "more like
// this", and the personal rails appear once the profile has earned itself.
//
// `perShelf` is how many suggestions each shelf holds. `history` is newest
// first, as readPlayEvents returns it.
//
// `graph`, when given, is where "Because you like <artist>" finds the artists
// that listeners of theirs also play; without it that shelf falls back to the
// catalogue's "Sounds like <artist>". `region` is the country the listener
// browses as, which decides where an artist is looked up first.
// `hideExplicit` is Settings' "Hide explicit titles": every row on every shelf
// is then held to explicitTitle() as well (see suitable.h). `exclude` is what
// may never be offered, and `rotation` which version of the page (both above).
QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf = 12,
                            const Graph *graph = nullptr,
                            const QString &region = QString(),
                            bool hideExplicit = false,
                            const Exclusions &exclude = Exclusions(),
                            quint64 rotation = 0);

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
//
// `hideExplicit` holds the graph's recording titles to explicitTitle() too.
//
// Rotation leaves "From <country>" alone: its reason line promises the
// most-rated first, and that is a canon, not a pick. "More from <country>"
// is "further down the same list", and a rotation moves where down the list
// it starts.
QVector<Shelf> buildRegionShelves(const Graph &graph,
                                  const Catalog *catalogue,
                                  const QString &region,
                                  const QString &regionName,
                                  const QVector<PlayEvent> &history,
                                  int perShelf = 12,
                                  bool hideExplicit = false,
                                  const Exclusions &exclude = Exclusions(),
                                  quint64 rotation = 0);

// More of one shelf, for its See all: up to `count` rows from the shelf's own
// anchor, none of them among `already` (the list so far) or excluded, the next
// best first. Fewer than `count` means the anchor has nothing more worth
// showing — a "More like" shelf runs out where songs stop being like it, a
// "Because you like" shelf where the artist's neighbours do. Needs the catalogue
// for everything but a country, and the graph for a country or a neighbour.
QVector<Suggestion> moreFrom(const Catalog *catalog,
                             const Graph *graph,
                             const Shelf &shelf,
                             const QVector<PlayEvent> &history,
                             const Exclusions &exclude,
                             const QVector<Suggestion> &already,
                             int count,
                             bool hideExplicit = false);

} // namespace Rec

// Carried to the worker thread with every build and every See all page.
Q_DECLARE_METATYPE(Rec::Exclusions)
