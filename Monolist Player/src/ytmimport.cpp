#include "ytmimport.h"

#include "appdatabase.h"
#include "library.h"
#include "ytmsession.h"

#include <QDateTime>
#include <QLocale>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <limits>

namespace {

const QString kEnabledKey = QStringLiteral("ytmusic.import_library");
// When SYNC NOW last started a sync, in UTC ms: its cooldown outlives a
// restart.
const QString kManualKey = QStringLiteral("ytmusic.library_manual_at");
// A table fills this many rows a frame (as Catalog's pages do).
constexpr int kFeedRows = 25;

const QString kLiked = QStringLiteral("liked");
const QString kPlaylists = QStringLiteral("playlists");
const QString kHistory = QStringLiteral("history");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

int randomBelow(int bound)
{
    return bound > 0 ? int(QRandomGenerator::global()->bounded(bound)) : 0;
}

int timerMs(qint64 ms)
{
    return int(std::clamp<qint64>(ms, 0, std::numeric_limits<int>::max()));
}

QString whenText(qint64 ms)
{
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    const QString time = QLocale::system().toString(at.time(), QLocale::ShortFormat);
    if (at.date() == QDate::currentDate())
        return QStringLiteral("today at ") + time;
    if (at.date() == QDate::currentDate().addDays(-1))
        return QStringLiteral("yesterday at ") + time;
    return QLocale::system().toString(at.date(), QLocale::ShortFormat) + QStringLiteral(" at ") + time;
}

SearchResultModel::Item itemOf(const QSqlQuery &q)
{
    SearchResultModel::Item item;
    item.sourceId = q.value(0).toString();
    item.title = q.value(1).toString();
    item.artist = q.value(2).toString();
    item.primaryArtist = q.value(3).toString();
    item.album = q.value(4).toString();
    item.albumId = q.value(5).toString();
    item.artwork = q.value(6).toString();
    item.durationMs = q.value(7).toLongLong();
    item.isVideo = q.value(8).toBool();
    return item;
}

}

// One sync, from its first call to its commit: what has been read so far, in
// memory, written only once every list has been read.
struct YtmImport::Run {
    quint64 id = 0;
    quint64 session = 0;
    QString account;
    bool manual = false;
    int pages = 0;        // every page read in this run, for the longer pauses
    int listPages = 0;    // of the list being read now
    InnerTube::Collection header;   // Liked music's own, to complete its rows
    QSet<QString> keys;
    QList<InnerTube::Track> liked;
    bool likedComplete = true;
    QList<InnerTube::Card> playlists;
    bool playlistsComplete = true;
    QList<QPair<QString, InnerTube::Track>> history;
};

YtmImport::YtmImport(Library *library, YtmSession *session, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_session(session)
{
    m_enabled = !m_library || m_library->settingValue(kEnabledKey) != QLatin1String("0");
    m_autoTimer.setSingleShot(true);
    connect(&m_autoTimer, &QTimer::timeout, this, [this]() { start(false); });
    m_cooldownTimer.setSingleShot(true);
    connect(&m_cooldownTimer, &QTimer::timeout, this, &YtmImport::changed);
    load();
    armCooldown();
    if (m_library) {
        // A playlist of the account's saved here (or no longer): the list,
        // which shows such a one once, with the saved, follows.
        connect(m_library, &Library::revisionChanged, this, [this]() {
            m_playlists.reload();
            Q_EMIT changed();
        });
    }
    if (m_session) {
        // Deleted only when the user signs out: a session that merely could
        // not be read at launch is not a sign-out.
        connect(m_session, &YtmSession::signedOut, this, [this]() {
            if (m_session && !m_session->isDemo())
                forget(QStringLiteral("signed out"));
        });
        connect(m_session, &YtmSession::sessionChanged, this, &YtmImport::sessionChanged);
        // A rest begins or ends: the status says so, and a sync under way
        // stops at its next page.
        connect(&m_session->guard(), &AccountGuard::pausedChanged, this, &YtmImport::changed);
        connect(m_session, &YtmSession::changed, this, &YtmImport::changed);
    }
    sessionChanged();
}

YtmImport::~YtmImport() = default;

void YtmImport::setTiming(const Timing &timing)
{
    m_timing = timing;
    armCooldown();
}

void YtmImport::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (m_library)
        m_library->setSetting(kEnabledKey, enabled ? QStringLiteral("1") : QStringLiteral("0"));
    qInfo("ytmlibrary: importing the account's library %s", enabled ? "on" : "off");
    if (!enabled)
        forget(QStringLiteral("the import is turned off"));
    Q_EMIT enabledChanged();
    sessionChanged();
}

