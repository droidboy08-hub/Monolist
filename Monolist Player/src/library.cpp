#include "library.h"
#include "appdatabase.h"

#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <lm.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace {

// The settings key for a name the user typed; empty means the system's.
const QString kNameKey = QStringLiteral("profile.name");

// "48 min", "1 hr 12 min": a playlist's length, as its header gives it.
QString lengthLabel(qint64 ms)
{
    const qint64 minutes = (ms + 30000) / 60000;
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    return QStringLiteral("%1 hr %2 min").arg(minutes / 60).arg(minutes % 60);
}

// Every YouTube video has a thumbnail at a predictable address, so a song
// saved without artwork still has a cover.
QString artworkOr(const QString &artwork, const QString &videoId)
{
    if (!artwork.isEmpty() || videoId.isEmpty())
        return artwork;
    return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId);
}

// Rows of video_id, title, artist, album, artwork, duration_ms and optionally
// an entry id, as the song lists QML shows.
QList<SearchResultModel::Item> readSongs(QSqlQuery &query, bool withEntryId)
{
    QList<SearchResultModel::Item> items;
    while (query.next()) {
        SearchResultModel::Item item;
        item.sourceId = query.value(0).toString();
        item.title = query.value(1).toString();
        item.artist = query.value(2).toString();
        item.album = query.value(3).toString();
        item.artwork = artworkOr(query.value(4).toString(), item.sourceId);
        item.durationMs = query.value(5).toLongLong();
        if (withEntryId)
            item.entryId = query.value(6).toInt();
        items.append(item);
    }
    return items;
}

bool insertEntry(int playlistId, const QString &videoId, const QVariantMap &track)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO playlist_tracks (playlist_id, position, video_id, title, artist, album, artwork, duration_ms)"
        " SELECT ?, (SELECT COALESCE(MAX(position) + 1, 0) FROM playlist_tracks WHERE playlist_id = ?),"
        " ?, ?, ?, ?, ?, ?"));
    q.addBindValue(playlistId);
    q.addBindValue(playlistId);
    q.addBindValue(videoId);
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
    q.addBindValue(AppDatabase::text(artworkOr(track.value(QStringLiteral("artwork")).toString(), videoId)));
    q.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    if (!q.exec()) {
        qWarning("Monolist: could not add to a playlist: %s", qPrintable(q.lastError().text()));
        return false;
    }
    return true;
}

void markUpdated(int playlistId)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE playlists SET updated_at = datetime('now') WHERE id = ?"));
    q.addBindValue(playlistId);
    q.exec();
}

} // namespace

Library::Library(QObject *parent)
    : QObject(parent) {}

void Library::load()
{
    m_playlists.reload();
    m_tracks.reload();
    reloadLiked();
    reloadSaved();
    reloadHistory();
}

void Library::touch()
{
    ++m_revision;
    Q_EMIT revisionChanged();
}

QString Library::trackVideoId(const QVariantMap &track)
{
    const QString sourceId = track.value(QStringLiteral("sourceId")).toString();
    return sourceId.isEmpty() ? track.value(QStringLiteral("videoId")).toString() : sourceId;
}

// ------------------------------------------------------------------ account

// The name the operating system knows the user by: the account's full name
// where one is set, otherwise the login name, capitalised if it is all lower
// case ("droid" reads as a name, "Droid").
QString Library::systemUserName()
{
    static const QString name = []() {
        QString full;
        QString login;
#if defined(Q_OS_WIN)
        wchar_t buffer[UNLEN + 1];
        DWORD size = UNLEN + 1;
        if (GetUserNameW(buffer, &size)) {
            login = QString::fromWCharArray(buffer);
            USER_INFO_10 *info = nullptr;
            if (NetUserGetInfo(nullptr, buffer, 10, reinterpret_cast<LPBYTE *>(&info)) == NERR_Success
                && info) {
                full = QString::fromWCharArray(info->usri10_full_name).trimmed();
                NetApiBufferFree(info);
            }
        }
        if (login.isEmpty())
            login = qEnvironmentVariable("USERNAME");
#else
        if (const passwd *entry = getpwuid(getuid())) {
            // The GECOS field: "Full Name,Room,Phone,..."
            full = QString::fromLocal8Bit(entry->pw_gecos).section(QLatin1Char(','), 0, 0).trimmed();
            login = QString::fromLocal8Bit(entry->pw_name);
        }
        if (login.isEmpty())
            login = qEnvironmentVariable("USER");
#endif
        if (!full.isEmpty())
            return full;
        if (!login.isEmpty() && login == login.toLower())
            login[0] = login.at(0).toUpper();
        return login.isEmpty() ? QStringLiteral("You") : login;
    }();
    return name;
}

