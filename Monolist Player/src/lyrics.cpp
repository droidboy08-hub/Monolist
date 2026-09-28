#include "lyrics.h"
#include "artistlinks.h"
#include "playbackcontroller.h"
#include "lyrics/lyricsstore.h"
#include "lyrics/providers/lrclibprovider.h"
#include "lyrics/providers/ytmlyricsprovider.h"

#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QRegularExpression>
#include <QVariant>

#include <algorithm>
#include <utility>

namespace {

// A line lights this far ahead of the audio, so it shows as it is sung rather
// than just after.
constexpr qint64 kLeadMs = 150;
// A provisional answer that stays on show this long (one song on repeat) is
// asked for again without waiting for the next view.
constexpr int kRecheckMs = 60 * 60 * 1000;
// A song the queue's lookups could not settle for good (a provider down, so
// an error or a provisional answer) is asked about again by the queue only
// after this long; the view asks whenever it is opened.
constexpr qint64 kQueueRetryMs = 10 * 60 * 1000;

bool sameWords(const LyricsAnswer &a, const LyricsAnswer &b)
{
    return a.synced == b.synced && a.plain == b.plain && a.instrumental == b.instrumental;
}

} // namespace

// --------------------------------------------------------------- LyricsModel

LyricsModel::LyricsModel(QObject *parent)
    : QAbstractListModel(parent) {}

int LyricsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_lines.size());
}

QVariant LyricsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_lines.size())
        return {};
    const Line &line = m_lines.at(index.row());
    switch (role) {
    case TextRole: return line.text;
    case TimeRole: return line.timeMs;
    default:       return {};
    }
}

QHash<int, QByteArray> LyricsModel::roleNames() const
{
    return { { TextRole, "text" }, { TimeRole, "timeMs" } };
}

void LyricsModel::replace(const QList<Line> &lines)
{
    beginResetModel();
    m_lines = lines;
    endResetModel();
    Q_EMIT countChanged();
}

// -------------------------------------------------------------------- Lyrics

Lyrics::Lyrics(PlaybackController *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_network(new QNetworkAccessManager(this))
    , m_lrclib(std::make_unique<LrclibProvider>(m_network))
    , m_ytm(std::make_unique<YtmLyricsProvider>(&m_innerTube))
{
    // The user's order, until there is a way to change it (LY-10): LRCLIB's
    // timed lines first, YouTube Music's plain text after.
    m_order = { m_lrclib.get(), m_ytm.get() };
    m_sessionClock.start();

    connect(m_player, &PlaybackController::currentTrackChanged, this, &Lyrics::trackChanged);
    connect(m_player, &PlaybackController::positionChanged, this, &Lyrics::updateCurrentLine);

    m_recheck.setSingleShot(true);
    m_recheck.setInterval(kRecheckMs);
    connect(&m_recheck, &QTimer::timeout, this, [this]() {
        if (m_active && m_hasStored && m_stored.provisional && !m_checking)
            checkAgain();
    });
}

Lyrics::~Lyrics()
{
    // Before the providers, and what they ask with, go.
    const QList<Job> jobs = m_jobs.values();
    m_jobs.clear();
    m_race = nullptr;
    for (const Job &job : jobs) {
        job.race->cancel();
        delete job.race;
    }
}

void Lyrics::setLrclibUrl(const QString &url)
{
    m_lrclib->setUrl(url);
}

void Lyrics::setLrclibExact(bool on)
{
    m_lrclib->setExact(on);
}

void Lyrics::setProviders(const QList<LyricsProvider *> &providers)
{
    m_order = providers;
}

QList<LyricsProvider *> Lyrics::providers(bool background) const
{
    if (!background)
        return m_order;
    QList<LyricsProvider *> allowed;
    for (LyricsProvider *provider : m_order) {
        if (provider->background())
            allowed << provider;
    }
    return allowed;
}

LyricsQuery::Query Lyrics::queryFor(const QVariantMap &track) const
{
    // The artist line as names: the song's own credits, else the names
    // ArtistLinks has seen linked, else the line whole.
    const QVariant credits = track.value(QStringLiteral("credits"));
    const QVariantList pieces = m_artistLinks
        ? m_artistLinks->credits(track.value(QStringLiteral("artist")).toString(), credits)
        : credits.toList();
    return LyricsQuery::fromTrack(track, pieces);
}

void Lyrics::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
    if (!m_active)
        return;
    if (m_state == QLatin1String("idle"))
        load(m_player->currentTrack(), false);
    else if (m_hasStored && m_stored.provisional && !m_checking && !m_race)
        checkAgain();   // looked at again: a stand-in is asked for again
}