// ---------------------------------------------------------------- what is shown

QString YtmImport::storedAccount() const
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT account FROM ytm_lists WHERE account <> '' LIMIT 1"));
    return q.next() ? q.value(0).toString() : QString();
}

qint64 YtmImport::syncedAt() const
{
    return m_lastSyncedAt;
}

bool YtmImport::shown() const
{
    if (m_demo)
        return m_hasData;
    if (!m_enabled || !m_hasData || !m_session || m_session->isDemo())
        return false;
    // The session ended: what was read before, marked as such.
    if (m_stale)
        return true;
    const QString key = m_session->accountKey();
    return !key.isEmpty() && key == storedAccount();
}

void YtmImport::load()
{
    QList<SearchResultModel::Item> liked;
    QList<SearchResultModel::Item> history;
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT video_id, title, artist, primary_artist, album, album_id, artwork, duration_ms, is_video, list"
        " FROM ytm_tracks ORDER BY list, position"));
    while (q.next()) {
        if (q.value(9).toString() == kLiked)
            liked.append(itemOf(q));
        else
            history.append(itemOf(q));
    }
    m_playlistIds.clear();
    q.exec(QStringLiteral("SELECT browse_id FROM ytm_playlists"));
    while (q.next())
        m_playlistIds.insert(q.value(0).toString());
    m_lastSyncedAt = 0;
    m_lastIncomplete = false;
    int lists = 0;
    q.exec(QStringLiteral("SELECT synced_at, complete FROM ytm_lists"));
    while (q.next()) {
        ++lists;
        m_lastSyncedAt = std::max(m_lastSyncedAt, q.value(0).toLongLong() * 1000);
        m_lastIncomplete = m_lastIncomplete || !q.value(1).toBool();
    }
    m_hasData = lists > 0;

    m_likedAll = liked;
    m_liked.replace(liked.mid(0, kFeedRows));
    m_likedFeed = liked.mid(kFeedRows);
    feedLiked();
    m_history.replace(history);
    m_playlists.reload();
    Q_EMIT changed();
}

// A few rows a frame, so a long list never stops the window while its table
// is made (as Catalog::feedPage).
void YtmImport::feedLiked()
{
    if (m_feedScheduled || m_likedFeed.isEmpty())
        return;
    m_feedScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_feedScheduled = false;
        const QList<SearchResultModel::Item> chunk = m_likedFeed.mid(0, kFeedRows);
        m_likedFeed.remove(0, chunk.size());
        m_liked.append(chunk);
        feedLiked();
    });
}

void YtmImport::forget(const QString &why)
{
    if (m_run)
        stop(QStringLiteral("stopped"), why);
    m_autoTimer.stop();
    const bool had = m_hasData;
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM ytm_tracks"));
    q.exec(QStringLiteral("DELETE FROM ytm_playlists"));
    q.exec(QStringLiteral("DELETE FROM ytm_lists"));
    db.commit();
    m_lastError.clear();
    m_stale = false;
    m_likedFeed.clear();
    load();
    if (had)
        qInfo("ytmlibrary: the imported library is deleted: %s", qPrintable(why));
}

QVariantList YtmImport::likedTrackList() const
{
    QVariantList list;
    list.reserve(m_likedAll.size());
    for (const SearchResultModel::Item &item : m_likedAll)
        list.append(SearchResultModel::toMap(item));
    return list;
}

