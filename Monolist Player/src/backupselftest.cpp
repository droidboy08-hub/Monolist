#include "backupselftest.h"

#include "appdatabase.h"
#include "drivesync.h"
#include "innertube.h"
#include "library.h"
#include "librarybackup.h"
#include "recommender.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

#include <functional>

namespace {

using Data = LibraryBackup::Data;
using Song = LibraryBackup::Song;
using Playlist = LibraryBackup::Playlist;
using Saved = LibraryBackup::Saved;
using Play = LibraryBackup::Play;
using Removed = LibraryBackup::Removed;
using Counts = LibraryBackup::Counts;

// One line per check, and a count at the end, as in the other self-tests.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("backup-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    bool same(const QString &got, const QString &want, const QString &what)
    {
        return check(got == want, what, QStringLiteral("got \"%1\", want \"%2\"").arg(got, want));
    }

    bool same(qint64 got, qint64 want, const QString &what)
    {
        return check(got == want, what, QStringLiteral("got %1, want %2").arg(got).arg(want));
    }

    void note(const QString &text) { qWarning("backup-test: %s", qPrintable(text)); }

    int finish()
    {
        qWarning("backup-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs)
            return false;
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

QSqlQuery run(const QString &sql, const QVariantList &binds = {})
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(sql);
    for (const QVariant &bind : binds)
        query.addBindValue(bind);
    if (!query.exec())
        qWarning("backup-test: the database refused a statement: %s", qPrintable(query.lastError().text()));
    return query;
}

QVariant one(const QString &sql, const QVariantList &binds = {})
{
    QSqlQuery query = run(sql, binds);
    return query.next() ? query.value(0) : QVariant();
}

qint64 rows(const QString &table, const QString &where = QString(), const QVariantList &binds = {})
{
    return one(QStringLiteral("SELECT COUNT(*) FROM %1%2").arg(table, where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + where),
               binds).toLongLong();
}

// Invented times, all on one day, a minute apart: t(10) is 10:10:00 UTC.
QString t(int minute)
{
    return QStringLiteral("2026-03-01 10:%1:00").arg(minute, 2, 10, QLatin1Char('0'));
}

Song song(const QString &id, const QString &at, const QString &title = QString())
{
    Song made;
    made.videoId = id;
    made.title = title.isEmpty() ? QStringLiteral("Selftest ") + id : title;
    made.artist = QStringLiteral("Selftest");
    made.artwork = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id);
    made.durationMs = 180000;
    made.at = at;
    return made;
}

QVariantMap track(const QString &id, const QString &title = QString())
{
    return {
        { QStringLiteral("sourceId"), id },
        { QStringLiteral("title"), title.isEmpty() ? QStringLiteral("Selftest ") + id : title },
        { QStringLiteral("artist"), QStringLiteral("Selftest") },
        { QStringLiteral("durationMs"), 180000 }
    };
}

Play play(const QString &kind, const QString &videoId, const QString &startedAt)
{
    Play made;
    made.kind = kind;
    made.videoId = videoId;
    made.title = QStringLiteral("Selftest ") + (videoId.isEmpty() ? QStringLiteral("song") : videoId);
    made.artist = QStringLiteral("Selftest");
    made.startedAt = startedAt;
    made.trackMs = 180000;
    made.source = QStringLiteral("search");
    return made;
}

Saved saved(const QString &browseId, const QString &at, const QString &title = QString())
{
    Saved made;
    made.browseId = browseId;
    made.title = title.isEmpty() ? QStringLiteral("Selftest ") + browseId : title;
    made.artist = QStringLiteral("Selftest");
    made.year = QStringLiteral("2020");
    made.format = QStringLiteral("ALBUM");
    made.artwork = QStringLiteral("https://lh3.googleusercontent.com/selftest-%1").arg(browseId);
    made.savedAt = at;
    return made;
}

void insertPlay(const Play &p)
{
    run(QStringLiteral("INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms, listened_ms,"
                       " completed, skipped, label, source, repeat_in_session) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
        { p.kind, AppDatabase::text(p.videoId), AppDatabase::text(p.title), AppDatabase::text(p.artist), p.startedAt,
          p.trackMs, p.listenedMs, p.completed ? 1 : 0, p.skipped ? 1 : 0, p.label, AppDatabase::text(p.source),
          p.repeatInSession });
}

void tombstone(const QString &kind, const QString &key, const QString &at)
{
    run(QStringLiteral("INSERT OR REPLACE INTO sync_removed (kind, key, at) VALUES (?, ?, ?)"),
        { kind, AppDatabase::text(key), at });
}

QString tombstoneAt(const QString &kind, const QString &key)
{
    return one(QStringLiteral("SELECT at FROM sync_removed WHERE kind = ? AND key = ?"),
               { kind, AppDatabase::text(key) }).toString();
}

// Everything a backup holds, and the device's own things the tests add,
// gone: a library as a new computer has it.
void emptyLibrary(Library *library)
{
    for (const QString &table : { QStringLiteral("playlist_tracks"), QStringLiteral("playlists"),
                                  QStringLiteral("tracks"), QStringLiteral("albums"), QStringLiteral("play_events"),
                                  QStringLiteral("recent"), QStringLiteral("history"), QStringLiteral("sync_removed"),
                                  QStringLiteral("downloads"), QStringLiteral("ytm_tracks"),
                                  QStringLiteral("ytm_playlists"), QStringLiteral("recent_places"),
                                  QStringLiteral("import_jobs") }) {
        run(QStringLiteral("DELETE FROM %1").arg(table));
    }
    library->reloadAll();
}

int playlistIdOf(const QString &uid)
{
    return one(QStringLiteral("SELECT id FROM playlists WHERE uid = ?"), { uid }).toInt();
}

// A playlist made the way the app makes one, then given the uid and times
// a test needs, its songs added at the times given.
int makePlaylist(Library *library, const QString &uid, const QString &name, const QString &updatedAt,
                 const QList<Song> &entries)
{
    const int id = library->createPlaylist(name);
    QVariantList tracks;
    for (const Song &entry : entries)
        tracks.append(track(entry.videoId, entry.title));
    if (!tracks.isEmpty())
        library->addAllToPlaylist(id, tracks);
    for (const Song &entry : entries) {
        run(QStringLiteral("UPDATE playlist_tracks SET added_at = ? WHERE playlist_id = ? AND video_id = ?"),
            { entry.at, id, entry.videoId });
    }
    run(QStringLiteral("UPDATE playlists SET uid = ?, created_at = ?, updated_at = ? WHERE id = ?"),
        { uid, t(1), updatedAt, id });
    return id;
}

QString entriesOf(int playlistId)
{
    QStringList ids;
    QSqlQuery query = run(QStringLiteral("SELECT video_id FROM playlist_tracks WHERE playlist_id = ?"
                                         " ORDER BY position ASC, id ASC"), { playlistId });
    while (query.next())
        ids << query.value(0).toString();
    return ids.join(QLatin1Char(' '));
}

// A library through the file and back: what a merge is handed is always
// what parse() makes of what toJson() wrote.
bool viaFile(Checks &checks, const Data &data, Data *parsed)
{
    QString error;
    const bool ok = LibraryBackup::parse(LibraryBackup::toJson(data), parsed, &error);
    if (!ok)
        checks.check(false, QStringLiteral("an invented library is read back"), error);
    return ok;
}

bool mergeData(Checks &checks, LibraryBackup &backup, const Data &data, Counts *counts)
{
    Data parsed;
    if (!viaFile(checks, data, &parsed))
        return false;
    QString error;
    const bool ok = backup.merge(parsed, counts, &error);
    if (!ok)
        checks.check(false, QStringLiteral("a merge is applied"), error);
    return ok;
}

QString scratchPath(const QString &name)
{
    return QDir(QFileInfo(AppDatabase::databaseFilePath()).absolutePath()).filePath(name);
}

// ------------------------------------------------------------- the library

void testLibraryWrites(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- what the library keeps for a backup"));
    emptyLibrary(library);

    checks.check(one(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('playlists') WHERE name = 'uid'")).toInt() == 1,
                 QStringLiteral("playlists have a uid column"));
    checks.check(one(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sync_removed'")).toInt() == 1,
                 QStringLiteral("the tombstones' table is there"));

    int edits = 0;
    const QMetaObject::Connection watch = QObject::connect(library, &Library::edited, [&edits]() { ++edits; });

    const int first = library->createPlaylist(QStringLiteral("Selftest one"));
    checks.same(edits, 1, QStringLiteral("creating a playlist is an edit"));
    const int second = library->createPlaylist(QStringLiteral("Selftest two"));
    const QString uid = one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { first }).toString();
    const QString otherUid = one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { second }).toString();
    checks.check(!QUuid::fromString(uid).isNull() && uid != otherUid, QStringLiteral("each new playlist has a uid of its own"),
                 uid + QStringLiteral(" / ") + otherUid);

    run(QStringLiteral("UPDATE playlists SET uid = '' WHERE id = ?"), { second });
    AppDatabase().migrate();
    const QString given = one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { second }).toString();
    checks.check(!QUuid::fromString(given).isNull(), QStringLiteral("a playlist from before uids is given one at launch"), given);
    checks.same(one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { first }).toString(), uid,
                QStringLiteral("  and one that has a uid keeps it"));

