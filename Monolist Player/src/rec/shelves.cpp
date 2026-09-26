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
#include <QThread>

#include <algorithm>
#include <climits>
#include <cmath>

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

// Below this a graph neighbour is taken to be a different artist who shares
// the name (see listenersAlso), or an edge that only says everyone plays them.
//
// Measured over 955 neighbours of 50 seed artists: the namesakes found by
// review scored 0.04 to 0.41 against their seeds (U2's Bono 0.04, Boris's
// MONO 0.09, SB19's justin 0.12 and PABLO 0.20, Can's Nico 0.41), and the
// real neighbours of those same seeds 0.53 and up. Between 0.25 and 0.45 lie
// mostly the saturated edges — Kraftwerk to The Beatles, U2 to P!nk — and
// raising the floor across that band left every seed that had a shelf with
// one (38 of 50 at 0.25 and at 0.45). So the higher of the two.
constexpr float kNeighbourFloor = 0.45f;

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

// What the page already holds: catalogue rows, songs by their text key, and
// titles.
//
// One song shows up once on one page. Rows alone let the same song through
// twice — a remaster, a version "(feat. JAY-Z)", a reissue are all different
// rows — and review found a quarter of the "Sounds like" shelves repeating
// one: Grimes' "Genesis", Pixies' "Where Is My Mind?" and its 2007 remaster.
// The text key strips exactly those differences.
//
// The key still includes the artist, so a duo's song reissued under one of
// them — "Maniatica Sexual" by Lito y Polaco, then by Polaco — passed as two
// songs. The title alone catches that. It also turns away a different song
// that happens to share a name, which costs nothing: a shelf is filled from
// a list many times its length.
struct Shown {
    QSet<int> rows;
    QSet<quint64> songs;
    QSet<QString> titles;

    bool has(int row, const QString &title, const QString &artist) const
    {
        if (row >= 0 && rows.contains(row))
            return true;
        if (songs.contains(Rec::strictKey(title, artist)))
            return true;
        const QString core = Rec::titleCore(title);
        return !core.isEmpty() && titles.contains(core);
    }
    void add(int row, const QString &title, const QString &artist)
    {
        if (row >= 0)
            rows.insert(row);
        songs.insert(Rec::strictKey(title, artist));
        const QString core = Rec::titleCore(title);
        if (!core.isEmpty())
            titles.insert(core);
    }
};

QVector<Suggestion> collect(const Catalog &catalog,
                            const QVector<Hit> &hits,
                            const QSet<quint64> &heard,
                            Shown &shown,
                            int wanted,
                            float floorScore,
                            bool hideExplicit)
{
    QVector<Suggestion> out;
    for (const Hit &hit : hits) {
        if (out.size() >= wanted)
            break;
        if (hit.score < floorScore)
            break;                     // hits arrive sorted, so the rest are worse
        const QString title = titleOf(catalog, hit.row);
        const QString artist = catalog.artist(hit.row);
        if (title.isEmpty() || !Rec::suitableForSuggestion(title, artist, hideExplicit))
            continue;
        if (heard.contains(Rec::strictKey(title, artist)) || shown.has(hit.row, title, artist))
            continue;
        shown.add(hit.row, title, artist);
        out.append({ title, artist, hit.row, hit.score });
    }
    return out;
}

// Where an artist's songs sit in the catalogue's space: the mean of the rows
// given, L2-normalised. Empty when there are none.
QVector<float> soundOf(const Catalog &catalog, const QVector<int> &rows)
{
    const int dims = catalog.dims();
    QVector<double> sum(dims, 0.0);
    int used = 0;
    for (const int row : rows) {
        const float *vector = catalog.vector(row);
        if (!vector)
            continue;
        for (int i = 0; i < dims; ++i)
            sum[i] += vector[i];
        ++used;
    }
    double norm = 0.0;
    for (const double x : std::as_const(sum))
        norm += x * x;
    norm = std::sqrt(norm);
    if (used == 0 || !(norm > 0.0))
        return {};
    QVector<float> out(dims);
    for (int i = 0; i < dims; ++i)
        out[i] = float(sum.at(i) / norm);
    return out;
}

