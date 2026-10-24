#include "justplayed.h"
#include "appdatabase.h"
#include "artistlinks.h"
#include "library.h"
#include "rec/shelves.h"
#include "rec/suitable.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlQuery>

#include <algorithm>
#include <cstdio>

namespace {

const QString kEnabledKey = QStringLiteral("home.justPlayed");
const QString kJarKey = QStringLiteral("home.justPlayed.jar");
const QString kPaceKey = QStringLiteral("home.justPlayed.pace");

constexpr int kMaxSeeds = 60;
constexpr int kMaxQueue = 3;
constexpr int kBackfill = 3;
constexpr int kKeptTracks = 50;
constexpr int kKeptRadios = 200;
constexpr qint64 kKeptSecs = 30 * 24 * 3600;
constexpr qint64 kHeardSecs = 48 * 3600;
constexpr qint64 kHeadHoldMs = 15 * 60 * 1000;
constexpr double kHeadLead = 1.25;

std::function<qint64()> &clockOverride()
{
    static std::function<qint64()> instance;
    return instance;
}

bool validVideoId(const QString &id)
{
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    return shape.match(id).hasMatch();
}

Rec::RadioTrack fromTrack(const InnerTube::Track &track)
{
    Rec::RadioTrack radio;
    radio.videoId = track.videoId;
    radio.title = track.title;
    radio.artist = track.artist;
    radio.primaryArtist = track.primaryArtist;
    radio.album = track.album;
    radio.albumId = track.albumId;
    radio.artwork = track.artwork;
    radio.durationMs = track.durationMs;
    radio.isVideo = track.isVideo;
    radio.credits = InnerTube::creditsToVariant(track.credits);
    return radio;
}

QJsonObject toJson(const Rec::RadioTrack &track)
{
    return {
        { QStringLiteral("videoId"), track.videoId },
        { QStringLiteral("title"), track.title },
        { QStringLiteral("artist"), track.artist },
        { QStringLiteral("primaryArtist"), track.primaryArtist },
        { QStringLiteral("album"), track.album },
        { QStringLiteral("albumId"), track.albumId },
        { QStringLiteral("artwork"), track.artwork },
        { QStringLiteral("durationMs"), track.durationMs },
        { QStringLiteral("isVideo"), track.isVideo },
        { QStringLiteral("credits"), QJsonArray::fromVariantList(track.credits) }
    };
}

Rec::RadioTrack fromJson(const QJsonObject &object)
{
    Rec::RadioTrack track;
    track.videoId = object.value(QLatin1String("videoId")).toString();
    track.title = object.value(QLatin1String("title")).toString();
    track.artist = object.value(QLatin1String("artist")).toString();
    track.primaryArtist = object.value(QLatin1String("primaryArtist")).toString();
    track.album = object.value(QLatin1String("album")).toString();
    track.albumId = object.value(QLatin1String("albumId")).toString();
    track.artwork = object.value(QLatin1String("artwork")).toString();
    track.durationMs = qint64(object.value(QLatin1String("durationMs")).toDouble());
    track.isVideo = object.value(QLatin1String("isVideo")).toBool();
    track.credits = object.value(QLatin1String("credits")).toArray().toVariantList();
    return track;
}

QVariantMap toCard(const Rec::RadioTrack &track, const QString &rowKey, int rowIndex)
{
    return {
        { QStringLiteral("type"), track.isVideo ? QStringLiteral("video") : QStringLiteral("song") },
        { QStringLiteral("videoId"), track.videoId },
        { QStringLiteral("title"), track.title },
        { QStringLiteral("subtitle"), track.artist },
        { QStringLiteral("artist"), track.artist },
        { QStringLiteral("artwork"), track.artwork },
        { QStringLiteral("album"), track.album },
        { QStringLiteral("albumId"), track.albumId },
        { QStringLiteral("credits"), track.credits },
        { QStringLiteral("durationMs"), track.durationMs },
        { QStringLiteral("primaryArtist"), track.primaryArtist },
        { QStringLiteral("suggestion"), true },
        { QStringLiteral("rowKey"), rowKey },
        { QStringLiteral("rowIndex"), rowIndex }
    };
}

// The seed a track is, as playback reports it (QueueModel's map).
Rec::SessionSeed seedOf(const QVariantMap &track, bool chosen)
{
    Rec::SessionSeed seed;
    seed.videoId = track.value(QStringLiteral("sourceId")).toString();
    if (seed.videoId.isEmpty())
        seed.videoId = track.value(QStringLiteral("videoId")).toString();
    seed.title = track.value(QStringLiteral("title")).toString().trimmed();
    seed.artist = track.value(QStringLiteral("artist")).toString().trimmed();
    const QString primary = track.value(QStringLiteral("primaryArtist")).toString().trimmed();
    seed.artistName = primary.isEmpty() ? Rec::leadArtistName(seed.artist) : primary;
    seed.leadKey = Rec::suggestionArtistKey(primary.isEmpty() ? seed.artist : primary);
    // Its page, from the credit that names it, or else the first linked.
    QString firstLinked;
    for (const QVariant &value : track.value(QStringLiteral("credits")).toList()) {
        const QVariantMap credit = value.toMap();
        const QString id = credit.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || !credit.value(QStringLiteral("link")).toBool())
            continue;
        if (firstLinked.isEmpty())
            firstLinked = id;
        if (credit.value(QStringLiteral("text")).toString().compare(seed.artistName, Qt::CaseInsensitive) == 0) {
            seed.artistId = id;
            break;
        }
    }
    if (seed.artistId.isEmpty())
        seed.artistId = firstLinked;
    seed.chosen = chosen;
    seed.live = true;
    seed.lastAt = JustPlayed::now() / 1000;
    return seed;
}

} // namespace