    // Every change to a playlist's content moves its updated_at.
    const QString old = QStringLiteral("2000-01-01 00:00:00");
    const auto bumped = [&](const QString &what, const std::function<void()> &change) {
        run(QStringLiteral("UPDATE playlists SET updated_at = ? WHERE id = ?"), { old, first });
        const int before = edits;
        change();
        const QString now = one(QStringLiteral("SELECT updated_at FROM playlists WHERE id = ?"), { first }).toString();
        checks.check(now > old, what + QStringLiteral(" moves the playlist's updated_at"), now);
        checks.check(edits > before, QStringLiteral("  and is an edit"));
    };
    bumped(QStringLiteral("a rename"), [&]() { library->renamePlaylist(first, QStringLiteral("Selftest renamed")); });
    bumped(QStringLiteral("a song added"), [&]() { library->addToPlaylist(first, track(QStringLiteral("w1"))); });
    bumped(QStringLiteral("songs added together"), [&]() {
        library->addAllToPlaylist(first, { track(QStringLiteral("w2")), track(QStringLiteral("w3")) });
    });
    bumped(QStringLiteral("songs imported"), [&]() { library->addImported(first, { track(QStringLiteral("w4")) }); });
    const auto entryOf = [&](const QString &videoId) {
        return one(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = ?"),
                   { first, videoId }).toInt();
    };
    bumped(QStringLiteral("a song moved"), [&]() { library->movePlaylistEntry(first, entryOf(QStringLiteral("w3")), 0); });
    bumped(QStringLiteral("a song taken out"), [&]() { library->removeFromPlaylist(first, entryOf(QStringLiteral("w2"))); });
    bumped(QStringLiteral("a pin"), [&]() { library->setPlaylistPinned(first, true); });

    // A song taken out of a playlist is kept as taken out, by playlist and
    // song: another computer's copy that still has it must not put it back.
    const QString w2Key = LibraryBackup::entryKey(uid, QStringLiteral("w2"));
    checks.check(!tombstoneAt(LibraryBackup::kRemovedPlaylistEntry, w2Key).isEmpty(),
                 QStringLiteral("a song taken out of a playlist is kept as taken out"), w2Key);
    library->addToPlaylist(first, track(QStringLiteral("w2")));
    const QString w2Out = tombstoneAt(LibraryBackup::kRemovedPlaylistEntry, w2Key);
    const QString w2In = one(QStringLiteral("SELECT added_at FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'w2'"),
                             { first }).toString();
    checks.check(w2Out.isEmpty() || w2In > w2Out, QStringLiteral("  put back at once, it is newer than its taking out"),
                 w2In + QStringLiteral(" / ") + w2Out);
    library->removeFromLibrary(track(QStringLiteral("w4")));
    checks.check(!tombstoneAt(LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::entryKey(uid, QStringLiteral("w4"))).isEmpty(),
                 QStringLiteral("  as is one Remove from library takes out of every playlist"));

    int before = edits;
    library->setSetting(QStringLiteral("selftest.setting"), QStringLiteral("1"));
    checks.same(edits, before, QStringLiteral("a setting is not an edit"));
    library->movePlaylist(second, 0);
    checks.same(edits, before, QStringLiteral("the sidebar's order is not an edit (it is this computer's)"));

    const QString deletedUid = one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { second }).toString();
    library->deletePlaylist(second);
    checks.check(!tombstoneAt(LibraryBackup::kRemovedPlaylist, deletedUid).isEmpty(),
                 QStringLiteral("deleting a playlist keeps its uid as taken out"));
    checks.check(edits > before, QStringLiteral("  and is an edit"));

    // An unlike is kept even when nothing is known of the song but its id.
    library->setLiked({ { QStringLiteral("sourceId"), QStringLiteral("nameless") } }, true);
    const qint64 events = rows(QStringLiteral("play_events"));
    library->setLiked({ { QStringLiteral("sourceId"), QStringLiteral("nameless") } }, false);
    checks.check(!tombstoneAt(LibraryBackup::kRemovedLike, QStringLiteral("nameless")).isEmpty(),
                 QStringLiteral("an unlike of a song with no title is kept as taken out"));
    checks.same(rows(QStringLiteral("play_events")), events, QStringLiteral("  (the recommender records nothing for it)"));

    // Liked, unliked and liked again at once: the like has to outlive its
    // tombstone, or the next merge would unlike it.
    library->setLiked(track(QStringLiteral("again")), true);
    library->setLiked(track(QStringLiteral("again")), false);
    library->setLiked(track(QStringLiteral("again")), true);
    const QString likedAt = one(QStringLiteral("SELECT liked_at FROM tracks WHERE source_id = 'again'")).toString();
    const QString unlikedAt = tombstoneAt(LibraryBackup::kRemovedLike, QStringLiteral("again"));
    checks.check(unlikedAt.isEmpty() || likedAt > unlikedAt,
                 QStringLiteral("a like put back at once is newer than its unlike"), likedAt + QStringLiteral(" / ") + unlikedAt);

    before = edits;
    QVariantMap page{ { QStringLiteral("browseId"), QStringLiteral("MPREb_selftest_w") },
                      { QStringLiteral("type"), QStringLiteral("album") },
                      { QStringLiteral("title"), QStringLiteral("Selftest album") },
                      { QStringLiteral("subtitle"), QStringLiteral("Album \u2022 Selftest \u2022 2020") } };
    library->setSaved(page, true);
    library->setSaved(page, false);
    checks.check(!tombstoneAt(LibraryBackup::kRemovedSaved, QStringLiteral("MPREb_selftest_w")).isEmpty(),
                 QStringLiteral("an unsave is kept as taken out"));
    checks.check(edits >= before + 2, QStringLiteral("  and a save and an unsave are edits"));

    run(QStringLiteral("INSERT INTO recent (video_id, title) VALUES ('hist1', 'Selftest hist1')"));
    library->reloadHistory();
    library->removeFromHistory(QStringLiteral("hist1"));
    checks.check(!tombstoneAt(LibraryBackup::kRemovedHistory, QStringLiteral("hist1")).isEmpty(),
                 QStringLiteral("a song taken out of History is kept as taken out"));
    library->clearHistory();
    checks.check(!tombstoneAt(LibraryBackup::kRemovedHistoryCleared, QString()).isEmpty(),
                 QStringLiteral("clearing History is kept, with when"));
    QObject::disconnect(watch);
}

