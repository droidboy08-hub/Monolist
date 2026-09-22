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

    // For binding to a NOT NULL text column. Qt binds a null QString — what an
    // absent map value or a default-constructed field gives — as SQL NULL, and
    // the column's DEFAULT does not apply to an explicit NULL, so the whole row
    // would be refused.
    static QString text(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

private:
    static bool hasColumn(const QString &table, const QString &column);
    // One-time removal of the interface prototype's invented content.
    static void removeSampleData();
};
