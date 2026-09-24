#pragma once

#include <QVector>

#include <functional>

namespace Rec {

class Catalog;

// Exact nearest-neighbour search over the embedding catalogue.
//
// Brute force, not a graph index, and that is a measured choice rather than a
// shortcut. An HNSW index over 400,000 rows stores its vectors inline as f32:
// ~80 MB on disk and 91-226 MB of dirty, non-evictable RAM next to an audio
// pipeline and a library database, plus ~0.8 s of index load per launch to save
// ~2 ms per query. A feed refresh asks about eight questions, so the index never
// earns back its load. The scan instead reads the mapped f16 payload, whose
// pages are clean and file-backed and cost nothing to give back.
//
// Filtering is what settles it. Every shelf wants a popularity floor, the
// library's own rows out, not-interested rows out and a per-artist cap. Here
// that is a predicate evaluated before any arithmetic, so an excluded row costs
// one call instead of a dot product; with a graph index it is post-filtering
// with over-fetch, and recall collapses exactly when the filter is selective.
//
// A full scan is tens of milliseconds, so none of this belongs on the GUI
// thread. Everything here only reads, and Catalog's const methods are safe to
// call from several threads at once, so a scan can go straight onto a worker.

struct Hit
{
    int row;
    float score;   // raw dot product of two unit vectors, so already a cosine
};

// Exact top-`k` rows for each query vector, each column sorted by descending
// score. The result always has one entry per query, in the order they were
// given; a query of the wrong length gets an empty column rather than shifting
// everything after it.
//
// Rows are walked in blocks of 4,096. A row that fails `minPopularity` or
// `keep` is dropped before it is even widened to float, which is the whole
// point of doing this by hand: the filters are cheap and the arithmetic is not.
// An empty `keep` means "every row", so the common unfiltered scan does not
// have to allocate a closure that always says yes.
//
// `k` is a scan depth, not a page size. Callers ask for more than they intend
// to show because capPerArtist() eats into the list afterwards.
//
// Up to 12 columns share one pass, which is what the per-song anchor family
// needs; more than that is allowed and simply costs another walk over the block
// scratch. Scores are comparable across columns only if every query is
// unit-length - nothing here normalises for the caller.
QVector<QVector<Hit>> topK(const Catalog &catalog,
                           const QVector<QVector<float>> &queries,
                           int k,
                           int minPopularity,
                           const std::function<bool(int row)> &keep);

// Keeps at most `cap` hits per artist, in the order given, dropping the rest.
//
// Diversity by cosine alone cannot do this. Every row by one artist shares the
// same 56 genre dimensions and differs only in the 8 acoustic ones, so two
// tracks by the same act sit at ~0.99 from each other - indistinguishable from
// two tracks by different acts in the same genre. A seed therefore returns six
// rows by the seed artist and reads as a discography, not a recommendation.
// Cosine cannot express "someone else" in a space built from genre; a cap can.
//
// Artists are counted by their normalised primary-artist key, not by interned
// id, so "Beyonce" and "Beyonce" spelled with the accent share one allowance.
// The 154 rows whose artist name normalises away share one bucket between them.
QVector<Hit> capPerArtist(const Catalog &catalog, const QVector<Hit> &hits, int cap);

// The anchor families the engine draws shelves from. Each one runs the same
// three steps in the same order - drop the query's own seed rows during the
// scan, cap per artist, then take the page - because doing it any other way
// hands the artist cap to rows that a later filter deletes, and the shelf ends
// up with nothing by the artists it was about. Scan depth is
// max(120, offset + limit) so that a deep page still has rows under it.

// Nearest rows to a vector already in hand: taste rails, genre rails, anything
// whose query was built elsewhere. One row per artist, because these are the
// widest shelves in the app and the listener is owed range.
//
// There is no seed argument: a caller holding rows to exclude (the shelf's own
// tracks, the library, not-interested keys) calls topK() with a `keep` and then
// capPerArtist() itself, which is the same three steps with its own filter.
QVector<Hit> nearest(const Catalog &catalog,
                     const QVector<float> &query,
                     int limit,
                     int offset,
                     int minPopularity);

// Rows near one artist's centroid - "more like <artist>". Two per artist, and
// the anchor's own rows are not excluded, so a couple of them may come back
// among other people's.
QVector<Hit> artistNeighbours(const Catalog &catalog, int artistId, int limit, int offset);

// Rows from inside one genre, ranked by how central they are to it. Empty when
// the catalogue shipped without genre labels. Two per artist.
QVector<Hit> genreNeighbours(const Catalog &catalog, int genreId, int limit, int offset);

} // namespace Rec