// Playlists from before uids: given one at launch that comes from the row
// itself, so a library put back from a daily copy made before the column,
// or copied to another computer before, gets the same ones again, and the
// merge does not take each playlist for a new one.
void testLegacyUids(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- playlists from before uids"));
    emptyLibrary(library);
    const int a = library->createPlaylist(QStringLiteral("Selftest legacy A"));
    const int b = library->createPlaylist(QStringLiteral("Selftest legacy B"));
    run(QStringLiteral("UPDATE playlists SET created_at = ? WHERE id = ?"), { t(1), a });
    run(QStringLiteral("UPDATE playlists SET created_at = ? WHERE id = ?"), { t(2), b });
    const auto uidOf = [](int id) { return one(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"), { id }).toString(); };
    const auto hasUids = []() {
        return one(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('playlists') WHERE name = 'uid'")).toInt() == 1;
    };

    // The library as a Monolist from before this one left it.
    run(QStringLiteral("ALTER TABLE playlists DROP COLUMN uid"));
    if (!checks.check(!hasUids(), QStringLiteral("a library from before uids (the column taken away)")))
        return;
    AppDatabase().migrate();
    const QString uidA = uidOf(a);
    const QString uidB = uidOf(b);
    checks.check(hasUids() && !QUuid::fromString(uidA).isNull() && !QUuid::fromString(uidB).isNull() && uidA != uidB,
                 QStringLiteral("at launch each playlist is given a uid of its own"), uidA + QStringLiteral(" / ") + uidB);

    // The same rows again, as a daily copy from before put back after damage
    // has them, or a copy of the library on another computer.
    run(QStringLiteral("UPDATE playlists SET uid = ''"));
    AppDatabase().migrate();
    checks.check(uidOf(a) == uidA && uidOf(b) == uidB, QStringLiteral("  and the same rows are given the same ones again"),
                 uidOf(a) + QStringLiteral(" / ") + uidOf(b));

    // The daily copy taken at that launch came before the column: another is
    // taken once it is there, so the copy that would be put back has them.
    const QFileInfoList copies = QDir(QFileInfo(AppDatabase::databaseFilePath()).absolutePath())
                                     .entryInfoList({ QStringLiteral("monolist.backup-*.db") }, QDir::Files,
                                                    QDir::Name | QDir::Reversed);
    QString copiedA;
    if (!copies.isEmpty()) {
        const QString name = QStringLiteral("selftest_daily_copy");
        {
            QSqlDatabase copy = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            copy.setDatabaseName(copies.first().filePath());
            copy.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (copy.open()) {
                QSqlQuery read(copy);
                read.prepare(QStringLiteral("SELECT uid FROM playlists WHERE id = ?"));
                read.addBindValue(a);
                if (read.exec() && read.next())
                    copiedA = read.value(0).toString();
            }
            copy.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    checks.check(!copiedA.isEmpty() && copiedA == uidA,
                 QStringLiteral("the daily copy is taken again once the column is there, with the uids"),
                 copies.isEmpty() ? QStringLiteral("no daily copy") : copiedA);
}

void testTurnDownUndo(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- a turn-down taken back"));
    emptyLibrary(library);
    Recommender recommender;
    int changes = 0;
    QObject::connect(&recommender, &Recommender::turnDownsChanged, [&changes]() { ++changes; });
    recommender.notInterested(track(QStringLiteral("nope1")));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("kind = 'notInterested'")), 1,
                QStringLiteral("Not interested is recorded"));
    const QString startedAt = one(QStringLiteral("SELECT started_at FROM play_events WHERE kind = 'notInterested'")).toString();
    const QString key = LibraryBackup::turnDownKey(QStringLiteral("notInterested"), QStringLiteral("nope1"),
                                                   QStringLiteral("Selftest nope1"), QStringLiteral("Selftest"), startedAt);
    recommender.undoNotInterested();
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("kind = 'notInterested'")), 0,
                QStringLiteral("Undo takes it back"));
    checks.check(!tombstoneAt(LibraryBackup::kRemovedTurnDown, key).isEmpty(),
                 QStringLiteral("  and keeps it as taken out, by song and time"), key);
    checks.same(changes, 2, QStringLiteral("both are told (turnDownsChanged)"));
}

// ------------------------------------------------------- the file, whole

void testRoundTrip(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- a library exported, emptied and brought back"));
    emptyLibrary(library);

    const int road = makePlaylist(library, QStringLiteral("selftest-road"), QStringLiteral("Selftest Road"), t(30),
                                  { song(QStringLiteral("a1"), t(2)), song(QStringLiteral("a2"), t(3)),
                                    song(QStringLiteral("a3"), t(4)) });
    const int pinned = makePlaylist(library, QStringLiteral("selftest-pinned"), QStringLiteral("Selftest Pinned"), t(31),
                                    { song(QStringLiteral("b1"), t(5)) });
    run(QStringLiteral("UPDATE playlists SET pinned = 1 WHERE id = ?"), { pinned });
    for (const QString &id : { QStringLiteral("l1"), QStringLiteral("l2"), QStringLiteral("l3") })
        library->setLiked(track(id), true);
    run(QStringLiteral("UPDATE tracks SET liked_at = ? WHERE source_id = 'l1'"), { t(11) });
    run(QStringLiteral("UPDATE tracks SET liked_at = ? WHERE source_id = 'l2'"), { t(12) });
    run(QStringLiteral("UPDATE tracks SET liked_at = ? WHERE source_id = 'l3'"), { t(13) });
    library->setSaved({ { QStringLiteral("browseId"), QStringLiteral("MPREb_selftest1") },
                        { QStringLiteral("type"), QStringLiteral("album") },
                        { QStringLiteral("title"), QStringLiteral("Selftest Album") },
                        { QStringLiteral("subtitle"), QStringLiteral("Album \u2022 Selftest \u2022 2020") } }, true);
    library->setSaved({ { QStringLiteral("browseId"), QStringLiteral("VLPLselftest") },
                        { QStringLiteral("type"), QStringLiteral("playlist") },
                        { QStringLiteral("title"), QStringLiteral("Selftest Mix") },
                        { QStringLiteral("subtitle"), QStringLiteral("Playlist \u2022 Selftest") } }, true);
    run(QStringLiteral("UPDATE albums SET saved_at = ? WHERE browse_id = 'MPREb_selftest1'"), { t(14) });
    Play heard = play(QStringLiteral("play"), QStringLiteral("a1"), t(20));
    heard.listenedMs = 180000;
    heard.completed = true;
    heard.label = 1.0;
    insertPlay(heard);
    Play skipped = play(QStringLiteral("play"), QStringLiteral("a2"), t(21));
    skipped.listenedMs = 4000;
    skipped.skipped = true;
    skipped.label = 0.0;
    insertPlay(skipped);
    insertPlay(play(QStringLiteral("play"), QStringLiteral("a1"), t(22)));   // still going: no label
    Play halfway = play(QStringLiteral("play"), QStringLiteral("b1"), t(23));
    halfway.label = 0.6;
    halfway.repeatInSession = 1;
    insertPlay(halfway);
    insertPlay(play(QStringLiteral("notInterested"), QStringLiteral("n1"), t(24)));
    Play artist = play(QStringLiteral("notInterestedArtist"), QString(), t(25));
    artist.title.clear();
    artist.artist = QStringLiteral("Selftest Band");
    insertPlay(artist);
    tombstone(LibraryBackup::kRemovedLike, QStringLiteral("gone1"), t(26));

    // This computer's own things, none of which may leave it.
    const QString privatePath = QStringLiteral("C:\\selftest-private\\Music\\l2.opus");
    run(QStringLiteral("UPDATE tracks SET source_url = ? WHERE source_id = 'l2'"), { privatePath });
    run(QStringLiteral("UPDATE tracks SET artwork = 'file:///C:/selftest-private/cover.jpg' WHERE source_id = 'l3'"));
    run(QStringLiteral("INSERT INTO downloads (video_id, title, file_path) VALUES ('dlonly1', 'Selftest download', ?)"),
        { QStringLiteral("C:\\selftest-private\\Music\\dlonly1.opus") });
    run(QStringLiteral("INSERT INTO tracks (title, artist, source_url, source_id, favourite)"
                       " VALUES ('Selftest download', 'Selftest', ?, 'dlonly1', 0)"),
        { QStringLiteral("C:\\selftest-private\\Music\\dlonly1.opus") });
    library->setSetting(QStringLiteral("profile.name"), QStringLiteral("Selftest Person Name"));
    library->setSetting(QStringLiteral("selftest.secretish"), QStringLiteral("selftest-setting-value"));
    run(QStringLiteral("INSERT INTO ytm_tracks (list, video_id, title) VALUES ('liked', 'ytmSelftestVid', 'Selftest ytm')"));
    run(QStringLiteral("INSERT INTO ytm_playlists (browse_id, title) VALUES ('VLytmSelftestList', 'Selftest ytm list')"));
    run(QStringLiteral("INSERT INTO recent_places (kind, ref, title) VALUES ('page', 'selftest-place-ref', 'Selftest place')"));
    run(QStringLiteral("INSERT INTO import_jobs (kind, url, name) VALUES ('list', 'https://selftest-import.invalid/x', 'Selftest import')"));
    library->reloadAll();

    const QString path = scratchPath(QStringLiteral("selftest-roundtrip.json"));
    QFile::remove(path);
    Data written;
    QString error;
    checks.check(backup.exportTo(path, &written, &error), QStringLiteral("the library is exported"), error);
    QFile file(path);
    checks.check(file.open(QIODevice::ReadOnly), QStringLiteral("  the file can be read"));
    const QByteArray bytes = file.readAll();
    file.close();
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    checks.same(root.value(QStringLiteral("format")).toString(), QStringLiteral("monolist-library"),
                QStringLiteral("the file says what it is"));
    checks.same(root.value(QStringLiteral("version")).toInt(), 1, QStringLiteral("  and its version"));
    checks.check(root.value(QStringLiteral("writtenAt")).toString().endsWith(QLatin1Char('Z')),
                 QStringLiteral("  its times are ISO-8601 UTC"), root.value(QStringLiteral("writtenAt")).toString());
    {
        // Nothing that names the computer: the host name is often the
        // person's own, and the file goes to their Drive and wherever they
        // put an export.
        const QByteArray host = QSysInfo::machineHostName().toUtf8();
        checks.check(!root.contains(QStringLiteral("device")) && (host.size() < 3 || !bytes.contains(host)),
                     QStringLiteral("  and it names no computer"));
    }
    checks.same(written.playlists.size(), 2, QStringLiteral("two playlists in it"));
    checks.same(written.likes.size(), 3, QStringLiteral("three liked songs in it (not the download alone)"));
    checks.same(written.saved.size(), 2, QStringLiteral("an album and a playlist saved"));
    checks.same(written.removed.size(), 1, QStringLiteral("the tombstone"));
    const qint64 plays = rows(QStringLiteral("play_events"));
    checks.same(written.plays.size(), plays, QStringLiteral("every play, like and turn-down"));
    const qint64 listens = rows(QStringLiteral("play_events"), QStringLiteral("kind = 'play'"));

    // Nothing of this computer's.
    for (const QString &mine : { QStringLiteral("selftest-private"), QStringLiteral("dlonly1"),
                                 QStringLiteral("Selftest Person Name"), QStringLiteral("selftest-setting-value"),
                                 QStringLiteral("ytmSelftestVid"), QStringLiteral("VLytmSelftestList"),
                                 QStringLiteral("selftest-place-ref"), QStringLiteral("selftest-import.invalid"),
                                 QStringLiteral("file:"), QStringLiteral("\"downloads\""), QStringLiteral("\"settings\""),
                                 QStringLiteral("profile.name"), QStringLiteral("source_url") }) {
        checks.check(!bytes.contains(mine.toUtf8()), QStringLiteral("nothing device-specific in the file: ") + mine);
    }
    const QString l3Artwork = [&]() {
        for (const Song &like : std::as_const(written.likes)) {
            if (like.videoId == QLatin1String("l3"))
                return like.artwork;
        }
        return QStringLiteral("(missing)");
    }();
    checks.same(l3Artwork, QString(), QStringLiteral("a cover that is a file here goes as none"));

    const QByteArray before = LibraryBackup::contentHash(written);
    emptyLibrary(library);
    checks.same(library->playlists()->rowCount(), 0, QStringLiteral("the library is emptied"));
    Counts counts;
    checks.check(backup.importFrom(path, &counts, &error), QStringLiteral("the file is brought back in"), error);
    checks.same(counts.playlistsAdded, 2, QStringLiteral("  two playlists added"));
    checks.same(counts.likesAdded, 3, QStringLiteral("  three liked songs added"));
    checks.same(counts.savedAdded, 2, QStringLiteral("  two saved"));
    checks.same(counts.playsAdded, listens, QStringLiteral("  every play"));
    checks.same(counts.eventsAdded, plays - listens, QStringLiteral("  every like and turn-down"));
    checks.same(counts.tombstonesAdded, 1, QStringLiteral("  and the tombstone"));
    checks.check(counts.summary().startsWith(QLatin1String("Imported: 2 playlists, 3 liked songs, 2 saved albums and playlists, ")),
                 QStringLiteral("  said in a line for the toast"), counts.summary());
    checks.same(QString::fromLatin1(LibraryBackup::contentHash(backup.snapshot())), QString::fromLatin1(before),
                QStringLiteral("exported again, it is the same library exactly"));

    checks.same(library->playlists()->rowCount(), 2, QStringLiteral("the sidebar has both playlists"));
    checks.same(library->liked()->rowCount(), 3, QStringLiteral("Liked songs has all three"));
    checks.check(library->isSaved(QStringLiteral("MPREb_selftest1")) && library->isSaved(QStringLiteral("VLPLselftest")),
                 QStringLiteral("both saves are back"));
    checks.same(one(QStringLiteral("SELECT liked_at FROM tracks WHERE source_id = 'l1'")).toString(), t(11),
                QStringLiteral("a like keeps when it was made"));
    const int roadAgain = playlistIdOf(QStringLiteral("selftest-road"));
    checks.same(entriesOf(roadAgain), QStringLiteral("a1 a2 a3"), QStringLiteral("a playlist's songs keep their order"));
    checks.same(one(QStringLiteral("SELECT added_at FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'a2'"),
                    { roadAgain }).toString(), t(3), QStringLiteral("  and when each was added"));
    checks.same(one(QStringLiteral("SELECT updated_at FROM playlists WHERE id = ?"), { roadAgain }).toString(), t(30),
                QStringLiteral("  and when it was last changed"));
    checks.check(library->isPlaylistPinned(playlistIdOf(QStringLiteral("selftest-pinned"))),
                 QStringLiteral("a pinned playlist comes back pinned"));
    checks.check(one(QStringLiteral("SELECT label FROM play_events WHERE video_id = 'a1' AND started_at = ?"), { t(22) }).isNull(),
                 QStringLiteral("a play with no label keeps none"));
    checks.same(rows(QStringLiteral("downloads")), 0, QStringLiteral("no download comes with it"));
    checks.same(rows(QStringLiteral("tracks"), QStringLiteral("source_url <> ''")), 0, QStringLiteral("  nor any file"));
    checks.same(one(QStringLiteral("SELECT play_count FROM recent WHERE video_id = 'a1'")).toInt(), 2,
                QStringLiteral("History is made from the plays: a1 twice"));
    checks.same(one(QStringLiteral("SELECT played_at FROM recent WHERE video_id = 'a1'")).toString(), t(22),
                QStringLiteral("  last played at its newest play"));
    checks.same(rows(QStringLiteral("recent")), 3, QStringLiteral("  one row a song played (a1, a2, b1)"));
    Q_UNUSED(road);

    // The same file again changes nothing at all.
    const qint64 events = rows(QStringLiteral("play_events"));
    const qint64 recent = rows(QStringLiteral("recent"));
    const QString counted = one(QStringLiteral("SELECT GROUP_CONCAT(play_count) FROM recent")).toString();
    Counts again;
    checks.check(backup.importFrom(path, &again, &error), QStringLiteral("the same file brought in twice"), error);
    checks.check(!again.changed(), QStringLiteral("  changes nothing the second time"), again.logLine());
    checks.check(again.summary().startsWith(QLatin1String("Nothing new")), QStringLiteral("  and says so"), again.summary());
    checks.same(rows(QStringLiteral("play_events")), events, QStringLiteral("  no play added"));
    checks.same(rows(QStringLiteral("recent")), recent, QStringLiteral("  History as it was"));
    checks.same(one(QStringLiteral("SELECT GROUP_CONCAT(play_count) FROM recent")).toString(), counted,
                QStringLiteral("  its counts as they were"));
    checks.same(QString::fromLatin1(LibraryBackup::contentHash(backup.snapshot())), QString::fromLatin1(before),
                QStringLiteral("  the library the same"));
    QFile::remove(path);
}

// ----------------------------------------------------------- merge rules

void testLikes(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- likes against unlikes, both ways"));
    emptyLibrary(library);
    const QString path = QStringLiteral("C:\\selftest-private\\k8.opus");
    for (const QString &id : { QStringLiteral("k1"), QStringLiteral("k4"), QStringLiteral("k5"), QStringLiteral("k7"),
                               QStringLiteral("k9") })
        library->setLiked(track(id), true);
    run(QStringLiteral("UPDATE tracks SET liked_at = ? WHERE source_id IN ('k1', 'k5', 'k7', 'k9')"), { t(10) });
    run(QStringLiteral("UPDATE tracks SET liked_at = ? WHERE source_id = 'k4'"), { t(20) });
    run(QStringLiteral("UPDATE tracks SET source_url = ? WHERE source_id = 'k9'"), { path });
    run(QStringLiteral("INSERT INTO tracks (title, artist, source_url, source_id, favourite) VALUES ('Selftest k8', 'Selftest', ?, 'k8', 0)"),
        { path });
    run(QStringLiteral("DELETE FROM sync_removed"));
    tombstone(LibraryBackup::kRemovedLike, QStringLiteral("k2"), t(20));
    tombstone(LibraryBackup::kRemovedLike, QStringLiteral("k3"), t(10));
    library->reloadAll();
    const qint64 events = rows(QStringLiteral("play_events"));

    Data incoming;
    incoming.removed = { { LibraryBackup::kRemovedLike, QStringLiteral("k1"), t(20) },
                         { LibraryBackup::kRemovedLike, QStringLiteral("k4"), t(10) },
                         { LibraryBackup::kRemovedLike, QStringLiteral("k5"), t(10) },
                         { LibraryBackup::kRemovedLike, QStringLiteral("k9"), t(20) } };
    incoming.likes = { song(QStringLiteral("k2"), t(10)), song(QStringLiteral("k3"), t(20)),
                       song(QStringLiteral("k6"), t(15)), song(QStringLiteral("k7"), t(30), QStringLiteral("Selftest k7 (newer)")),
                       song(QStringLiteral("k8"), t(16)) };
    Counts counts;
    mergeData(checks, backup, incoming, &counts);
    checks.check(!library->isLiked(QStringLiteral("k1")), QStringLiteral("a like here, unliked later there: unliked"));
    checks.same(rows(QStringLiteral("tracks"), QStringLiteral("source_id = 'k1'")), 0, QStringLiteral("  its row gone, as an unlike leaves it"));
    checks.check(!library->isLiked(QStringLiteral("k2")), QStringLiteral("a like there, unliked later here: stays unliked"));
    checks.check(library->isLiked(QStringLiteral("k3")), QStringLiteral("a like there later than the unlike here: liked"));
    checks.same(one(QStringLiteral("SELECT liked_at FROM tracks WHERE source_id = 'k3'")).toString(), t(20),
                QStringLiteral("  with its own time"));
    checks.check(library->isLiked(QStringLiteral("k4")), QStringLiteral("a like here later than the unlike there: kept"));
    checks.check(!library->isLiked(QStringLiteral("k5")), QStringLiteral("a like and an unlike in the same second: the unlike"));
    checks.check(library->isLiked(QStringLiteral("k6")), QStringLiteral("a like new to this library is added"));
    checks.same(one(QStringLiteral("SELECT liked_at FROM tracks WHERE source_id = 'k6'")).toString(), t(15),
                QStringLiteral("  keeping when it was liked"));
    checks.same(one(QStringLiteral("SELECT liked_at FROM tracks WHERE source_id = 'k7'")).toString(), t(30),
                QStringLiteral("liked on both: the newer like's time"));
    checks.same(one(QStringLiteral("SELECT title FROM tracks WHERE source_id = 'k7'")).toString(),
                QStringLiteral("Selftest k7 (newer)"), QStringLiteral("  and its details"));
    checks.check(library->isLiked(QStringLiteral("k8")), QStringLiteral("a downloaded song liked there is liked here"));
    checks.same(one(QStringLiteral("SELECT source_url FROM tracks WHERE source_id = 'k8'")).toString(), path,
                QStringLiteral("  and still plays its file"));
    checks.check(!library->isLiked(QStringLiteral("k9")), QStringLiteral("a downloaded song unliked there is unliked here"));
    checks.same(one(QStringLiteral("SELECT source_url FROM tracks WHERE source_id = 'k9'")).toString(), path,
                QStringLiteral("  and keeps its file"));
    checks.same(counts.likesAdded, 3, QStringLiteral("three likes added (k3, k6, k8)"));
    checks.same(counts.likesRemoved, 3, QStringLiteral("three removed (k1, k5, k9)"));
    checks.same(rows(QStringLiteral("play_events")), events, QStringLiteral("a merge records no like or unlike of its own"));
    checks.same(library->liked()->rowCount(), 5, QStringLiteral("Liked songs is read again"));

    Counts again;
    mergeData(checks, backup, incoming, &again);
    checks.check(!again.changed(), QStringLiteral("the same likes merged twice change nothing"), again.logLine());
}

void testSaved(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- saved albums and playlists against unsaves"));
    emptyLibrary(library);
    for (const QString &id : { QStringLiteral("s1"), QStringLiteral("s4") }) {
        library->setSaved({ { QStringLiteral("browseId"), id }, { QStringLiteral("type"), QStringLiteral("album") },
                            { QStringLiteral("title"), QStringLiteral("Selftest ") + id },
                            { QStringLiteral("subtitle"), QStringLiteral("Album \u2022 Selftest \u2022 2020") } }, true);
    }
    run(QStringLiteral("UPDATE albums SET saved_at = ? WHERE browse_id = 's1'"), { t(10) });
    run(QStringLiteral("UPDATE albums SET saved_at = ? WHERE browse_id = 's4'"), { t(20) });
    tombstone(LibraryBackup::kRemovedSaved, QStringLiteral("s2"), t(20));
    library->reloadAll();

    Data incoming;
    incoming.removed = { { LibraryBackup::kRemovedSaved, QStringLiteral("s1"), t(20) },
                         { LibraryBackup::kRemovedSaved, QStringLiteral("s4"), t(10) } };
    incoming.saved = { saved(QStringLiteral("s2"), t(10)), saved(QStringLiteral("s3"), t(15)) };
    Counts counts;
    mergeData(checks, backup, incoming, &counts);
    checks.check(!library->isSaved(QStringLiteral("s1")), QStringLiteral("saved here, unsaved later there: unsaved"));
    checks.check(!library->isSaved(QStringLiteral("s2")), QStringLiteral("saved there, unsaved later here: stays unsaved"));
    checks.check(library->isSaved(QStringLiteral("s3")), QStringLiteral("saved there only: saved here"));
    checks.same(one(QStringLiteral("SELECT saved_at FROM albums WHERE browse_id = 's3'")).toString(), t(15),
                QStringLiteral("  keeping when"));
    checks.check(library->isSaved(QStringLiteral("s4")), QStringLiteral("saved here after the unsave there: kept"));
    checks.same(counts.savedAdded, 1, QStringLiteral("one added"));
    checks.same(counts.savedRemoved, 1, QStringLiteral("one removed"));
    Counts again;
    mergeData(checks, backup, incoming, &again);
    checks.check(!again.changed(), QStringLiteral("merged twice: nothing the second time"), again.logLine());
}

void testPlaylists(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- playlists by uid"));
    emptyLibrary(library);
    const int changed = makePlaylist(library, QStringLiteral("u1"), QStringLiteral("Selftest u1"), t(10),
                                     { song(QStringLiteral("x1"), t(1)), song(QStringLiteral("x2"), t(2)),
                                       song(QStringLiteral("x3"), t(3)) });
    const int x1 = one(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x1'"), { changed }).toInt();
    const int x3 = one(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x3'"), { changed }).toInt();
    makePlaylist(library, QStringLiteral("u2"), QStringLiteral("Selftest Mine"), t(30), { song(QStringLiteral("y1"), t(1)) });
    makePlaylist(library, QStringLiteral("u3"), QStringLiteral("Selftest u3"), t(10), { song(QStringLiteral("z1"), t(1)) });
    makePlaylist(library, QStringLiteral("u7"), QStringLiteral("Selftest u7"), t(10), {});
    // Both computers changed these since they last met (the finding's case):
    // a song added on each; a song taken out here, then the other pinned it;
    // a song taken out here and put back there later.
    makePlaylist(library, QStringLiteral("u8"), QStringLiteral("Selftest u8"), t(20),
                 { song(QStringLiteral("c1"), t(1)), song(QStringLiteral("a1"), t(15)) });
    makePlaylist(library, QStringLiteral("u9"), QStringLiteral("Selftest u9"), t(20), { song(QStringLiteral("r1"), t(1)) });
    makePlaylist(library, QStringLiteral("u11"), QStringLiteral("Selftest u11"), t(10), { song(QStringLiteral("e1"), t(1)) });
    run(QStringLiteral("DELETE FROM sync_removed"));
    tombstone(LibraryBackup::kRemovedPlaylist, QStringLiteral("u4"), t(20));
    tombstone(LibraryBackup::kRemovedPlaylist, QStringLiteral("u5"), t(20));
    tombstone(LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::entryKey(QStringLiteral("u9"), QStringLiteral("r2")), t(19));
    tombstone(LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::entryKey(QStringLiteral("u11"), QStringLiteral("e2")), t(5));
    library->reloadAll();
    library->openPlaylist(changed);
    const int lastPosition = one(QStringLiteral("SELECT MAX(position) FROM playlists")).toInt();

    const auto list = [](const QString &uid, const QString &name, const QString &updatedAt, const QList<Song> &entries) {
        Playlist made;
        made.uid = uid;
        made.name = name;
        made.createdAt = t(1);
        made.updatedAt = updatedAt;
        made.entries = entries;
        return made;
    };
    Data incoming;
    Playlist renamed = list(QStringLiteral("u1"), QStringLiteral("Selftest Renamed"), t(20),
                            { song(QStringLiteral("x3"), t(7)), song(QStringLiteral("x4"), t(8)),
                              song(QStringLiteral("x1"), t(9)) });
    renamed.pinned = true;
    Playlist pinnedThere = list(QStringLiteral("u9"), QStringLiteral("Selftest u9"), t(30),
                                { song(QStringLiteral("r1"), t(1)), song(QStringLiteral("r2"), t(2)) });
    pinnedThere.pinned = true;
    incoming.playlists = { renamed,
                           list(QStringLiteral("u2"), QStringLiteral("Selftest Theirs"), t(20), {}),
                           list(QStringLiteral("u3"), QStringLiteral("Selftest u3"), t(10), { song(QStringLiteral("z1"), t(1)) }),
                           list(QStringLiteral("u4"), QStringLiteral("Selftest u4"), t(30), { song(QStringLiteral("v1"), t(1)) }),
                           list(QStringLiteral("u5"), QStringLiteral("Selftest u5"), t(10), {}),
                           list(QStringLiteral("u6"), QStringLiteral("Selftest u6"), t(10), { song(QStringLiteral("v2"), t(5)) }),
                           list(QStringLiteral("u8"), QStringLiteral("Selftest u8 there"), t(25),
                                { song(QStringLiteral("c1"), t(1)), song(QStringLiteral("b1"), t(24)) }),
                           pinnedThere,
                           list(QStringLiteral("u11"), QStringLiteral("Selftest u11"), t(9),
                                { song(QStringLiteral("e1"), t(1)), song(QStringLiteral("e2"), t(9)) }) };
    // x2 was taken out of u1 there, after it was added here.
    incoming.removed = { { LibraryBackup::kRemovedPlaylist, QStringLiteral("u3"), t(20) },
                         { LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::entryKey(QStringLiteral("u1"), QStringLiteral("x2")),
                           t(15) } };
    Counts counts;
    mergeData(checks, backup, incoming, &counts);

    checks.same(library->playlistName(changed), QStringLiteral("Selftest Renamed"),
                QStringLiteral("changed later there: its name taken"));
    checks.check(library->isPlaylistPinned(changed), QStringLiteral("  its pin"));
    checks.same(entriesOf(changed), QStringLiteral("x3 x4 x1"), QStringLiteral("  its songs in their order"));
    checks.same(one(QStringLiteral("SELECT added_at FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x4'"),
                    { changed }).toString(), t(8), QStringLiteral("  each with when it was added"));
    checks.same(one(QStringLiteral("SELECT added_at FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x1'"),
                    { changed }).toString(), t(9), QStringLiteral("  a song it had too, with the time it has there"));
    checks.check(one(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x1'"), { changed }).toInt() == x1
                     && one(QStringLiteral("SELECT id FROM playlist_tracks WHERE playlist_id = ? AND video_id = 'x3'"), { changed }).toInt() == x3,
                 QStringLiteral("  the songs it had keep their rows"));
    checks.same(one(QStringLiteral("SELECT updated_at FROM playlists WHERE id = ?"), { changed }).toString(), t(20),
                QStringLiteral("  and its time is the one it came with"));
    checks.same(library->playlistTrackList().size(), 3, QStringLiteral("  the open playlist read again"));
    checks.same(library->playlistName(playlistIdOf(QStringLiteral("u2"))), QStringLiteral("Selftest Mine"),
                QStringLiteral("changed later here: kept as it is"));
    checks.same(playlistIdOf(QStringLiteral("u3")), 0, QStringLiteral("deleted there after its last change: deleted here"));
    checks.same(rows(QStringLiteral("playlist_tracks"), QStringLiteral("video_id = 'z1'")), 0,
                QStringLiteral("  with its songs"));
    checks.check(playlistIdOf(QStringLiteral("u4")) > 0, QStringLiteral("deleted here, then changed there later: back"));
    checks.same(playlistIdOf(QStringLiteral("u5")), 0, QStringLiteral("deleted here after its last change there: stays deleted"));
    const int added = playlistIdOf(QStringLiteral("u6"));
    checks.check(added > 0, QStringLiteral("a playlist new to this library is added"));
    checks.check(one(QStringLiteral("SELECT position FROM playlists WHERE id = ?"), { added }).toInt() > lastPosition,
                 QStringLiteral("  at the end of the sidebar"));
    checks.same(one(QStringLiteral("SELECT added_at FROM playlist_tracks WHERE playlist_id = ?"), { added }).toString(), t(5),
                QStringLiteral("  its songs with when they were added"));
    checks.check(playlistIdOf(QStringLiteral("u7")) > 0, QStringLiteral("one only here, never deleted: kept"));

    // Songs merged as a set, not the newer copy whole.
    const int both = playlistIdOf(QStringLiteral("u8"));
    checks.same(entriesOf(both), QStringLiteral("c1 a1 b1"),
                QStringLiteral("a song added on each computer: both kept, the one from here after the song it followed"));
    checks.same(library->playlistName(both), QStringLiteral("Selftest u8 there"),
                QStringLiteral("  the name the newer copy's"));
    const int pinnedLater = playlistIdOf(QStringLiteral("u9"));
    checks.same(entriesOf(pinnedLater), QStringLiteral("r1"),
                QStringLiteral("a song taken out here, the playlist pinned there later: it stays out"));
    checks.check(library->isPlaylistPinned(pinnedLater), QStringLiteral("  and the pin is taken"));
    checks.same(entriesOf(playlistIdOf(QStringLiteral("u11"))), QStringLiteral("e1 e2"),
                QStringLiteral("a song taken out here, put back there later: back, though this copy is the newer"));
    checks.same(counts.playlistsAdded, 2, QStringLiteral("two added (u4, u6)"));
    checks.same(counts.playlistsUpdated, 4, QStringLiteral("four changed (u1, u8, u9, u11)"));
    checks.same(counts.playlistsRemoved, 1, QStringLiteral("one removed (u3)"));
    checks.same(library->playlists()->rowCount(), 8, QStringLiteral("the sidebar read again"));

    Counts again;
    mergeData(checks, backup, incoming, &again);
    checks.check(!again.changed(), QStringLiteral("merged twice: nothing the second time"), again.logLine());
}

void testPlays(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- plays, each once, and turn-downs taken back"));
    emptyLibrary(library);
    Play started = play(QStringLiteral("play"), QStringLiteral("v1"), t(5));
    started.listenedMs = 10000;
    started.skipped = true;
    insertPlay(started);
    Play labelled = play(QStringLiteral("play"), QStringLiteral("v3"), t(7));
    labelled.label = 0.6;
    labelled.listenedMs = 120000;
    insertPlay(labelled);
    Play turnedDown = play(QStringLiteral("notInterested"), QStringLiteral("v9"), t(8));
    turnedDown.label = 0.0;
    insertPlay(turnedDown);
    const QString keptKey = LibraryBackup::turnDownKey(QStringLiteral("notInterested"), QStringLiteral("v10"),
                                                       QStringLiteral("Selftest v10"), QStringLiteral("Selftest"), t(8));
    tombstone(LibraryBackup::kRemovedTurnDown, keptKey, t(9));
    const qint64 before = rows(QStringLiteral("play_events"));

    Data incoming;
    Play finished = started;
    finished.listenedMs = 175000;
    finished.completed = true;
    finished.skipped = false;
    finished.label = 1.0;
    Play unlabelled = labelled;
    unlabelled.label = QVariant();
    unlabelled.listenedMs = 60000;
    Play other = turnedDown;
    other.videoId = QStringLiteral("v10");
    other.title = QStringLiteral("Selftest v10");
    Play band = play(QStringLiteral("notInterestedArtist"), QString(), t(8));
    band.title.clear();
    band.artist = QStringLiteral("Selftest Band");
    incoming.plays = { finished, unlabelled, play(QStringLiteral("play"), QStringLiteral("v2"), t(6)),
                       play(QStringLiteral("play"), QStringLiteral("v2"), t(6)), other, band };
    incoming.removed = { { LibraryBackup::kRemovedTurnDown,
                           LibraryBackup::turnDownKey(QStringLiteral("notInterested"), QStringLiteral("v9"),
                                                      turnedDown.title, turnedDown.artist, t(8)), t(9) } };
    Counts counts;
    mergeData(checks, backup, incoming, &counts);

    checks.same(one(QStringLiteral("SELECT listened_ms FROM play_events WHERE video_id = 'v1'")).toLongLong(), 175000,
                QStringLiteral("a play on both: the most heard"));
    checks.same(one(QStringLiteral("SELECT completed FROM play_events WHERE video_id = 'v1'")).toInt(), 1,
                QStringLiteral("  completed on either side"));
    checks.same(one(QStringLiteral("SELECT skipped FROM play_events WHERE video_id = 'v1'")).toInt(), 1,
                QStringLiteral("  skipped on either side"));
    checks.same(one(QStringLiteral("SELECT label FROM play_events WHERE video_id = 'v1'")).toString(), QStringLiteral("1"),
                QStringLiteral("  the label one side has"));
    checks.same(one(QStringLiteral("SELECT label FROM play_events WHERE video_id = 'v3'")).toString(), QStringLiteral("0.6"),
                QStringLiteral("  a label kept when the other has none"));
    checks.same(one(QStringLiteral("SELECT listened_ms FROM play_events WHERE video_id = 'v3'")).toLongLong(), 120000,
                QStringLiteral("  and the more heard kept"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("video_id = 'v2'")), 1,
                QStringLiteral("a play new here is added, once"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("video_id = 'v9'")), 0,
                QStringLiteral("a turn-down here taken back there: gone"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("video_id = 'v10'")), 0,
                QStringLiteral("a turn-down there taken back here: not added"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("kind = 'notInterestedArtist' AND artist = 'Selftest Band'")), 1,
                QStringLiteral("a turn-down of an artist is added"));
    checks.same(counts.playsAdded, 1, QStringLiteral("one play added (v2)"));
    checks.same(counts.eventsAdded, 1, QStringLiteral("  and one turn-down (the artist)"));
    checks.same(counts.turnDownsRemoved, 1, QStringLiteral("one turn-down removed"));
    checks.same(rows(QStringLiteral("play_events")), before + 2 - 1, QStringLiteral("and nothing else written"));
    Counts again;
    mergeData(checks, backup, incoming, &again);
    checks.check(!again.changed(), QStringLiteral("merged twice: nothing the second time"), again.logLine());
}

void testHistory(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- History from the plays"));
    emptyLibrary(library);
    run(QStringLiteral("INSERT INTO recent (video_id, title, played_at, play_count) VALUES ('h1', 'Selftest h1', ?, 2)"), { t(5) });
    run(QStringLiteral("INSERT INTO recent (video_id, title, played_at, play_count) VALUES ('h2', 'Selftest h2', ?, 3)"), { t(30) });
    run(QStringLiteral("INSERT INTO recent (video_id, title, played_at, play_count) VALUES ('h6', 'Selftest h6', ?, 1)"), { t(12) });
    library->reloadAll();

    Data incoming;
    incoming.removed = { { LibraryBackup::kRemovedHistoryCleared, QString(), t(10) },
                         { LibraryBackup::kRemovedHistory, QStringLiteral("h5"), t(20) },
                         { LibraryBackup::kRemovedHistory, QStringLiteral("h6"), t(13) } };
    incoming.plays = { play(QStringLiteral("play"), QStringLiteral("h3"), t(8)),
                       play(QStringLiteral("play"), QStringLiteral("h4"), t(12)),
                       play(QStringLiteral("play"), QStringLiteral("h4"), t(14)),
                       play(QStringLiteral("play"), QStringLiteral("h5"), t(15)),
                       play(QStringLiteral("play"), QStringLiteral("h2"), t(40)) };
    incoming.plays[2].title = QStringLiteral("Selftest h4 (latest)");
    Counts counts;
    mergeData(checks, backup, incoming, &counts);
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'h1'")), 0,
                QStringLiteral("History cleared there: what was played here before it goes"));
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'h3'")), 0,
                QStringLiteral("  a play from before the clearing stays out"));
    checks.same(one(QStringLiteral("SELECT play_count FROM recent WHERE video_id = 'h4'")).toInt(), 2,
                QStringLiteral("plays after it come into History, counted"));
    checks.same(one(QStringLiteral("SELECT played_at FROM recent WHERE video_id = 'h4'")).toString(), t(14),
                QStringLiteral("  at the newest"));
    checks.same(one(QStringLiteral("SELECT title FROM recent WHERE video_id = 'h4'")).toString(),
                QStringLiteral("Selftest h4 (latest)"), QStringLiteral("  named as the newest play names it"));
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'h5'")), 0,
                QStringLiteral("a song taken out of History there stays out for its plays before"));
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'h6'")), 0,
                QStringLiteral("  and goes from History here"));
    checks.same(one(QStringLiteral("SELECT play_count FROM recent WHERE video_id = 'h2'")).toInt(), 4,
                QStringLiteral("a song played here and there: one more play"));
    checks.same(one(QStringLiteral("SELECT played_at FROM recent WHERE video_id = 'h2'")).toString(), t(40),
                QStringLiteral("  last played at the later"));
    checks.same(library->history()->rowCount(), 2, QStringLiteral("History read again (h2, h4)"));
    checks.same(counts.historyRemoved, 2, QStringLiteral("  two rows taken out of History, counted (h1, h6)"));

    // A clearing already applied here is not applied again at every merge:
    // a row in History here that it would cover (written by a clock a little
    // behind, say) stays.
    run(QStringLiteral("INSERT INTO recent (video_id, title, played_at, play_count) VALUES ('h8', 'Selftest h8', ?, 1)"), { t(9) });
    Counts again;
    mergeData(checks, backup, incoming, &again);
    checks.check(!again.changed(), QStringLiteral("merged twice: nothing the second time"), again.logLine());
    checks.same(one(QStringLiteral("SELECT play_count FROM recent WHERE video_id = 'h4'")).toInt(), 2,
                QStringLiteral("  History not counted twice"));
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'h8'")), 1,
                QStringLiteral("  nor cleared twice"));
}