void JustPlayed::setClock(std::function<qint64()> clock)
{
    clockOverride() = std::move(clock);
}

qint64 JustPlayed::now()
{
    const std::function<qint64()> &clock = clockOverride();
    return clock ? clock() : QDateTime::currentMSecsSinceEpoch();
}

JustPlayed::JustPlayed(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &JustPlayed::pump);
    m_launchTimer.setSingleShot(true);
    connect(&m_launchTimer, &QTimer::timeout, this, &JustPlayed::backfill);
    // A 400 is a failure here, rested like any: never a second /next for
    // it, nor the user's country dropped (and Home refetched) on its say.
    m_innerTube.setKeepsRegion(true);
}

JustPlayed::~JustPlayed()
{
    m_quitting = true;
    ++m_generation;
}

// ------------------------------------------------------------------ settings

QString JustPlayed::setting(const QString &key) const
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return {};
}

void JustPlayed::storeSetting(const QString &key, const QString &value) const
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    query.addBindValue(AppDatabase::text(value));
    query.exec();
}

void JustPlayed::loadPace()
{
    const QJsonObject pace = QJsonDocument::fromJson(setting(kPaceKey).toUtf8()).object();
    const qint64 nowSecs = now() / 1000;
    m_pace = Pace();
    if (pace.contains(QLatin1String("sent"))) {
        for (const QJsonValue &value : pace.value(QLatin1String("sent")).toArray())
            m_pace.sent.append(qint64(value.toDouble()));
    } else {
        // The first beta's fixed windows: their counts, as made when the
        // windows began.
        const int hourCount = pace.value(QLatin1String("hourCount")).toInt();
        const int dayCount = pace.value(QLatin1String("dayCount")).toInt();
        for (int i = 0; i < qMax(0, dayCount - hourCount); ++i)
            m_pace.sent.append(qint64(pace.value(QLatin1String("day")).toDouble()));
        for (int i = 0; i < hourCount; ++i)
            m_pace.sent.append(qint64(pace.value(QLatin1String("hourStart")).toDouble()));
    }
    m_pace.restUntil = qint64(pace.value(QLatin1String("restUntil")).toDouble());
    m_pace.failStreak = pace.value(QLatin1String("failStreak")).toInt();
    m_pace.emptyStreak = pace.value(QLatin1String("emptyStreak")).toInt();
    // A clock that ran ahead (2026-10-21) must not stop the asking for days:
    // a rest ends a day from now at the latest, and a look-up dated later
    // than now counts as made now.
    m_pace.restUntil = qMin(m_pace.restUntil, nowSecs + 24 * 3600);
    for (qint64 &at : m_pace.sent)
        at = qMin(at, nowSecs);
    std::sort(m_pace.sent.begin(), m_pace.sent.end());
    pruneSent(nowSecs);
}

// The look-ups of the last 24 hours, and no more of them than a cap counts.
void JustPlayed::pruneSent(qint64 nowSecs)
{
    while (!m_pace.sent.isEmpty() && nowSecs - m_pace.sent.constFirst() >= 24 * 3600)
        m_pace.sent.removeFirst();
    const int kept = qMax(1, qMax(m_timing.hourCap, m_timing.dayCap));
    if (m_pace.sent.size() > kept)
        m_pace.sent.remove(0, m_pace.sent.size() - kept);
}

// How long until one more look-up fits under `cap` in the last
// `windowSecs`: 0 when it does now.
qint64 JustPlayed::capWaitMs(qint64 atMs, qint64 windowSecs, int cap) const
{
    const qint64 nowSecs = atMs / 1000;
    int inside = 0;
    for (const qint64 sent : m_pace.sent)
        inside += nowSecs - sent < windowSecs ? 1 : 0;
    if (inside < cap)
        return 0;
    if (cap <= 0)
        return m_timing.recheckMs;
    // Once the oldest of the newest `cap` is a window old.
    return (m_pace.sent.at(m_pace.sent.size() - cap) + windowSecs) * 1000 - atMs;
}

void JustPlayed::savePace() const
{
    if (m_quitting)
        return;
    QJsonArray sent;
    for (const qint64 at : m_pace.sent)
        sent.append(at);
    const QJsonObject pace{
        { QStringLiteral("sent"), sent },
        { QStringLiteral("restUntil"), m_pace.restUntil },
        { QStringLiteral("failStreak"), m_pace.failStreak },
        { QStringLiteral("emptyStreak"), m_pace.emptyStreak }
    };
    storeSetting(kPaceKey, QString::fromUtf8(QJsonDocument(pace).toJson(QJsonDocument::Compact)));
}