void Lyrics::setBackground(bool on)
{
    if (on == m_background)
        return;
    m_background = on;
    Q_EMIT backgroundChanged();
    if (!m_background) {
        // The queue's lookups stop; one the view is waiting on goes on.
        m_queueSongs.clear();
        prune();
        return;
    }
    prefetchQueue();
}

void Lyrics::trackChanged()
{
    const QVariantMap track = m_player->currentTrack();
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (!videoId.isEmpty() && videoId == m_videoId)
        return;   // the same song; the queue moved around it
    if (m_active) {
        load(track, false);
        return;
    }
    // Nobody is looking: the last song's lines go. Its lookup carries on
    // only while the queue still wants it.
    detach();
    m_track.clear();
    m_videoId.clear();
    m_source.clear();
    m_error.clear();
    m_hasStored = false;
    m_checking = false;
    m_lines.replace({});
    setState(QStringLiteral("idle"));
    updateCurrentLine();
    prune();
}

void Lyrics::lookup(const QVariantMap &track)
{
    load(track, false);
}

void Lyrics::retry()
{
    load(m_track.isEmpty() ? m_player->currentTrack() : m_track, true);
}

void Lyrics::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

// The view lets go of the race it was waiting on; prune() then decides
// whether that goes on for the queue.
void Lyrics::detach()
{
    m_race = nullptr;
    m_interim = false;
    m_recheck.stop();
}

void Lyrics::load(const QVariantMap &track, bool askAgain)
{
    detach();
    m_track = track;
    m_videoId = track.value(QStringLiteral("sourceId")).toString();
    m_source.clear();
    m_error.clear();
    m_hasStored = false;
    m_checking = false;
    m_lines.replace({});
    updateCurrentLine();

    if (m_videoId.isEmpty() || track.value(QStringLiteral("title")).toString().isEmpty()) {
        setState(QStringLiteral("none"));
        prune();
        return;
    }
    // The length mpv found, for a song listed without one: most providers
    // match on it.
    if (m_track.value(QStringLiteral("durationMs")).toLongLong() <= 0
        && m_videoId == m_player->currentSourceId() && m_player->duration() > 0)
        m_track.insert(QStringLiteral("durationMs"), m_player->duration());
    const LyricsQuery::Query query = queryFor(m_track);
    if (askAgain)
        dropJob(m_videoId);   // asked afresh, past whatever is out already

    // A lookup already out for this song (the queue's): the view waits on it
    // rather than asking a second time.
    if (m_jobs.contains(m_videoId)) {
        const Job job = m_jobs.value(m_videoId);
        m_race = job.race;
        if (job.hasKept) {
            m_checking = true;
            show(job.kept);
        } else {
            setState(QStringLiteral("loading"));
            if (job.race->hasOffer())
                showOffer(job.race);
        }
        // Any provider the queue may not ask joins it now.
        job.race->include(providers(false));
        prune();
        return;
    }

    LyricsAnswer kept;
    const LyricsStore::Stored stored = LyricsStore::read(m_videoId, &kept);
    if (!askAgain && stored == LyricsStore::Stored::Show) {
        show(kept);
        prune();
        return;
    }
    if (!askAgain && stored == LyricsStore::Stored::ShowAndCheck) {
        // Shown at once, and asked for again behind it.
        m_checking = true;
        show(kept);
        startRace(m_videoId, query, kept, true, false, false);
        prune();
        return;
    }
    // Asked again by hand: what is kept stays the answer if nothing better
    // is found.
    bool hasKept = false;
    if (stored != LyricsStore::Stored::Nothing && kept.found()) {
        m_stored = kept;
        m_hasStored = true;
        hasKept = true;
    }
    setState(QStringLiteral("loading"));
    startRace(m_videoId, query, kept, hasKept, askAgain, false);
    prune();
}