// Two libraries with every kind of disagreement, merged one way and the
// other: the same library either way, which is what keeps two devices from
// handing the Drive file back and forth for ever.
void testConverge(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- two libraries merged either way round"));
    Data a;
    Data b;
    a.likes = { song(QStringLiteral("c1"), t(10)), song(QStringLiteral("c2"), t(10), QStringLiteral("Selftest c2 A")),
                song(QStringLiteral("c3"), t(10)) };
    b.likes = { song(QStringLiteral("c1"), t(12)), song(QStringLiteral("c2"), t(10), QStringLiteral("Selftest c2 B")),
                song(QStringLiteral("c4"), t(10)) };
    b.removed = { { LibraryBackup::kRemovedLike, QStringLiteral("c3"), t(11) },
                  { LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::entryKey(QStringLiteral("cu2"), QStringLiteral("f2")), t(19) } };
    Playlist one;
    one.uid = QStringLiteral("cu1");
    one.name = QStringLiteral("Selftest same time A");
    one.updatedAt = t(20);
    one.entries = { song(QStringLiteral("e1"), t(1)) };
    Playlist two = one;
    two.name = QStringLiteral("Selftest same time B");
    two.entries = { song(QStringLiteral("e2"), t(1)), song(QStringLiteral("e1"), t(2)) };
    // A playlist both changed: a song added on A, another on B, one taken
    // out on B. The songs as a set, either way round.
    Playlist three;
    three.uid = QStringLiteral("cu2");
    three.name = QStringLiteral("Selftest both changed");
    three.updatedAt = t(20);
    three.entries = { song(QStringLiteral("f1"), t(1)), song(QStringLiteral("f2"), t(2)), song(QStringLiteral("f4"), t(5)) };
    Playlist four = three;
    four.updatedAt = t(21);
    four.entries = { song(QStringLiteral("f1"), t(1)), song(QStringLiteral("f3"), t(3)) };
    a.playlists = { one, three };
    b.playlists = { two, four };
    Play p = play(QStringLiteral("play"), QStringLiteral("c1"), t(30));
    p.listenedMs = 1000;
    Play q = p;
    q.listenedMs = 90000;
    q.label = 0.6;
    a.plays = { p, play(QStringLiteral("play"), QStringLiteral("c5"), t(31)) };
    b.plays = { q };
    a.saved = { saved(QStringLiteral("cs1"), t(10)) };
    b.removed.append({ LibraryBackup::kRemovedSaved, QStringLiteral("cs1"), t(10) });
    a.removed = { { LibraryBackup::kRemovedHistoryCleared, QString(), t(5) } };

    emptyLibrary(library);
    mergeData(checks, backup, a, nullptr);
    mergeData(checks, backup, b, nullptr);
    const QByteArray ab = LibraryBackup::contentHash(backup.snapshot());
    emptyLibrary(library);
    mergeData(checks, backup, b, nullptr);
    mergeData(checks, backup, a, nullptr);
    const QByteArray ba = LibraryBackup::contentHash(backup.snapshot());
    checks.same(QString::fromLatin1(ab), QString::fromLatin1(ba), QStringLiteral("A then B is B then A"));
    checks.same(library->playlistName(playlistIdOf(QStringLiteral("cu1"))), QStringLiteral("Selftest same time B"),
                QStringLiteral("  two playlists changed in the same second: one chosen, the same both ways"));
    checks.check(!library->isLiked(QStringLiteral("c3")) && library->isLiked(QStringLiteral("c4")),
                 QStringLiteral("  likes and unlikes from both"));
    checks.check(!library->isSaved(QStringLiteral("cs1")), QStringLiteral("  a save and its unsave in one second: unsaved"));
    checks.same(entriesOf(playlistIdOf(QStringLiteral("cu2"))), QStringLiteral("f1 f4 f3"),
                QStringLiteral("  a playlist both changed: both songs added kept, the one taken out gone"));

    // The merged library is what either side then writes: merging it into
    // either changes nothing more.
    Data merged = backup.snapshot();
    Counts settled;
    mergeData(checks, backup, merged, &settled);
    checks.check(!settled.changed(), QStringLiteral("  and it is settled: merging it again changes nothing"), settled.logLine());
}

