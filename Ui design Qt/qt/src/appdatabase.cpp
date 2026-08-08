#include "appdatabase.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {
const char *kConnectionName = "phono_local";
}

AppDatabase::AppDatabase() = default;

QString AppDatabase::databaseFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("phono.db"));
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
        qWarning("Phono: cannot open database: %s", qPrintable(db.lastError().text()));
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
        " artwork TEXT NOT NULL DEFAULT '',"
        " favourite INTEGER NOT NULL DEFAULT 0)"));
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
}

void AppDatabase::seedSampleDataIfEmpty()
{
    QSqlDatabase db = connection();
    QSqlQuery count(db);

    count.exec(QStringLiteral("SELECT COUNT(*) FROM playlists"));
    if (count.next() && count.value(0).toInt() == 0) {
        const QList<QPair<QString, int>> playlists = {
            { QStringLiteral("Studio Monitors"), 34 },
            { QStringLiteral("Concrete & Glass"), 58 },
            { QStringLiteral("Night Drive 03"), 21 },
            { QString::fromUtf8("Archiv — Tape A"), 46 },
            { QStringLiteral("Rotterdam Mixes"), 17 },
            { QStringLiteral("Field Recordings"), 63 },
            { QStringLiteral("Red Line Radio"), 29 }
        };
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO playlists (position, name, track_count) VALUES (?, ?, ?)"));
        for (int i = 0; i < playlists.size(); ++i) {
            insert.addBindValue(i);
            insert.addBindValue(playlists.at(i).first);
            insert.addBindValue(playlists.at(i).second);
            insert.exec();
        }
    }

    count.exec(QStringLiteral("SELECT COUNT(*) FROM albums"));
    if (count.next() && count.value(0).toInt() == 0) {
        struct AlbumSeed { const char *title; const char *artist; const char *year; };
        const QList<AlbumSeed> albums = {
            { "Blueprint", "Mira Volt", "2026" },
            { "Terminal West", "The Cantilevers", "2025" },
            { "Signalfarbe", "Neu Maschine", "2026" },
            { "Grid City", "Otto Frey Ensemble", "2024" }
        };
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO albums (position, title, artist, year, format, artwork)"
            " VALUES (?, ?, ?, ?, 'LP', '')"));
        for (int i = 0; i < albums.size(); ++i) {
            insert.addBindValue(i);
            insert.addBindValue(QString::fromUtf8(albums.at(i).title));
            insert.addBindValue(QString::fromUtf8(albums.at(i).artist));
            insert.addBindValue(QString::fromUtf8(albums.at(i).year));
            insert.exec();
        }
    }

    count.exec(QStringLiteral("SELECT COUNT(*) FROM featured"));
    if (count.next() && count.value(0).toInt() == 0) {
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO featured (kicker, title_line1, title_line2, meta, album_id, active)"
            " VALUES (?, ?, ?, ?, (SELECT id FROM albums WHERE title = 'Signalfarbe'), 1)"));
        insert.addBindValue(QString::fromUtf8("NEW ALBUM — OUT NOW"));
        insert.addBindValue(QString::fromUtf8("Neu Maschine"));
        insert.addBindValue(QString::fromUtf8("— Signalfarbe"));
        insert.addBindValue(QString::fromUtf8("11 tracks · 47 min"));
        insert.exec();
    }

    count.exec(QStringLiteral("SELECT COUNT(*) FROM tracks"));
    if (count.next() && count.value(0).toInt() == 0) {
        struct TrackSeed { const char *title; const char *artist; const char *album; int ms; };
        const QList<TrackSeed> tracks = {
            { "Rot / 07", "Neu Maschine", "Signalfarbe", 227000 },
            { "Elevation Study", "Mira Volt", "Blueprint", 252000 },
            { "Westbound Platform", "The Cantilevers", "Terminal West", 178000 },
            { "Béton Brut", "Otto Frey Ensemble", "Grid City", 304000 },
            { "Interval (Loop II)", "Mira Volt", "Blueprint", 201000 },
            { "Schwarzweiß", "Neu Maschine", "Signalfarbe", 280000 }
        };
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral(
            "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, artwork, favourite)"
            " VALUES (?, ?, ?, ?, ?, '', '', 0)"));
        for (int i = 0; i < tracks.size(); ++i) {
            insert.addBindValue(i);
            insert.addBindValue(QString::fromUtf8(tracks.at(i).title));
            insert.addBindValue(QString::fromUtf8(tracks.at(i).artist));
            insert.addBindValue(QString::fromUtf8(tracks.at(i).album));
            insert.addBindValue(tracks.at(i).ms);
            insert.exec();
        }
    }
}
