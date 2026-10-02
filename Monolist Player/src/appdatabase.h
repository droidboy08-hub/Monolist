#pragma once

#include <QObject>
#include <QSqlDatabase>

// Owns the local SQLite store: the library (liked and downloaded songs),
// playlists, saved albums, what was played, lyrics found, and settings. The
// interface reads it through Library and the models it owns.
class AppDatabase
{
public:
    AppDatabase();

    bool open();
    void createSchema();
    void migrate();                 // additive column/table changes, safe to re-run

    static QSqlDatabase connection();
    static QString databaseFilePath();

    // What became of a damaged library at this launch, in a sentence for
    // the user; empty when it was sound. open() checks the file (quick_check)
    // before anything reads it: a damaged one is moved aside, whole, into a
    // dated folder beside it, and the day's backup put in its place where
    // there is a sound one; otherwise the library starts afresh.
    static QString recoveryNote();
    // Where the damaged copy was put, in full; empty when nothing was.
    static QString recoveryFolder();

    // For binding to a NOT NULL text column. Qt binds a null QString — what an
    // absent map value or a default-constructed field gives — as SQL NULL, and
    // the column's DEFAULT does not apply to an explicit NULL, so the whole row
    // would be refused.
    static QString text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

private:
    static bool sound(QSqlDatabase &db);
    // A dated copy of a sound library (monolist.backup-yyyyMMdd.db), made at
    // most once a day after the full check, the newest seven kept, for
    // open() to fall back on.
    static void keepBackup(QSqlDatabase &db);
    static bool hasColumn(const QString &table, const QString &column);
    // One-time removal of the interface prototype's invented content.
    static void removeSampleData();
};