QString Library::userName() const
{
    const QString typed = settingValue(kNameKey).trimmed();
    return typed.isEmpty() ? systemUserName() : typed;
}

QString Library::userInitials() const
{
    const QStringList parts = userName().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString initials;
    for (const QString &part : parts)
        initials.append(part.left(1).toUpper());
    return initials.left(2);
}

void Library::setUserName(const QString &name)
{
    const QString trimmed = name.trimmed();
    // Typing the system's own name back in is the same as clearing it.
    setSetting(kNameKey, trimmed == systemUserName() ? QString() : trimmed);
    Q_EMIT userChanged();
}

QString Library::settingValue(const QString &key, const QString &fallback) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return fallback;
}

void Library::setSetting(const QString &key, const QString &value)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("INSERT INTO settings (key, value) VALUES (?, ?)"
                             " ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(AppDatabase::text(value));
    q.exec();
}

// ---------------------------------------------------------------- playlists

int Library::createPlaylist(const QString &requested)
{
    QString name = requested.trimmed();
    if (name.isEmpty()) {
        QStringList taken;
        QSqlQuery names(AppDatabase::connection());
        names.exec(QStringLiteral("SELECT name FROM playlists"));
        while (names.next())
            taken << names.value(0).toString();
        name = QStringLiteral("New playlist");
        for (int n = 2; taken.contains(name); ++n)
            name = QStringLiteral("New playlist %1").arg(n);
    }

    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO playlists (position, name, track_count, created_at, updated_at)"
        " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM playlists), ?, 0,"
        " datetime('now'), datetime('now')"));
    q.addBindValue(name);
    if (!q.exec()) {
        qWarning("Monolist: could not create a playlist: %s", qPrintable(q.lastError().text()));
        return 0;
    }
    const int id = q.lastInsertId().toInt();
    m_playlists.reload();
    return id;
}

void Library::renamePlaylist(int playlistId, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == playlistName(playlistId))
        return;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE playlists SET name = ?, updated_at = datetime('now') WHERE id = ?"));
    q.addBindValue(trimmed);
    q.addBindValue(playlistId);
    q.exec();
    m_playlists.reload();
    if (playlistId == m_openPlaylistId) {
        m_playlist.insert(QStringLiteral("name"), trimmed);
        Q_EMIT playlistChanged();
    }
}

void Library::deletePlaylist(int playlistId)
{
    const QString name = playlistName(playlistId);
    if (name.isNull())
        return;
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery songs(db);
    songs.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
    songs.addBindValue(playlistId);
    songs.exec();
    QSqlQuery list(db);
    list.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
    list.addBindValue(playlistId);
    list.exec();
    db.commit();

    m_playlists.reload();
    if (playlistId == m_openPlaylistId)
        reloadOpenPlaylist();
    Q_EMIT notice(QStringLiteral("Deleted “%1”").arg(name));
}

QString Library::playlistName(int playlistId) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT name FROM playlists WHERE id = ?"));
    q.addBindValue(playlistId);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

bool Library::addToPlaylist(int playlistId, const QVariantMap &track)
{
    const QString videoId = trackVideoId(track);
    const QString name = playlistName(playlistId);
    if (videoId.isEmpty() || name.isNull())
        return false;

    QSqlQuery present(AppDatabase::connection());
    present.prepare(QStringLiteral("SELECT 1 FROM playlist_tracks WHERE playlist_id = ? AND video_id = ?"));
    present.addBindValue(playlistId);
    present.addBindValue(videoId);
    if (present.exec() && present.next()) {
        Q_EMIT notice(QStringLiteral("Already in %1").arg(name));
        return false;
    }
    if (!insertEntry(playlistId, videoId, track))
        return false;
    markUpdated(playlistId);

    m_playlists.reload();
    if (playlistId == m_openPlaylistId)
        reloadOpenPlaylist();
    Q_EMIT notice(QStringLiteral("Added to %1").arg(name));
    return true;
}

