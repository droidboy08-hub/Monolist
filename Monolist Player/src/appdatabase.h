#pragma once

#include <QObject>
#include <QSqlDatabase>

// Owns the local SQLite store: history, playlists, favourites, settings.
// Everything the UI shows is read through the models in library.h, so the
// real catalogue can later replace the seeded sample rows without UI changes.
class AppDatabase
{
public:
    AppDatabase();

    bool open();
    void createSchema();
    void migrate();                 // additive column/table changes, safe to re-run

    static QSqlDatabase connection();
    static QString databaseFilePath();

private:
    static bool hasColumn(const QString &table, const QString &column);
    // One-time removal of the interface prototype's invented content.
    static void removeSampleData();
};
