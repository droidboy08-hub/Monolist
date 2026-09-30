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
    // MONOLIST_DATA_DIR keeps the database somewhere else: for trying a
    // change, or running the self-tests, without touching the real library.
    QString dir = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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

    // What the recommender learns from: one row per listen, plus a row for each
    // like and each dismissal.
    //
    // `recent` and `history` cannot answer this. They record that a song was
    // played, and taste is mostly in what was *not* — a song skipped after nine
    // seconds is evidence against, and both tables record it identically to one
    // played through. So this keeps how much was heard, and the label derived
    // from it.
    //
    // The iOS player this is ported from keeps the same events in an append-only
    // JSON Lines file. A table is the same thing where the rest of this app
    // already lives: it is read by one query instead of a scan, it cannot be
    // half-written, and it needs no rotation.
    //
    // `listened_ms` is the playhead where the track was left, not time spent
    // watching — seeking to the end counts as a full listen, exactly as it does
    // on iOS, and any change to that would make an imported history mean
    // something different from a local one.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS play_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        // play | like | unliked | notInterested | notInterestedArtist (the
        // last two are Search's "Not interested" and "Don't suggest")
        " kind TEXT NOT NULL DEFAULT 'play',"
        " video_id TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " started_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " track_ms INTEGER NOT NULL DEFAULT 0,"
        " listened_ms INTEGER NOT NULL DEFAULT 0,"
        " completed INTEGER NOT NULL DEFAULT 0,"
        " skipped INTEGER NOT NULL DEFAULT 0,"
        // NULL is meaningful: "played, but not long enough to say anything".
        // The profile compares labels for exact equality, so they are stored,
        // never re-derived by arithmetic.
        " label REAL,"
        " source TEXT NOT NULL DEFAULT '',"
        " playlist_id INTEGER NOT NULL DEFAULT 0,"
        " repeat_in_session INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS play_events_started ON play_events (started_at DESC)"));

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

    // How loud YouTube measured each song (Loudness): the chosen format's
    // loudnessDb, kept so a download or a later launch levels it the same.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS loudness ("
        " video_id TEXT PRIMARY KEY,"
        " db REAL NOT NULL,"
        " measured_at INTEGER NOT NULL DEFAULT 0)"));

    // Home's last good answers, signed out, per country (Catalog::showSaved):
    // shown at once at the next launch, and when the network fails. The
    // body is the answer as it came, compressed.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS home_cache ("
        " part TEXT PRIMARY KEY,"
        " region TEXT NOT NULL DEFAULT '',"
        " saved_at INTEGER NOT NULL DEFAULT 0,"
        " body BLOB NOT NULL)"));

    // The downloads not finished yet, in the order they will run, so a quit
    // does not lose them (DownloadManager::saveQueue): queued ones, those
    // running when the app closed, and failed ones with why.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS download_queue ("
        " video_id TEXT PRIMARY KEY,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " is_video INTEGER NOT NULL DEFAULT 0,"
        " failed INTEGER NOT NULL DEFAULT 0,"
        " error TEXT NOT NULL DEFAULT '')"));

    // A playlist's songs, each a copy of what is needed to show and play it,
    // so a playlist can hold songs that are in no other list.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlist_tracks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " playlist_id INTEGER NOT NULL,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " video_id TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " added_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " FOREIGN KEY(playlist_id) REFERENCES playlists(id) ON DELETE CASCADE)"));
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_playlist_tracks_playlist ON playlist_tracks(playlist_id, position)"));

    // Lyrics as found, so a song's lyrics are asked for once. A row with
    // neither kind records that none were found, and when.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS lyrics ("
        " video_id TEXT PRIMARY KEY,"
        " synced TEXT NOT NULL DEFAULT '',"
        " plain TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT '',"
        " fetched_at TEXT NOT NULL DEFAULT (datetime('now')))"));
    // Each lyrics provider's own answer for a song, found or none (LyricsStore),
    // so one that has answered is not asked again while that is fresh. Never a
    // failure. `duration` is the length of the entry it came from, -1 unknown.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS lyrics_results ("
        " video_id TEXT NOT NULL,"
        " provider TEXT NOT NULL,"
        " synced TEXT NOT NULL DEFAULT '',"
        " plain TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT '',"
        " instrumental INTEGER NOT NULL DEFAULT 0,"
        " loose INTEGER NOT NULL DEFAULT 0,"
        " duration REAL NOT NULL DEFAULT -1,"
        " fetched_at TEXT NOT NULL DEFAULT (datetime('now')),"
        " PRIMARY KEY (video_id, provider))"));

    // Scrobbles not yet accepted by Last.fm, oldest first. A row is written
    // the moment a listen qualifies, before anything is sent, so a crash, a
    // killed process or a week offline loses none of them; it is deleted
    // only once Last.fm has answered for it. `account` is the Last.fm user it
    // was heard under, so a backlog is never sent to someone else who
    // connects later. `started_at` and `queued_at` are UTC seconds.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS scrobble_queue ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " account TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " track TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " album_artist TEXT NOT NULL DEFAULT '',"
        " duration_s INTEGER NOT NULL DEFAULT 0,"
        " started_at INTEGER NOT NULL DEFAULT 0,"
        " chosen_by_user INTEGER NOT NULL DEFAULT 1,"
        " video_id TEXT NOT NULL DEFAULT '',"
        " attempts INTEGER NOT NULL DEFAULT 0,"
        " last_error TEXT NOT NULL DEFAULT '',"
        " queued_at INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_scrobble_queue_account ON scrobble_queue(account, id)"));

    // Which page an artist's name opens, as YouTube Music linked it the last
    // time the name was seen (ArtistLinks). The songs kept above keep only
    // the name; this is how a liked song's artist, or one in the history,
    // still opens the right page. `artist_page` is 0 for a plain channel,
    // which an artist's own page of the same name replaces.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS artist_links ("
        " name TEXT PRIMARY KEY,"
        " browse_id TEXT NOT NULL,"
        " artist_page INTEGER NOT NULL DEFAULT 1,"
        " seen_at TEXT NOT NULL DEFAULT (datetime('now')))"));

    // What JioSaavn answered for a song, so it is asked once a week rather
    // than at every play (StreamResolver): its link and bitrate where it has
    // the same recording (`matched` 1), or that it has not (`matched` 0),
    // which is asked again sooner. `signature` is a hash of the title,
    // artist, album and length the answer was for, and `matcher` the version
    // of the rules that gave it; either differing, the song is asked about
    // afresh. Times are UTC seconds.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS saavn_matches ("
        " video_id TEXT PRIMARY KEY,"
        " matched INTEGER NOT NULL DEFAULT 0,"
        " saavn_id TEXT NOT NULL DEFAULT '',"
        " url TEXT NOT NULL DEFAULT '',"
        " kbps INTEGER NOT NULL DEFAULT 0,"
        " expires_at INTEGER NOT NULL DEFAULT 0,"
        " signature TEXT NOT NULL DEFAULT '',"
        " matcher INTEGER NOT NULL DEFAULT 0)"));

    // The YouTube Music account's library, read into Monolist and never
    // written back (YtmImport). Kept apart from everything above: the
    // user's own likes, playlists, saves and history never change with a
    // sync, and none of this feeds the recommendations or Last.fm. All of it
    // goes when the user signs out.
    //
    // ytm_lists: how each list's last sync went ('liked', 'playlists',
    // 'history'); `account` a hash that tells one account from another,
    // never its id; `complete` 0 when a cap stopped the reading.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS ytm_lists ("
        " list TEXT PRIMARY KEY,"
        " account TEXT NOT NULL DEFAULT '',"
        " synced_at INTEGER NOT NULL DEFAULT 0,"
        " item_count INTEGER NOT NULL DEFAULT 0,"
        " complete INTEGER NOT NULL DEFAULT 1)"));
    // The songs of Liked on YouTube Music and of the account's history, in
    // YouTube Music's own order; `section` is a history day's heading.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS ytm_tracks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " list TEXT NOT NULL,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " section TEXT NOT NULL DEFAULT '',"
        " video_id TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " artist TEXT NOT NULL DEFAULT '',"
        " primary_artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " album_id TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " is_video INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_ytm_tracks_list ON ytm_tracks(list, position)"));
    // The account's own playlists, by reference: their songs are read when
    // one is opened, with the account, rather than all of them at every sync.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS ytm_playlists ("
        " browse_id TEXT PRIMARY KEY,"
        " position INTEGER NOT NULL DEFAULT 0,"
        " title TEXT NOT NULL DEFAULT '',"
        " subtitle TEXT NOT NULL DEFAULT '',"
        " artwork TEXT NOT NULL DEFAULT '')"));
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

    // When a song was liked, so Liked songs can list the latest first. SQLite
    // cannot add a column whose default is a function, hence '' and the
    // explicit values the writes give.
    if (!hasColumn(QStringLiteral("tracks"), QStringLiteral("liked_at")))
        q.exec(QStringLiteral("ALTER TABLE tracks ADD COLUMN liked_at TEXT NOT NULL DEFAULT ''"));

    if (!hasColumn(QStringLiteral("playlists"), QStringLiteral("created_at")))
        q.exec(QStringLiteral("ALTER TABLE playlists ADD COLUMN created_at TEXT NOT NULL DEFAULT ''"));
    if (!hasColumn(QStringLiteral("playlists"), QStringLiteral("updated_at")))
        q.exec(QStringLiteral("ALTER TABLE playlists ADD COLUMN updated_at TEXT NOT NULL DEFAULT ''"));

    // Saved albums and playlists open their YouTube Music page.
    if (!hasColumn(QStringLiteral("albums"), QStringLiteral("browse_id"))) {
        q.exec(QStringLiteral("ALTER TABLE albums ADD COLUMN browse_id TEXT NOT NULL DEFAULT ''"));
        q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_albums_browse_id ON albums(browse_id)"));
    }
    if (!hasColumn(QStringLiteral("albums"), QStringLiteral("saved_at")))
        q.exec(QStringLiteral("ALTER TABLE albums ADD COLUMN saved_at TEXT NOT NULL DEFAULT ''"));

    // Lyrics kept only because a better source failed to answer (Lyrics):
    // shown, and asked for again. Rows from before read as final answers.
    if (!hasColumn(QStringLiteral("lyrics"), QStringLiteral("provisional")))
        q.exec(QStringLiteral("ALTER TABLE lyrics ADD COLUMN provisional INTEGER NOT NULL DEFAULT 0"));

    // Whether a song is a music video, with a picture worth showing. Only
    // search and YouTube Music's own lists know it, so every copy of a song
    // kept here carries it too — otherwise a video liked, saved to a playlist,
    // downloaded or replayed from History would come back as sound alone,
    // with no way to ask for its picture.
    for (const QString &table : { QStringLiteral("tracks"), QStringLiteral("recent"),
                                  QStringLiteral("playlist_tracks"), QStringLiteral("downloads") }) {
        if (!hasColumn(table, QStringLiteral("is_video")))
            q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN is_video INTEGER NOT NULL DEFAULT 0").arg(table));
    }

    // JioSaavn's answers from before they were tied to the song they were
    // for: the rows stay, unmatched by any signature, until they run out.
    if (!hasColumn(QStringLiteral("saavn_matches"), QStringLiteral("signature")))
        q.exec(QStringLiteral("ALTER TABLE saavn_matches ADD COLUMN signature TEXT NOT NULL DEFAULT ''"));
    if (!hasColumn(QStringLiteral("saavn_matches"), QStringLiteral("matcher")))
        q.exec(QStringLiteral("ALTER TABLE saavn_matches ADD COLUMN matcher INTEGER NOT NULL DEFAULT 0"));

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