QVariantList YtmImport::historyTrackList() const
{
    QVariantList list;
    for (int row = 0; row < m_history.rowCount(); ++row)
        list.append(m_history.get(row));
    return list;
}

bool YtmImport::showDemo()
{
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        qWarning("ytmlibrary: the demonstration library is shown only under MONOLIST_DATA_DIR");
        return false;
    }
    m_demo = true;
    m_autoTimer.stop();
    static const char *const kSongs[][3] = {
        { "Blue Room", "The Demo Band", "Paper Lights" }, { "Slow Machine", "Aurel Vance", "Northbound" },
        { "Grey Harbour", "Linnea Holt", "Grey Harbour" }, { "Signal Fires", "The Demo Band", "Paper Lights" },
        { "Low Tide", "Mara & the Keys", "Salt" }, { "Letters", "Aurel Vance", "Northbound" },
        { "Night Bus", "Ottavio Ruiz", "City Hours" }, { "Glass Garden", "Linnea Holt", "Grey Harbour" },
    };
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM ytm_tracks"));
    q.exec(QStringLiteral("DELETE FROM ytm_playlists"));
    q.exec(QStringLiteral("DELETE FROM ytm_lists"));
    q.prepare(QStringLiteral("INSERT INTO ytm_tracks (list, position, section, video_id, title, artist, primary_artist,"
                             " album, duration_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (int i = 0; i < 40; ++i) {
        const auto &song = kSongs[i % 8];
        q.addBindValue(kLiked);
        q.addBindValue(i);
        q.addBindValue(AppDatabase::text(QString()));
        q.addBindValue(QStringLiteral("DEMOLK%1").arg(i, 5, 10, QLatin1Char('0')));
        q.addBindValue(QString::fromLatin1(song[0]));
        q.addBindValue(QString::fromLatin1(song[1]));
        q.addBindValue(QString::fromLatin1(song[1]));
        q.addBindValue(QString::fromLatin1(song[2]));
        q.addBindValue(180000 + i * 7000);
        q.exec();
    }
    for (int i = 0; i < 8; ++i) {
        const auto &song = kSongs[(i + 3) % 8];
        q.addBindValue(kHistory);
        q.addBindValue(i);
        q.addBindValue(i < 4 ? QStringLiteral("Today") : QStringLiteral("Yesterday"));
        q.addBindValue(QStringLiteral("DEMOHS%1").arg(i, 5, 10, QLatin1Char('0')));
        q.addBindValue(QString::fromLatin1(song[0]));
        q.addBindValue(QString::fromLatin1(song[1]));
        q.addBindValue(QString::fromLatin1(song[1]));
        q.addBindValue(QString::fromLatin1(song[2]));
        q.addBindValue(200000);
        q.exec();
    }
    q.prepare(QStringLiteral("INSERT INTO ytm_playlists (browse_id, position, title, subtitle) VALUES (?, ?, ?, ?)"));
    const char *const kDemoPlaylists[][2] = { { "Sunday morning", "Playlist • Demo Listener" },
                                          { "Running, private", "Playlist • Demo Listener" },
                                          { "Late trains", "Playlist • A friend" } };
    for (int i = 0; i < 3; ++i) {
        q.addBindValue(QStringLiteral("VLDEMOPLAYLIST%1").arg(i));
        q.addBindValue(i);
        q.addBindValue(QString::fromUtf8(kDemoPlaylists[i][0]));
        q.addBindValue(QString::fromUtf8(kDemoPlaylists[i][1]));
        q.exec();
    }
    q.prepare(QStringLiteral("INSERT INTO ytm_lists (list, account, synced_at, item_count) VALUES (?, 'demo', ?, ?)"));
    for (const auto &[list, count] : { std::pair{ kLiked, 40 }, std::pair{ kPlaylists, 3 }, std::pair{ kHistory, 8 } }) {
        q.addBindValue(list);
        q.addBindValue(QDateTime::currentSecsSinceEpoch() - 600);
        q.addBindValue(count);
        q.exec();
    }
    db.commit();
    load();
    qInfo("ytmlibrary: showing a demonstration library (invented, nothing asked)");
    return true;
}

