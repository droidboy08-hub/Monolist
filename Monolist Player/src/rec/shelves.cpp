#include "shelves.h"

#include "catalog.h"
#include "matchkey.h"
#include "taste.h"
#include "vectorsearch.h"

#include <QSet>

#include <algorithm>

namespace Rec {
namespace {

// At most two songs by one artist in a shelf. The catalogue's vectors put
// every track by an artist within about 0.99 of each other — 56 of the 64
// dimensions are the artist's genre embedding, identical across their whole
// discography — so without a cap every shelf is one artist's back catalogue.
constexpr int kPerArtistCap = 2;

// Below this a "more like this" row is not like it in any way a listener
// would recognise. The absolute floor is the iOS one.
constexpr float kSongFloor = 0.85f;

// How deep to scan. Asking for more than a shelf holds leaves room for the
// per-artist cap and the already-heard filter to throw rows away.
int scanDepth(int wanted)
{
    return std::max(120, wanted * 8);
}

QString titleOf(const Catalog &catalog, int row)
{
    return catalog.title(row);
}

// Songs already played are not suggestions. This is the "owned" filter the
// spec describes, keyed on the text key rather than a stream id, because a
// catalogue row has no stream id to compare.
QSet<quint64> heardKeys(const QVector<PlayEvent> &history)
{
    QSet<quint64> keys;
    for (const PlayEvent &event : history) {
        if (event.title.isEmpty())
            continue;
        keys.insert(Rec::strictKey(event.title, event.artist));
    }
    return keys;
}

QVector<Suggestion> collect(const Catalog &catalog,
                            const QVector<Hit> &hits,
                            const QSet<quint64> &heard,
                            QSet<int> &alreadyShown,
                            int wanted,
                            float floorScore)
{
    QVector<Suggestion> out;
    for (const Hit &hit : hits) {
        if (out.size() >= wanted)
            break;
        if (hit.score < floorScore)
            break;                     // hits arrive sorted, so the rest are worse
        if (alreadyShown.contains(hit.row))
            continue;                  // an earlier shelf already used it
        const QString title = titleOf(catalog, hit.row);
        const QString artist = catalog.artist(hit.row);
        if (title.isEmpty())
            continue;
        if (heard.contains(Rec::strictKey(title, artist)))
            continue;
        alreadyShown.insert(hit.row);
        out.append({ title, artist, hit.row, hit.score });
    }
    return out;
}

} // namespace

QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf)
{
    QVector<Shelf> shelves;
    if (!catalog.isLoaded())
        return shelves;

    const QSet<quint64> heard = heardKeys(history);
    // One song shows up on one shelf. The alternative — the same track under
    // three headings — reads as a bug however defensible each heading is.
    QSet<int> shown;

    // — what they played, and what is like it —
    //
    // First because it needs no profile and no gate: one play is enough, and
    // it is the shelf a new listener sees on their second visit.
    int songShelves = 0;
    QSet<int> seededRows;
    for (const PlayEvent &event : history) {
        if (songShelves >= 3)
            break;
        if (event.kind != QLatin1String("play") || event.title.isEmpty())
            continue;
        const Catalog::Match match = catalog.match(event.title, event.artist);
        if (match.row < 0 || seededRows.contains(match.row))
            continue;
        seededRows.insert(match.row);

        QVector<float> query(catalog.dims());
        std::copy_n(catalog.vector(match.row), catalog.dims(), query.begin());
        const int seedRow = match.row;
        QVector<Hit> hits = topK(catalog, { query }, scanDepth(perShelf), 0,
                                 [seedRow](int row) { return row != seedRow; }).value(0);
        hits = capPerArtist(catalog, hits, kPerArtistCap);

        Shelf shelf;
        shelf.kind = QStringLiteral("song");
        shelf.title = QStringLiteral("More like %1").arg(catalog.title(match.row));
        shelf.reason = QStringLiteral("Because you played %1 by %2")
                           .arg(event.title, event.artist.isEmpty()
                                                 ? QStringLiteral("them") : event.artist);
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, kSongFloor);
        if (shelf.rows.size() >= 4) {
            shelves.append(shelf);
            ++songShelves;
        }
    }

    // — the profile's own shelves —
    if (taste.valid && !taste.positive.isEmpty()) {
        QVector<Hit> hits = topK(catalog, { taste.positive }, scanDepth(perShelf), 0, {}).value(0);
        hits = capPerArtist(catalog, hits, kPerArtistCap);
        Shelf shelf;
        shelf.kind = QStringLiteral("taste");
        shelf.title = QStringLiteral("Made for you");
        shelf.reason = QStringLiteral("From everything you have played, weighted towards what you finished");
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, 0.0f);
        if (shelf.rows.size() >= 4)
            shelves.append(shelf);
    }

    if (taste.valid && !taste.recent.isEmpty()) {
        QVector<Hit> hits = topK(catalog, { taste.recent }, scanDepth(perShelf), 0, {}).value(0);
        hits = capPerArtist(catalog, hits, kPerArtistCap);
        Shelf shelf;
        shelf.kind = QStringLiteral("recent");
        shelf.title = QStringLiteral("On repeat lately");
        shelf.reason = QStringLiteral("Weighted to the last few days rather than the last few months");
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, 0.0f);
        if (shelf.rows.size() >= 4)
            shelves.append(shelf);
    }

    // — the artists they keep coming back to —
    int artistShelves = 0;
    for (const QString &artist : taste.topArtists) {
        if (artistShelves >= 2)
            break;
        const Catalog::Match match = catalog.match(QString(), artist);
        const int artistId = match.artistId >= 0 ? match.artistId : -1;
        if (artistId < 0)
            continue;
        QVector<Hit> hits = artistNeighbours(catalog, artistId, perShelf * 4, 0);
        Shelf shelf;
        shelf.kind = QStringLiteral("artist");
        shelf.title = QStringLiteral("Because you like %1").arg(artist);
        shelf.reason = QStringLiteral("Artists whose sound sits closest to theirs");
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, 0.0f);
        if (shelf.rows.size() >= 4) {
            shelves.append(shelf);
            ++artistShelves;
        }
    }

    // — the cold start —
    //
    // Last, and only when nothing else filled the page. It is not a
    // recommendation and does not pretend to be: it is the catalogue's most
    // played, so a listener with no history has somewhere to begin.
    if (shelves.isEmpty()) {
        QVector<Hit> popular;
        popular.reserve(perShelf * 6);
        for (int row = 0; row < catalog.count() && popular.size() < perShelf * 6; ++row) {
            if (catalog.popularity(row) < 90)
                continue;
            popular.append({ row, float(catalog.popularity(row)) });
        }
        popular = capPerArtist(catalog, popular, 1);
        Shelf shelf;
        shelf.kind = QStringLiteral("popular");
        shelf.title = QStringLiteral("Somewhere to start");
        shelf.reason = QStringLiteral("Not personal yet — play a few songs and this page becomes yours");
        shelf.rows = collect(catalog, popular, heard, shown, perShelf, 0.0f);
        if (!shelf.rows.isEmpty())
            shelves.append(shelf);
    }

    return shelves;
}

} // namespace Rec
