#include "shelves.h"

#include "catalog.h"
#include "graph.h"
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

namespace {

// Below this many MusicBrainz ratings an artist's rank is noise: most of the
// shards' long tail sits at 0, 1 or 2, and ties there fall back to the order
// the build happened to insert rows in. Three is where the order starts to
// mean something (measured across all 72 shards with --graph-test).
constexpr int kMinRatings = 3;

// How sure the build was of where an artist is from. Below this are the
// attributions that are simply wrong — David Bowie filed under the US, Sonic
// Youth under Brazil.
constexpr double kMinConfidence = 0.8;

// A country needs this many artists that pass everything before it gets a
// shelf at all: one per row, so the shelf is a list of the country's artists
// rather than a handful of them twice over. 22 of the 72 shards reach it.
constexpr int kMinArtists = 12;

// Entries the gates above already exclude, named here as well so that a
// different catalogue — one the listener points the app at themselves — can
// never let them through. A banned neo-Nazi band, and recordings of Hitler's
// speeches filed as an artist; both are in the German shard with enough
// ratings and a confident enough attribution to pass every other filter.
// Han, kana or hangul anywhere in the text: the scripts an East Asian
// edition's release titles are written in.
bool eastAsian(const QString &text)
{
    for (const char32_t c : text.toUcs4()) {
        switch (QChar::script(c)) {
        case QChar::Script_Han:
        case QChar::Script_Hiragana:
        case QChar::Script_Katakana:
        case QChar::Script_Hangul:
            return true;
        default:
            break;
        }
    }
    return false;
}

bool blocked(const QString &mbid)
{
    static const QSet<QString> never = {
        QStringLiteral("808adccd-e52b-4382-9ff6-70a3b2ab25a2"),   // Landser
        QStringLiteral("8530d70e-778c-4eba-b08d-831d16783c03"),   // Adolf Hitler
    };
    return never.contains(mbid);
}

} // namespace