// The answer on show, asked for again behind it.
void Lyrics::checkAgain()
{
    if (m_race || m_videoId.isEmpty())
        return;
    m_recheck.stop();
    m_checking = true;
    if (m_jobs.contains(m_videoId)) {
        m_race = m_jobs.value(m_videoId).race;
        m_race->include(providers(false));
        return;
    }
    startRace(m_videoId, queryFor(m_track), m_stored, true, false, false);
}

// ------------------------------------------------------------ the lookups

void Lyrics::startRace(const QString &videoId, const LyricsQuery::Query &query, const LyricsAnswer &kept,
                       bool hasKept, bool askAgain, bool background)
{
    auto *race = new LyricsRace({ videoId, query }, providers(background), m_options, this);
    Job job;
    job.race = race;
    job.kept = kept;
    job.hasKept = hasKept;
    job.background = background;
    m_jobs.insert(videoId, job);
    // Each provider's own answer is kept as it comes, whoever wins.
    race->onAnswered = [videoId](LyricsProvider *provider, const LyricsOutcome &outcome) {
        LyricsStore::writeResult(videoId, provider->id(), outcome);
    };
    race->onOffer = [this, race]() { showOffer(race); };
    race->onFinished = [this, race]() { finishRace(race); };
    // The view's before it starts: it may be decided at once, from what the
    // providers answered before.
    if (!background)
        m_race = race;
    // Asked again by hand, every provider is asked afresh; and under
    // lyrics.race=serial, as the chain was before the race, every time.
    race->start(askAgain || m_options.serial ? QHash<QString, LyricsOutcome>() : LyricsStore::results(videoId));
}

void Lyrics::finishRace(LyricsRace *race)
{
    const QString videoId = race->videoId();
    const auto it = m_jobs.find(videoId);
    if (it == m_jobs.end() || it->race != race) {
        race->deleteLater();
        return;
    }
    const Job job = *it;
    m_jobs.erase(it);
    const Settled settled = commit(videoId, *race, job);

    // Not settled for good: the queue leaves the song alone for a while.
    const bool final = race->verdict() == LyricsRace::Verdict::None
                       || (race->verdict() == LyricsRace::Verdict::Found && !race->answer().provisional);
    if (final)
        m_queueTried.remove(videoId);
    else
        m_queueTried.insert(videoId, m_sessionClock.elapsed());

    const bool onShow = race == m_race;
    race->deleteLater();
    if (onShow) {
        m_race = nullptr;
        display(settled);
    }
    // The song after the one playing waits for the one playing.
    if (m_following)
        QTimer::singleShot(0, this, &Lyrics::prefetchQueue);
}

// Keeps what a finished lookup found, by the rules above, and says what is
// left on show. The same for the view's lookups and the queue's.
Lyrics::Settled Lyrics::commit(const QString &videoId, const LyricsRace &race, const Job &job)
{
    Settled settled;
    switch (race.verdict()) {
    case LyricsRace::Verdict::Found: {
        const LyricsAnswer answer = race.answer();
        // What is kept for good is not given up for an answer that is only
        // standing in.
        if (job.hasKept && answer.provisional && !job.kept.provisional) {
            qInfo("lyrics: %s: kept lyrics stay; a stand-in was found (%s)", qPrintable(videoId),
                  qPrintable(race.error()));
            settled.answer = job.kept;
            settled.keptStays = true;
            return settled;
        }
        LyricsStore::write(videoId, answer);
        if (answer.provisional)
            qInfo("lyrics: %s kept as provisional, to be asked again (%s)", qPrintable(videoId),
                  qPrintable(race.error()));
        settled.answer = answer;
        return settled;
    }
    case LyricsRace::Verdict::None:
        if (job.hasKept) {
            // Lyrics were found before and nobody has them now: they stay,
            // as they are, until the refresh age comes round again.
            LyricsStore::touch(videoId);
            settled.answer = job.kept;
            settled.answer.provisional = false;
            settled.keptStays = true;
            return settled;
        }
        LyricsStore::write(videoId, LyricsAnswer());
        return settled;   // an empty answer: "none"
    case LyricsRace::Verdict::Error:
        break;
    }
    if (job.hasKept) {
        qInfo("lyrics: %s: kept lyrics stay; asking again failed (%s)", qPrintable(videoId),
              qPrintable(race.error()));
        settled.answer = job.kept;
        settled.keptStays = true;
        return settled;
    }
    // Nothing is stored, and the view offers to try again.
    qInfo("lyrics: %s: nothing stored, a provider could not be asked (%s)", qPrintable(videoId),
          qPrintable(race.error()));
    settled.kind = Settled::Error;
    settled.error = race.error();
    return settled;
}

