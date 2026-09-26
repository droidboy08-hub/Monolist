#include "taste.h"

#include "catalog.h"
#include "matchkey.h"
#include "../appdatabase.h"

#include <QHash>
#include <QLocale>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace Rec {
namespace {

// Half-lives of 41.6 and 2.08 days. The long view is what someone likes; the
// short one is what they are listening to this week, and the two disagree
// often enough to be worth keeping apart.
constexpr double kLongDecayDays = 60.0;
constexpr double kShortDecayDays = 3.0;

// "Back to a couple of months ago" is this window, undecayed — decay would
// defeat the point of looking backwards.
constexpr double kEarlierMinDays = 30.0;
constexpr double kEarlierMaxDays = 90.0;

// Below this there is not enough to say anything, and a profile built from
// three songs would confidently recommend the neighbours of three songs.
constexpr double kMinPositiveMass = 3.0;
constexpr int kMinMatchedTracks = 8;

// Each secondary centroid needs its own mass before it is trusted; the
// weekend one needs more because a single weekend afternoon would otherwise
// define what weekends sound like.
constexpr double kMinNegativeMass = 1.5;
constexpr double kMinRecentMass = 1.5;
constexpr double kMinWeekendMass = 2.0;
constexpr double kMinEarlierMass = 2.0;

constexpr double kNegativeWeightCap = 0.35;

// How much each surface's opinion counts. Someone who searched for a song by
// name has said far more than someone who let a queue run.
double sourceMultiplier(const QString &source)
{
    if (source == QLatin1String("notInterested")) return 2.0;
    if (source == QLatin1String("like"))          return 1.6;
    if (source == QLatin1String("explore"))       return 1.0;
    if (source == QLatin1String("search"))        return 1.0;
    if (source == QLatin1String("home"))          return 0.9;
    if (source == QLatin1String("queue"))         return 0.8;
    if (source == QLatin1String("playlist"))      return 0.7;
    if (source == QLatin1String("library"))       return 0.7;
    if (source == QLatin1String("resume"))        return 0.3;
    return 1.0;                        // unknown surface: neutral, never zero
}

// The label ladder, read as opinion. Note 0.2 is worth exactly nothing rather
// than a little: half-listening to something says neither yes nor no.
//
// Comparisons are exact equality on purpose. The labels are stored, never
// re-derived by arithmetic, precisely so that 0.6 is the same 0.6 everywhere.
bool labelWeight(const PlayEvent &event, double *out)
{
    if (event.kind == QLatin1String("like"))          { *out = 1.0;  return true; }
    if (event.kind == QLatin1String("notInterested")) { *out = -1.0; return true; }
    if (event.kind != QLatin1String("play"))
        return false;                  // an "unliked" tombstone is handled before this

    if (event.hasLabel) {
        if (event.label == 1.0) { *out = 1.0;   return true; }
        if (event.label == 0.6) { *out = 0.45;  return true; }
        if (event.label == 0.2) { *out = 0.0;   return true; }
        if (event.label == 0.0) { *out = -0.20; return true; }
        return false;
    }
    // No label, but finalised and listened to for a while: a track of unknown
    // length played for half a minute is a quiet yes.
    if (event.listenedMs >= 30000) { *out = 0.45; return true; }
    return false;
}

bool isWeekend(const QDateTime &when)
{
    // The locale's weekend, not Saturday and Sunday: in much of the world it
    // is neither. Qt names the working days, so the weekend is what is left —
    // which also handles the locales whose weekend wraps the week end.
    //
    // And the listener's own day, not UTC's, which is what the events are
    // stored in: a Friday night in New York is already Saturday in UTC, and a
    // Saturday morning in Sydney still Friday.
    static const QList<Qt::DayOfWeek> working = QLocale::system().weekdays();
    return !working.contains(Qt::DayOfWeek(when.toLocalTime().date().dayOfWeek()));
}

void addScaled(QVector<double> &sum, const float *vector, int dims, double weight)
{
    for (int i = 0; i < dims; ++i)
        sum[i] += double(vector[i]) * weight;
}

// L2-normalised, or empty when there is nothing there. An unnormalised
// centroid would make a shelf's scores depend on how much was listened to
// rather than on what.
QVector<float> normalised(const QVector<double> &sum, double mass, double minimumMass)
{
    if (mass < minimumMass)
        return {};
    double norm = 0.0;
    for (double value : sum)
        norm += value * value;
    norm = std::sqrt(norm);
    if (!(norm > 0.0))
        return {};
    QVector<float> out(sum.size());
    for (int i = 0; i < sum.size(); ++i)
        out[i] = float(sum.at(i) / norm);
    return out;
}

double dot(const QVector<float> &centroid, const float *vector, int dims)
{
    if (centroid.size() != dims)
        return 0.0;
    double total = 0.0;
    for (int i = 0; i < dims; ++i)
        total += double(centroid.at(i)) * double(vector[i]);
    return total;
}

} // namespace

