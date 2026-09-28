#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

#include "lyricsprovider.h"

// One song's lyrics, asked of every provider at once and taken in the user's
// order (BitChord §10.2, written from its description, with its defects L1-L4
// left out).
//
// Every provider starts together, so a song with no lyrics anywhere costs the
// slowest provider rather than the sum of them, and a provider that is down
// costs nothing while another has the answer. The answers are still taken in
// order: a fast provider lower down never pre-empts a slower one above it,
// so the order keeps its meaning and speed only helps. What that costs is
// bounded twice over: after the patience window the best answer in hand is
// put forward (`onOffer`) while a better provider is still out, and every
// provider has a deadline of its own, on the wall clock.
//
// The race is decided the moment nothing a provider still out could find
// would change the answer: the first timed answer in order wins at once, a
// provider that only ever has plain text cannot beat plain text from above
// it, and whoever cannot win is cancelled then and there (L1: nobody waits
// for a loser). Timed and plain answers never compete for one slot (L3): a
// plain answer from above waits for a timed one from below, and loses to it.
//
// A provider that failed is not one that has none (L14): no answer with a
// failure among the providers that could have beaten it is "no lyrics", and
// an answer found only because a better provider failed is provisional.
class LyricsRace : public QObject
{
public:
    struct Options {
        // After this long the best answer in hand is put forward while a
        // better provider is still out. Negative: never; the race waits for
        // every provider that could win.
        int patienceMs = 1200;
        // Each provider's own, on the wall clock (Qt's timeouts measure
        // silence; a refused connection took ~4.5 s on Windows).
        int deadlineMs = 6000;
        // One provider at a time, each only once those above it have
        // answered: the chain as it was before the race (lyrics.race=serial).
        bool serial = false;
    };

    enum class State { Waiting, Pending, Found, Missed, Failed, Cancelled };
    enum class Verdict { Found, None, Error };

    LyricsRace(const LyricsRequest &request, const QList<LyricsProvider *> &order, const Options &options,
               QObject *parent = nullptr);
    ~LyricsRace() override;

    // Told once each: a provider answered (found or none, not a failure,
    // and not an answer passed to start()), for the per-provider store; the
    // best answer in hand is put forward before the race is decided; the
    // race is decided. None is called after cancel().
    std::function<void(LyricsProvider *provider, const LyricsOutcome &outcome)> onAnswered;
    std::function<void()> onOffer;
    std::function<void()> onFinished;

    // Starts it. `known` holds the outcomes already kept for some providers,
    // by id: each is taken as that provider's answer, and it is not asked.
    void start(const QHash<QString, LyricsOutcome> &known = {});
    // The providers the race should have, in order: any it does not have yet
    // join it in their place and start (a background lookup the lyrics pane
    // now waits on, which asked only the providers allowed in the
    // background). Nothing once the race is decided.
    void include(const QList<LyricsProvider *> &order);
    // Called off: every request out is aborted and nothing more is told.
    void cancel();

    QString videoId() const { return m_request.videoId; }
    const LyricsRequest &request() const { return m_request; }
    bool finished() const { return m_finished; }
    qint64 elapsed() const { return m_clock.isValid() ? m_clock.elapsed() : 0; }

    // What was put forward last (onOffer), or decided.
    bool hasOffer() const { return m_offerFrom != nullptr; }
    LyricsAnswer offer() const { return m_offer; }

    // Once decided.
    Verdict verdict() const { return m_verdict; }
    LyricsAnswer answer() const { return m_answer; }   // Found; `provisional` set
    QString error() const { return m_error; }          // Error; or why a Found is provisional
    LyricsProvider *winner() const { return m_winner; }

    // For the self-test.
    State state(LyricsProvider *provider) const;
    bool known(LyricsProvider *provider) const;

private:
    struct Entry {
        LyricsProvider *provider = nullptr;
        State state = State::Waiting;
        LyricsOutcome outcome;
        bool known = false;              // from the store, not asked
        QPointer<LyricsLookup> lookup;   // while Pending
        QTimer *deadline = nullptr;      // while Pending
        qint64 startedMs = -1;
        qint64 endedMs = -1;
    };

    int indexOf(LyricsProvider *provider) const;
    int bestIndex() const;
    int reach(int index) const;          // the best rank it could still answer with
    bool couldBeat(int index, int best) const;
    bool wanted(int index, int best) const;
    bool mayStart(int index, int best) const;
    void startEntry(int index);
    void drop(int index);
    void settle(LyricsProvider *provider, LyricsOutcome outcome);
    LyricsOutcome gate(LyricsOutcome outcome) const;
    void evaluate();
    void put(int best);
    void decide(int best);
    QString describe(const Entry &entry) const;

    LyricsRequest m_request;
    Options m_options;
    QList<Entry> m_entries;
    QElapsedTimer m_clock;
    QTimer m_patience;
    bool m_patienceOver = false;
    bool m_started = false;
    bool m_finished = false;
    bool m_evaluating = false;
    bool m_again = false;

    LyricsProvider *m_offerFrom = nullptr;
    LyricsAnswer m_offer;
    Verdict m_verdict = Verdict::None;
    LyricsAnswer m_answer;
    QString m_error;
    LyricsProvider *m_winner = nullptr;
};