// Times from a clock that ran ahead. None may win a comparison, and none is
// moved to now (a play's time is what makes it itself): what a file holds
// dated later than now is left out until it is due, and what this library
// holds so dated decides nothing and goes in no file.
void testFutureTimes(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- times later than now"));
    emptyLibrary(library);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto at = [&now](qint64 seconds) {
        return now.addSecs(seconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    };
    const QString farAhead = QStringLiteral("2099-01-01 00:00:00");
    const QString hourAhead = at(3600);
    const QString soon = at(4 * 60);   // within the ten minutes two right clocks can be apart
    const QString past = at(-60);

    // Played here a minute ago, and liked now.
    run(QStringLiteral("INSERT INTO recent (video_id, title, played_at, play_count) VALUES ('fh1', 'Selftest fh1', ?, 1)"),
        { past });
    library->setLiked(track(QStringLiteral("fl1")), true);
    library->reloadAll();

    Data incoming;
    incoming.removed = { { LibraryBackup::kRemovedHistoryCleared, QString(), farAhead },
                         { LibraryBackup::kRemovedLike, QStringLiteral("fl1"), hourAhead } };
    incoming.likes = { song(QStringLiteral("fl2"), farAhead), song(QStringLiteral("fl3"), soon),
                       song(QStringLiteral("fl4"), past) };
    Playlist ahead;
    ahead.uid = QStringLiteral("fu1");
    ahead.name = QStringLiteral("Selftest ahead");
    ahead.createdAt = past;
    ahead.updatedAt = farAhead;
    ahead.entries = { song(QStringLiteral("fe1"), past) };
    Playlist sane = ahead;
    sane.uid = QStringLiteral("fu2");
    sane.updatedAt = past;
    sane.entries = { song(QStringLiteral("fe2"), past), song(QStringLiteral("fe3"), hourAhead) };
    incoming.playlists = { ahead, sane };
    incoming.saved = { saved(QStringLiteral("fs1"), farAhead), saved(QStringLiteral("fs2"), past) };
    incoming.plays = { play(QStringLiteral("play"), QStringLiteral("fp1"), hourAhead),
                       play(QStringLiteral("play"), QStringLiteral("fp2"), past) };
    const QByteArray file = LibraryBackup::toJson(incoming);

    Data parsed;
    QString error;
    checks.check(LibraryBackup::parse(file, &parsed, &error), QStringLiteral("a file holding times later than now is read"),
                 error);
    checks.same(parsed.leftOut, 7, QStringLiteral("  what is dated later than now is left out, and counted"));
    Counts counts;
    checks.check(backup.merge(parsed, &counts, &error), QStringLiteral("  and merged"), error);
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'fh1'")), 1,
                QStringLiteral("a Clear history dated 2099 empties no History"));
    checks.check(library->isLiked(QStringLiteral("fl1")),
                 QStringLiteral("an unlike dated an hour ahead undoes no like made since"));
    checks.check(!library->isLiked(QStringLiteral("fl2")) && library->isLiked(QStringLiteral("fl3"))
                     && library->isLiked(QStringLiteral("fl4")),
                 QStringLiteral("a like dated 2099 is left out; one minutes ahead (two right clocks apart) is taken"));
    checks.same(playlistIdOf(QStringLiteral("fu1")), 0,
                QStringLiteral("a playlist changed in 2099 is left out: it would be the newer copy at every merge"));
    checks.same(entriesOf(playlistIdOf(QStringLiteral("fu2"))), QStringLiteral("fe2"),
                QStringLiteral("  a song added an hour ahead is left out of its playlist"));
    checks.check(!library->isSaved(QStringLiteral("fs1")) && library->isSaved(QStringLiteral("fs2")),
                 QStringLiteral("a save dated 2099 is left out"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("video_id = 'fp1'")), 0,
                QStringLiteral("a play an hour ahead is left out, not moved to now"));
    checks.same(rows(QStringLiteral("play_events"), QStringLiteral("video_id = 'fp2'")), 1,
                QStringLiteral("  one a minute ago is taken"));
    checks.check(tombstoneAt(LibraryBackup::kRemovedHistoryCleared, QString()).isEmpty()
                     && tombstoneAt(LibraryBackup::kRemovedLike, QStringLiteral("fl1")).isEmpty(),
                 QStringLiteral("no tombstone dated later than now is kept"));
    Counts twice;
    checks.check(backup.merge(parsed, &twice, &error) && !twice.changed(),
                 QStringLiteral("the same file again changes nothing"), twice.logLine());

    // Import says what it left out, and why.
    const QString path = scratchPath(QStringLiteral("selftest-future.json"));
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(file);
        out.close();
    }
    Counts imported;
    checks.check(backup.importFrom(path, &imported, &error) && imported.futureLeftOut == 7,
                 QStringLiteral("Import counts what it left out"), error);
    checks.same(imported.summary(),
                QStringLiteral("Nothing was brought in: 7 things in that file are dated later than now (that computer's "
                               "clock may be wrong)"),
                QStringLiteral("  and the toast says so, and what to look at"));
    QFile::remove(path);

    // This library's own clock ran ahead once: what it wrote then decides
    // nothing here, and goes in no file.
    tombstone(LibraryBackup::kRemovedHistoryCleared, QString(), farAhead);
    tombstone(LibraryBackup::kRemovedLike, QStringLiteral("fl5"), farAhead);
    Data elsewhere;
    elsewhere.likes = { song(QStringLiteral("fl5"), past) };
    elsewhere.plays = { play(QStringLiteral("play"), QStringLiteral("fp3"), past) };
    mergeData(checks, backup, elsewhere, nullptr);
    checks.check(library->isLiked(QStringLiteral("fl5")),
                 QStringLiteral("an unlike dated 2099 here undoes no like from another computer"));
    checks.same(rows(QStringLiteral("recent"), QStringLiteral("video_id = 'fp3'")), 1,
                QStringLiteral("a Clear history dated 2099 here keeps no play from another computer out of History"));
    bool written = false;
    for (const Removed &removed : backup.snapshot().removed)
        written = written || removed.at > LibraryBackup::latestTrueTime();
    checks.check(!written, QStringLiteral("  and neither goes in a file this library writes"));
    library->clearHistory();
    checks.check(tombstoneAt(LibraryBackup::kRemovedHistoryCleared, QString()) <= LibraryBackup::latestTrueTime(),
                 QStringLiteral("Clear history now replaces one dated 2099"),
                 tombstoneAt(LibraryBackup::kRemovedHistoryCleared, QString()));
    Data sound;
    sound.removed = { { LibraryBackup::kRemovedLike, QStringLiteral("fl5"), past } };
    mergeData(checks, backup, sound, nullptr);
    checks.same(tombstoneAt(LibraryBackup::kRemovedLike, QStringLiteral("fl5")), past,
                QStringLiteral("one dated 2099 here gives way to another computer's that can be true"));
    Counts settled;
    mergeData(checks, backup, sound, &settled);
    checks.check(!settled.changed(), QStringLiteral("  once, then nothing changes"), settled.logLine());
}