bool YtmImport::isAccountPage(const QString &browseId) const
{
    if (!m_enabled || !m_session || m_session->stateValue() != YtmSession::State::Active)
        return false;
    return browseId == QLatin1String("VLLM") || m_playlistIds.contains(browseId);
}

QString YtmImport::countText(int n, const char *one, const char *many)
{
    return QLocale::system().toString(n) + QLatin1Char(' ') + QLatin1String(n == 1 ? one : many);
}

QString YtmImport::status() const
{
    if (!m_enabled)
        return QStringLiteral("Off: nothing is read from your YouTube Music library.");
    if (m_run) {
        QString line = QStringLiteral("Reading your library from YouTube Music, a page at a time");
        if (!m_run->liked.isEmpty())
            line += QStringLiteral(": ") + countText(int(m_run->liked.size()), "liked song", "liked songs") + QStringLiteral(" so far");
        return line + QStringLiteral("…");
    }
    const bool resting = m_session && m_session->resting();
    if (m_stale) {
        return QStringLiteral("Your YouTube Music session has ended, so this is what was read %1. Import a new "
                              "sign-in to sync again.").arg(whenText(m_lastSyncedAt));
    }
    QString line;
    if (m_hasData) {
        line = QStringLiteral("Synced %1: %2, %3 and %4.")
                   .arg(whenText(m_lastSyncedAt), countText(likedCount(), "liked song", "liked songs"),
                        countText(playlistCount(), "playlist", "playlists"),
                        countText(historyCount(), "song of history", "songs of history"));
        if (m_lastIncomplete)
            line += QStringLiteral(" A list longer than Monolist reads at once (5,000 liked songs) is cut there.");
    } else if (m_session && m_session->stateValue() == YtmSession::State::Active) {
        line = QStringLiteral("Not synced yet: it starts a minute or two after the sign-in is confirmed, or with "
                              "SYNC NOW.");
    } else {
        line = QStringLiteral("Read once YouTube Music confirms the sign-in.");
    }
    if (!m_lastError.isEmpty())
        line += QStringLiteral(" The last sync stopped: %1. What was imported is kept.").arg(m_lastError);
    if (resting)
        line += QStringLiteral(" Paused while your account rests.");
    return line;
}

// ---------------------------------------------------------------- when to sync

void YtmImport::sessionChanged()
{
    if (!m_session)
        return;
    // An invented account (--ytm-demo): nothing is read, forgotten or shown
    // of the real library because of it.
    if (m_session->isDemo() && !m_demo) {
        if (m_run)
            stop(QStringLiteral("stopped"), QStringLiteral("a demonstration is shown"));
        m_autoTimer.stop();
        m_stale = false;
        Q_EMIT changed();
        return;
    }
    switch (m_session->stateValue()) {
    case YtmSession::State::SignedOut:
        // Hidden (shown() asks for the account's key, and there is none).
        // Deleted when the user signed out (signedOut); a session that could
        // not be read at launch keeps it for when it can.
        if (m_run)
            stop(QStringLiteral("stopped"), QStringLiteral("signed out"));
        m_autoTimer.stop();
        m_stale = false;
        break;
    case YtmSession::State::Rejected:
        if (m_run)
            stop(QStringLiteral("stopped"), QStringLiteral("the session ended"));
        m_autoTimer.stop();
        m_stale = m_hasData;
        break;
    case YtmSession::State::Checking:
    case YtmSession::State::Unreachable:
        if (m_run)
            stop(QStringLiteral("stopped"), QStringLiteral("the session is being checked"));
        m_autoTimer.stop();
        m_stale = false;
        break;
    case YtmSession::State::Active: {
        m_stale = false;
        const QString key = m_session->accountKey();
        const QString stored = storedAccount();
        // A session with no key yet says nothing of whose it is.
        if (!stored.isEmpty() && !key.isEmpty() && stored != key)
            forget(QStringLiteral("another account is signed in"));
        if (!m_enabled || m_run)
            break;
        const qint64 since = nowMs() - m_lastSyncedAt;
        // Never at the moment the session is confirmed, which is a few
        // seconds into a launch, beside the check's own calls.
        const qint64 settle = m_timing.startDelayMs + randomBelow(m_timing.startJitterMs);
        scheduleAuto(m_lastSyncedAt == 0 || since >= m_timing.autoEveryMs
                         ? settle : std::max<qint64>(settle, m_timing.autoEveryMs - since));
        break;
    }
    }
    Q_EMIT changed();
}