void Lyrics::dropJob(const QString &videoId)
{
    const auto it = m_jobs.find(videoId);
    if (it == m_jobs.end())
        return;
    LyricsRace *race = it->race;
    m_jobs.erase(it);
    if (race == m_race)
        m_race = nullptr;
    race->cancel();
    race->deleteLater();
}

// Calls off every lookup nobody wants any more: not the one the view waits
// on, nor the queue's for the songs playing and next.
void Lyrics::prune()
{
    QList<LyricsRace *> gone;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->race == m_race || m_queueSongs.contains(it.key())) {
            ++it;
            continue;
        }
        gone << it->race;
        it = m_jobs.erase(it);
    }
    for (LyricsRace *race : std::as_const(gone)) {
        race->cancel();
        race->deleteLater();
    }
}

// ------------------------------------------------------- the queue's lookups

void Lyrics::followQueue()
{
    if (m_following)
        return;
    m_following = true;
    // Once the song's sound has started, not when it became the current
    // track: a song only loaded (the one a launch opens on) may never be
    // played, and the first second of a new song is the stream's.
    connect(m_player, &PlaybackController::listenStarted, this, [this](const QVariantMap &track) {
        m_heard = track.value(QStringLiteral("sourceId")).toString();
        m_heardTrack = track;
        prefetchQueue();
    });
    connect(m_player, &PlaybackController::currentTrackChanged, this, &Lyrics::prefetchQueue);
    connect(m_player, &PlaybackController::durationChanged, this, &Lyrics::prefetchQueue);
    connect(m_player, &PlaybackController::repeatModeChanged, this, &Lyrics::prefetchQueue);
    connect(m_player->queue(), &QueueModel::upcomingChanged, this, &Lyrics::prefetchQueue);
    prefetchQueue();
}

void Lyrics::prefetchQueue()
{
    if (!m_following || !m_background)
        return;
    const QString current = m_player->currentSourceId();
    if (current.isEmpty() || (!m_followWithoutSound && current != m_heard))
        return;

    QVariantMap playing = m_player->currentTrack();
    // The length the engine found, for a song listed without one.
    if (playing.value(QStringLiteral("durationMs")).toLongLong() <= 0) {
        qint64 known = m_player->duration();
        if (known <= 0 && current == m_heard)
            known = m_heardTrack.value(QStringLiteral("durationMs")).toLongLong();
        if (known > 0)
            playing.insert(QStringLiteral("durationMs"), known);
    }
    QueueModel *queue = m_player->queue();
    // A new queue is in place before its first song is: until then the song
    // playing is not the queue's, and neither is "next".
    const QueueTrack *here = queue->current();
    if (!here || here->videoId != current)
        return;
    const QueueTrack *next = queue->at(queue->currentIndex() + 1);
    if (!next && m_player->repeatMode() == PlaybackController::RepeatAll)
        next = queue->at(0);
    const QString nextId = next ? next->videoId : QString();

    QSet<QString> songs{ current };
    if (!nextId.isEmpty())
        songs.insert(nextId);
    m_queueSongs = songs;
    prune();

    prefetch(playing);
    // The song after once the one playing is settled, and not on a metered
    // connection: it may be skipped, and its lookup would be spent.
    if (!nextId.isEmpty() && nextId != current && !m_jobs.contains(current) && !metered())
        prefetch(next->toMap());
}

// Starts a queue lookup for `track` when it needs one; false when not.
bool Lyrics::prefetch(const QVariantMap &track)
{
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (videoId.isEmpty() || track.value(QStringLiteral("title")).toString().isEmpty())
        return false;
    if (m_jobs.contains(videoId))
        return false;   // one lookup per song, whoever asked for it
    // Most providers match on the length, and a lookup without it may keep
    // a worse match for good (L8): the song playing gets it from the engine.
    if (track.value(QStringLiteral("durationMs")).toLongLong() <= 0)
        return false;
    const auto tried = m_queueTried.constFind(videoId);
    if (tried != m_queueTried.constEnd() && m_sessionClock.elapsed() - *tried < kQueueRetryMs)
        return false;
    LyricsAnswer kept;
    const LyricsStore::Stored stored = LyricsStore::read(videoId, &kept);
    if (stored == LyricsStore::Stored::Show)
        return false;
    const bool hasKept = stored == LyricsStore::Stored::ShowAndCheck && kept.found();
    startRace(videoId, queryFor(track), kept, hasKept, false, true);
    return true;
}