// What a toast says a merge did: what was added, what only changed, what
// was taken out, History among it, and what was left out.
void testSummaries(Checks &checks)
{
    checks.note(QStringLiteral("- what the toast says"));
    Counts changed;
    changed.likesAdded = 3;
    changed.playlistsUpdated = 2;
    checks.same(changed.summary(), QStringLiteral("Imported: 3 liked songs added; 2 playlists changed"),
                QStringLiteral("a playlist that only changed is not called added"));
    Counts cleared;
    cleared.historyRemoved = 120;
    cleared.tombstonesAdded = 1;
    checks.same(cleared.summary(), QStringLiteral("Imported: 120 songs from History removed, as they were deleted since"),
                QStringLiteral("History taken out is named, never \"up to date\""));
    Counts mixed;
    mixed.playlistsAdded = 1;
    mixed.futureLeftOut = 2;
    checks.same(mixed.summary(),
                QStringLiteral("Imported: 1 playlist added; 2 things left out, as they were dated later than now (that "
                               "computer's clock may be wrong)"),
                QStringLiteral("what was left out is said after what came in"));
}

// A long listening history: the file keeps the newest 100,000 plays and
// every turn-down, and a library that size goes out and comes back in
// seconds, in one transaction.
void testLarge(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- a long history"));
    emptyLibrary(library);
    const int total = LibraryBackup::kMaxPlays + 50;
    run(QStringLiteral("INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms, listened_ms)"
                       " WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < ?)"
                       " SELECT 'play', 'big' || i, 'Selftest big ' || i, 'Selftest',"
                       "        datetime('2025-01-01 00:00:00', '+' || i || ' minutes'), 180000, 90000 FROM n"),
        { total });
    for (int i = 0; i < 3; ++i)
        insertPlay(play(QStringLiteral("notInterested"), QStringLiteral("old%1").arg(i), QStringLiteral("2024-01-0%1 00:00:00").arg(i + 1)));
    checks.same(rows(QStringLiteral("play_events")), total + 3, QStringLiteral("a history of more plays than a file keeps"));

    QElapsedTimer clock;
    clock.start();
    const QString path = scratchPath(QStringLiteral("selftest-large.json"));
    Data written;
    QString error;
    checks.check(backup.exportTo(path, &written, &error), QStringLiteral("it is exported"), error);
    const qint64 exportMs = clock.restart();
    checks.same(written.plays.size(), LibraryBackup::kMaxPlays, QStringLiteral("  with 100,000 rows of it"));
    int turnDowns = 0;
    QString oldest;
    for (const Play &p : std::as_const(written.plays)) {
        if (p.kind == QLatin1String("notInterested"))
            ++turnDowns;
        else if (oldest.isEmpty() || p.startedAt < oldest)
            oldest = p.startedAt;
    }
    checks.same(turnDowns, 3, QStringLiteral("  every turn-down, however old"));
    checks.same(oldest, one(QStringLiteral("SELECT started_at FROM play_events WHERE video_id = 'big54'")).toString(),
                QStringLiteral("  and the newest plays: the oldest ones left out"));

    emptyLibrary(library);
    clock.restart();
    Counts counts;
    checks.check(backup.importFrom(path, &counts, &error), QStringLiteral("it is brought back"), error);
    const qint64 importMs = clock.elapsed();
    checks.same(counts.playsAdded, LibraryBackup::kMaxPlays - 3, QStringLiteral("  every play in it"));
    checks.same(counts.eventsAdded, 3, QStringLiteral("  and the turn-downs"));
    checks.note(QStringLiteral("  export %1 ms, import %2 ms (%3 KB)")
                    .arg(exportMs).arg(importMs).arg(QFileInfo(path).size() / 1024));
    checks.check(exportMs + importMs < 60000, QStringLiteral("  in well under a minute"));
    Counts again;
    checks.check(backup.importFrom(path, &again, &error) && !again.changed(),
                 QStringLiteral("  and brought back again, changes nothing"), again.logLine());
    QFile::remove(path);
}