float similarity(const QVector<float> &a, const QVector<float> &b)
{
    if (a.size() != b.size() || a.isEmpty())
        return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < a.size(); ++i)
        dot += double(a.at(i)) * double(b.at(i));
    return float(dot);
}

float rowSimilarity(const Catalog &catalog, int a, int b)
{
    const float *x = catalog.vector(a);
    const float *y = catalog.vector(b);
    if (!x || !y)
        return 0.0f;
    double dot = 0.0;
    for (int i = 0; i < catalog.dims(); ++i)
        dot += double(x[i]) * double(y[i]);
    return float(dot);
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

// MusicBrainz's placeholders — "[unknown]", "[anonymous]", "[traditional]" —
// which it writes in brackets by convention. They are not artists, and
// "[unknown]" has edges in the worldwide shard: folded, it is "unknown", which
// the catalogue credits to an actual act of that name, so it passed every gate.
bool placeholder(const QString &name)
{
    const QString trimmed = name.trimmed();
    return trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'));
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

// A credit as the listener would say it: YouTube's auto-generated channels are
// "Adele - Topic", which is Adele.
QString withoutTopic(const QString &artist)
{
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+topic\s*$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    return QString(artist).remove(topic).trimmed();
}

// The lead of a joint credit — "Bruno Mars & Lady Gaga" is Bruno Mars — for
// when the full credit is not an artist anywhere. Only ever the fallback: a
// band called "AC/DC" or "Simon & Garfunkel" is not "AC" or "Simon", and cutting
// first produced "Sounds like AC" over three tribute bands. Written in the
// original casing, because it is used as a name, not as a key.
QString firstPerformer(const QString &artist)
{
    static const QRegularExpression separator(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    return withoutTopic(artist).section(separator, 0, 0).trimmed();
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

// Two songs credited to one name, this close or closer, are by one person.
//
// A name is not an artist. The catalogue's "LISA" is LiSA the anime singer,
// BLACKPINK's Lisa and a third; its "MONO" a V-pop act, the Japanese post-rock
// band and a lo-fi channel; its "Mayhem" the black metal band and a UK drill
// artist. Songs by one person sit very close — 56 of the 64 dimensions are
// the artist's genre, the same on every song — and across the names measured
// no song sat further than 0.906 from its artist's best-known one, while the
// nearest namesake came to 0.655 (Rosé the Swedish singer, to ROSÉ).
constexpr float kSamePerson = 0.8f;

// The songs credited to exactly `name`, split into the people behind them.
// Each list is most popular first, at most `limit` long, and the lists are in
// the order of their best-known song — so the first is the famous one.
QVector<QVector<int>> peopleNamed(const Catalog &catalog, const QString &name, int limit)
{
    QVector<QVector<int>> people;
    for (const int row : creditedRows(catalog, name, 40)) {
        bool placed = false;
        for (QVector<int> &person : people) {
            if (rowSimilarity(catalog, person.first(), row) >= kSamePerson) {
                if (person.size() < limit)
                    person.append(row);
                placed = true;
                break;
            }
        }
        if (!placed)
            people.append(QVector<int>{ row });
    }
    return people;
}

// Of the people sharing a name, the one whose songs sound most like
// `towards`; the best known when there is nothing to compare with.
QVector<int> closestPerson(const Catalog &catalog, const QVector<QVector<int>> &people,
                           const QVector<float> &towards)
{
    if (people.isEmpty())
        return {};
    if (towards.isEmpty())
        return people.first();
    int best = 0;
    float bestScore = -2.0f;
    for (int i = 0; i < people.size(); ++i) {
        const float score = similarity(towards, soundOf(catalog, people.at(i)));
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return people.at(best);
}

// Where the songs the listener actually played by this artist sit: which, of
// the people sharing the name, is the one they mean. Empty when none of the
// plays is a catalogue row.
QVector<float> heardSoundOf(const Catalog &catalog, const QVector<PlayEvent> &history,
                            const QString &artist)
{
    const QString key = artistKey(artist);
    QVector<int> rows;
    for (const PlayEvent &event : history) {
        if (rows.size() >= 20)
            break;
        const QString credit = withoutTopic(event.artist);
        if (event.title.isEmpty() || artistKey(credit) != key)
            continue;
        const Catalog::Match match = catalog.match(event.title, credit);
        if (match.row >= 0 && !rows.contains(match.row))
            rows.append(match.row);
    }
    return soundOf(catalog, rows);
}

// "Because you like" is a claim, and one play is not enough to make it: a song
// left early says nothing about liking its artist. An artist counts once they
// have a like the listener has not taken back, or two plays heard at least
// halfway through.
//
// Halfway is read from the playhead and the length directly, not from the
// label: the label is upgraded to 1.0 when a song is replayed in one sitting,
// which is right for the taste profile but would let three fifteen-second
// plays of one song claim that someone likes its artist.
//
// Ordered by the taste profile's own ranking when there is one — it weighs
// recency and where a play came from, which a count does not — and otherwise
// by likes and good plays, the most recent first on ties.
QStringList likedArtists(const QVector<PlayEvent> &history, const TasteProfile &taste)
{
    struct Tally {
        // The credit as the listener's newest play of them spells it, whole:
        // the lookup tries the full name before its lead, and the heading is
        // whatever name the lookup finally matched.
        QString name;
        bool liked = false;
        int goodPlays = 0;
        int firstSeen = 0;             // history is newest first, so lower is more recent
    };
    QHash<QString, Tally> tallies;
    // The newest like-or-unlike per song decides whether it is liked now.
    const QSet<quint64> liked = likedSongs(history);

    for (int i = 0; i < history.size(); ++i) {
        const PlayEvent &event = history.at(i);
        const QString credit = withoutTopic(event.artist);
        if (credit.isEmpty())
            continue;
        const QString key = artistKey(credit);
        Tally &tally = tallies[key];
        if (tally.name.isEmpty()) {
            tally.name = credit;
            tally.firstSeen = i;
        }
        if (event.kind == QLatin1String("like")) {
            if (liked.contains(Rec::strictKey(event.title, event.artist)))
                tally.liked = true;
        } else if (event.kind == QLatin1String("play") && event.trackMs > 0
                   && event.listenedMs * 2 >= event.trackMs) {
            ++tally.goodPlays;
        }
    }

    QVector<Tally> qualifying;
    for (const Tally &tally : std::as_const(tallies)) {
        if (tally.liked || tally.goodPlays >= 2)
            qualifying.append(tally);
    }

    // An artist can appear more than once in the profile's twelve under
    // different credits; their best position is the one that counts.
    QHash<QString, int> tasteRank;
    if (taste.valid) {
        for (int i = 0; i < taste.topArtists.size(); ++i) {
            const QString key = artistKey(withoutTopic(taste.topArtists.at(i)));
            if (!tasteRank.contains(key))
                tasteRank.insert(key, i);
        }
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
//
// And one thing iOS never needed, because it resolved every neighbour to a
// YouTube video by MBID: a neighbour here is a NAME, looked up in a catalogue
// that has its own artists by the same names. U2's neighbour Bono is, in the
// catalogue, only an Albanian rapper; SB19's Justin is only a Hawaiian singer.
// Each is exactly credited, so no name check can tell them apart. What does is
// sound. Of everyone in the catalogue by a neighbour's name, the one that
// sounds most like the seed is taken — Can's Nico is then the Velvet
// Underground's, not a Romanian pop singer — and if even that one sounds
// nothing like the seed, it is somebody else, and left out. The rest are
// ordered by how close they sound. The seed is picked the same way, towards
// the songs the listener actually played by them.
Shelf listenersAlso(const Catalog &catalog, const Graph &graph, const QString &artist,
                    const QVector<float> &heardSound, const QString &region,
                    const QSet<quint64> &heard, Shown &shown, int perShelf, bool hideExplicit)
{
    Shelf shelf;
    // The whole credit first — "Simon & Garfunkel" is an artist — and its lead
    // only when the whole is not.
    GraphArtist seed = graph.findArtist(artist, region);
    if (seed.mbid.isEmpty()) {
        const QString first = firstPerformer(artist);
        if (!first.isEmpty() && first != artist)
            seed = graph.findArtist(first, region);
    }
    if (seed.mbid.isEmpty() || blocked(seed.mbid) || placeholder(seed.name))
        return shelf;

    const QString seedRegion = seed.region.isEmpty() ? region : seed.region;
    const QString seedKey = artistKey(seed.name);
    const QVector<int> seedRows = closestPerson(catalog, peopleNamed(catalog, seed.name, 20), heardSound);
    const QVector<float> seedSound = seedRows.isEmpty() ? heardSound : soundOf(catalog, seedRows);

    struct Candidate {
        QString name;
        QVector<int> rows;
        float closeness = 0.0f;
        bool listening = false;
    };
    QVector<Candidate> candidates;
    QSet<QString> neighbourNames;
    // iOS asks for twelve and uses eight. Twice that is read here, because the
    // sound check below turns away more than the old gates did, and a shelf
    // of three artists is not worth a heading.
    for (const GraphNeighbour &neighbour : graph.neighbours(seed.mbid, seedRegion, 24)) {
        if (blocked(neighbour.mbid))
            continue;
        const QString name = graph.artistName(neighbour.mbid, seedRegion);
        if (name.isEmpty() || placeholder(name) || artistKey(name) == seedKey)
            continue;
        const QString plain = plainName(name);
        if (plain.isEmpty() || neighbourNames.contains(plain))
            continue;
        // Only songs credited to exactly this name: the safety gate, and the
        // guard against "Belle and Sebastian" becoming a nursery-rhymes act.
        // Then only one person's songs of those.
        const QVector<int> credited =
            closestPerson(catalog, peopleNamed(catalog, name, 8), seedSound);
        if (credited.isEmpty())
            continue;
        neighbourNames.insert(plain);

        Candidate candidate;
        candidate.name = name;
        candidate.listening = neighbour.source == QLatin1String("listenbrainz");
        if (!seedSound.isEmpty()) {
            candidate.closeness = similarity(seedSound, soundOf(catalog, credited));
            if (candidate.closeness < kNeighbourFloor)
                continue;
        }
        Shown own;
        for (const int row : credited) {
            if (candidate.rows.size() >= kPerArtistCap)
                break;
            const QString title = catalog.title(row);
            const QString credit = catalog.artist(row);
            if (title.isEmpty() || !Rec::suitableForSuggestion(title, credit, hideExplicit))
                continue;
            if (heard.contains(Rec::strictKey(title, credit)) || shown.has(row, title, credit)
                || own.has(row, title, credit))
                continue;
            own.add(row, title, credit);
            candidate.rows.append(row);
        }
        if (!candidate.rows.isEmpty())
            candidates.append(candidate);
    }

    // The graph's own order is a score, and the ListenBrainz scores are
    // saturated — in the US, UK, German and Irish shards every one reads 1.00
    // — so for Miles Davis it was whichever rock act the build inserted first.
    // Closeness in sound is the order that still means something. Stable, so
    // with no seed sound to compare against the graph's order stands.
    if (!seedSound.isEmpty()) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate &a, const Candidate &b) {
                             return a.closeness > b.closeness;
                         });
    }
    if (candidates.size() > 8)
        candidates.resize(8);

    // Each neighbour's best song first, then their second, so the shelf is a
    // spread of artists rather than two apiece from the top three.
    bool anyListening = false;
    bool anyStructural = false;
    for (int pass = 0; pass < kPerArtistCap; ++pass) {
        for (const Candidate &candidate : std::as_const(candidates)) {
            if (pass >= candidate.rows.size() || shelf.rows.size() >= perShelf)
                continue;
            const int row = candidate.rows.at(pass);
            const QString title = catalog.title(row);
            const QString credit = catalog.artist(row);
            if (shown.has(row, title, credit))
                continue;               // two neighbours credited on one song
            shown.add(row, title, credit);
            shelf.rows.append({ title, credit, row, candidate.closeness });
            (candidate.listening ? anyListening : anyStructural) = true;
        }
    }

    shelf.kind = QStringLiteral("artist");
    // The name the graph knows them by, which is the one the lookup matched:
    // "Because you like Bruno Mars", not "…Bruno Mars & Anderson .Paak".
    shelf.title = QStringLiteral("Because you like %1").arg(seed.name);
    // Says which kind of connection it is, and only what the rows on the shelf
    // bear out. The ListenBrainz edges are what listeners actually play
    // together; the structural ones are MusicBrainz's own links — members,
    // side projects, shared tags — and a shelf built from those should not
    // claim to know what anyone listens to.
    if (anyListening && !anyStructural)
        shelf.reason = QStringLiteral("People who listen to %1 also play these").arg(seed.name);
    else if (anyStructural && !anyListening)
        shelf.reason = QStringLiteral("Artists MusicBrainz connects with %1").arg(seed.name);
    else
        shelf.reason = QStringLiteral("What listeners of %1 play, and artists linked to them").arg(seed.name);
    return shelf;
}

} // namespace

bool stopRequested()
{
    return QThread::currentThread()->isInterruptionRequested();
}

QVector<Shelf> buildShelves(const Catalog &catalog,
                            const TasteProfile &taste,
                            const QVector<PlayEvent> &history,
                            int perShelf,
                            const Graph *graph,
                            const QString &region,
                            bool hideExplicit)
{
    QVector<Shelf> shelves;
    if (!catalog.isLoaded())
        return shelves;

    const QSet<quint64> heard = heardKeys(history);
    // One song shows up on one shelf. The alternative — the same track under
    // three headings — reads as a bug however defensible each heading is.
    Shown shown;

    // — what they played, and what is like it —
    //
    // First because it needs no profile and no gate: one play is enough, and
    // it is the shelf a new listener sees on their second visit.
    //
    // One play that said yes, though. A song abandoned after five seconds
    // counts against itself in the taste profile, and "More like it, because
    // you played it" over a song they skipped reads as not listening. Yes is
    // what the profile takes for yes: a label of 0.6 or better, a song liked
    // now, or, for a listen never labelled, half a minute heard.
    const QSet<quint64> liked = likedSongs(history);
    const auto saidYes = [&liked](const PlayEvent &event) {
        if (liked.contains(Rec::strictKey(event.title, event.artist)))
            return true;
        if (event.hasLabel)
            return event.label >= 0.6;
        return event.listenedMs >= 30000;
    };
    int songShelves = 0;
    QSet<int> seededRows;
    for (const PlayEvent &event : history) {
        if (songShelves >= 3 || stopRequested())
            break;
        if (event.kind != QLatin1String("play") || event.title.isEmpty() || !saidYes(event))
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
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, kSongFloor, hideExplicit);
        if (shelf.rows.size() >= 4) {
            shelves.append(shelf);
            ++songShelves;
        }
    }

    // — the profile's own shelves —
    if (stopRequested())
        return shelves;
    if (taste.valid && !taste.positive.isEmpty()) {
        QVector<Hit> hits = topK(catalog, { taste.positive }, scanDepth(perShelf), 0, {}).value(0);
        hits = capPerArtist(catalog, hits, kPerArtistCap);
        Shelf shelf;
        shelf.kind = QStringLiteral("taste");
        shelf.title = QStringLiteral("Made for you");
        shelf.reason = QStringLiteral("From everything you have played, weighted towards what you finished");
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, 0.0f, hideExplicit);
        if (shelf.rows.size() >= 4)
            shelves.append(shelf);
    }

    if (taste.valid && !taste.recent.isEmpty() && !stopRequested()) {
        QVector<Hit> hits = topK(catalog, { taste.recent }, scanDepth(perShelf), 0, {}).value(0);
        hits = capPerArtist(catalog, hits, kPerArtistCap);
        Shelf shelf;
        shelf.kind = QStringLiteral("recent");
        shelf.title = QStringLiteral("On repeat lately");
        shelf.reason = QStringLiteral("Weighted to the last few days rather than the last few months");
        shelf.rows = collect(catalog, hits, heard, shown, perShelf, 0.0f, hideExplicit);
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
        if (artistShelves >= 2 || stopRequested())
            break;

        Shown trial = shown;
        Shelf shelf;
        const QVector<float> heardSound = heardSoundOf(catalog, history, artist);
        if (graph && graph->isOpen())
            shelf = listenersAlso(catalog, *graph, artist, heardSound, region, heard, trial, perShelf,
                                  hideExplicit);

        if (shelf.rows.size() < 4) {
            trial = shown;
            // The sound of exactly this artist's songs. artistNeighbours took
            // the centroid of everyone sharing the artist's KEY, and the key
            // cuts "AC/DC" to "ac" — so "Sounds like AC/DC" was the sound of
            // three tribute bands averaged together. The whole credit first,
            // as with the graph, then its lead; and of the people by that
            // name, the one the listener has been playing.
            QString name = artist;
            QVector<QVector<int>> people = peopleNamed(catalog, name, 20);
            if (people.isEmpty()) {
                name = firstPerformer(artist);
                if (!name.isEmpty() && name != artist)
                    people = peopleNamed(catalog, name, 20);
            }
            const QVector<float> sound = soundOf(catalog, closestPerson(catalog, people, heardSound));
            if (sound.isEmpty())
                continue;
            // Without this the artist is their own nearest neighbour: "Sounds
            // like Bad Bunny" opened with Bad Bunny, twice. A joint credit is
            // a different artist with the same lead ("Bad Bunny & Drake"), so
            // the lead is what is compared.
            const QString seedKey = artistKey(name);
            QVector<Hit> hits;
            for (const Hit &hit : nearest(catalog, sound, perShelf * 4, 0, 0)) {
                if (artistKey(catalog.artist(hit.row)) != seedKey)
                    hits.append(hit);
            }
            shelf = Shelf();
            shelf.kind = QStringLiteral("artist");
            shelf.title = QStringLiteral("Sounds like %1").arg(name);
            shelf.reason = QStringLiteral("Artists whose sound sits closest to theirs");
            shelf.rows = collect(catalog, hits, heard, trial, perShelf, 0.0f, hideExplicit);
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
    if (shelves.isEmpty() && !stopRequested()) {
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
        shelf.rows = collect(catalog, popular, heard, shown, perShelf, 0.0f, hideExplicit);
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
                                  int perShelf,
                                  bool hideExplicit)
{
    QVector<Shelf> shelves;
    // No catalogue, no shelf: it is what keeps the non-musicians out.
    if (!graph.isOpen() || !catalogue || !catalogue->isLoaded() || stopRequested())
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
        if (blocked(artist.mbid) || placeholder(artist.name))
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
        if (stopRequested())
            return shelves;
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
                || !Rec::suitableForSuggestion(track.title, track.artistName, hideExplicit))
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