// Where Qt cannot tell (no backend, or none that knows), the connection is
// taken as unmetered.
bool Lyrics::metered() const
{
    if (m_meteredForTest >= 0)
        return m_meteredForTest > 0;
    QNetworkInformation *info = QNetworkInformation::instance();
    if (!info) {
        QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered);
        info = QNetworkInformation::instance();
    }
    return info && info->supports(QNetworkInformation::Feature::Metered) && info->isMetered();
}

// ---------------------------------------------------------- what is on show

// The best answer in hand of the race the view waits on, while a better
// provider is still out. A kept answer being checked stays instead.
void Lyrics::showOffer(LyricsRace *race)
{
    if (race != m_race || m_checking)
        return;
    const LyricsAnswer offer = race->offer();
    if (m_hasStored && m_state != QLatin1String("loading")) {
        if (sameWords(offer, m_stored))
            return;
        // An earlier one on show already (a third provider answered): only
        // a better kind replaces it while someone reads along.
        if (m_interim && offer.rank() <= m_stored.rank() && m_active)
            return;
    }
    m_interim = true;
    show(offer);
}

void Lyrics::display(const Settled &settled)
{
    const bool wasInterim = std::exchange(m_interim, false);
    if (settled.kind == Settled::Error) {
        m_checking = false;
        m_hasStored = false;
        m_source.clear();
        m_error = settled.error;
        m_lines.replace({});
        m_state = QStringLiteral("error");
        updateCurrentLine();
        Q_EMIT stateChanged();
        return;
    }
    const LyricsAnswer &answer = settled.answer;
    if (settled.keptStays) {
        if (m_checking) {
            // The check is over; nothing on show changed.
            m_checking = false;
            m_stored = answer;
            if (answer.provisional)
                m_recheck.start();
            Q_EMIT stateChanged();
            return;
        }
        show(answer);
        return;
    }
    // Found lyrics on show already: the kept ones being checked, or this
    // race's best in hand.
    if (m_hasStored && m_state != QLatin1String("loading")) {
        if (sameWords(answer, m_stored)) {
            // The lines on show are the answer: no reset under the reader.
            m_stored = answer;
            m_checking = false;
            if (answer.provisional)
                m_recheck.start();
            Q_EMIT stateChanged();
            return;
        }
        // A better kind of answer replaces the interim one at once. One of
        // the same kind, only from higher in the order, waits while someone
        // reads along: it is kept, and shown next time, unless the view is
        // hidden or the first timed line has not been sung yet.
        const bool readingAlong = m_active && !(m_state == QLatin1String("synced") && m_current < 0);
        if (wasInterim && answer.rank() <= m_stored.rank() && readingAlong) {
            qInfo("lyrics: %s: the answer on show stays while it is read; %s's is kept for next time",
                  qPrintable(m_videoId), qPrintable(answer.source));
            m_checking = false;
            Q_EMIT stateChanged();
            return;
        }
    }
    m_checking = false;
    show(answer);
}

void Lyrics::show(const LyricsAnswer &answer)
{
    QList<LyricsModel::Line> lines;
    QString state;
    if (!answer.synced.isEmpty()) {
        lines = parseLrc(answer.synced);
        if (!lines.isEmpty())
            state = QStringLiteral("synced");
    }
    if (state.isEmpty() && !answer.plain.isEmpty()) {
        lines = plainLines(answer.plain);
        if (!lines.isEmpty())
            state = QStringLiteral("plain");
    }
    if (state.isEmpty()) {
        lines.clear();
        state = answer.instrumental ? QStringLiteral("instrumental") : QStringLiteral("none");
    }
    const bool found = !lines.isEmpty() || answer.instrumental;
    m_stored = answer;
    m_hasStored = found;
    if (found && answer.provisional && !m_checking)
        m_recheck.start();
    m_source = lines.isEmpty() ? QString() : answer.source;
    m_error.clear();
    m_lines.replace(lines);
    // Before the current line is worked out, which only follows synced lines.
    m_state = state;
    updateCurrentLine();
    Q_EMIT stateChanged();
}