void YtmImport::scheduleAuto(qint64 inMs)
{
    m_autoTimer.start(timerMs(inMs));
    qInfo("ytmlibrary: the account's library is read again in %lld min", (long long)(inMs / 60000));
}

bool YtmImport::canSyncNow() const
{
    if (!m_enabled || m_run || !m_session || m_session->stateValue() != YtmSession::State::Active
        || m_session->resting())
        return false;
    const qint64 last = m_library ? m_library->settingValue(kManualKey).toLongLong() : 0;
    return last <= 0 || nowMs() - last >= m_timing.manualCooldownMs;
}

void YtmImport::syncNow()
{
    if (!m_session || m_session->stateValue() != YtmSession::State::Active) {
        Q_EMIT notice(QStringLiteral("Your library is read once YouTube Music confirms the sign-in."));
        return;
    }
    if (m_session->resting()) {
        Q_EMIT notice(m_session->restLine());
        return;
    }
    if (m_run) {
        Q_EMIT notice(QStringLiteral("Your library is being read already."));
        return;
    }
    const qint64 last = m_library ? m_library->settingValue(kManualKey).toLongLong() : 0;
    if (last > 0 && nowMs() - last < m_timing.manualCooldownMs) {
        const QString at = QLocale::system().toString(
            QDateTime::fromMSecsSinceEpoch(last + m_timing.manualCooldownMs).time(), QLocale::ShortFormat);
        Q_EMIT notice(QStringLiteral("Synced a few minutes ago. To go easy on your account, SYNC NOW works again "
                                     "at %1.").arg(at));
        return;
    }
    if (!m_enabled) {
        Q_EMIT notice(QStringLiteral("Importing your YouTube Music library is off."));
        return;
    }
    if (m_library)
        m_library->setSetting(kManualKey, QString::number(nowMs()));
    armCooldown();
    start(true);
}

void YtmImport::armCooldown()
{
    const qint64 last = m_library ? m_library->settingValue(kManualKey).toLongLong() : 0;
    const qint64 left = last > 0 ? last + m_timing.manualCooldownMs - nowMs() : 0;
    if (left > 0)
        m_cooldownTimer.start(timerMs(left + 500));
    else
        m_cooldownTimer.stop();
}

// ---------------------------------------------------------------- a sync

void YtmImport::start(bool manual)
{
    m_autoTimer.stop();
    if (m_demo || m_run || !m_enabled || !m_session || m_session->stateValue() != YtmSession::State::Active)
        return;
    if (m_session->resting()) {
        // Once the rest is over, and the session checked again: that check
        // confirming it is a sessionChanged only when it was not active, so
        // the next launch's, or SYNC NOW, starts it otherwise.
        qInfo("ytmlibrary: not read now: the account is resting");
        return;
    }
    m_run = std::make_unique<Run>();
    m_run->id = ++m_runs;
    m_run->session = m_session->session();
    m_run->account = m_session->accountKey();
    m_run->manual = manual;
    m_lastError.clear();
    qInfo("ytmlibrary: reading the account's library (%s): liked songs, playlists, history",
          manual ? "asked for" : "on its own");
    Q_EMIT changed();
    readLiked(QString());
}

bool YtmImport::current(quint64 run) const
{
    return m_run && m_run->id == run;
}

void YtmImport::stop(const QString &outcome, const QString &why)
{
    if (!m_run)
        return;
    qInfo("ytmlibrary: the sync stops (%s): nothing is changed", qPrintable(why));
    m_run.reset();
    Q_EMIT changed();
    Q_EMIT synced(outcome);
}