float TasteProfile::score(const float *vector, int dims) const
{
    if (!valid || !vector || positive.isEmpty())
        return 0.0f;

    const double liked = dot(positive, vector, dims);
    if (negative.isEmpty() || negativeMass < kMinNegativeMass)
        return float(liked);

    // The negative side never outweighs the positive: someone who skips a lot
    // is not thereby someone with no taste, and a runaway negative centroid
    // would push a shelf into the odd corners of the catalogue that resemble
    // nothing they have rejected.
    const double weight = kNegativeWeightCap * std::min(1.0, positiveMass / negativeMass);
    return float(liked - weight * dot(negative, vector, dims));
}

QSet<quint64> likedSongs(const QVector<PlayEvent> &events)
{
    QSet<quint64> liked;
    // A song is its text key, so "Halo" liked and "HALO" unliked are one song.
    QSet<quint64> decided;
    for (const PlayEvent &event : events) {
        const bool like = event.kind == QLatin1String("like");
        if (!like && event.kind != QLatin1String("unliked"))
            continue;
        const quint64 song = Rec::strictKey(event.title, event.artist);
        if (decided.contains(song))
            continue;                  // an older opinion, already overruled
        decided.insert(song);
        if (like)
            liked.insert(song);
    }
    return liked;
}

TasteProfile buildTaste(const Catalog &catalog,
                        const QVector<PlayEvent> &events,
                        const QDateTime &now)
{
    TasteProfile profile;
    if (!catalog.isLoaded() || events.isEmpty())
        return profile;

    const int dims = catalog.dims();

    // A like that was taken back should stop counting. The iOS player writes
    // nothing when someone un-likes, so an old like keeps voting for ever;
    // this app records a tombstone. Whether a like still stands is the song's
    // newest like or unlike (likedSongs), and the like that counts is that
    // newest one: a song liked again after an unlike votes, with the age of
    // the second like, and a song liked twice votes once.
    const QSet<quint64> liked = likedSongs(events);
    QSet<quint64> likeCounted;

    QVector<double> positive(dims, 0.0);
    QVector<double> negative(dims, 0.0);
    QVector<double> recent(dims, 0.0);
    QVector<double> weekend(dims, 0.0);
    QVector<double> earlier(dims, 0.0);
    QHash<QString, double> artistWeight;
    QSet<int> matchedRows;
    int considered = 0;

    for (const PlayEvent &event : events) {
        if (event.kind == QLatin1String("unliked"))
            continue;
        if (event.kind == QLatin1String("like")) {
            // Newest first, so the first like met of a song liked now is the
            // one that decided it.
            const quint64 song = Rec::strictKey(event.title, event.artist);
            if (!liked.contains(song) || likeCounted.contains(song))
                continue;
            likeCounted.insert(song);
        }

        ++considered;

        double base = 0.0;
        if (!labelWeight(event, &base))
            continue;
        if (base == 0.0)
            continue;                 // heard, but said nothing either way

        // A song has to be findable in the catalogue before its vector can
        // mean anything. This is where most events are lost, and why coverage
        // is worth reporting.
        const Catalog::Match match = catalog.match(event.title, event.artist);
        QVector<float> centroid;
        const float *vector = nullptr;
        if (match.row >= 0) {
            vector = catalog.vector(match.row);
            matchedRows.insert(match.row);
        } else if (match.artistId >= 0) {
            centroid = catalog.artistCentroid(match.artistId);
            if (!centroid.isEmpty())
                vector = centroid.constData();
        }
        if (!vector)
            continue;

        // Confidence is part of the weight, not a filter: a song matched only
        // by its artist still says something, just less.
        const double weight = base * sourceMultiplier(
            event.kind == QLatin1String("play") ? event.source : event.kind) * double(match.confidence);
        if (weight == 0.0)
            continue;

        const double ageDays = event.when.isValid()
            ? std::max(0.0, double(event.when.msecsTo(now)) / 86400000.0)
            : 0.0;
        const double longDecay = std::exp(-ageDays / kLongDecayDays);
        const double shortDecay = std::exp(-ageDays / kShortDecayDays);

        const double longWeight = weight * longDecay;
        if (longWeight > 0.0) {
            addScaled(positive, vector, dims, longWeight);
            profile.positiveMass += longWeight;
            if (!event.artist.isEmpty())
                artistWeight[event.artist] += longWeight;
        } else if (longWeight < 0.0) {
            addScaled(negative, vector, dims, -longWeight);
            profile.negativeMass += -longWeight;
        }

        const double shortWeight = weight * shortDecay;
        if (shortWeight > 0.0) {
            addScaled(recent, vector, dims, shortWeight);
            profile.recentMass += shortWeight;
        }

        // The last two are undecayed: they are about when something was
        // played, so fading them by age would erase the very thing they select.
        if (weight > 0.0 && event.when.isValid() && isWeekend(event.when)) {
            addScaled(weekend, vector, dims, weight);
            profile.weekendMass += weight;
        }
        if (weight > 0.0 && ageDays >= kEarlierMinDays && ageDays <= kEarlierMaxDays) {
            addScaled(earlier, vector, dims, weight);
            profile.earlierMass += weight;
        }
    }

    profile.eventsConsidered = considered;
    profile.tracksMatched = matchedRows.size();

    if (profile.positiveMass < kMinPositiveMass || profile.tracksMatched < kMinMatchedTracks)
        return profile;               // not enough listening to claim anything

    profile.positive = normalised(positive, profile.positiveMass, 0.0);
    profile.negative = normalised(negative, profile.negativeMass, kMinNegativeMass);
    profile.recent   = normalised(recent,   profile.recentMass,   kMinRecentMass);
    profile.weekend  = normalised(weekend,  profile.weekendMass,  kMinWeekendMass);
    profile.earlier  = normalised(earlier,  profile.earlierMass,  kMinEarlierMass);
    if (profile.positive.isEmpty())
        return profile;

    QList<QPair<double, QString>> ranked;
    ranked.reserve(artistWeight.size());
    for (auto it = artistWeight.cbegin(); it != artistWeight.cend(); ++it)
        ranked.append({ it.value(), it.key() });
    // Ties break on the name, so the same events always give the same twelve.
    // Qt's hash order is randomised per process, and a shelf that reshuffles
    // itself between launches looks broken.
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    for (int i = 0; i < ranked.size() && i < 12; ++i)
        profile.topArtists.append(ranked.at(i).second);

    profile.coverage = considered > 0 ? double(profile.tracksMatched) / double(considered) : 0.0;
    profile.valid = true;
    return profile;
}

QVector<PlayEvent> readPlayEvents(int limit)
{
    QVector<PlayEvent> events;
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "SELECT kind, title, artist, source, started_at, listened_ms, track_ms, label,"
        " repeat_in_session FROM play_events ORDER BY id DESC LIMIT ?"));
    query.addBindValue(limit);
    if (!query.exec())
        return events;

    while (query.next()) {
        PlayEvent event;
        event.kind = query.value(0).toString();
        event.title = query.value(1).toString();
        event.artist = query.value(2).toString();
        event.source = query.value(3).toString();
        // SQLite's datetime('now') is UTC without a zone marker, so it has to
        // be told which zone it is in or every event reads as local time and
        // the ages come out hours wrong.
        event.when = QDateTime::fromString(query.value(4).toString(), Qt::ISODate);
        if (event.when.isValid())
            event.when.setTimeSpec(Qt::UTC);
        event.listenedMs = query.value(5).toLongLong();
        event.trackMs = query.value(6).toLongLong();
        const QVariant label = query.value(7);
        event.hasLabel = !label.isNull();
        event.label = label.toDouble();
        event.repeatInSession = query.value(8).toInt() != 0;
        events.append(event);
    }
    return events;
}

} // namespace Rec