// ------------------------------------------------------------------- state

void JustPlayed::load()
{
    m_loaded = true;
    const QString stored = setting(kEnabledKey);
    m_enabled = stored.isEmpty() || stored == QLatin1String("1");
    // Still signed out either way; the jar holds only YouTube's anonymous
    // cookies, the account's being set per request (innertube.cpp).
    m_jarMode = setting(kJarKey) == QLatin1String("1");
    m_innerTube.setCookieless(!m_jarMode);
    if (!m_enabled)
        return;
    loadPace();
    m_launchUntil = now() + m_timing.launchDelayMs;
    reloadSeeds();
    m_launchTimer.start(int(qMax<qint64>(0, m_timing.launchDelayMs)));
}

void JustPlayed::shutdown()
{
    m_quitting = true;
    ++m_generation;
    m_timer.stop();
    m_launchTimer.stop();
    m_queue.clear();
}

void JustPlayed::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    storeSetting(kEnabledKey, on ? QStringLiteral("1") : QStringLiteral("0"));
    // Whatever is on its way is not wanted now, whatever it says.
    ++m_generation;
    m_timer.stop();
    m_launchTimer.stop();
    m_queue.clear();
    m_hasLive = false;
    if (on) {
        loadPace();
        reloadSeeds();
    } else {
        m_plays.clear();
        m_seeds.clear();
        m_radios.clear();
        m_radioAbsent.clear();
        m_signature.clear();
        m_headKey.clear();
        const bool had = !m_rows.isEmpty() || !m_candidates.isEmpty();
        m_rows.clear();
        m_candidates.clear();
        if (had)
            Q_EMIT rowsChanged();
    }
    Q_EMIT enabledChanged();
}

void JustPlayed::setWatching(bool on)
{
    if (on == m_watching)
        return;
    m_watching = on;
    Q_EMIT watchingChanged();
    pump();
}

void JustPlayed::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    pump();
}

void JustPlayed::setAccountResting(bool resting)
{
    if (resting == m_accountResting)
        return;
    m_accountResting = resting;
    pump();
}

void JustPlayed::setFilter(std::function<bool(const QString &, const QString &, const QString &)> filter)
{
    m_filter = std::move(filter);
}

void JustPlayed::setHideExplicit(bool hide)
{
    if (hide == m_hideExplicit)
        return;
    m_hideExplicit = hide;
    recompose();
}

void JustPlayed::visit()
{
    ++m_visit;
    recompose();
}

void JustPlayed::onRegionChanged()
{
    // An answer for the old country still on its way is dropped (the next
    // waits its turn after it all the same), and that seed is asked again;
    // what is kept for the old country is shown, and asked again for the
    // newest seed.
    ++m_generation;
    recompose();
    if (!m_enabled || m_quitting)
        return;
    const QString dropped = m_inFlight;
    if (m_hasLive && m_live.videoId != dropped && wantsFetch(m_live.videoId, /*newest=*/true))
        enqueue(m_live.videoId);
    if (!dropped.isEmpty()) {
        m_queue.removeAll(dropped);
        m_queue.prepend(dropped);
        while (m_queue.size() > kMaxQueue)
            m_queue.removeLast();
    }
}

// ------------------------------------------------------------------- seeds

bool JustPlayed::suitableSeed(const Rec::SessionSeed &seed) const
{
    if (!validVideoId(seed.videoId) || seed.title.isEmpty())
        return false;
    if (m_filter && m_filter(seed.videoId, seed.title, seed.artist))
        return false;
    return Rec::suitableForSuggestion(seed.title, seed.artist, m_hideExplicit);
}

void JustPlayed::noteSettled(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    if (chosenByUser)
        noteSeed(track, startedAt, true);
}

void JustPlayed::noteQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    if (!chosenByUser)
        noteSeed(track, startedAt, false);
}

void JustPlayed::noteSeed(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    if (!m_enabled || m_quitting)
        return;
    Rec::SessionSeed seed = seedOf(track, chosenByUser);
    if (!suitableSeed(seed))
        return;
    // Liked now, by the newest like or unlike of it.
    {
        QSqlQuery liked(AppDatabase::connection());
        liked.prepare(QStringLiteral(
            "SELECT kind FROM play_events WHERE video_id = ? AND kind IN ('like', 'unliked')"
            " ORDER BY started_at DESC, id DESC LIMIT 1"));
        liked.addBindValue(seed.videoId);
        seed.liked = liked.exec() && liked.next() && liked.value(0).toString() == QLatin1String("like");
    }
    // The same song again before its last listen closed (repeat-one): one
    // seed, one more listen.
    if (m_hasLive && m_live.videoId == seed.videoId) {
        m_live.listens += 1;
        m_live.lastAt = seed.lastAt;
        m_live.liked = seed.liked;
    } else {
        m_live = seed;
        m_hasLive = true;
        m_liveStartedAt = startedAt > 0 ? startedAt : seed.lastAt;
    }
    // The song playing is heard: the rows leave it out from now on.
    m_recent.insert(seed.videoId);
    m_recentHeard.insert(seed.videoId);
    mergeSeeds();
    recompose();
    if (wantsFetch(seed.videoId, /*newest=*/true))
        enqueue(seed.videoId);
}