// Derived from GraphChannelProvider's "Popular in", with the differences that
// the data forced (see shelves.h) and two that were choices.
//
// iOS resolves forty recordings to YouTube before showing a single row; here a
// row is a name until it is pressed, so none of that is paid. And iOS takes the
// first fourteen candidates in the order they were gathered — four tracks from
// the top artist, then four from the next — so its shelf is three or four
// artists deep. This takes one track per artist per pass, so the shelf reads as
// a list of the country's artists, and holds to the same two-per-artist rule as
// every other shelf.
QVector<Shelf> buildRegionShelves(const Graph &graph,
                                  const Catalog *catalogue,
                                  const QString &region,
                                  const QString &regionName,
                                  const QVector<PlayEvent> &history,
                                  int perShelf)
{
    QVector<Shelf> shelves;
    // No catalogue, no shelf: it is what keeps the non-musicians out.
    if (!graph.isOpen() || !catalogue || !catalogue->isLoaded())
        return shelves;

    // Read deeper than the 40 iOS reads, because the gates below remove a good
    // share. Reading deeper is only safe because of them — the band and the
    // speeches are at ranks 40 and 81 in Germany.
    QVector<GraphArtist> seeds;
    for (const GraphArtist &artist : graph.artists(region, 120)) {
        // Only the country's own shards. Asking for "JP" walks on to the
        // global shard to top the list up, and a heading that says "From
        // Japan" over The Beatles is exactly the mislabelling this avoids.
        if (!artist.region.startsWith(region, Qt::CaseInsensitive))
            continue;
        if (blocked(artist.mbid))
            continue;
        if (artist.popularity < kMinRatings || artist.confidence < kMinConfidence)
            continue;
        if (catalogue->match(QString(), artist.name).artistId < 0)
            continue;
        seeds.append(artist);
    }
    if (seeds.size() < kMinArtists)
        return shelves;

    // SQLite returns ties in whatever order the rows were inserted, which a
    // rebuilt shard would change. Ordering on the id as well makes the same
    // shard give the same shelf every time.
    std::stable_sort(seeds.begin(), seeds.end(), [](const GraphArtist &a, const GraphArtist &b) {
        return a.popularity != b.popularity ? a.popularity > b.popularity : a.mbid < b.mbid;
    });

    const QSet<quint64> heard = heardKeys(history);
    static const QStringList eastAsianCountries = {
        QStringLiteral("JP"), QStringLiteral("KR"), QStringLiteral("CN"),
        QStringLiteral("TW"), QStringLiteral("HK")
    };
    const bool eastAsianRegion = eastAsianCountries.contains(region.left(2).toUpper());

    // Up to four tracks for each of the first 28 artists — the iOS budget —
    // kept per artist so the passes below can interleave them.
    QVector<QVector<GraphTrack>> byArtist;
    QSet<QString> seenRecordings;
    QSet<quint64> seenKeys;
    for (int i = 0; i < seeds.size() && i < 28; ++i) {
        // A recording's title is whatever its release called it, and the
        // most-rated recordings of the German composers are East Asian
        // editions: Bach's top track reads トッカータとフーガ ニ短調, Beethoven's
        // is a Waldstein titled in Traditional Chinese. Under "From Germany"
        // that reads as broken. So outside the East Asian countries, when the
        // artist's own name is not in one of those scripts, a title that is
        // comes last — preferred against, never dropped, so an artist with
        // nothing else still gets a row. On a Japanese or Korean shelf those
        // titles are the native ones, BABYMETAL's included, and nothing moves.
        QVector<GraphTrack> candidates = graph.tracks(seeds.at(i).mbid, region, 8);
        if (!eastAsianRegion && !eastAsian(seeds.at(i).name)) {
            std::stable_partition(candidates.begin(), candidates.end(),
                                  [](const GraphTrack &track) { return !eastAsian(track.title); });
        }
        QVector<GraphTrack> kept;
        for (const GraphTrack &track : std::as_const(candidates)) {
            if (kept.size() >= 4)
                break;
            if (track.title.isEmpty() || seenRecordings.contains(track.recordingMbid))
                continue;
            seenRecordings.insert(track.recordingMbid);
            // The same song is often several MusicBrainz recordings — the single,
            // the album cut, a remaster. By name they are one row.
            const quint64 key = Rec::strictKey(track.title, track.artistName);
            if (seenKeys.contains(key) || heard.contains(key))
                continue;
            seenKeys.insert(key);
            kept.append(track);
        }
        if (!kept.isEmpty())
            byArtist.append(kept);
    }

    // First pass takes each artist's best track, the second their next.
    // Two passes is the per-artist cap.
    QVector<Suggestion> chart;
    for (int pass = 0; pass < kPerArtistCap; ++pass) {
        for (const QVector<GraphTrack> &tracks : byArtist) {
            if (pass >= tracks.size())
                continue;
            const GraphTrack &track = tracks.at(pass);
            Suggestion row;
            row.title = track.title;
            row.artist = track.artistName;
            row.lengthMs = track.lengthMs;
            chart.append(row);
        }
    }
    if (chart.size() < 4)
        return shelves;

    Shelf popular;
    popular.kind = QStringLiteral("region");
    popular.title = QStringLiteral("From %1").arg(regionName);
    // Says what the ordering is, because "most rated" is not "most played" and
    // the list reads as a canon rather than a chart — the reason line is where
    // a listener finds that out instead of being left to assume.
    popular.reason = QStringLiteral("Artists from %1, the most-rated on MusicBrainz first").arg(regionName);
    popular.rows = chart.mid(0, perShelf);
    shelves.append(popular);

    // Starts where the first ended: the same song under two headings on one
    // page is the thing iOS had to fix here.
    const QVector<Suggestion> deeper = chart.mid(perShelf, perShelf);
    if (deeper.size() >= 4) {
        Shelf more;
        more.kind = QStringLiteral("region");
        more.title = QStringLiteral("More from %1").arg(regionName);
        more.reason = QStringLiteral("Further down the same list");
        more.rows = deeper;
        shelves.append(more);
    }
    return shelves;
}

} // namespace Rec
