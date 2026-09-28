#include "lyricsrace.h"

#include "lyrics.h"

#include <QStringList>

namespace {

// The shared gate on an entry's length against this recording's, in seconds:
// timing is trusted within 3 s, the words within 10 s (as LRCLIB's own search
// has taken them since LY-2). Past the first a timed answer keeps its words
// as another recording's; past the second it is not this song.
constexpr double kTimedGapS = 3.0;
constexpr double kWordsGapS = 10.0;

QString kindOf(const LyricsAnswer &answer)
{
    if (answer.instrumental)
        return QStringLiteral("instrumental");
    if (!answer.synced.trimmed().isEmpty())
        return QStringLiteral("synced");
    if (answer.loose)
        return QStringLiteral("another edit's words");
    return QStringLiteral("plain");
}

} // namespace

LyricsRace::LyricsRace(const LyricsRequest &request, const QList<LyricsProvider *> &order, const Options &options,
                       QObject *parent)
    : QObject(parent)
    , m_request(request)
    , m_options(options)
{
    for (LyricsProvider *provider : order) {
        if (provider && indexOf(provider) < 0) {
            Entry entry;
            entry.provider = provider;
            m_entries.append(entry);
        }
    }
    m_patience.setSingleShot(true);
    // What the reader waits for, so to the millisecond: a coarse timer may
    // fire 5% late, 60 ms of a 1.2 s window (measured: 16-75 ms).
    m_patience.setTimerType(Qt::PreciseTimer);
    connect(&m_patience, &QTimer::timeout, this, [this]() {
        m_patienceOver = true;
        evaluate();
    });
}

LyricsRace::~LyricsRace()
{
    // Deleted without being called off (its owner going): nothing may call
    // back into an owner that is on its way out.
    onAnswered = nullptr;
    onOffer = nullptr;
    onFinished = nullptr;
    for (int i = 0; i < m_entries.size(); ++i)
        drop(i);
}

int LyricsRace::indexOf(LyricsProvider *provider) const
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).provider == provider)
            return i;
    }
    return -1;
}

LyricsRace::State LyricsRace::state(LyricsProvider *provider) const
{
    const int i = indexOf(provider);
    return i < 0 ? State::Waiting : m_entries.at(i).state;
}

bool LyricsRace::known(LyricsProvider *provider) const
{
    const int i = indexOf(provider);
    return i >= 0 && m_entries.at(i).known;
}

void LyricsRace::start(const QHash<QString, LyricsOutcome> &known)
{
    if (m_started)
        return;
    m_started = true;
    m_clock.start();
    for (Entry &entry : m_entries) {
        const auto it = known.constFind(entry.provider->id());
        if (it == known.constEnd())
            continue;
        entry.known = true;
        entry.outcome = gate(*it);
        entry.state = entry.outcome.kind == LyricsOutcome::Found  ? State::Found
                      : entry.outcome.kind == LyricsOutcome::Missed ? State::Missed
                                                                    : State::Failed;
        entry.startedMs = entry.endedMs = 0;
    }
    if (m_options.patienceMs == 0)
        m_patienceOver = true;
    else if (m_options.patienceMs > 0)
        m_patience.start(m_options.patienceMs);
    evaluate();
}

void LyricsRace::include(const QList<LyricsProvider *> &order)
{
    if (m_finished)
        return;
    QList<Entry> entries;
    bool added = false;
    for (LyricsProvider *provider : order) {
        const int i = indexOf(provider);
        if (i >= 0) {
            entries.append(m_entries.at(i));
            continue;
        }
        if (!provider)
            continue;
        Entry entry;
        entry.provider = provider;
        entries.append(entry);
        added = true;
    }
    if (!added)
        return;
    // Whatever the new order leaves out keeps running, after it.
    for (const Entry &entry : std::as_const(m_entries)) {
        if (!order.contains(entry.provider))
            entries.append(entry);
    }
    m_entries = entries;
    if (m_started)
        evaluate();
}