void JustPlayed::reloadSeeds()
{
    if (!m_enabled || m_quitting)
        return;
    const qint64 nowSecs = now() / 1000;

    m_recent.clear();
    m_recentHeard.clear();
    {
        QSqlQuery recent(AppDatabase::connection());
        recent.exec(QStringLiteral("SELECT video_id, strftime('%s', played_at) FROM recent"));
        while (recent.next()) {
            const QString id = recent.value(0).toString();
            if (id.isEmpty())
                continue;
            m_recent.insert(id);
            if (nowSecs - recent.value(1).toLongLong() <= kHeardSecs)
                m_recentHeard.insert(id);
        }
    }

    // Newest first: the first like or unlike of a song read is its state
    // now, and the first closed play its latest verdict.
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT p.kind, p.video_id, p.title, p.artist, strftime('%s', p.started_at), p.label, p.listened_ms,"
        " p.source FROM play_events p WHERE p.video_id <> '' AND p.kind IN ('play','like','unliked')"
        " AND p.started_at >= datetime('now','-14 days') ORDER BY p.started_at DESC, p.id DESC LIMIT 600"));
    struct Play {
        Rec::SessionSeed seed;
        QVariant label;
        qint64 listenedMs = 0;
    };
    QVector<Play> plays;
    QHash<QString, bool> liked;
    while (query.next()) {
        const QString kind = query.value(0).toString();
        const QString id = query.value(1).toString();
        if (kind != QLatin1String("play")) {
            if (!liked.contains(id))
                liked.insert(id, kind == QLatin1String("like"));
            continue;
        }
        Play play;
        play.seed.videoId = id;
        play.seed.title = query.value(2).toString().trimmed();
        play.seed.artist = query.value(3).toString().trimmed();
        play.seed.lastAt = query.value(4).toLongLong();
        play.label = query.value(5);
        play.listenedMs = query.value(6).toLongLong();
        play.seed.chosen = query.value(7).toString() != QLatin1String("radio");
        play.seed.artistName = Rec::leadArtistName(play.seed.artist);
        play.seed.leadKey = Rec::suggestionArtistKey(play.seed.artist);
        plays.append(play);
    }

    // The live seed's own listen has closed: from now on its play speaks
    // for it, and drops it if it was a skip. Gone from Recently played
    // (Clear history, Remove from history): gone.
    if (m_hasLive) {
        if (!m_recent.contains(m_live.videoId)) {
            m_hasLive = false;
        } else {
            for (const Play &play : std::as_const(plays)) {
                // Its own play row, written as it began to load.
                if (play.seed.videoId == m_live.videoId && play.seed.lastAt >= m_liveStartedAt - 120) {
                    if (!play.label.isNull())
                        m_hasLive = false;
                    break;
                }
            }
        }
    }

    QSet<QString> verdict;   // ids whose latest closed play was read
    m_plays.clear();
    for (const Play &play : std::as_const(plays)) {
        const QString &id = play.seed.videoId;
        if (!m_recent.contains(id))
            continue;
        const bool isLiked = liked.value(id, false);
        const bool closed = !play.label.isNull() || play.listenedMs > 0;
        const double label = play.label.toDouble();
        const bool turnedAway = !play.label.isNull() && (qFuzzyIsNull(label) || qFuzzyCompare(label, 0.2));
        // The latest verdict on a song decides: a skip of it last drops it.
        if (closed && !verdict.contains(id)) {
            verdict.insert(id);
            if (turnedAway && !isLiked) {
                verdict.insert(QLatin1String("!") + id);
                continue;
            }
        }
        if (verdict.contains(QLatin1String("!") + id))
            continue;
        const bool qualifies = isLiked || (!play.label.isNull() && label >= 0.6)
                               || (play.label.isNull() && play.listenedMs >= 30000);
        if (!qualifies || (turnedAway && !isLiked))
            continue;
        Rec::SessionSeed seed = play.seed;
        seed.liked = isLiked;
        if (!suitableSeed(seed))
            continue;
        m_plays.append(seed);
    }

    // What rec_radio holds now: an entry it no longer has (cleared with the
    // history) is forgotten here too.
    {
        QHash<QString, qint64> kept;
        QSqlQuery radios(AppDatabase::connection());
        radios.exec(QStringLiteral("SELECT video_id, fetched_at FROM rec_radio"));
        while (radios.next())
            kept.insert(radios.value(0).toString(), radios.value(1).toLongLong());
        for (auto it = m_radios.begin(); it != m_radios.end();) {
            if (kept.value(it.key(), -1) != it->fetchedAt)
                it = m_radios.erase(it);
            else
                ++it;
        }
        for (auto it = m_radioAbsent.begin(); it != m_radioAbsent.end();) {
            if (kept.contains(*it))
                it = m_radioAbsent.erase(it);
            else
                ++it;
        }
    }

    mergeSeeds();
    recompose();
}

void JustPlayed::mergeSeeds()
{
    QVector<Rec::SessionSeed> plays;
    plays.reserve(m_plays.size() + 1);
    if (m_hasLive)
        plays.append(m_live);
    plays += m_plays;
    m_seeds = Rec::collapseSeeds(plays).mid(0, kMaxSeeds);
}