// ------------------------------------------------------------ refusals

void testRefusals(Checks &checks, Library *library, LibraryBackup &backup)
{
    checks.note(QStringLiteral("- files that are not a library this Monolist can read"));
    emptyLibrary(library);
    library->setLiked(track(QStringLiteral("r1")), true);
    const qint64 tracksBefore = rows(QStringLiteral("tracks"));
    const qint64 eventsBefore = rows(QStringLiteral("play_events"));

    const auto refused = [&](const QByteArray &bytes, const QString &what, const QString &expected) {
        Data data;
        QString error;
        const bool ok = LibraryBackup::parse(bytes, &data, &error);
        checks.check(!ok && error.contains(expected), what, error);
        const QString path = scratchPath(QStringLiteral("selftest-refused.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            checks.check(false, QStringLiteral("  a scratch file can be written"), path);
        file.write(bytes);
        file.close();
        Counts counts;
        QString importError;
        checks.check(!backup.importFrom(path, &counts, &importError) && !importError.isEmpty(),
                     QStringLiteral("  and Import refuses it"), importError);
        QFile::remove(path);
    };
    Data newer;
    newer.likes = { song(QStringLiteral("r2"), t(1)) };
    QJsonObject later = QJsonDocument::fromJson(LibraryBackup::toJson(newer)).object();
    later.insert(QStringLiteral("version"), 2);
    refused(QJsonDocument(later).toJson(), QStringLiteral("a newer version is refused"), QStringLiteral("newer Monolist"));
    refused(QByteArrayLiteral("{\"format\":\"something-else\",\"version\":1,\"likes\":[]}"),
            QStringLiteral("JSON that is not a Monolist library is refused"), QStringLiteral("isn't a Monolist library"));
    refused(QByteArrayLiteral("[1, 2, 3]"), QStringLiteral("a JSON list is refused"), QStringLiteral("isn't a Monolist library"));
    refused(QByteArrayLiteral("artist,title\nSelftest,Song\n"), QStringLiteral("a CSV file is refused"),
            QStringLiteral("isn't a Monolist library"));
    refused(QByteArrayLiteral("{\"format\":\"monolist-library\",\"version\":1,\"likes\":{\"a\":1}}"),
            QStringLiteral("a library with a section of the wrong kind is refused"), QStringLiteral("damaged"));
    refused(QByteArrayLiteral("{\"format\":\"monolist-library\"}"),
            QStringLiteral("a library with no version is refused"), QStringLiteral("isn't a Monolist library"));
    checks.same(rows(QStringLiteral("tracks")), tracksBefore, QStringLiteral("nothing in the library changed"));
    checks.same(rows(QStringLiteral("play_events")), eventsBefore, QStringLiteral("  no play written"));

    // Keys it does not know and items it cannot use are passed over.
    Data data;
    QString error;
    const QByteArray lenient = QByteArrayLiteral(
        "\xEF\xBB\xBF{\"format\":\"monolist-library\",\"version\":1,\"futureThing\":{\"x\":1},"
        "\"likes\":[{\"videoId\":\"ok1\",\"likedAt\":\"2026-03-01T10:02:00Z\",\"mood\":\"new key\"},"
        "{\"title\":\"no id\"},7,{\"videoId\":\"ok2\",\"likedAt\":\"2026-03-01T15:31:00+05:30\","
        "\"artwork\":\"C:\\\\Users\\\\x\\\\cover.jpg\"}],"
        "\"plays\":[{\"kind\":\"teleport\",\"startedAt\":\"2026-03-01T10:00:00Z\"},{\"kind\":\"play\",\"videoId\":\"ok1\"}]}");
    checks.check(LibraryBackup::parse(lenient, &data, &error), QStringLiteral("unknown keys and a byte-order mark are passed over"), error);
    checks.same(data.likes.size(), 2, QStringLiteral("  items with no id are left out"));
    checks.same(data.plays.size(), 0, QStringLiteral("  a play of no known kind, or with no time, is left out"));
    QString ok2;
    for (const Song &like : std::as_const(data.likes)) {
        if (like.videoId == QLatin1String("ok2"))
            ok2 = like.at;
    }
    checks.same(ok2, t(1), QStringLiteral("  a time with an offset is read as UTC"));
    bool pathFound = false;
    for (const Song &like : std::as_const(data.likes))
        pathFound = pathFound || !like.artwork.isEmpty();
    checks.check(!pathFound, QStringLiteral("  a cover that is a path is dropped"));
}

// ----------------------------------------------- Export and Import buttons

void testButtons(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- Export and Import as Settings calls them"));
    emptyLibrary(library);
    makePlaylist(library, QStringLiteral("bu1"), QStringLiteral("Selftest button"), t(10), { song(QStringLiteral("q1"), t(1)) });
    library->setLiked(track(QStringLiteral("q2")), true);
    insertPlay(play(QStringLiteral("play"), QStringLiteral("q1"), t(3)));

    DriveSync sync(library);
    QStringList notices;
    QObject::connect(&sync, &DriveSync::notice, [&notices](const QString &text) { notices << text; });
    int edited = 0;
    QObject::connect(&sync, &DriveSync::libraryEdited, [&edited]() { ++edited; });
    QString offered;
    QString answer;
    sync.setFilePicker([&](bool save, const QString &suggested) {
        if (save)
            offered = suggested;
        return answer;
    });

    const QString path = scratchPath(QStringLiteral("selftest-button.json"));
    QFile::remove(path);
    answer = path;
    sync.exportLibrary();
    checks.check(waitUntil([&]() { return !notices.isEmpty(); }, 3000), QStringLiteral("EXPORT says what it saved"));
    checks.same(notices.value(0), QStringLiteral("Saved your library: 1 playlist, 1 liked song, 1 play"),
                QStringLiteral("  in a line for the toast"));
    checks.check(QFileInfo::exists(path), QStringLiteral("  the file is where the dialog said"));
    checks.check(offered.startsWith(QLatin1String("Monolist library ")) && offered.endsWith(QLatin1String(".json")),
                 QStringLiteral("  the name offered is the library's and the day's"), offered);

    notices.clear();
    answer.clear();
    sync.exportLibrary();
    waitUntil([]() { return false; }, 200);
    checks.check(notices.isEmpty(), QStringLiteral("a dialog cancelled does nothing"));

    const QString notLibrary = scratchPath(QStringLiteral("selftest-notalibrary.json"));
    QFile other(notLibrary);
    if (!other.open(QIODevice::WriteOnly | QIODevice::Truncate))
        checks.check(false, QStringLiteral("a scratch file can be written"), notLibrary);
    other.write("{\"playlists\":[]}");
    other.close();
    answer = notLibrary;
    sync.importLibrary();
    checks.check(waitUntil([&]() { return !notices.isEmpty(); }, 3000)
                     && notices.value(0) == QLatin1String("That file isn't a Monolist library, so nothing was brought in."),
                 QStringLiteral("IMPORT of a file that is not a library says so plainly"), notices.value(0));
    checks.same(edited, 0, QStringLiteral("  and nothing is to upload"));

    notices.clear();
    answer = path;
    sync.importLibrary();
    checks.check(waitUntil([&]() { return !notices.isEmpty(); }, 3000)
                     && notices.value(0).startsWith(QLatin1String("Nothing new")),
                 QStringLiteral("IMPORT of the library's own file finds nothing new"), notices.value(0));

    emptyLibrary(library);
    notices.clear();
    sync.importLibrary();
    checks.check(waitUntil([&]() { return !notices.isEmpty(); }, 3000)
                     && notices.value(0) == QLatin1String("Imported: 1 playlist, 1 liked song, 1 play added"),
                 QStringLiteral("IMPORT into an empty library says what came in"), notices.value(0));
    checks.check(edited > 0 && sync.editedSinceSync(), QStringLiteral("  and the Drive backup is told to upload"));
    QFile::remove(path);
    QFile::remove(notLibrary);
}

} // namespace

int runBackupSelfTest(Library *library)
{
    Checks checks;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        checks.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                     QStringLiteral("this test empties the library and runs only in a scratch data folder"));
        return checks.finish();
    }
    // Nothing here goes to the network. The recommender made for the
    // turn-down's undo has an InnerTube of its own, which would warm up its
    // connections to YouTube and fetch a visitor id: pointed at a port on
    // this computer that nothing listens on, it does neither.
    InnerTube::setTestServer(QStringLiteral("http://127.0.0.1:9"));
    LibraryBackup backup(library);
    testLibraryWrites(checks, library);
    testLegacyUids(checks, library);
    testTurnDownUndo(checks, library);
    testRoundTrip(checks, library, backup);
    testLikes(checks, library, backup);
    testSaved(checks, library, backup);
    testPlaylists(checks, library, backup);
    testPlays(checks, library, backup);
    testHistory(checks, library, backup);
    testConverge(checks, library, backup);
    testFutureTimes(checks, library, backup);
    testSummaries(checks);
    testLarge(checks, library, backup);
    testRefusals(checks, library, backup);
    testButtons(checks, library);
    emptyLibrary(library);
    return checks.finish();
}