void LyricsRace::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    m_patience.stop();
    QStringList still;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).state == State::Pending)
            still << m_entries.at(i).provider->name();
        drop(i);
    }
    if (!still.isEmpty())
        qInfo("lyrics: race for %s called off at %lld ms, %s still out", qPrintable(m_request.videoId),
              (long long)elapsed(), qPrintable(still.join(QStringLiteral(", "))));
}

// ------------------------------------------------------------- the rules

// The best answer in hand: the highest rank, and at the same rank the one
// highest in the order.
int LyricsRace::bestIndex() const
{
    int best = -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        if (entry.state != State::Found)
            continue;
        if (best < 0 || entry.outcome.answer.rank() > m_entries.at(best).outcome.answer.rank())
            best = i;
    }
    return best;
}

int LyricsRace::reach(int index) const
{
    return m_entries.at(index).provider->bestTiming() == LyricsProvider::Timing::Plain ? LyricsAnswer::Plain
                                                                                      : LyricsAnswer::Timed;
}

// Whether provider `index`, not answered yet, could still take the answer
// from `best`: with a better kind of answer than it could ever give, or the
// same kind from higher in the order.
bool LyricsRace::couldBeat(int index, int best) const
{
    if (best < 0)
        return true;
    const int theirs = m_entries.at(best).outcome.answer.rank();
    return reach(index) > theirs || (reach(index) == theirs && index < best);
}

// Still worth waiting for. A lazy provider only while nothing at all is in
// hand: it is there for when every other provider has come up empty.
bool LyricsRace::wanted(int index, int best) const
{
    if (m_entries.at(index).provider->lazy())
        return best < 0;
    return couldBeat(index, best);
}

// A provider not started yet starts once every provider above it has
// answered, when it is lazy or the race runs one at a time; otherwise at once.
bool LyricsRace::mayStart(int index, int) const
{
    if (!m_entries.at(index).provider->lazy() && !m_options.serial)
        return true;
    for (int i = 0; i < index; ++i) {
        const State state = m_entries.at(i).state;
        if (state == State::Waiting || state == State::Pending)
            return false;
    }
    return true;
}

// What the race takes from a provider, whoever it is: an entry whose length
// is too far from this recording's keeps its words without its timing, or,
// further still, is not this song at all. A provider that reports no length
// has matched the song itself.
LyricsOutcome LyricsRace::gate(LyricsOutcome outcome) const
{
    if (outcome.kind != LyricsOutcome::Found)
        return outcome;
    LyricsAnswer &answer = outcome.answer;
    const double wanted = m_request.query.durationS;
    if (wanted <= 0 || answer.durationS <= 0)
        return outcome;
    const double gap = qAbs(answer.durationS - wanted);
    if (gap > kWordsGapS) {
        qInfo("lyrics: %s: an answer %.0f s off this recording's length is not this song",
              qPrintable(m_request.videoId), gap);
        outcome.kind = LyricsOutcome::Missed;
        outcome.answer = LyricsAnswer();
        return outcome;
    }
    if (gap > kTimedGapS && (answer.instrumental || !answer.synced.trimmed().isEmpty())) {
        if (answer.plain.trimmed().isEmpty()) {
            QStringList lines;
            for (const LyricsModel::Line &line : Lyrics::parseLrc(answer.synced))
                lines << line.text;
            answer.plain = lines.join(QLatin1Char('\n'));
        }
        answer.synced.clear();
        answer.instrumental = false;
        answer.loose = true;
        if (answer.plain.trimmed().isEmpty()) {
            outcome.kind = LyricsOutcome::Missed;
            outcome.answer = LyricsAnswer();
        }
    }
    return outcome;
}

// ------------------------------------------------------------ the running

