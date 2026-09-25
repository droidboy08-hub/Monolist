#include "shelves.h"

#include "catalog.h"
#include "graph.h"
#include "matchkey.h"
#include "suitable.h"
#include "taste.h"
#include "vectorsearch.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <climits>

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
        if (title.isEmpty() || !Rec::suitableForSuggestion(title, artist))
            continue;
        if (heard.contains(Rec::strictKey(title, artist)))
            continue;
        alreadyShown.insert(hit.row);
        out.append({ title, artist, hit.row, hit.score });
    }
    return out;
}

// — graph gates, shared by every shelf that reads the shards —

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

// Entries every other gate already excludes, named here as well so that a
// different catalogue — one the listener points the app at themselves — can
// never let them through. A banned neo-Nazi band, and recordings of Hitler's
// speeches filed as an artist; both are in the German shard with enough
// ratings and a confident enough attribution to pass every other filter, and
// either could as easily turn up as somebody's neighbour on an edge.
bool blocked(const QString &mbid)
{
    static const QSet<QString> never = {
        QStringLiteral("808adccd-e52b-4382-9ff6-70a3b2ab25a2"),   // Landser
        QStringLiteral("8530d70e-778c-4eba-b08d-831d16783c03"),   // Adolf Hitler
    };
    return never.contains(mbid);
}

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

// — the artists someone can fairly be said to like —

// Two names for one artist compare equal here. The primary-artist key when
// there is one; otherwise the plain lowercase name, because the key throws
// some names away entirely ("X JAPAN" loses its X to the "x" separator and
// comes out empty) and an empty key would make every such artist the same.
QString artistKey(const QString &name)
{
    const QString key = Rec::primaryArtist(name);
    return key.isEmpty() ? name.trimmed().toLower() : key;
}

// "Bruno Mars & Lady Gaga" is looked up as Bruno Mars when the full credit is
// not an artist anywhere. Written in the original casing and accents, because
// the shards are searched by name and "beyonce" is not "Beyoncé" to them.
QString firstPerformer(const QString &artist)
{
    static const QRegularExpression separator(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    return artist.section(separator, 0, 0).trimmed();
}

// A name reduced to what two spellings of it share: no accents, no case, no
// punctuation. "Beyoncé" and "BEYONCE" agree; "Belle and Sebastian" and
// "Belle and the Nursery Rhymes Band" do not.
QString plainName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name.normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        out += c.isLetterOrNumber() ? c.toLower() : QLatin1Char(' ');
    }
    return out.simplified();
}

// The catalogue rows credited to exactly this artist, most popular first.
//
// catalog.match() on a name alone finds an artist by its primary-artist key,
// and that key cuts a name at " and ", "with", "x" and the rest — so "Belle and
// Sebastian" keys as "belle" and matched a nursery-rhymes act's songs, which
// were then offered to Coldplay fans. The key finds the group; this keeps only
// the rows whose credit, or whose credit's first performer, is the name asked
// for. Empty means the catalogue does not know this artist — which is also
// what every graph gate tests, so a key collision cannot carry an unvetted
// name through on somebody else's credit.
QVector<int> creditedRows(const Catalog &catalog, const QString &name, int limit)
{
    const Catalog::Match match = catalog.match(QString(), name);
    if (match.artistId < 0)
        return {};
    const QString wanted = plainName(name);
    if (wanted.isEmpty())
        return {};
    QVector<int> rows;
    for (const int row : catalog.artistRows(match.artistId, 40)) {
        if (rows.size() >= limit)
            break;
        const QString credit = catalog.artist(row);
        if (plainName(credit) == wanted || plainName(firstPerformer(credit)) == wanted)
            rows.append(row);
    }
    return rows;
}

