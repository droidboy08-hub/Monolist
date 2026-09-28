// review <catalogue> <graph> <out.txt> <seeds.tsv>
// seeds.tsv: name \t country   (UTF-8)
// For each seed: buildShelves exactly as --artist-test (two full listens, TasteProfile()), region US,
// then the seed's own country (on a second Graph, so findArtist's cache from the US pass is not reused).
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <cmath>

#include "rec/catalog.h"
#include "rec/graph.h"
#include "rec/matchkey.h"
#include "rec/shelves.h"
#include "rec/taste.h"

using namespace Rec;

// copies of the anonymous-namespace helpers in shelves.cpp, for diagnosis only
static QString firstPerformer(const QString &artist)
{
    static const QRegularExpression separator(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    return artist.section(separator, 0, 0).trimmed();
}
static QVector<float> soundOf(const Catalog &catalog, const QVector<int> &rows)
{
    const int dims = catalog.dims();
    QVector<double> sum(dims, 0.0);
    int used = 0;
    for (const int row : rows) {
        const float *v = catalog.vector(row);
        if (!v) continue;
        for (int i = 0; i < dims; ++i) sum[i] += v[i];
        ++used;
    }
    double norm = 0.0;
    for (double x : sum) norm += x * x;
    norm = std::sqrt(norm);
    if (used == 0 || !(norm > 0.0)) return {};
    QVector<float> out(dims);
    for (int i = 0; i < dims; ++i) out[i] = float(sum[i] / norm);
    return out;
}
static float similarity(const QVector<float> &a, const QVector<float> &b)
{
    if (a.size() != b.size() || a.isEmpty()) return -9.0f;
    double dot = 0.0;
    for (int i = 0; i < a.size(); ++i) dot += double(a[i]) * double(b[i]);
    return float(dot);
}
static QString artistKey(const QString &name)
{
    const QString key = Rec::primaryArtist(name);
    return key.isEmpty() ? name.trimmed().toLower() : key;
}
static QVector<int> creditedRows(const Catalog &catalog, const QString &name, int limit)
{
    const Catalog::Match match = catalog.match(QString(), name);
    if (match.artistId < 0) return {};
    const QString wanted = plainName(name);
    if (wanted.isEmpty()) return {};
    QVector<int> rows;
    for (const int row : catalog.artistRows(match.artistId, 40)) {
        if (rows.size() >= limit) break;
        const QString credit = catalog.artist(row);
        if (plainName(credit) == wanted || plainName(firstPerformer(credit)) == wanted)
            rows.append(row);
    }
    return rows;
}

static QVector<QVector<int>> peopleNamed(const Catalog &catalog, const QString &name, int limit)
{
    QVector<QVector<int>> people;
    for (const int row : creditedRows(catalog, name, 40)) {
        bool placed = false;
        for (QVector<int> &person : people) {
            const float *x = catalog.vector(person.first()); const float *y = catalog.vector(row);
            double d = 0; for (int i = 0; i < catalog.dims(); ++i) d += double(x[i]) * y[i];
            if (d >= 0.8) { if (person.size() < limit) person.append(row); placed = true; break; }
        }
        if (!placed) people.append(QVector<int>{ row });
    }
    return people;
}
static QVector<int> closestPerson(const Catalog &catalog, const QVector<QVector<int>> &people, const QVector<float> &towards)
{
    if (people.isEmpty()) return {};
    if (towards.isEmpty()) return people.first();
    int best = 0; float bs = -2;
    for (int i = 0; i < people.size(); ++i) { const float sc = similarity(towards, soundOf(catalog, people[i])); if (sc > bs) { bs = sc; best = i; } }
    return people[best];
}

static QVector<PlayEvent> likedTwice(const QString &artist)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVector<PlayEvent> history;
    for (int i = 0; i < 2; ++i) {
        PlayEvent event;
        event.kind = QStringLiteral("play");
        event.title = QStringLiteral("seed song %1").arg(i);
        event.artist = artist;
        event.source = QStringLiteral("search");
        event.when = now.addDays(-i);
        event.hasLabel = true;
        event.label = 1.0;
        event.trackMs = 200000;
        event.listenedMs = 200000;
        history.append(event);
    }
    return history;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    Catalog catalog;
    Graph gUS, gOwn;
    const bool ok = catalog.load(args.at(1)) && gUS.open(args.at(2)) && gOwn.open(args.at(2));
    QFile outFile(args.at(3));
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QTextStream out(&outFile);
    out.setEncoding(QStringConverter::Utf8);
    out << "loaded " << (ok ? "yes" : "NO") << "\n";

    QFile seedFile(args.at(4));
    if (!seedFile.open(QIODevice::ReadOnly)) return 3;
    QList<QPair<QString, QString>> seeds;
    for (const QByteArray &line : seedFile.readAll().split('\n')) {
        const QString l = QString::fromUtf8(line).trimmed();
        if (l.isEmpty() || l.startsWith('#')) continue;
        const QStringList parts = l.split('\t');
        seeds.append({ parts.at(0), parts.value(1, QStringLiteral("US")) });
    }

    int nGraph = 0, nSounds = 0, nNone = 0, nSeedHits = 0, nDup = 0;
    for (const auto &seed : seeds) {
        for (int pass = 0; pass < 2; ++pass) {
            const QString region = pass == 0 ? QStringLiteral("US") : seed.second;
            if (pass == 1 && region == QLatin1String("US")) continue;
            Graph &g = pass == 0 ? gUS : gOwn;
            const QVector<Shelf> shelves = buildShelves(catalog, TasteProfile(), likedTwice(seed.first), 12, &g, region);
            Shelf shelf;
            QStringList kinds;
            for (const Shelf &s : shelves) {
                kinds << s.kind;
                if (s.kind == QLatin1String("artist") && shelf.title.isEmpty()) shelf = s;
            }
            // what the shelf asked the graph for
            QString liked = seed.first;
            GraphArtist found = g.findArtist(liked, region);
            if (found.mbid.isEmpty() && firstPerformer(liked) != liked && !firstPerformer(liked).isEmpty()) {
                liked = firstPerformer(liked);
                found = g.findArtist(liked, region);
            }
            out << "\n=== " << seed.first << "  [region " << region << "]  liked-as \"" << liked << "\"\n";
            out << "  seed in graph: " << (found.mbid.isEmpty() ? QStringLiteral("NOT FOUND")
                     : found.name + " / " + found.region + " / conf " + QString::number(found.confidence) + " / pop " + QString::number(found.popularity) + " / " + found.mbid) << "\n";
            if (!found.mbid.isEmpty()) {
                const QString seedRegion = found.region.isEmpty() ? region : found.region;
                const QVector<GraphNeighbour> nb = g.neighbours(found.mbid, seedRegion, 24);
                const QVector<float> seedSound = soundOf(catalog, closestPerson(catalog, peopleNamed(catalog, found.name, 20), {}));
                out << "  seed sound: " << (seedSound.isEmpty() ? "NONE" : "yes") << "\n";
                QStringList desc;
                int used = 0;
                for (const GraphNeighbour &n : nb) {
                    const QString name = g.artistName(n.mbid, seedRegion);
                    const QVector<QVector<int>> ppl = peopleNamed(catalog, name, 8); const QVector<int> credited = closestPerson(catalog, ppl, seedSound);
                    const bool cred = !credited.isEmpty();
                    const QString tag = n.source == QLatin1String("listenbrainz") ? QStringLiteral("LB") : QStringLiteral("st");
                    const QString close = cred ? QString::number(similarity(seedSound, soundOf(catalog, credited)), 'f', 3) : QStringLiteral("-nocat");
                    desc << QStringLiteral("%1(%2 %3 ~%4)").arg(name, tag, QString::number(n.score, 'f', 2), close);
                    if (cred) {
                        ++used;
                        out << "    NB\t" << seed.first << "\t" << name << "\t" << tag << "\t" << close << "\t" << catalog.genreName(catalog.genreId(credited.first())) << "\tpeople " << ppl.size() << "\n";
                    }
                }
                out << "  graph neighbours (" << nb.size() << "): " << desc.join(", ") << "\n";
            }
            if (shelf.rows.isEmpty()) {
                out << "  -> NO ARTIST SHELF  (shelves: " << kinds.join(",") << ")\n";
                if (pass == 0) ++nNone;
                continue;
            }
            const bool graphShelf = shelf.title.startsWith(QLatin1String("Because"));
            if (pass == 0) { graphShelf ? ++nGraph : ++nSounds; }
            out << "  -> " << shelf.title << "  |  " << shelf.reason << "  (" << shelf.rows.size() << " rows)\n";
            const QString seedPlain = plainName(liked);
            const QString seedFullPlain = plainName(seed.first);
            QSet<QString> cores;
            QSet<quint64> keys;
            for (const Suggestion &r : shelf.rows) {
                QStringList flags;
                const QString creditPlain = plainName(r.artist);
                const QString padded = QLatin1Char(' ') + creditPlain + QLatin1Char(' ');
                if (artistKey(r.artist) == artistKey(liked)
                    || padded.contains(QLatin1Char(' ') + seedPlain + QLatin1Char(' '))
                    || padded.contains(QLatin1Char(' ') + seedFullPlain + QLatin1Char(' ')))
                    flags << QStringLiteral("SEED");
                const QString core = Rec::titleCore(r.title);
                if (cores.contains(core)) flags << QStringLiteral("DUPTITLE");
                cores.insert(core);
                const quint64 k = Rec::strictKey(r.title, r.artist);
                if (keys.contains(k)) flags << QStringLiteral("DUPKEY");
                keys.insert(k);
                if (pass == 0 && flags.contains(QStringLiteral("SEED"))) ++nSeedHits;
                if (pass == 0 && (flags.contains(QStringLiteral("DUPTITLE")) || flags.contains(QStringLiteral("DUPKEY")))) ++nDup;
                out << "     " << r.artist << " — " << r.title << "   [" << catalog.genreName(catalog.genreId(r.row))
                    << ", pop " << catalog.popularity(r.row) << ", ~" << QString::number(r.score, 'f', 3) << "]" << (flags.isEmpty() ? QString() : QStringLiteral("   <<") + flags.join(",")) << "\n";
            }
        }
        out.flush();
    }
    // Does findArtist's cache ignore the region? Ask the US graph the own-country question.
    out << "\n=== cache check: findArtist(name, own country) on the US-warmed graph vs a graph that only saw that country\n";
    for (const auto &seed : seeds) {
        if (seed.second == QLatin1String("US")) continue;
        const QString liked = firstPerformer(seed.first).isEmpty() ? seed.first : firstPerformer(seed.first);
        const GraphArtist a = gUS.findArtist(liked, seed.second);
        const GraphArtist b = gOwn.findArtist(liked, seed.second);
        if (a.mbid != b.mbid || a.region != b.region)
            out << "  DIFFERS " << liked << ": US-warmed " << a.region << " vs fresh " << b.region << "\n";
    }
    out << "\nSUMMARY (US pass): graph " << nGraph << ", sounds-like " << nSounds << ", none " << nNone
        << ", seed-in-own-shelf rows " << nSeedHits << ", duplicate rows " << nDup << "\n";
    return 0;
}
