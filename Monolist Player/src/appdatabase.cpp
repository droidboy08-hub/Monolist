#include "appdatabase.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {
const char *kConnectionName = "monolist_local";
}

AppDatabase::AppDatabase() = default;

QString AppDatabase::databaseFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("monolist.db"));
}

QSqlDatabase AppDatabase::connection()
{
    if (QSqlDatabase::contains(QLatin1String(kConnectionName)))
        return QSqlDatabase::database(QLatin1String(kConnectionName));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kConnectionName));
    db.setDatabaseName(databaseFilePath());
    return db;
}

bool AppDatabase::open()
{
    QSqlDatabase db = connection();
    if (db.isOpen())
        return true;
    if (!db.open()) {
        qWarning("Monolist: cannot open database: %s", qPrintable(db.lastError().text()));
        return false;
    }
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    return true;
}

void AppDatabase::createSchema()
{
    QSqlQuery q(connection());
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlists ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " name TEXT NOT NULL,"
        " track_count INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS albums ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " title TEXT NOT NULL,"
        " artist TEXT NOT NULL,"
        " year TEXT NOT NULL DEFAULT '',"
        " format TEXT NOT NULL DEFAULT 'LP',"
        " artwork TEXT NOT NULL DEFAULT '')"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS tracks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " title TEXT NOT NULL,"
        " artist TEXT NOT NULL,"
        " album TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " source_url TEXT NOT NULL DEFAULT '',"
        " source_id TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " favourite INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_tracks_source_id ON tracks(source_id)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS history ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " track_id INTEGER,"
        " played_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE SET NULL)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS featured ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " kicker TEXT NOT NULL DEFAULT '',"
        " title_line1 TEXT NOT NULL DEFAULT '',"
        " title_line2 TEXT NOT NULL DEFAULT '',"
        " meta TEXT NOT NULL DEFAULT '',"
        " album_id INTEGER,"
        " active INTEGER NOT NULL DEFAULT 1,"
        " FOREIGN KEY(album_id) REFERENCES albums(id) ON DELETE SET NULL)"));
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings ("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL)"));

    // Every song played, library or not, newest first, for "Recently played".
    // Keyed by video id, so replaying a song moves it up instead of repeating it.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS recent ("
        " video_id TEXT PRIMARY KEY,"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " played_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " play_count INTEGER NOT NULL DEFAULT 1)"));

    // Offline set. Keyed by the upstream video id rather than the track row, so
    // a download survives the library being rebuilt and can be matched back to
    // a search result that was never in the library to begin with.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS downloads ("
        " video_id TEXT PRIMARY KEY,"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " file_path TEXT NOT NULL,"
        " bytes INTEGER NOT NULL DEFAULT 0,"
        " downloaded_at TEXT NOT NULL DEFAULT (datetime('now')))"));
}

bool AppDatabase::hasColumn(const QString &table, const QString &column)
{
    QSqlQuery q(connection());
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (q.next()) {
        if (q.value(1).toString().compare(column, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// Runs on every launch. Only additive changes belong here — a music library is
// the user's data, and a schema step that can lose rows is not worth the tidiness.
void AppDatabase::migrate()
{
    QSqlQuery q(connection());

    if (!hasColumn(QStringLiteral("tracks"), QStringLiteral("source_id"))) {
        q.exec(QStringLiteral(
            "ALTER TABLE tracks ADD COLUMN source_id TEXT NOT NULL DEFAULT ''"));
        q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_tracks_source_id ON tracks(source_id)"));
    }

    removeSampleData();

    // history.track_id is ON DELETE SET NULL, so removing a track leaves a row
    // that records only a timestamp and points at nothing. Not guarded by the
    // one-time marker: it stays true whenever a track is deleted.
    q.exec(QStringLiteral("DELETE FROM history WHERE track_id IS NULL"));
}

// The interface prototype seeded invented albums and tracks so the layout had
// something to render. They are not the user's data and there is no real
// content to confuse them with, so they are removed once, on the launch after
// this version lands.
//
// Deliberately narrow: only rows matching the seeded titles exactly, and only
// where nothing real has attached to them — no source id, no local file, not
// favourited. A downloaded track or anything played from a real source is left
// alone even if it happens to share a title.
void AppDatabase::removeSampleData()
{
    QSqlQuery guard(connection());
    guard.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    guard.addBindValue(QStringLiteral("sample_data_removed"));
    if (guard.exec() && guard.next())
        return;                       // already done; never run twice

    static const QStringList seededTracks = {
        QStringLiteral("Rot / 07"),
        QStringLiteral("Elevation Study"),
        QStringLiteral("Westbound Platform"),
        QString::fromUtf8("Béton Brut"),
        QStringLiteral("Interval (Loop II)"),
        QString::fromUtf8("Schwarzweiß")
    };
    static const QStringList seededAlbums = {
        QStringLiteral("Blueprint"),
        QStringLiteral("Terminal West"),
        QStringLiteral("Signalfarbe"),
        QStringLiteral("Grid City")
    };
    static const QStringList seededPlaylists = {
        QStringLiteral("Studio Monitors"),
        QStringLiteral("Concrete & Glass"),
        QStringLiteral("Night Drive 03"),
        QString::fromUtf8("Archiv — Tape A"),
        QStringLiteral("Rotterdam Mixes"),
        QStringLiteral("Field Recordings"),
        QStringLiteral("Red Line Radio")
    };

    QSqlDatabase db = connection();
    db.transaction();

    QSqlQuery track(db);
    track.prepare(QStringLiteral(
        "DELETE FROM tracks WHERE title = ?"
        " AND source_id = '' AND source_url = '' AND favourite = 0"));
    for (const QString &title : seededTracks) {
        track.addBindValue(title);
        track.exec();
    }

    QSqlQuery album(db);
    album.prepare(QStringLiteral("DELETE FROM albums WHERE title = ? AND artwork = ''"));
    for (const QString &title : seededAlbums) {
        album.addBindValue(title);
        album.exec();
    }

    QSqlQuery playlist(db);
    playlist.prepare(QStringLiteral("DELETE FROM playlists WHERE name = ?"));
    for (const QString &name : seededPlaylists) {
        playlist.addBindValue(name);
        playlist.exec();
    }

    // The featured poster referenced a seeded album.
    QSqlQuery featured(db);
    featured.exec(QStringLiteral(
        "DELETE FROM featured WHERE album_id IS NULL"
        " OR album_id NOT IN (SELECT id FROM albums)"));

    QSqlQuery history(db);
    history.exec(QStringLiteral(
        "DELETE FROM history WHERE track_id IS NOT NULL"
        " AND track_id NOT IN (SELECT id FROM tracks)"));

    QSqlQuery mark(db);
    mark.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    mark.addBindValue(QStringLiteral("sample_data_removed"));
    mark.addBindValue(QStringLiteral("1"));
    mark.exec();

    db.commit();
}