// "Because you like" is a claim, and one play is not enough to make it: a song
// left halfway through says nothing about liking its artist. An artist counts
// once they have a like the listener has not taken back, or two plays heard to
// at least 60 per cent.
//
// Ordered by the taste profile's own ranking when there is one — it weighs
// recency and where a play came from, which a count does not — and otherwise
// by likes and good plays, the most recent first on ties.
QStringList likedArtists(const QVector<PlayEvent> &history, const TasteProfile &taste)
{
    struct Tally {
        // The first performer, as the listener's own history spells it: a
        // joint credit keys under its lead artist, so it is also titled by them
        // ("Because you like Bruno Mars", not "… Bruno Mars & Lady Gaga").
        QString name;
        bool liked = false;
        int goodPlays = 0;
        int firstSeen = 0;             // history is newest first, so lower is more recent
    };
    QHash<QString, Tally> tallies;
    // The newest like-or-unlike per song decides whether it is liked now.
    QSet<QString> decided;

    for (int i = 0; i < history.size(); ++i) {
        const PlayEvent &event = history.at(i);
        if (event.artist.trimmed().isEmpty())
            continue;
        const QString key = artistKey(event.artist);
        Tally &tally = tallies[key];
        if (tally.name.isEmpty()) {
            const QString lead = firstPerformer(event.artist);
            tally.name = lead.isEmpty() ? event.artist.trimmed() : lead;
            tally.firstSeen = i;
        }
        const QString song = event.title + QLatin1Char('\u0001') + key;
        if (event.kind == QLatin1String("like") || event.kind == QLatin1String("unliked")) {
            if (!decided.contains(song)) {
                decided.insert(song);
                if (event.kind == QLatin1String("like"))
                    tally.liked = true;
            }
        } else if (event.kind == QLatin1String("play") && event.hasLabel && event.label >= 0.6) {
            ++tally.goodPlays;
        }
    }

    QVector<Tally> qualifying;
    for (const Tally &tally : std::as_const(tallies)) {
        if (tally.liked || tally.goodPlays >= 2)
            qualifying.append(tally);
    }

    QHash<QString, int> tasteRank;
    if (taste.valid) {
        for (int i = 0; i < taste.topArtists.size(); ++i)
            tasteRank.insert(artistKey(taste.topArtists.at(i)), i);
    }
    std::sort(qualifying.begin(), qualifying.end(), [&](const Tally &a, const Tally &b) {
        const int ra = tasteRank.value(artistKey(a.name), INT_MAX);
        const int rb = tasteRank.value(artistKey(b.name), INT_MAX);
        if (ra != rb)
            return ra < rb;
        const int sa = (a.liked ? 2 : 0) + a.goodPlays;
        const int sb = (b.liked ? 2 : 0) + b.goodPlays;
        if (sa != sb)
            return sa > sb;
        return a.firstSeen < b.firstSeen;
    });

    QStringList names;
    for (const Tally &tally : std::as_const(qualifying))
        names.append(tally.name);
    return names;
}

// "Because you like X": the artists people who listen to X also listen to,
// from the graph's ListenBrainz edges, and a song or two by each.
//
// Built from iOS's relatedTracks(to:), with three changes.
//
// The seed is found by name in any shard and its edges read from that shard's
// own chain, not the listener's: an artist's edges live where the artist does.
//
// The songs come from the catalogue, not the graph. The graph's track ranking
// is rating counts again — it offers "Love Story / interlude" as Rod Wave's
// best — while the catalogue's popularity is derived from listening. The
// graph decides who; the catalogue decides which songs.
//
// And every neighbour must be an artist the catalogue knows, which is the same
// safety gate the country shelves pass through: an edge can lead anywhere the
// shards go, and the shards go to places no shelf should.
//
// Kept from iOS without change: no edges means no shelf. A heading that says
// "Because you like X" over songs nothing connects to X is worse than nothing,
// because the listener cannot tell the difference.
Shelf listenersAlso(const Catalog &catalog, const Graph &graph, const QString &artist,
                    const QString &region, const QSet<quint64> &heard, QSet<int> &shown,
                    int perShelf)
{
    Shelf shelf;
    GraphArtist seed = graph.findArtist(artist, region);
    if (seed.mbid.isEmpty()) {
        const QString first = firstPerformer(artist);
        if (!first.isEmpty() && first != artist)
            seed = graph.findArtist(first, region);
    }
    if (seed.mbid.isEmpty() || blocked(seed.mbid))
        return shelf;

    const QString seedRegion = seed.region.isEmpty() ? region : seed.region;
    const QString seedKey = artistKey(artist);

    QVector<QVector<int>> byNeighbour;
    QSet<QString> neighbourNames;
    bool anyListening = false;
    // Twelve asked for and eight used, as on iOS: the spare four cover the
    // neighbours the gates below turn away.
    for (const GraphNeighbour &neighbour : graph.neighbours(seed.mbid, seedRegion, 12)) {
        if (byNeighbour.size() >= 8)
            break;
        if (blocked(neighbour.mbid))
            continue;
        const QString name = graph.artistName(neighbour.mbid, seedRegion);
        if (name.isEmpty() || artistKey(name) == seedKey)
            continue;
        const QString plain = plainName(name);
        if (neighbourNames.contains(plain))
            continue;
        // Only songs credited to exactly this artist: the safety gate and the
        // guard against "Belle and Sebastian" becoming a nursery-rhymes act.
        const QVector<int> credited = creditedRows(catalog, name, 8);
        if (credited.isEmpty())
            continue;
        neighbourNames.insert(plain);

        QVector<int> rows;
        for (const int row : credited) {
            if (rows.size() >= kPerArtistCap)
                break;
            if (shown.contains(row))
                continue;
            const QString title = catalog.title(row);
            if (title.isEmpty() || heard.contains(Rec::strictKey(title, catalog.artist(row)))
                || !Rec::suitableForSuggestion(title, catalog.artist(row)))
                continue;
            rows.append(row);
        }
        if (rows.isEmpty())
            continue;
        if (neighbour.source == QLatin1String("listenbrainz"))
            anyListening = true;
        byNeighbour.append(rows);
    }

    // Each neighbour's best song first, then their second, so the shelf is a
    // spread of artists rather than two apiece from the top three.
    for (int pass = 0; pass < kPerArtistCap; ++pass) {
        for (const QVector<int> &rows : std::as_const(byNeighbour)) {
            if (pass >= rows.size() || shelf.rows.size() >= perShelf)
                continue;
            const int row = rows.at(pass);
            shown.insert(row);
            shelf.rows.append({ catalog.title(row), catalog.artist(row), row, 0.0f });
        }
    }

    shelf.kind = QStringLiteral("artist");
    shelf.title = QStringLiteral("Because you like %1").arg(artist);
    // Says which kind of connection it is. The ListenBrainz edges are what
    // listeners actually play together; the structural ones are MusicBrainz's
    // own links — members, side projects, shared tags — and a shelf built only
    // from those should not claim to know what anyone listens to.
    shelf.reason = anyListening
        ? QStringLiteral("People who listen to %1 also play these").arg(artist)
        : QStringLiteral("Artists MusicBrainz connects with %1").arg(artist);
    return shelf;
}

} // namespace

QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf,
                            const Graph *graph,
                            const QString &region)
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
    //
    // One shelf per artist, the best signal available. The graph's edges come
    // first: what people who play this artist also play is the stronger reason
    // to suggest something. Where the graph has nothing for them, the
    // catalogue's nearest artists by sound stand in — under a heading that
    // says "sounds like", because that is all it knows, and a claim about
    // listeners it cannot back would be the same dishonesty the country shelf
    // was renamed to avoid.
    //
    // Each attempt works on a copy of `shown`, committed only if the shelf is
    // kept: a shelf thrown away for being too short must not have used up
    // songs the next one could have had.
    int artistShelves = 0;
    for (const QString &artist : likedArtists(history, taste)) {
        if (artistShelves >= 2)
            break;

        QSet<int> trial = shown;
        Shelf shelf;
        if (graph && graph->isOpen())
            shelf = listenersAlso(catalog, *graph, artist, region, heard, trial, perShelf);

        if (shelf.rows.size() < 4) {
            trial = shown;
            const Catalog::Match match = catalog.match(QString(), artist);
            if (match.artistId < 0)
                continue;
            // Without this the artist is their own nearest neighbour: "Sounds
            // like Bad Bunny" opened with Bad Bunny, twice. artistNeighbours
            // drops the seed's own artist id, but a joint credit is a different
            // id with the same lead ("Bad Bunny & Drake"), so the lead is
            // compared instead.
            const QString seedKey = artistKey(artist);
            QVector<Hit> hits;
            for (const Hit &hit : artistNeighbours(catalog, match.artistId, perShelf * 4, 0)) {
                if (artistKey(catalog.artist(hit.row)) != seedKey)
                    hits.append(hit);
            }
            shelf = Shelf();
            shelf.kind = QStringLiteral("artist");
            shelf.title = QStringLiteral("Sounds like %1").arg(artist);
            shelf.reason = QStringLiteral("Artists whose sound sits closest to theirs");
            shelf.rows = collect(catalog, hits, heard, trial, perShelf, 0.0f);
        }

        if (shelf.rows.size() >= 4) {
            shown = trial;
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
        // Exactly this artist, not merely one sharing its key: the key cuts a
        // name at " and " and the like, so a match on the key alone would let
        // an unvetted graph artist through on a different artist's credit.
        if (creditedRows(*catalogue, artist.name, 1).isEmpty())
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
            if (track.title.isEmpty() || seenRecordings.contains(track.recordingMbid)
                || !Rec::suitableForSuggestion(track.title, track.artistName))
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