// ------------------------------------------------------------------- radios

void JustPlayed::ensureRadios(const QStringList &ids)
{
    QStringList wanted;
    for (const QString &id : ids) {
        if (!m_radios.contains(id) && !m_radioAbsent.contains(id) && !wanted.contains(id))
            wanted << id;
    }
    if (wanted.isEmpty())
        return;
    QStringList marks;
    for (qsizetype i = 0; i < wanted.size(); ++i)
        marks << QStringLiteral("?");
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT video_id, region, fetched_at, ok, body FROM rec_radio WHERE video_id IN (")
                  + marks.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const QString &id : std::as_const(wanted))
        query.addBindValue(id);
    QSet<QString> found;
    if (query.exec()) {
        while (query.next()) {
            Radio radio;
            radio.region = query.value(1).toString();
            radio.fetchedAt = query.value(2).toLongLong();
            radio.ok = query.value(3).toInt() != 0;
            if (radio.ok) {
                const QJsonArray tracks = QJsonDocument::fromJson(qUncompress(query.value(4).toByteArray())).array();
                for (const QJsonValue &track : tracks)
                    radio.tracks.append(fromJson(track.toObject()));
            }
            const QString id = query.value(0).toString();
            found.insert(id);
            m_radios.insert(id, radio);
        }
    }
    for (const QString &id : std::as_const(wanted)) {
        if (!found.contains(id))
            m_radioAbsent.insert(id);
    }
}

void JustPlayed::storeRadio(const QString &videoId, const QList<InnerTube::Track> &tracks, bool ok)
{
    if (m_quitting)
        return;
    ensureRadios({ videoId });
    const qint64 nowSecs = now() / 1000;
    QSqlQuery query(AppDatabase::connection());
    // An empty or failed answer for a song whose radio is kept keeps that
    // radio, and is not asked again for as long as an answer would be.
    if (!ok && m_radios.value(videoId).ok) {
        query.prepare(QStringLiteral("UPDATE rec_radio SET fetched_at = ? WHERE video_id = ?"));
        query.addBindValue(nowSecs);
        query.addBindValue(videoId);
        query.exec();
        m_radios[videoId].fetchedAt = nowSecs;
        return;
    }
    Radio radio;
    radio.region = InnerTube::region();
    radio.fetchedAt = nowSecs;
    radio.ok = ok;
    QJsonArray body;
    if (ok) {
        for (const InnerTube::Track &track : tracks) {
            if (radio.tracks.size() >= kKeptTracks)
                break;
            radio.tracks.append(fromTrack(track));
            body.append(toJson(radio.tracks.last()));
        }
    }
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO rec_radio (video_id, region, fetched_at, ok, body) VALUES (?, ?, ?, ?, ?)"));
    query.addBindValue(videoId);
    query.addBindValue(AppDatabase::text(radio.region));
    query.addBindValue(nowSecs);
    query.addBindValue(ok ? 1 : 0);
    query.addBindValue(ok ? qCompress(QJsonDocument(body).toJson(QJsonDocument::Compact)) : QByteArray(""));
    query.exec();
    m_radios.insert(videoId, radio);
    m_radioAbsent.remove(videoId);

    // A month at most, and the newest 200.
    QSqlQuery prune(AppDatabase::connection());
    prune.prepare(QStringLiteral("DELETE FROM rec_radio WHERE fetched_at < ?"));
    prune.addBindValue(nowSecs - kKeptSecs);
    prune.exec();
    prune.prepare(QStringLiteral(
        "DELETE FROM rec_radio WHERE video_id NOT IN"
        " (SELECT video_id FROM rec_radio ORDER BY fetched_at DESC LIMIT ?)"));
    prune.addBindValue(kKeptRadios);
    prune.exec();
}

void JustPlayed::noteRadio(const QString &seed, const QList<InnerTube::Track> &tracks)
{
    if (!m_enabled || m_quitting || tracks.isEmpty() || !validVideoId(seed))
        return;
    storeRadio(seed, tracks, true);
    m_queue.removeAll(seed);
    logOverlaps(seed);
    recompose();
}

void JustPlayed::logOverlaps(const QString &videoId)
{
    if (!QLoggingCategory::defaultCategory()->isDebugEnabled())
        return;
    const QVector<Rec::RadioTrack> mine = m_radios.value(videoId).tracks;
    if (mine.isEmpty())
        return;
    for (const Rec::SessionSeed &seed : std::as_const(m_seeds)) {
        if (seed.videoId == videoId)
            continue;
        const auto other = m_radios.constFind(seed.videoId);
        if (other == m_radios.cend() || !other->ok)
            continue;
        qDebug("home: cluster overlap %s/%s = %.2f", qPrintable(videoId), qPrintable(seed.videoId),
               Rec::artistOverlap(mine, other->tracks));
    }
}

