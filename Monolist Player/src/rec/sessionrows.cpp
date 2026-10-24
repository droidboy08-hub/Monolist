#include "sessionrows.h"
#include "matchkey.h"
#include "shelves.h"
#include "suitable.h"

#include <QPair>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Rec {

namespace {

constexpr qint64 kRecentSecs = 48 * 3600;      // S2's reach, and the artist row's
constexpr qint64 kAnchorSecs = 14 * 24 * 3600;  // S3's
constexpr int kOverlapTracks = 25;
constexpr double kClusterOverlap = 0.30;
constexpr int kDrawFrom = 24;                   // a seed row draws from its first 24
constexpr int kKeepFirst = 4;                   // and shows its first 4 whatever the draw
constexpr int kMixSeeds = 5;
constexpr int kMixPerCluster = 2;

// The draw rec/shelves.cpp makes (drawOf, rotatedPicks), copied rather than
// shared so these rows cannot move its baselines.

quint64 mixBits(quint64 x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

double drawOf(quint64 rotation, quint64 item)
{
    return (double(mixBits(rotation ^ mixBits(item)) >> 11) + 0.5) / 9007199254740992.0;   // 2^53
}

// Which `wanted` of a ranked list a rotation shows, as positions in ranked
// order: a weighted draw without replacement (Efraimidis and Spirakis), the
// item ranked i weighing exp(-i / wanted), the first `keepFirst` shown
// whatever the draw. Rotation 0 is the first `wanted`.
QVector<int> rotatedPicks(const QVector<quint64> &items, int wanted, quint64 rotation, int keepFirst = 0)
{
    const int count = int(items.size());
    QVector<int> picks;
    if (rotation == 0 || count <= wanted) {
        for (int i = 0; i < std::min(wanted, count); ++i)
            picks.append(i);
        return picks;
    }
    for (int i = 0; i < std::min(keepFirst, count); ++i)
        picks.append(i);
    QVector<QPair<double, int>> keyed;
    keyed.reserve(count);
    for (int i = int(picks.size()); i < count; ++i) {
        const double weight = std::exp(-double(i) / double(std::max(1, wanted)));
        keyed.append({ std::log(drawOf(rotation, items.at(i))) / weight, i });
    }
    std::sort(keyed.begin(), keyed.end(), [](const QPair<double, int> &a, const QPair<double, int> &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    for (int i = 0; i < keyed.size() && picks.size() < wanted; ++i)
        picks.append(keyed.at(i).second);
    std::sort(picks.begin(), picks.end());
    return picks;
}

quint64 idKey(const QString &videoId)
{
    return fnv1a64(videoId.toUtf8());
}

// One song however it was uploaded: its cleaned title and lead artist.
quint64 songKey(const QString &title, const QString &artist, const QString &leadKey)
{
    return strictKey(cleanSeedTitle(title, artist), leadKey);
}

QString seenKey(quint64 key)
{
    return QLatin1Char('#') + QString::number(key);
}

void claim(QSet<QString> &seen, const QVector<RadioTrack> &items)
{
    for (const RadioTrack &track : items) {
        seen.insert(track.videoId);
        seen.insert(seenKey(songKey(track.title, track.artist, radioLeadKey(track))));
    }
}

// A seed row's songs: 12 drawn from the first 24 it could offer, the first 4
// always.
QVector<RadioTrack> drawRow(const QVector<RadioTrack> &eligible, int wanted, quint64 rotation)
{
    const QVector<RadioTrack> pool = eligible.mid(0, kDrawFrom);
    QVector<quint64> keys;
    keys.reserve(pool.size());
    for (const RadioTrack &track : pool)
        keys.append(idKey(track.videoId));
    QVector<RadioTrack> items;
    for (const int at : rotatedPicks(keys, wanted, rotation, kKeepFirst))
        items.append(pool.at(at));
    return items;
}

int findRoot(QVector<int> &parent, int i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

QString moreLikeTitle(const SessionSeed &seed)
{
    return QStringLiteral("More like ") + cleanSeedTitle(seed.title, seed.artist);
}

} // namespace

QString cleanSeedTitle(const QString &title, const QString &artist)
{
    // How it was uploaded, never what it is: Slowed, Sped Up, Remix, Live
    // and Reverb are not here.
    static const QRegularExpression upload(
        QStringLiteral(R"(\s*[\(\[]\s*(?:official\s+)?(?:music\s+)?(?:lyrics?|lyric\s+video|video|audio|)"
                       R"(visuali[sz]er|hd|4k|hq)(?:\s+video)?\s*[\)\]]\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    QString cleaned = title.trimmed();
    QStringList names;
    if (!artist.trimmed().isEmpty())
        names << artist.trimmed();
    const QString lead = leadArtistName(artist);
    if (!lead.isEmpty() && !names.contains(lead))
        names << lead;
    for (bool changed = true; changed;) {
        changed = false;
        const QString stripped = QString(cleaned).remove(upload).trimmed();
        if (stripped != cleaned) {
            cleaned = stripped;
            changed = true;
        }
        // "<song> - <artist>" and "<artist> - <song>".
        for (const QString &name : std::as_const(names)) {
            for (const QString &dash : { QStringLiteral(" - "), QStringLiteral(" – "), QStringLiteral(" — ") }) {
                if (cleaned.endsWith(dash + name, Qt::CaseInsensitive)) {
                    cleaned.chop(dash.size() + name.size());
                    cleaned = cleaned.trimmed();
                    changed = true;
                }
                if (cleaned.startsWith(name + dash, Qt::CaseInsensitive)) {
                    cleaned = cleaned.mid(name.size() + dash.size()).trimmed();
                    changed = true;
                }
            }
        }
    }
    return cleaned.isEmpty() ? title.trimmed() : cleaned;
}

QString leadArtistName(const QString &artist)
{
    static const QRegularExpression joints(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+topic\s*$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    return QString(artist).remove(topic).section(joints, 0, 0).trimmed();
}

QString radioLeadKey(const RadioTrack &track)
{
    return suggestionArtistKey(track.primaryArtist.isEmpty() ? track.artist : track.primaryArtist);
}

double artistOverlap(const QVector<RadioTrack> &a, const QVector<RadioTrack> &b)
{
    const auto artists = [](const QVector<RadioTrack> &radio) {
        QSet<QString> keys;
        for (int i = 0; i < std::min<int>(kOverlapTracks, int(radio.size())); ++i) {
            const QString key = radioLeadKey(radio.at(i));
            if (!key.isEmpty())
                keys.insert(key);
        }
        return keys;
    };
    const QSet<QString> first = artists(a);
    const QSet<QString> second = artists(b);
    const qsizetype smaller = std::min(first.size(), second.size());
    if (smaller == 0)
        return 0.0;
    return double(QSet<QString>(first).intersect(second).size()) / double(smaller);
}

QVector<int> clusterSeeds(const QVector<SessionSeed> &seeds, const QHash<QString, QVector<RadioTrack>> &radios)
{
    const int count = int(seeds.size());
    QVector<int> parent(count);
    std::iota(parent.begin(), parent.end(), 0);
    for (int i = 0; i < count; ++i) {
        const QVector<RadioTrack> radioI = radios.value(seeds.at(i).videoId);
        for (int j = i + 1; j < count; ++j) {
            bool join = !seeds.at(i).leadKey.isEmpty() && seeds.at(i).leadKey == seeds.at(j).leadKey;
            if (!join && !radioI.isEmpty()) {
                const auto other = radios.constFind(seeds.at(j).videoId);
                join = other != radios.cend() && !other->isEmpty() && artistOverlap(radioI, *other) >= kClusterOverlap;
            }
            if (join) {
                const int a = findRoot(parent, i);
                const int b = findRoot(parent, j);
                if (a != b)
                    parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    QVector<int> clusters(count);
    for (int i = 0; i < count; ++i)
        clusters[i] = findRoot(parent, i);
    return clusters;
}

double seedWeight(const SessionSeed &seed, qint64 nowSecs, double halfLifeHours)
{
    const double ageHours = double(std::max<qint64>(0, nowSecs - seed.lastAt)) / 3600.0;
    return std::exp(-ageHours / halfLifeHours) * (seed.chosen ? 1.0 : 0.5)
           * (1.0 + 0.25 * std::log(double(std::max(1, seed.listens)))) * (seed.liked ? 1.3 : 1.0);
}

QVector<SessionSeed> collapseSeeds(const QVector<SessionSeed> &plays)
{
    // The newest stands for the rest, which add their plays to it.
    const auto collapse = [](const QVector<SessionSeed> &in, const std::function<QString(const SessionSeed &)> &keyOf) {
        QVector<SessionSeed> out;
        QHash<QString, int> at;
        for (const SessionSeed &seed : in) {
            const QString key = keyOf(seed);
            const auto found = at.constFind(key);
            if (key.isEmpty() || found == at.cend()) {
                if (!key.isEmpty())
                    at.insert(key, int(out.size()));
                out.append(seed);
                continue;
            }
            SessionSeed &kept = out[*found];
            kept.listens += std::max(1, seed.listens);
            kept.liked = kept.liked || seed.liked;
            kept.chosen = kept.chosen || seed.chosen;
            kept.live = kept.live || seed.live;
            if (kept.artistId.isEmpty())
                kept.artistId = seed.artistId;
        }
        return out;
    };
    const QVector<SessionSeed> byId = collapse(plays, [](const SessionSeed &seed) { return seed.videoId; });
    return collapse(byId, [](const SessionSeed &seed) {
        return seenKey(songKey(seed.title, seed.artist, seed.leadKey));
    });
}

QVector<RadioTrack> eligibleItems(const QVector<RadioTrack> &radio, const QVector<SessionSeed> &seeds,
                                  const SessionOptions &options, const SessionFilter &unwanted,
                                  const QSet<QString> &seen)
{
    QSet<QString> seedIds;
    QSet<quint64> seedKeys;
    for (const SessionSeed &seed : seeds) {
        seedIds.insert(seed.videoId);
        seedKeys.insert(songKey(seed.title, seed.artist, seed.leadKey));
    }
    QVector<RadioTrack> items;
    QSet<QString> ids;
    QSet<quint64> keys;
    QHash<QString, int> perArtist;
    for (const RadioTrack &track : radio) {
        if (track.videoId.isEmpty() || track.title.isEmpty())
            continue;
        if (seedIds.contains(track.videoId) || options.recentHeard.contains(track.videoId)
            || seen.contains(track.videoId) || ids.contains(track.videoId))
            continue;
        const QString lead = radioLeadKey(track);
        const quint64 key = songKey(track.title, track.artist, lead);
        if (seedKeys.contains(key) || keys.contains(key) || seen.contains(seenKey(key)))
            continue;
        if (unwanted && unwanted(track.videoId, track.title, track.artist))
            continue;
        if (!suitableForSuggestion(track.title, track.artist, options.hideExplicit))
            continue;
        if (!lead.isEmpty() && perArtist.value(lead) >= options.perArtist)
            continue;
        perArtist[lead] += 1;
        ids.insert(track.videoId);
        keys.insert(key);
        items.append(track);
    }
    return items;
}

QVector<SessionRow> buildSessionRows(const QVector<SessionSeed> &seeds,
                                     const QHash<QString, QVector<RadioTrack>> &radios, qint64 nowSecs,
                                     const SessionOptions &options, const SessionFilter &unwanted)
{
    const int count = int(seeds.size());
    const QVector<int> clusters = clusterSeeds(seeds, radios);
    QSet<QString> seen;
    const auto usable = [&](int i) {
        const auto radio = radios.constFind(seeds.at(i).videoId);
        if (radio == radios.cend() || radio->isEmpty())
            return QVector<RadioTrack>();
        return eligibleItems(*radio, seeds, options, unwanted, seen);
    };
    const auto seedRow = [&](const QString &kind, int i, const QVector<RadioTrack> &eligible) {
        SessionRow row;
        row.kind = kind;
        row.seedVideoId = seeds.at(i).videoId;
        row.key = kind + QLatin1Char(':') + row.seedVideoId;
        row.title = moreLikeTitle(seeds.at(i));
        row.items = drawRow(eligible, options.perRow, options.rotation);
        claim(seen, row.items);
        return row;
    };

    // S1: the newest with enough to show.
    int s1 = -1;
    SessionRow rowS1;
    for (int i = 0; i < count && s1 < 0; ++i) {
        const QVector<RadioTrack> eligible = usable(i);
        if (eligible.size() >= options.minRow) {
            s1 = i;
            rowS1 = seedRow(QStringLiteral("s1"), i, eligible);
        }
    }
    if (s1 < 0)
        return {};

    // S2: another cluster's, within 48 hours, the user's own choices first;
    // failing any, another lead artist's. The rotation draws between the
    // first two that have enough.
    int s2 = -1;
    SessionRow rowS2;
    {
        QVector<int> tiers[4];
        for (int j = 0; j < count; ++j) {
            if (j == s1 || nowSecs - seeds.at(j).lastAt > kRecentSecs)
                continue;
            const bool chosen = seeds.at(j).chosen;
            if (clusters.at(j) != clusters.at(s1))
                tiers[chosen ? 0 : 1].append(j);
            else if (seeds.at(j).leadKey != seeds.at(s1).leadKey)
                tiers[chosen ? 2 : 3].append(j);
        }
        QVector<QPair<int, QVector<RadioTrack>>> found;
        for (int tier = 0; tier < 4 && found.isEmpty(); tier += 2) {
            const QVector<int> pool = tiers[tier] + tiers[tier + 1];
            for (const int j : pool) {
                const QVector<RadioTrack> eligible = usable(j);
                if (eligible.size() >= options.minRow)
                    found.append({ j, eligible });
                if (found.size() == 2)
                    break;
            }
        }
        if (!found.isEmpty()) {
            QVector<quint64> keys;
            for (const auto &candidate : std::as_const(found))
                keys.append(idKey(seeds.at(candidate.first).videoId));
            const int pick = rotatedPicks(keys, 1, options.rotation).value(0, 0);
            s2 = found.at(pick).first;
            rowS2 = seedRow(QStringLiteral("s2"), s2, found.at(pick).second);
        }
    }

    // S3: the anchor, the heaviest cluster of the fortnight that S1 and S2
    // leave out, by its newest seed with enough.
    int s3 = -1;
    SessionRow rowS3;
    {
        QHash<int, double> weights;
        for (int i = 0; i < count; ++i) {
            if (nowSecs - seeds.at(i).lastAt <= kAnchorSecs)
                weights[clusters.at(i)] += seedWeight(seeds.at(i), nowSecs, 72.0);
        }
        QVector<int> order = weights.keys();
        std::sort(order.begin(), order.end(), [&weights](int a, int b) {
            return weights.value(a) != weights.value(b) ? weights.value(a) > weights.value(b) : a < b;
        });
        for (const int cluster : std::as_const(order)) {
            if (cluster == clusters.at(s1) || (s2 >= 0 && cluster == clusters.at(s2)))
                continue;
            for (int i = 0; i < count && s3 < 0; ++i) {
                if (clusters.at(i) != cluster || nowSecs - seeds.at(i).lastAt > kAnchorSecs)
                    continue;
                const QVector<RadioTrack> eligible = usable(i);
                if (eligible.size() >= options.minRow) {
                    s3 = i;
                    rowS3 = seedRow(QStringLiteral("s3"), i, eligible);
                }
            }
            if (s3 >= 0)
                break;
        }
    }

    // The mix: what the rows above left, from S1 and up to four more of the
    // newest seeds, two a cluster at most, taken in turn (S1 twice a turn).
    SessionRow mix;
    {
        QVector<QVector<RadioTrack>> lists;
        QHash<int, int> perCluster;
        const auto take = [&](int i) {
            if (lists.size() >= kMixSeeds || perCluster.value(clusters.at(i)) >= kMixPerCluster)
                return;
            const QVector<RadioTrack> eligible = usable(i);
            if (eligible.isEmpty())
                return;
            const QVector<RadioTrack> pool = eligible.mid(0, 2 * options.mixRow);
            QVector<quint64> keys;
            for (const RadioTrack &track : pool)
                keys.append(idKey(track.videoId));
            QVector<RadioTrack> drawn;
            for (const int at : rotatedPicks(keys, options.mixRow, options.rotation))
                drawn.append(pool.at(at));
            perCluster[clusters.at(i)] += 1;
            lists.append(drawn);
        };
        take(s1);
        for (int i = 0; i < count; ++i) {
            if (i != s1)
                take(i);
        }
        if (lists.size() >= 2) {
            QVector<int> turn{ 0, 0 };
            for (int k = 1; k < lists.size(); ++k)
                turn.append(k);
            QVector<int> next(lists.size(), 0);
            QSet<QString> ids;
            QSet<quint64> keys;
            QHash<QString, int> perArtist;
            for (bool more = true; more && mix.items.size() < options.mixRow;) {
                more = false;
                for (const int k : std::as_const(turn)) {
                    if (mix.items.size() >= options.mixRow)
                        break;
                    // The next song of this seed's that the mix can still take.
                    while (next[k] < lists.at(k).size()) {
                        const RadioTrack &track = lists.at(k).at(next[k]++);
                        const QString lead = radioLeadKey(track);
                        const quint64 key = songKey(track.title, track.artist, lead);
                        if (ids.contains(track.videoId) || keys.contains(key)
                            || (!lead.isEmpty() && perArtist.value(lead) >= options.perArtist))
                            continue;
                        ids.insert(track.videoId);
                        keys.insert(key);
                        perArtist[lead] += 1;
                        mix.items.append(track);
                        break;
                    }
                    more = more || next[k] < lists.at(k).size();
                }
            }
            if (mix.items.size() >= options.minMix) {
                mix.kind = QStringLiteral("mix");
                mix.seedVideoId = seeds.at(s1).videoId;
                mix.key = QStringLiteral("mix:") + mix.seedVideoId;
                mix.title = QStringLiteral("Based on your recent listening");
                claim(seen, mix.items);
            } else {
                mix.items.clear();
            }
        }
    }

    QVector<SessionRow> rows{ rowS1 };
    if (!mix.items.isEmpty())
        rows.append(mix);
    if (s2 >= 0)
        rows.append(rowS2);
    if (s3 >= 0)
        rows.append(rowS3);
    return rows;
}

QVector<SessionSeed> artistCandidates(const QVector<SessionSeed> &seeds, qint64 nowSecs,
                                      const std::function<bool(const QString &name)> &isArtistPage,
                                      QVector<double> *weights)
{
    QVector<SessionSeed> found;
    QVector<double> summed;
    QHash<QString, int> at;
    for (const SessionSeed &seed : seeds) {
        if (seed.leadKey.isEmpty() || seed.artistName.isEmpty() || nowSecs - seed.lastAt > kRecentSecs)
            continue;
        const double weight = seedWeight(seed, nowSecs, 24.0);
        const auto known = at.constFind(seed.leadKey);
        if (known != at.cend()) {
            summed[*known] += weight;
            if (found[*known].artistId.isEmpty())
                found[*known].artistId = seed.artistId;
            continue;
        }
        at.insert(seed.leadKey, int(found.size()));
        found.append(seed);
        summed.append(weight);
    }
    QVector<int> order(found.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&summed](int a, int b) { return summed.at(a) > summed.at(b); });
    QVector<SessionSeed> candidates;
    if (weights)
        weights->clear();
    for (const int i : std::as_const(order)) {
        if (isArtistPage && !isArtistPage(found.at(i).artistName))
            continue;
        candidates.append(found.at(i));
        if (weights)
            weights->append(summed.at(i));
    }
    return candidates;
}

} // namespace Rec