void YtmImport::fail(const QString &why)
{
    if (!m_run)
        return;
    const bool manual = m_run->manual;
    m_lastError = why;
    qInfo("ytmlibrary: the sync failed (%s); what was imported is kept", qPrintable(why));
    m_run.reset();
    if (manual)
        Q_EMIT notice(QStringLiteral("Your YouTube Music library could not be read: %1").arg(why));
    // Tried again on its own a while later, never at once.
    if (m_enabled && m_session && m_session->stateValue() == YtmSession::State::Active)
        scheduleAuto(m_timing.autoEveryMs / 4 + randomBelow(timerMs(m_timing.autoEveryMs / 8)));
    Q_EMIT changed();
    Q_EMIT synced(QStringLiteral("failed"));
}

void YtmImport::after(std::function<void()> next)
{
    if (!m_run)
        return;
    const quint64 run = m_run->id;
    qint64 wait = m_timing.pageGapMs + randomBelow(m_timing.pageJitterMs);
    if (m_timing.restEveryPages > 0 && m_run->pages > 0 && m_run->pages % m_timing.restEveryPages == 0)
        wait += m_timing.restMs + randomBelow(m_timing.restJitterMs);
    QTimer::singleShot(timerMs(wait), this, [this, run, next = std::move(next)]() {
        if (!current(run))
            return;
        // Whatever changed while it waited is checked before the account is
        // asked again.
        if (!m_session || m_session->stateValue() != YtmSession::State::Active
            || m_session->session() != m_run->session) {
            stop(QStringLiteral("stopped"), QStringLiteral("the session changed"));
            return;
        }
        if (m_session->resting()) {
            stop(QStringLiteral("stopped"), QStringLiteral("the account is resting"));
            return;
        }
        next();
    });
}

void YtmImport::readLiked(const QString &token)
{
    const quint64 run = m_run->id;
    const auto answered = [this, run, token](const QJsonObject &root, const QString &error) {
        if (!current(run))
            return;
        if (!error.isEmpty()) {
            fail(QStringLiteral("liked songs: %1").arg(error));
            return;
        }
        const QString loggedIn = InnerTube::parseLoggedIn(root);
        if (loggedIn == QLatin1String("0")) {
            if (m_session)
                m_session->doubt(QStringLiteral("YouTube Music answered the library's liked songs as signed out"));
            fail(QStringLiteral("YouTube Music answered as if signed out"));
            return;
        }
        Run &r = *m_run;
        ++r.pages;
        ++r.listPages;
        QList<InnerTube::Track> tracks;
        QString next;
        if (token.isEmpty()) {
            InnerTube::Collection collection = InnerTube::parseCollection(QStringLiteral("VLLM"), root);
            tracks = collection.tracks;
            next = collection.continuation;
            r.header = collection;
            r.header.tracks.clear();
            // Nothing liked, said by an answer that does not say it is the
            // account's: not believed, and nothing replaced with it.
            if (tracks.isEmpty() && loggedIn != QLatin1String("1")) {
                fail(QStringLiteral("YouTube Music's answer for liked songs held nothing, and did not say whose it was"));
                return;
            }
        } else {
            const InnerTube::Continuation part = InnerTube::parseContinuation(root);
            tracks = part.tracks;
            next = part.next;
        }
        int fresh = 0;
        for (InnerTube::Track track : tracks) {
            const QString key = track.setVideoId.isEmpty() ? track.videoId : track.setVideoId;
            if (key.isEmpty() || r.keys.contains(key))
                continue;
            r.keys.insert(key);
            InnerTube::completeTrack(track, r.header);
            r.liked.append(track);
            ++fresh;
        }
        // A page with nothing new in it is the end, whatever its token says
        // (as Catalog finds with some of YouTube Music's own lists).
        if (fresh == 0)
            next.clear();
        if (!next.isEmpty() && r.listPages >= m_timing.likedPages) {
            r.likedComplete = false;
            next.clear();
        }
        Q_EMIT changed();
        if (!next.isEmpty()) {
            after([this, next]() { readLiked(next); });
        } else {
            r.listPages = 0;
            after([this]() { readPlaylists(QString()); });
        }
    };
    if (token.isEmpty())
        m_innerTube.browse(QStringLiteral("VLLM"), answered, InnerTube::Auth::Required);
    else
        m_innerTube.continueBrowse(token, answered, InnerTube::Auth::Required);
}