bool JustPlayed::wantsFetch(const QString &videoId, bool newest)
{
    ensureRadios({ videoId });
    const auto kept = m_radios.constFind(videoId);
    if (kept == m_radios.cend())
        return true;
    const qint64 age = now() / 1000 - kept->fetchedAt;
    // An empty or failed answer, and the newest seed's radio, are asked
    // again after three days; the newest's too when it was for another
    // country.
    if (!kept->ok)
        return age > m_timing.freshS1Secs;
    return newest && (age > m_timing.freshS1Secs || kept->region != InnerTube::region());
}

// ------------------------------------------------------------------- pacing

void JustPlayed::enqueue(const QString &videoId)
{
    if (!m_enabled || m_quitting || videoId == m_inFlight)
        return;
    m_queue.removeAll(videoId);
    m_queue.prepend(videoId);
    // The oldest waiting are dropped, not asked later.
    while (m_queue.size() > kMaxQueue)
        m_queue.removeLast();
    m_debounceUntil = now() + m_timing.debounceMs;
    pump();
}

void JustPlayed::pump()
{
    m_timer.stop();
    if (!m_enabled || m_quitting || !m_inFlight.isEmpty() || m_queue.isEmpty())
        return;
    // Held: the seeds are kept, nothing is sent until the hold ends.
    if (m_paused || m_accountResting || !m_watching)
        return;
    const qint64 at = now();
    const qint64 nowSecs = at / 1000;
    qint64 wait = qMax(qMax(m_launchUntil, m_debounceUntil), m_nextAllowedAt) - at;
    wait = qMax(wait, m_pace.restUntil * 1000 - at);
    // 30 in any hour and 200 in any day, counted back from now.
    pruneSent(nowSecs);
    wait = qMax(wait, capWaitMs(at, 3600, m_timing.hourCap));
    wait = qMax(wait, capWaitMs(at, 24 * 3600, m_timing.dayCap));
    if (wait > 0) {
        m_timer.start(int(qBound<qint64>(1, wait, qMax<qint64>(1, m_timing.recheckMs))));
        return;
    }
    send(m_queue.takeFirst());
}

void JustPlayed::send(const QString &videoId)
{
    // Kept meanwhile (autoplay answered for it): nothing to ask.
    if (!wantsFetch(videoId, /*newest=*/m_hasLive && m_live.videoId == videoId)) {
        pump();
        return;
    }
    m_inFlight = videoId;
    const qint64 nowSecs = now() / 1000;
    m_pace.sent.append(nowSecs);
    pruneSent(nowSecs);
    savePace();
    const quint64 generation = m_generation;
    // Signed out, no cookies (setCookieless), no slot and no second try.
    m_innerTube.watchPlaylist(
        { videoId, QStringLiteral("RDAMVM") + videoId, QStringLiteral("wAEB") },
        [self = QPointer<JustPlayed>(this), generation, videoId](const QList<InnerTube::Track> &tracks,
                                                                const QString &error) {
            if (!self || self->m_quitting)
                return;
            self->m_inFlight.clear();
            if (generation != self->m_generation) {
                self->dropped(error);
                return;
            }
            self->answered(videoId, tracks, error);
        },
        /*retries=*/0);
}

void JustPlayed::spaceNext()
{
    m_nextAllowedAt = now() + m_timing.gapMs
                      + QRandomGenerator::global()->bounded(int(qMax<qint64>(0, m_timing.jitterMs)) + 1);
}

void JustPlayed::failed(const QString &error)
{
    m_pace.failStreak += 1;
    const QList<qint64> &rests = m_timing.restsMs;
    const qint64 ms = rests.isEmpty() ? 0 : rests.at(qMin(m_pace.failStreak, int(rests.size())) - 1);
    rest(ms, error);
}

void JustPlayed::dropped(const QString &error)
{
    // Not wanted now (another country, or switched off and on), but asked
    // all the same: the next waits its turn, and a refusal rests as any.
    spaceNext();
    if (!error.isEmpty() && error != InnerTube::noSongsError())
        failed(error);
    savePace();
    pump();
}

void JustPlayed::answered(const QString &videoId, const QList<InnerTube::Track> &tracks, const QString &error)
{
    spaceNext();
    QString title = seedTitle(videoId);
    // A seed a newer one has taken the place of: named as its radio names it.
    for (qsizetype i = 0; title.isEmpty() && i < tracks.size(); ++i) {
        if (tracks.at(i).videoId == videoId)
            title = tracks.at(i).title;
    }
    if (error.isEmpty() && !tracks.isEmpty()) {
        m_pace.failStreak = 0;
        m_pace.emptyStreak = 0;
        storeRadio(videoId, tracks, true);
        qInfo("home: songs like \"%s\" (%s) asked signed out, %d kept", qPrintable(title), qPrintable(videoId),
              int(m_radios.value(videoId).tracks.size()));
        logOverlaps(videoId);
    } else if (error == InnerTube::noSongsError()) {
        // An answer, with nothing in it: remembered, and the next seed
        // takes the row.
        m_pace.failStreak = 0;
        m_pace.emptyStreak += 1;
        storeRadio(videoId, {}, false);
        qInfo("home: songs like \"%s\" (%s) asked signed out, 0 kept", qPrintable(title), qPrintable(videoId));
        if (m_pace.emptyStreak >= 3) {
            m_pace.emptyStreak = 0;
            rest(m_timing.emptyStreakRestMs, QStringLiteral("3 empty answers"));
        }
    } else {
        // No retry: a rest, longer each time in a row.
        storeRadio(videoId, {}, false);
        failed(error);
    }
    savePace();
    recompose();
    pump();
}