void LyricsRace::startEntry(int index)
{
    Entry &entry = m_entries[index];
    LyricsProvider *provider = entry.provider;
    entry.state = State::Pending;
    entry.startedMs = elapsed();
    LyricsLookup *lookup = provider->lookUp(m_request, this);
    entry.lookup = lookup;
    lookup->onFinished = [this, provider](const LyricsOutcome &outcome) { settle(provider, outcome); };
    if (m_options.deadlineMs > 0) {
        auto *deadline = new QTimer(this);
        deadline->setSingleShot(true);
        entry.deadline = deadline;
        connect(deadline, &QTimer::timeout, this, [this, provider]() {
            const int i = indexOf(provider);
            if (i < 0 || m_entries.at(i).state != State::Pending)
                return;
            if (LyricsLookup *late = m_entries.at(i).lookup)
                late->cancel();
            LyricsOutcome failed;
            failed.kind = LyricsOutcome::Failed;
            failed.error = QStringLiteral("%1 did not answer within %2 s")
                               .arg(provider->name())
                               .arg(m_options.deadlineMs / 1000.0, 0, 'g', 3);
            settle(provider, failed);
        });
    }
    lookup->start();
    // After the provider's own start, so a deadline of its own at the same
    // time (LRCLIB's) fires first and says why in its own words. Not at all
    // when it answered the moment it started.
    const int i = indexOf(provider);
    if (i >= 0 && m_entries.at(i).state == State::Pending && m_entries.at(i).deadline)
        m_entries.at(i).deadline->start(m_options.deadlineMs);
}

// Stops a provider still out, or never started: a loser, or the race over.
void LyricsRace::drop(int index)
{
    Entry &entry = m_entries[index];
    if (entry.state != State::Waiting && entry.state != State::Pending)
        return;
    if (entry.deadline) {
        entry.deadline->stop();
        entry.deadline->deleteLater();
        entry.deadline = nullptr;
    }
    if (LyricsLookup *lookup = entry.lookup) {
        entry.lookup = nullptr;
        lookup->cancel();
        lookup->deleteLater();
    }
    if (entry.state == State::Pending)
        entry.endedMs = elapsed();
    entry.state = State::Cancelled;
}

void LyricsRace::settle(LyricsProvider *provider, LyricsOutcome outcome)
{
    const int index = indexOf(provider);
    // Late: cancelled as a loser, timed out, or the race is over.
    if (index < 0 || m_finished || m_entries.at(index).state != State::Pending)
        return;
    Entry &entry = m_entries[index];
    if (entry.deadline) {
        entry.deadline->stop();
        entry.deadline->deleteLater();
        entry.deadline = nullptr;
    }
    if (LyricsLookup *lookup = entry.lookup) {
        entry.lookup = nullptr;
        lookup->deleteLater();
    }
    entry.endedMs = elapsed();
    entry.outcome = gate(outcome);
    entry.state = entry.outcome.kind == LyricsOutcome::Found  ? State::Found
                  : entry.outcome.kind == LyricsOutcome::Missed ? State::Missed
                                                                : State::Failed;
    if (onAnswered && entry.outcome.kind != LyricsOutcome::Failed && !entry.outcome.partial)
        onAnswered(provider, entry.outcome);
    evaluate();
}

// Where the race stands, after any change: losers cancelled, providers whose
// turn has come started, and either the answer decided or, after the
// patience window, the best in hand put forward.
void LyricsRace::evaluate()
{
    if (m_finished || !m_started)
        return;
    // A provider can answer the moment it is started (from nothing but a
    // failed connection, say): that is looked at once this pass is over.
    if (m_evaluating) {
        m_again = true;
        return;
    }
    m_evaluating = true;
    do {
        m_again = false;
        const int best = bestIndex();
        bool blocked = false;
        for (int i = 0; i < m_entries.size() && !m_finished; ++i) {
            const State state = m_entries.at(i).state;
            if (state != State::Waiting && state != State::Pending)
                continue;
            if (!wanted(i, best)) {
                drop(i);
                continue;
            }
            blocked = true;
            if (state == State::Waiting && mayStart(i, best))
                startEntry(i);
        }
        if (m_again || m_finished)
            continue;
        if (!blocked)
            decide(best);
        else if (best >= 0 && m_patienceOver)
            put(best);
    } while (m_again && !m_finished);
    m_evaluating = false;
}

