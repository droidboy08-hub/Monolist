#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Rec {

class Catalog;

// One thing the listener did. Read from the play_events table, but kept
// independent of it so the profile can be built from an imported history or
// from a fixed set in a test.
struct PlayEvent {
    QString kind;            // play | like | notInterested | unliked
    QString title;
    QString artist;
    QString source;          // the surface it came from; empty counts as neutral
    QDateTime when;
    qint64 listenedMs = 0;
    qint64 trackMs = 0;
    bool hasLabel = false;   // separate from the value: no label is not zero
    double label = 0.0;
    bool repeatInSession = false;
};

// What the listener likes, as five directions in the catalogue's own vector
// space. Not a genre list and not a set of rules: each centroid is the
// weighted mean of the vectors of songs they played, so "more like this" is a
// dot product rather than a taxonomy.
//
// Ported from RECOMMENDATION_PORTING.md 5.6. The weights, half-lives and gates
// are that document's, unchanged, because a profile built here has to mean the
// same thing as one built on the phone — otherwise a listening history carried
// across gives a different answer on each.
struct TasteProfile {
    // False until enough has been listened to to say anything. Everything
    // below is meaningless when this is false, and the caller shows the
    // ordinary catalogue instead of a personal one.
    bool valid = false;

    // Each is empty or L2-normalised, in the catalogue's dimensions.
    QVector<float> positive;   // the long view: what they return to
    QVector<float> negative;   // what they skip and dismiss
    QVector<float> recent;     // the last few days, which move much faster
    QVector<float> weekend;    // Saturdays and Sundays sound different
    QVector<float> earlier;    // one to three months ago: "back to"

    double positiveMass = 0.0;
    double negativeMass = 0.0;
    double recentMass = 0.0;
    double weekendMass = 0.0;
    double earlierMass = 0.0;

    // Distinct matched tracks over events considered. Not a percentage of
    // anything intuitive — heavy repeat play lowers it — but it is what the
    // re-ranker scales its confidence by.
    double coverage = 0.0;

    // The twelve artists with the most positive weight behind them, by their
    // raw tag string. Skips never subtract from an artist here.
    QStringList topArtists;

    int eventsConsidered = 0;
    int tracksMatched = 0;

    // How much this listener would like a catalogue vector. Positive is good.
    float score(const float *vector, int dims) const;
};

// Builds the profile. `now` is passed rather than read so the same events
// always give the same answer, which is what makes this testable at all.
TasteProfile buildTaste(const Catalog &catalog,
                        const QVector<PlayEvent> &events,
                        const QDateTime &now);

// The newest events from the app's own database, newest first. `limit` is the
// spec's 4,000 — enough to describe a listener, few enough to rebuild in
// milliseconds.
QVector<PlayEvent> readPlayEvents(int limit = 4000);

} // namespace Rec