void JustPlayed::rest(qint64 ms, const QString &reason)
{
    m_pace.restUntil = (now() + ms + 999) / 1000;
    qInfo("home: radio resting %lld min after %s", ms / 60000, qPrintable(reason.left(160)));
}

void JustPlayed::backfill()
{
    if (!m_enabled || m_quitting)
        return;
    // What the first rows would need and has nothing kept at all: the
    // newest seed of up to three lead artists.
    QStringList picked;
    QSet<QString> leads;
    QStringList ids;
    for (const Rec::SessionSeed &seed : std::as_const(m_seeds))
        ids << seed.videoId;
    ensureRadios(ids);
    for (const Rec::SessionSeed &seed : std::as_const(m_seeds)) {
        if (picked.size() >= kBackfill)
            break;
        if (leads.contains(seed.leadKey))
            continue;
        leads.insert(seed.leadKey);
        if (!m_radios.contains(seed.videoId))
            picked << seed.videoId;
    }
    // Oldest first, so the newest goes first.
    for (auto it = picked.crbegin(); it != picked.crend(); ++it)
        enqueue(*it);
}

QString JustPlayed::seedTitle(const QString &videoId) const
{
    for (const Rec::SessionSeed &seed : m_seeds) {
        if (seed.videoId == videoId)
            return seed.title;
    }
    return QString();
}

// ------------------------------------------------------------------- rows

void JustPlayed::recompose()
{
    if (m_quitting)
        return;
    QVariantList rows;
    QVariantList candidates;
    if (m_enabled) {
        const qint64 at = now();
        const qint64 nowSecs = at / 1000;
        QStringList ids;
        for (const Rec::SessionSeed &seed : std::as_const(m_seeds))
            ids << seed.videoId;
        ensureRadios(ids);
        QHash<QString, QVector<Rec::RadioTrack>> radios;
        for (const QString &id : std::as_const(ids)) {
            const auto kept = m_radios.constFind(id);
            if (kept != m_radios.cend() && kept->ok && nowSecs - kept->fetchedAt <= m_timing.displaySecs)
                radios.insert(id, kept->tracks);
        }
        Rec::SessionOptions options;
        options.recentHeard = m_recentHeard;
        options.hideExplicit = m_hideExplicit;
        options.rotation = Rec::rotationFor(m_visit, 0);
        const QVector<Rec::SessionRow> built = Rec::buildSessionRows(m_seeds, radios, nowSecs, options, m_filter);
        for (const Rec::SessionRow &row : built) {
            QVariantList cards;
            for (int i = 0; i < row.items.size(); ++i)
                cards.append(toCard(row.items.at(i), row.key, i));
            rows.append(QVariantMap{
                { QStringLiteral("key"), row.key },
                { QStringLiteral("kind"), row.kind },
                { QStringLiteral("title"), row.title },
                { QStringLiteral("seedVideoId"), row.seedVideoId },
                { QStringLiteral("items"), cards }
            });
        }

        // More like <artist>: its head holds for 15 minutes, and then gives
        // way only to someone a quarter heavier.
        QVector<double> weights;
        QVector<Rec::SessionSeed> artists = Rec::artistCandidates(
            m_seeds, nowSecs,
            [this](const QString &name) { return m_artists && m_artists->isArtistPage(name); }, &weights);
        if (artists.isEmpty()) {
            m_headKey.clear();
        } else {
            int head = -1;
            for (int i = 0; i < artists.size(); ++i) {
                if (artists.at(i).leadKey == m_headKey) {
                    head = i;
                    break;
                }
            }
            if (head < 0 || (head > 0 && weights.at(0) >= kHeadLead * weights.at(head)
                             && at - m_headChangedAt >= kHeadHoldMs)) {
                if (artists.at(0).leadKey != m_headKey) {
                    m_headKey = artists.at(0).leadKey;
                    m_headChangedAt = at;
                }
            } else if (head > 0) {
                artists.move(head, 0);
            }
        }
        for (const Rec::SessionSeed &artist : std::as_const(artists)) {
            QString browseId = artist.artistId;
            if (browseId.isEmpty() && m_artists)
                browseId = m_artists->idFor(artist.artistName);
            candidates.append(QVariantMap{
                { QStringLiteral("name"), artist.artistName },
                { QStringLiteral("browseId"), browseId }
            });
        }
    }

    // Told only when what is shown would change: a loop on a kept song
    // changes nothing.
    QStringList parts;
    for (const QVariant &value : std::as_const(rows)) {
        const QVariantMap row = value.toMap();
        parts << row.value(QStringLiteral("key")).toString() << row.value(QStringLiteral("title")).toString();
        for (const QVariant &card : row.value(QStringLiteral("items")).toList())
            parts << card.toMap().value(QStringLiteral("videoId")).toString();
        parts << QStringLiteral("|");
    }
    parts << QStringLiteral("artists");
    for (const QVariant &value : std::as_const(candidates)) {
        const QVariantMap candidate = value.toMap();
        parts << candidate.value(QStringLiteral("name")).toString()
              << candidate.value(QStringLiteral("browseId")).toString();
    }
    const QString signature = parts.join(QLatin1Char('\n'));
    if (signature == m_signature)
        return;
    m_signature = signature;
    m_rows = rows;
    m_candidates = candidates;
    Q_EMIT rowsChanged();
}