int Library::addAllToPlaylist(int playlistId, const QVariantList &tracks)
{
    const QString name = playlistName(playlistId);
    if (name.isNull())
        return 0;

    QSet<QString> present;
    QSqlQuery existing(AppDatabase::connection());
    existing.prepare(QStringLiteral("SELECT video_id FROM playlist_tracks WHERE playlist_id = ?"));
    existing.addBindValue(playlistId);
    if (existing.exec()) {
        while (existing.next())
            present.insert(existing.value(0).toString());
    }

    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    int added = 0;
    for (const QVariant &value : tracks) {
        const QVariantMap track = value.toMap();
        const QString videoId = trackVideoId(track);
        if (videoId.isEmpty() || present.contains(videoId))
            continue;
        if (insertEntry(playlistId, videoId, track)) {
            present.insert(videoId);
            ++added;
        }
    }
    db.commit();

    if (added > 0) {
        markUpdated(playlistId);
        m_playlists.reload();
        if (playlistId == m_openPlaylistId)
            reloadOpenPlaylist();
    }
    Q_EMIT notice(added == 0 ? QStringLiteral("Already in %1").arg(name)
                : added == 1 ? QStringLiteral("Added to %1").arg(name)
                             : QStringLiteral("Added %1 songs to %2").arg(added).arg(name));
    return added;
}

void Library::removeFromPlaylist(int playlistId, int entryId)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE id = ? AND playlist_id = ?"));
    q.addBindValue(entryId);
    q.addBindValue(playlistId);
    if (!q.exec() || q.numRowsAffected() == 0)
        return;
    markUpdated(playlistId);
    m_playlists.reload();
    if (playlistId == m_openPlaylistId)
        reloadOpenPlaylist();
    Q_EMIT notice(QStringLiteral("Removed from %1").arg(playlistName(playlistId)));
}

void Library::openPlaylist(int playlistId)
{
    m_openPlaylistId = playlistId;
    reloadOpenPlaylist();
}

void Library::reloadOpenPlaylist()
{
    const int id = m_openPlaylistId;
    const QString name = playlistName(id);

    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT video_id, title, artist, album, artwork, duration_ms, id FROM playlist_tracks"
        " WHERE playlist_id = ? ORDER BY position ASC, id ASC"));
    q.addBindValue(id);
    QList<SearchResultModel::Item> items;
    if (q.exec())
        items = readSongs(q, /*withEntryId=*/true);
    qint64 total = 0;
    for (const SearchResultModel::Item &item : std::as_const(items))
        total += item.durationMs;

    m_playlistTracks.replace(items);
    m_playlist = {
        { QStringLiteral("playlistId"), id },
        // Null when the playlist is gone: deleted, or a stale link.
        { QStringLiteral("exists"), !name.isNull() },
        { QStringLiteral("name"), name },
        { QStringLiteral("trackCount"), int(items.size()) },
        { QStringLiteral("durationText"), items.isEmpty() ? QString() : lengthLabel(total) },
        { QStringLiteral("artworks"), PlaylistModel::artworksFor(id) }
    };
    Q_EMIT playlistChanged();
}

QVariantList Library::playlistTrackList() const
{
    QVariantList list;
    for (int row = 0; row < m_playlistTracks.rowCount(); ++row)
        list.append(m_playlistTracks.get(row));
    return list;
}

QVariantList Library::likedTrackList() const
{
    QVariantList list;
    for (int row = 0; row < m_liked.rowCount(); ++row)
        list.append(m_liked.get(row));
    return list;
}

// -------------------------------------------------------------------- likes

void Library::reloadLiked()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT source_id, title, artist, album, artwork, duration_ms FROM tracks"
        " WHERE favourite = 1 AND source_id <> '' ORDER BY liked_at DESC, id DESC"));
    const QList<SearchResultModel::Item> items = readSongs(q, /*withEntryId=*/false);
    m_likedIds.clear();
    for (const SearchResultModel::Item &item : items)
        m_likedIds.insert(item.sourceId);
    m_liked.replace(items);
}

bool Library::isLiked(const QString &videoId) const
{
    return !videoId.isEmpty() && m_likedIds.contains(videoId);
}