void YtmImport::readPlaylists(const QString &token)
{
    const quint64 run = m_run->id;
    const auto answered = [this, run, token](const QJsonObject &root, const QString &error) {
        if (!current(run))
            return;
        if (!error.isEmpty()) {
            fail(QStringLiteral("playlists: %1").arg(error));
            return;
        }
        if (InnerTube::parseLoggedIn(root) == QLatin1String("0")) {
            fail(QStringLiteral("YouTube Music answered as if signed out"));
            return;
        }
        Run &r = *m_run;
        ++r.pages;
        ++r.listPages;
        QList<InnerTube::Card> cards;
        QString next;
        if (token.isEmpty()) {
            const InnerTube::Listing listing = InnerTube::parseListing(root);
            for (const InnerTube::Shelf &shelf : listing.sections)
                cards += shelf.cards;
            next = listing.itemsContinuation;
        } else {
            const InnerTube::Continuation part = InnerTube::parseContinuation(root);
            cards = part.cards;
            for (const InnerTube::Shelf &shelf : part.shelves)
                cards += shelf.cards;
            next = part.next;
        }
        int fresh = 0;
        for (const InnerTube::Card &card : std::as_const(cards)) {
            // The account's own playlists, and the ones it saved: not Liked
            // music (its own list here), nor the Episodes for later list, nor
            // the "New playlist" tile.
            if (card.type != QLatin1String("playlist") || !card.browseId.startsWith(QLatin1String("VL"))
                || card.browseId == QLatin1String("VLLM") || card.browseId == QLatin1String("VLSE"))
                continue;
            bool seen = false;
            for (const InnerTube::Card &kept : std::as_const(r.playlists))
                seen = seen || kept.browseId == card.browseId;
            if (seen)
                continue;
            r.playlists.append(card);
            ++fresh;
        }
        if (fresh == 0 && !token.isEmpty())
            next.clear();
        if (!next.isEmpty() && r.listPages >= m_timing.playlistPages) {
            r.playlistsComplete = false;
            next.clear();
        }
        if (!next.isEmpty())
            after([this, next]() { readPlaylists(next); });
        else
            after([this]() { readHistory(); });
    };
    if (token.isEmpty())
        m_innerTube.browse(QStringLiteral("FEmusic_liked_playlists"), answered, InnerTube::Auth::Required);
    else
        m_innerTube.continueBrowse(token, answered, InnerTube::Auth::Required);
}

void YtmImport::readHistory()
{
    const quint64 run = m_run->id;
    // One call: its first page, which is the last days' listening. The
    // history is kept by YouTube Music, and reading all of it would be many
    // calls for little.
    m_innerTube.browse(QStringLiteral("FEmusic_history"), [this, run](const QJsonObject &root, const QString &error) {
        if (!current(run))
            return;
        if (!error.isEmpty()) {
            fail(QStringLiteral("history: %1").arg(error));
            return;
        }
        if (InnerTube::parseLoggedIn(root) == QLatin1String("0")) {
            fail(QStringLiteral("YouTube Music answered as if signed out"));
            return;
        }
        Run &r = *m_run;
        ++r.pages;
        const InnerTube::Listing listing = InnerTube::parseListing(root);
        for (const InnerTube::Shelf &shelf : listing.sections) {
            for (const InnerTube::Track &track : shelf.songs) {
                if (r.history.size() >= m_timing.historyRows)
                    break;
                if (!track.videoId.isEmpty())
                    r.history.append({ shelf.title, track });
            }
        }
        commit();
    }, InnerTube::Auth::Required);
}