// ------------------------------------------------------------------- probe

int runJustPlayedProbe(Library *library, const QStringList &videoIds)
{
    const auto say = [](const QString &line) {
        std::printf("%s\n", qPrintable(line));
        std::fflush(stdout);
    };
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        say(QStringLiteral("just-played-probe: needs MONOLIST_DATA_DIR (a scratch folder)"));
        return 1;
    }
    QStringList ids;
    for (const QString &id : videoIds) {
        if (!validVideoId(id)) {
            say(QStringLiteral("just-played-probe: not a video id: %1").arg(id));
            return 1;
        }
        if (ids.size() < 5 && !ids.contains(id))
            ids << id;
    }
    if (ids.isEmpty()) {
        say(QStringLiteral("just-played-probe: give 1 to 5 video ids"));
        return 1;
    }
    const bool jar = library->settingValue(kJarKey) == QLatin1String("1");
    const bool hideExplicit = library->settingValue(QStringLiteral("rec.hideExplicit")) == QLatin1String("1");
    say(QStringLiteral("just-played-probe: %1 ids, 20 s apart, signed out, %2")
            .arg(ids.size())
            .arg(jar ? QStringLiteral("shared anonymous jar") : QStringLiteral("cookieless")));

    // The country the app would ask for (main.cpp sets it after the flags).
    InnerTube::setRegion(library->settingValue(QStringLiteral("region")));
    InnerTube tube(nullptr, /*warmUp=*/false);
    tube.setCookieless(!jar);
    int good = 0;
    for (qsizetype i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            QEventLoop pause;
            QTimer::singleShot(20000, &pause, &QEventLoop::quit);
            pause.exec();
        }
        const QString id = ids.at(i);
        QEventLoop loop;
        QList<InnerTube::Track> tracks;
        QString error;
        tube.watchPlaylist({ id, QStringLiteral("RDAMVM") + id, QStringLiteral("wAEB") },
                           [&](const QList<InnerTube::Track> &found, const QString &why) {
                               tracks = found;
                               error = why;
                               loop.quit();
                           },
                           /*retries=*/0);
        loop.exec();

        QVector<Rec::RadioTrack> radio;
        for (const InnerTube::Track &track : std::as_const(tracks))
            radio.append(fromTrack(track));
        // The seed as the radio names it, first.
        QVector<Rec::SessionSeed> seeds;
        QString seedLine = id;
        for (const Rec::RadioTrack &track : std::as_const(radio)) {
            if (track.videoId != id)
                continue;
            Rec::SessionSeed seed;
            seed.videoId = id;
            seed.title = track.title;
            seed.artist = track.artist;
            seed.leadKey = Rec::radioLeadKey(track);
            seeds.append(seed);
            seedLine = QStringLiteral("%1 \"%2\" by %3").arg(id, track.title, track.artist);
            break;
        }
        Rec::SessionOptions options;
        options.hideExplicit = hideExplicit;
        const QVector<Rec::RadioTrack> usable = Rec::eligibleItems(radio, seeds, options, nullptr);
        QStringList leads;
        QSet<QString> leadKeys;
        for (const Rec::RadioTrack &track : std::as_const(radio)) {
            if (track.videoId == id)
                continue;
            const QString key = Rec::radioLeadKey(track);
            if (key.isEmpty() || leadKeys.contains(key))
                continue;
            leadKeys.insert(key);
            leads << Rec::leadArtistName(track.primaryArtist.isEmpty() ? track.artist : track.primaryArtist);
        }
        if (usable.size() >= 12)
            ++good;
        say(QStringLiteral("just-played-probe: %1").arg(seedLine));
        say(QStringLiteral("just-played-probe:   tracks %1, usable %2, distinct lead artists %3%4")
                .arg(radio.size())
                .arg(usable.size())
                .arg(leadKeys.size())
                .arg(error.isEmpty() ? QString() : QStringLiteral(" (") + error.left(160) + QLatin1Char(')')));
        say(QStringLiteral("just-played-probe:   artists: %1").arg(leads.mid(0, 12).join(QStringLiteral(", "))));
        QStringList firstFew;
        for (qsizetype k = 0; k < qMin<qsizetype>(5, usable.size()); ++k)
            firstFew << usable.at(k).title + QStringLiteral(" / ") + usable.at(k).artist;
        say(QStringLiteral("just-played-probe:   first usable: %1").arg(firstFew.join(QStringLiteral("; "))));
    }
    const int needed = int((ids.size() * 4 + 4) / 5);
    say(QStringLiteral("just-played-probe: %1 of %2 gave 12 or more usable songs (pass needs %3)")
            .arg(good)
            .arg(ids.size())
            .arg(needed));
    return good >= needed ? 0 : 1;
}