// A liked song is a library row with the favourite flag. Liking a song that is
// not in the library adds it; unliking one that only the like put there (no
// downloaded file) takes it out again, so the library holds what was liked or
// downloaded and nothing else.
void Library::setLiked(const QVariantMap &track, bool liked)
{
    const QString videoId = trackVideoId(track);
    if (videoId.isEmpty() || liked == isLiked(videoId))
        return;

    QSqlQuery find(AppDatabase::connection());
    find.prepare(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE source_id = ?"));
    find.addBindValue(videoId);
    const bool inLibrary = find.exec() && find.next() && find.value(0).toInt() > 0;

    QSqlQuery write(AppDatabase::connection());
    if (liked && inLibrary) {
        write.prepare(QStringLiteral(
            "UPDATE tracks SET favourite = 1, liked_at = datetime('now') WHERE source_id = ?"));
        write.addBindValue(videoId);
    } else if (liked) {
        write.prepare(QStringLiteral(
            "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id,"
            " artwork, favourite, liked_at)"
            " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, ?, ?, '', ?, ?, 1,"
            " datetime('now')"));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
        write.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
        write.addBindValue(videoId);
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("artwork")).toString()));
    } else {
        QSqlQuery drop(AppDatabase::connection());
        drop.prepare(QStringLiteral("DELETE FROM tracks WHERE source_id = ? AND source_url = ''"));
        drop.addBindValue(videoId);
        drop.exec();
        write.prepare(QStringLiteral("UPDATE tracks SET favourite = 0, liked_at = '' WHERE source_id = ?"));
        write.addBindValue(videoId);
    }
    if (!write.exec()) {
        qWarning("Monolist: could not save a like: %s", qPrintable(write.lastError().text()));
        return;
    }

    m_tracks.reload();
    reloadLiked();
    touch();
    Q_EMIT likesChanged();
    Q_EMIT notice(liked ? QStringLiteral("Added to Liked songs") : QStringLiteral("Removed from Liked songs"));
}

// ------------------------------------------------------------ saved from YTM

void Library::reloadSaved()
{
    m_albums.reload();
    m_savedPlaylists.reload();
    m_savedIds.clear();
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT browse_id FROM albums WHERE browse_id <> ''"));
    while (q.next())
        m_savedIds.insert(q.value(0).toString());
}

bool Library::isSaved(const QString &browseId) const
{
    return !browseId.isEmpty() && m_savedIds.contains(browseId);
}

void Library::setSaved(const QVariantMap &page, bool saved)
{
    const QString browseId = page.value(QStringLiteral("browseId")).toString();
    if (browseId.isEmpty() || saved == isSaved(browseId))
        return;

    QSqlQuery write(AppDatabase::connection());
    if (saved) {
        // "Album • 2026", "Single • 2025", "Playlist • YouTube Music • 2026"
        static const QRegularExpression yearPattern(QStringLiteral(R"(\b(19|20)\d{2}\b)"));
        const QString subtitle = page.value(QStringLiteral("subtitle")).toString();
        const QStringList parts = subtitle.split(QStringLiteral(" • "), Qt::SkipEmptyParts);
        const bool playlist = page.value(QStringLiteral("type")).toString() == QLatin1String("playlist");
        QString format = playlist ? QStringLiteral("PLAYLIST") : parts.value(0).trimmed().toUpper();
        if (!playlist && format != QLatin1String("SINGLE") && format != QLatin1String("EP"))
            format = QStringLiteral("ALBUM");
        QString artist = page.value(QStringLiteral("artist")).toString();
        if (artist.isEmpty()) {
            for (const QString &part : parts.mid(1)) {
                if (!yearPattern.match(part).hasMatch()) {
                    artist = part.trimmed();
                    break;
                }
            }
        }
        write.prepare(QStringLiteral(
            "INSERT INTO albums (position, title, artist, year, format, artwork, browse_id, saved_at)"
            " VALUES (0, ?, ?, ?, ?, ?, ?, datetime('now'))"));
        write.addBindValue(AppDatabase::text(page.value(QStringLiteral("title")).toString()));
        write.addBindValue(AppDatabase::text(artist));
        write.addBindValue(AppDatabase::text(yearPattern.match(subtitle).captured(0)));
        write.addBindValue(format);
        write.addBindValue(AppDatabase::text(page.value(QStringLiteral("artwork")).toString()));
        write.addBindValue(browseId);
    } else {
        write.prepare(QStringLiteral("DELETE FROM albums WHERE browse_id = ?"));
        write.addBindValue(browseId);
    }
    if (!write.exec()) {
        qWarning("Monolist: could not save to the library: %s", qPrintable(write.lastError().text()));
        return;
    }

    reloadSaved();
    touch();
    Q_EMIT notice(saved ? QStringLiteral("Saved to your library") : QStringLiteral("Removed from your library"));
}

// ------------------------------------------------------------------ history

void Library::reloadHistory()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT video_id, title, artist, album, artwork, duration_ms FROM recent"
        " ORDER BY played_at DESC, rowid DESC LIMIT 200"));
    m_history.replace(readSongs(q, /*withEntryId=*/false));
}

void Library::clearHistory()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("DELETE FROM recent"));
    q.exec(QStringLiteral("DELETE FROM history"));
    reloadHistory();
    Q_EMIT historyCleared();
    Q_EMIT notice(QStringLiteral("History cleared"));
}