void Lyrics::updateCurrentLine()
{
    int index = -1;
    if (m_state == QLatin1String("synced") && m_videoId == m_player->currentSourceId()) {
        const qint64 at = m_player->position() + kLeadMs;
        const QList<LyricsModel::Line> &lines = m_lines.lines();
        const auto after = std::upper_bound(lines.cbegin(), lines.cend(), at,
                                            [](qint64 time, const LyricsModel::Line &line) {
                                                return time < line.timeMs;
                                            });
        index = int(after - lines.cbegin()) - 1;
    }
    if (index == m_current)
        return;
    m_current = index;
    Q_EMIT currentLineChanged();
}

void Lyrics::seekToLine(int row)
{
    const QList<LyricsModel::Line> &lines = m_lines.lines();
    if (m_state != QLatin1String("synced") || m_videoId != m_player->currentSourceId()
        || row < 0 || row >= lines.size())
        return;
    m_player->setPosition(lines.at(row).timeMs);
    if (!m_player->playing())
        m_player->play();
}

// "[01:02.34] words", several stamps to a line allowed, "[offset:+120]" honoured,
// word-level "<01:02.50>" stamps (enhanced LRC) dropped. Lines without a stamp,
// like the "[ar:]" and "[ti:]" tags, are not lyrics.
QList<LyricsModel::Line> Lyrics::parseLrc(const QString &lrc)
{
    static const QRegularExpression stamp(QStringLiteral(R"(^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])"));
    static const QRegularExpression offsetTag(QStringLiteral(R"(\[offset:\s*([+-]?\d+)\s*\])"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wordStamp(QStringLiteral(R"(<\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?>)"));

    // A positive offset shows the lyrics sooner.
    const QRegularExpressionMatch offsetMatch = offsetTag.match(lrc);
    const qint64 offset = offsetMatch.hasMatch() ? offsetMatch.captured(1).toLongLong() : 0;

    QList<LyricsModel::Line> lines;
    for (const QString &raw : lrc.split(QLatin1Char('\n'))) {
        QString rest = raw.trimmed();
        QList<qint64> times;
        for (QRegularExpressionMatch match = stamp.match(rest); match.hasMatch(); match = stamp.match(rest)) {
            const qint64 minutes = match.captured(1).toLongLong();
            const qint64 seconds = match.captured(2).toLongLong();
            // "5" is tenths, "05" hundredths, "005" thousandths.
            const qint64 fraction = match.captured(3).leftJustified(3, QLatin1Char('0')).left(3).toLongLong();
            times << (minutes * 60 + seconds) * 1000 + fraction - offset;
            rest = rest.mid(match.capturedLength()).trimmed();
        }
        if (times.isEmpty())
            continue;
        rest.remove(wordStamp);
        const QString text = rest.simplified();
        for (const qint64 time : std::as_const(times))
            lines.append({ qMax<qint64>(0, time), text });
    }
    std::stable_sort(lines.begin(), lines.end(), [](const LyricsModel::Line &a, const LyricsModel::Line &b) {
        return a.timeMs < b.timeMs;
    });
    // Nothing but empty lines is no lyrics.
    const bool anyWords = std::any_of(lines.cbegin(), lines.cend(),
                                      [](const LyricsModel::Line &line) { return !line.text.isEmpty(); });
    return anyWords ? lines : QList<LyricsModel::Line>();
}

// Plain text as lines, a stanza break kept as one empty line.
QList<LyricsModel::Line> Lyrics::plainLines(const QString &text)
{
    QList<LyricsModel::Line> lines;
    bool afterBreak = true;   // no break before the first line
    QString cleaned = text;
    cleaned.remove(QLatin1Char('\r'));
    for (const QString &raw : cleaned.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        if (line.isEmpty()) {
            if (!afterBreak)
                lines.append({ -1, QString() });
            afterBreak = true;
            continue;
        }
        afterBreak = false;
        lines.append({ -1, line });
    }
    while (!lines.isEmpty() && lines.last().text.isEmpty())
        lines.removeLast();
    return lines;
}