void YtmImport::commit()
{
    Run &r = *m_run;
    const qint64 now = nowMs();
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM ytm_tracks"));
    q.exec(QStringLiteral("DELETE FROM ytm_playlists"));
    q.exec(QStringLiteral("DELETE FROM ytm_lists"));
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO ytm_tracks (list, position, section, video_id, title, artist, primary_artist, album, album_id,"
        " artwork, duration_ms, is_video) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    const auto put = [&insert](const QString &list, int position, const QString &section,
                               const InnerTube::Track &track) {
        insert.addBindValue(list);
        insert.addBindValue(position);
        insert.addBindValue(AppDatabase::text(section));
        insert.addBindValue(AppDatabase::text(track.videoId));
        insert.addBindValue(AppDatabase::text(track.title));
        insert.addBindValue(AppDatabase::text(track.artist));
        insert.addBindValue(AppDatabase::text(track.primaryArtist));
        insert.addBindValue(AppDatabase::text(track.album));
        insert.addBindValue(AppDatabase::text(track.albumId));
        insert.addBindValue(AppDatabase::text(track.artwork));
        insert.addBindValue(track.durationMs);
        insert.addBindValue(track.isVideo ? 1 : 0);
        return insert.exec();
    };
    bool ok = true;
    for (int i = 0; i < r.liked.size(); ++i)
        ok = put(kLiked, i, QString(), r.liked.at(i)) && ok;
    for (int i = 0; i < r.history.size(); ++i)
        ok = put(kHistory, i, r.history.at(i).first, r.history.at(i).second) && ok;
    QSqlQuery playlist(db);
    playlist.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO ytm_playlists (browse_id, position, title, subtitle, artwork) VALUES (?, ?, ?, ?, ?)"));
    for (int i = 0; i < r.playlists.size(); ++i) {
        const InnerTube::Card &card = r.playlists.at(i);
        playlist.addBindValue(card.browseId);
        playlist.addBindValue(i);
        playlist.addBindValue(AppDatabase::text(card.title));
        playlist.addBindValue(AppDatabase::text(card.subtitle));
        playlist.addBindValue(AppDatabase::text(card.artwork));
        ok = playlist.exec() && ok;
    }
    QSqlQuery list(db);
    list.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO ytm_lists (list, account, synced_at, item_count, complete) VALUES (?, ?, ?, ?, ?)"));
    const auto record = [&](const QString &name, int count, bool complete) {
        list.addBindValue(name);
        list.addBindValue(AppDatabase::text(r.account));
        list.addBindValue(now / 1000);
        list.addBindValue(count);
        list.addBindValue(complete ? 1 : 0);
        return list.exec();
    };
    ok = record(kLiked, int(r.liked.size()), r.likedComplete) && ok;
    ok = record(kPlaylists, int(r.playlists.size()), r.playlistsComplete) && ok;
    ok = record(kHistory, int(r.history.size()), true) && ok;
    if (!ok) {
        db.rollback();
        fail(QStringLiteral("it could not be saved: %1").arg(db.lastError().text()));
        return;
    }
    db.commit();
    const bool manual = r.manual;
    const int liked = int(r.liked.size());
    const int playlists = int(r.playlists.size());
    const int history = int(r.history.size());
    const int pages = r.pages;
    m_run.reset();
    m_lastError.clear();
    load();
    m_lastSyncedAt = now;
    qInfo("ytmlibrary: read %d liked songs%s, %d playlists and %d songs of history in %d calls", liked,
          m_lastIncomplete ? " (cut at the cap)" : "", playlists, history, pages);
    if (manual) {
        Q_EMIT notice(QStringLiteral("Your YouTube Music library is synced: %1 and %2.")
                          .arg(countText(liked, "liked song", "liked songs"),
                               countText(playlists, "playlist", "playlists")));
    }
    if (m_enabled && m_session && m_session->stateValue() == YtmSession::State::Active)
        scheduleAuto(m_timing.autoEveryMs + randomBelow(timerMs(m_timing.autoEveryMs / 12)));
    Q_EMIT changed();
    Q_EMIT synced(QStringLiteral("done"));
}