// The best in hand, put forward while a better provider is still out.
void LyricsRace::put(int best)
{
    const Entry &entry = m_entries.at(best);
    if (m_offerFrom == entry.provider)
        return;
    m_offerFrom = entry.provider;
    m_offer = entry.outcome.answer;
    QStringList still;
    for (const Entry &other : std::as_const(m_entries)) {
        if (other.state == State::Pending || other.state == State::Waiting)
            still << other.provider->name();
    }
    qInfo("lyrics: race for %s: %s's %s put forward at %lld ms, %s still out", qPrintable(m_request.videoId),
          qPrintable(entry.provider->name()), qPrintable(kindOf(m_offer)), (long long)elapsed(),
          qPrintable(still.join(QStringLiteral(", "))));
    if (onOffer)
        onOffer();
}

void LyricsRace::decide(int best)
{
    m_finished = true;
    m_patience.stop();
    for (int i = 0; i < m_entries.size(); ++i)
        drop(i);

    // Provisional when a provider that could have beaten the answer failed:
    // it is only the answer because that one could not be asked.
    QString failures;
    const int rank = best >= 0 ? m_entries.at(best).outcome.answer.rank() : LyricsAnswer::Nothing;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries.at(i);
        const bool failed = entry.state == State::Failed
                            || (entry.state == State::Found && entry.outcome.partial);
        if (!failed)
            continue;
        const bool couldHave = best < 0 || reach(i) > rank || (reach(i) == rank && i < best);
        if (couldHave && failures.isEmpty())
            failures = entry.outcome.error;
    }
    if (best >= 0) {
        m_verdict = Verdict::Found;
        m_winner = m_entries.at(best).provider;
        m_answer = m_entries.at(best).outcome.answer;
        m_answer.provisional = !failures.isEmpty();
        m_error = failures;
    } else {
        m_verdict = failures.isEmpty() ? Verdict::None : Verdict::Error;
        m_error = failures;
    }

    QStringList parts;
    for (const Entry &entry : std::as_const(m_entries))
        parts << describe(entry);
    const QString verdict = best >= 0
        ? QStringLiteral("%1's %2%3").arg(m_winner->name(), kindOf(m_answer),
                                          m_answer.provisional ? QStringLiteral(", provisional") : QString())
        : m_verdict == Verdict::None ? QStringLiteral("no lyrics") : QStringLiteral("error");
    // The winner's own time beside the race's: taken the moment it answered,
    // not once the others did (L1).
    qInfo("lyrics: race for %s decided in %lld ms: %s; %s", qPrintable(m_request.videoId), (long long)elapsed(),
          qPrintable(parts.join(QStringLiteral(", "))), qPrintable(verdict));
    if (onFinished)
        onFinished();
}

QString LyricsRace::describe(const Entry &entry) const
{
    const QString name = entry.provider->name();
    const QString when = entry.known ? QStringLiteral("kept") : QStringLiteral("%1 ms").arg(entry.endedMs);
    switch (entry.state) {
    case State::Found:
        return QStringLiteral("%1 %2 %3%4").arg(name, kindOf(entry.outcome.answer), when,
                                                entry.outcome.partial ? QStringLiteral(" (part failed)") : QString());
    case State::Missed:
        return QStringLiteral("%1 none %2").arg(name, when);
    case State::Failed:
        return QStringLiteral("%1 failed %2 (%3)").arg(name, when, entry.outcome.error);
    case State::Cancelled:
        return entry.startedMs < 0 ? QStringLiteral("%1 not asked").arg(name)
                                   : QStringLiteral("%1 cancelled at %2 ms").arg(name).arg(entry.endedMs);
    case State::Waiting:
    case State::Pending:
        break;
    }
    return name + QStringLiteral(" out");
}
