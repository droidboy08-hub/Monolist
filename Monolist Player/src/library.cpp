#include "library.h"
#include "trackfiltermodel.h"
#include <QDateTime>
#include <QHash>
#include "appdatabase.h"
#include "innertube.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QLocale>
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
// Likewise the country to browse; empty means the system's.
const QString kRegionKey = QStringLiteral("region");
const QString kVideoHeightKey = QStringLiteral("video.height");

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

// Rows of video_id, title, artist, album, artwork, duration_ms, is_video and
// optionally an entry id, as the song lists QML shows.
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
        item.isVideo = query.value(6).toBool();
        if (withEntryId)
            item.entryId = query.value(7).toInt();
        items.append(item);
    }
    return items;
}

bool insertEntry(int playlistId, const QString &videoId, const QVariantMap &track)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO playlist_tracks (playlist_id, position, video_id, title, artist, album, artwork,"
        " duration_ms, is_video)"
        " SELECT ?, (SELECT COALESCE(MAX(position) + 1, 0) FROM playlist_tracks WHERE playlist_id = ?),"
        " ?, ?, ?, ?, ?, ?, ?"));
    q.addBindValue(playlistId);
    q.addBindValue(playlistId);
    q.addBindValue(videoId);
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
    q.addBindValue(AppDatabase::text(artworkOr(track.value(QStringLiteral("artwork")).toString(), videoId)));
    q.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
    q.addBindValue(track.value(QStringLiteral("isVideo")).toBool() ? 1 : 0);
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

void Library::reloadSongs()
{
    // One row a song, from whichever copy was added last: a like, a
    // download, a playlist's entry. SQLite takes the other columns from the
    // row MAX() picks.
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT source_id, title, artist, album, artwork, duration_ms, is_video, MAX(added) FROM ("
        "   SELECT t.source_id, t.title, t.artist, t.album, t.artwork, t.duration_ms, t.is_video,"
        "          COALESCE(NULLIF(t.liked_at, ''), d.downloaded_at, '') AS added"
        "   FROM tracks t LEFT JOIN downloads d ON d.video_id = t.source_id"
        "   WHERE t.source_id <> ''"
        " UNION ALL"
        "   SELECT video_id, title, artist, album, artwork, duration_ms, is_video, added_at"
        "   FROM playlist_tracks WHERE video_id <> ''"
        ") GROUP BY source_id ORDER BY MAX(added) DESC, title COLLATE NOCASE"));
    m_songs.replace(readSongs(q, /*withEntryId=*/false));
}

QVariantList Library::songArtists() const
{
    struct Artist {
        QString name;
        int count = 0;
        QString artwork;
    };
    QHash<QString, Artist> byName;   // by the name folded, so "ABBA" and "Abba" are one
    for (int row = 0; row < m_songs.rowCount(); ++row) {
        const QVariantMap song = m_songs.get(row);
        for (const QString &name : TrackFilterModel::artistsIn(song.value(QStringLiteral("artist")).toString())) {
            Artist &artist = byName[name.toCaseFolded()];
            if (artist.name.isEmpty())
                artist.name = name;
            ++artist.count;
            if (artist.artwork.isEmpty())
                artist.artwork = song.value(QStringLiteral("artwork")).toString();
        }
    }
    QList<Artist> artists = byName.values();
    std::sort(artists.begin(), artists.end(), [](const Artist &a, const Artist &b) {
        return a.count != b.count ? a.count > b.count : a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    QVariantList list;
    for (const Artist &artist : std::as_const(artists)) {
        list.append(QVariantMap{ { QStringLiteral("name"), artist.name },
                                 { QStringLiteral("count"), artist.count },
                                 { QStringLiteral("artwork"), artist.artwork } });
    }
    return list;
}

void Library::setPlaylistPinned(int playlistId, bool pinned)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE playlists SET pinned = ? WHERE id = ?"));
    q.addBindValue(pinned ? 1 : 0);
    q.addBindValue(playlistId);
    q.exec();
    m_playlists.reload();
    touch();
    Q_EMIT notice(pinned ? QStringLiteral("Pinned to the top") : QStringLiteral("Unpinned"));
}

bool Library::isPlaylistPinned(int playlistId) const
{
    const int row = m_playlists.indexOf(playlistId);
    return row >= 0 && m_playlists.get(row).value(QStringLiteral("pinned")).toBool();
}

int Library::playlistRow(int playlistId) const
{
    return m_playlists.indexOf(playlistId);
}

void Library::movePlaylist(int playlistId, int to)
{
    const int from = m_playlists.indexOf(playlistId);
    if (from < 0)
        return;
    QList<int> ids;
    int pinnedCount = 0;
    for (int row = 0; row < m_playlists.rowCount(); ++row) {
        const QVariantMap item = m_playlists.get(row);
        ids << item.value(QStringLiteral("playlistId")).toInt();
        if (item.value(QStringLiteral("pinned")).toBool())
            ++pinnedCount;
    }
    // Within its group: a pinned playlist among the pinned, any other below them.
    const bool pinned = from < pinnedCount;
    const int first = pinned ? 0 : pinnedCount;
    const int last = pinned ? pinnedCount - 1 : int(ids.size()) - 1;
    to = qBound(first, to, last);
    if (to == from)
        return;
    ids.move(from, to);

    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE playlists SET position = ? WHERE id = ?"));
    for (int row = 0; row < ids.size(); ++row) {
        q.addBindValue(row);
        q.addBindValue(ids.at(row));
        q.exec();
    }
    db.commit();
    m_playlists.reload();
    touch();
}

void Library::rememberSearch(const QString &term)
{
    const QString kept = term.simplified();
    if (kept.isEmpty())
        return;
    QSqlQuery query(AppDatabase::connection());
    // Replaced whole, so the spelling kept is the last one typed.
    query.prepare(QStringLiteral("DELETE FROM search_history WHERE term = ?"));
    query.addBindValue(kept);
    query.exec();
    query.prepare(QStringLiteral("INSERT INTO search_history (term, searched_at) VALUES (?, ?)"));
    query.addBindValue(kept);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.exec();
    query.exec(QStringLiteral(
        "DELETE FROM search_history WHERE term NOT IN"
        " (SELECT term FROM search_history ORDER BY searched_at DESC, rowid DESC LIMIT 50)"));
    reloadSearches();
}

void Library::forgetSearch(const QString &term)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("DELETE FROM search_history WHERE term = ?"));
    query.addBindValue(term.simplified());
    query.exec();
    reloadSearches();
}

void Library::clearSearches()
{
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM search_history"));
    reloadSearches();
}

void Library::reloadSearches()
{
    QStringList terms;
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral("SELECT term FROM search_history ORDER BY searched_at DESC, rowid DESC LIMIT 50"));
    while (query.next())
        terms << query.value(0).toString();
    if (terms == m_recentSearches)
        return;
    m_recentSearches = terms;
    Q_EMIT recentSearchesChanged();
}

void Library::rememberPlace(const QVariantMap &place)
{
    const QString kind = place.value(QStringLiteral("kind")).toString();
    const QString ref = place.value(QStringLiteral("ref")).toString();
    if (kind.isEmpty() || ref.isEmpty())
        return;
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO recent_places (kind, ref, title, subtitle, artwork, type, opened_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(kind);
    query.addBindValue(ref);
    query.addBindValue(AppDatabase::text(place.value(QStringLiteral("title")).toString()));
    query.addBindValue(AppDatabase::text(place.value(QStringLiteral("subtitle")).toString()));
    query.addBindValue(AppDatabase::text(place.value(QStringLiteral("artwork")).toString()));
    query.addBindValue(AppDatabase::text(place.value(QStringLiteral("type")).toString()));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.exec();
    query.exec(QStringLiteral(
        "DELETE FROM recent_places WHERE rowid NOT IN"
        " (SELECT rowid FROM recent_places ORDER BY opened_at DESC LIMIT 40)"));
    reloadPlaces();
}

void Library::forgetPlace(const QString &kind, const QString &ref)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("DELETE FROM recent_places WHERE kind = ? AND ref = ?"));
    query.addBindValue(kind);
    query.addBindValue(ref);
    query.exec();
    reloadPlaces();
}

void Library::reloadPlaces()
{
    QVariantList places;
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT kind, ref, title, subtitle, artwork, type FROM recent_places"
        " ORDER BY opened_at DESC LIMIT 16"));
    while (query.next()) {
        places.append(QVariantMap{
            { QStringLiteral("kind"), query.value(0).toString() },
            { QStringLiteral("ref"), query.value(1).toString() },
            { QStringLiteral("title"), query.value(2).toString() },
            { QStringLiteral("subtitle"), query.value(3).toString() },
            { QStringLiteral("artwork"), query.value(4).toString() },
            { QStringLiteral("type"), query.value(5).toString() }
        });
    }
    if (places == m_recentPlaces)
        return;
    m_recentPlaces = places;
    Q_EMIT recentPlacesChanged();
}

QString Library::topArtist() const
{
    // Plays that were heard (half a minute, or to the end), by credit, the
    // first name of a joint one standing for it.
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT artist, COUNT(*) FROM play_events"
        " WHERE kind = 'play' AND artist <> '' AND started_at >= datetime('now', '-56 days')"
        "   AND (listened_ms >= 30000 OR completed = 1)"
        " GROUP BY artist"));
    // As the suggestions read a credit (rec/shelves.cpp firstPerformer):
    // spaced joiners only, so "AC/DC" and "Malcolm X" stay whole, and the
    // " - Topic" of an auto-generated channel off.
    static const QRegularExpression joints(
        QStringLiteral(R"(\s*(?:,|&|;|/)\s*|\s+(?:feat\.?|ft\.?|with|x)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+topic\s*$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    QHash<QString, int> counts;
    QHash<QString, QString> spelling;
    while (query.next()) {
        const QString first = query.value(0).toString().remove(topic).section(joints, 0, 0).trimmed();
        if (first.isEmpty())
            continue;
        const QString key = first.toLower();
        counts[key] += query.value(1).toInt();
        spelling.insert(key, first);
    }
    QString best;
    int most = 2;   // three plays at least
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > most || (it.value() == most && !best.isEmpty() && it.key() < best.toLower())) {
            most = it.value();
            best = spelling.value(it.key());
        }
    }
    return best;
}

void Library::load()
{
    reloadSearches();
    reloadPlaces();
    m_playlists.reload();
    m_tracks.reload();
    reloadLiked();
    reloadSaved();
    reloadHistory();
    reloadPlaylistSongs();
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

// ------------------------------------------------------------------- region

QString Library::region() const
{
    return settingValue(kRegionKey);
}

void Library::setRegion(const QString &code)
{
    const QString wanted = code.trimmed().toUpper();
    if (wanted == region())
        return;
    setSetting(kRegionKey, wanted);
    InnerTube::setRegion(wanted);
    Q_EMIT regionChanged();
    Q_EMIT notice(wanted.isEmpty()
                      ? QStringLiteral("Following the system: %1").arg(systemRegionName())
                      : QStringLiteral("Browsing %1").arg(countryName(wanted)));
}

void Library::dropRegion(const QString &code)
{
    if (region().isEmpty())
        return;   // already following the system
    setSetting(kRegionKey, QString());
    Q_EMIT regionChanged();
    Q_EMIT notice(QStringLiteral("YouTube Music has no %1 — back to %2")
                      .arg(countryName(code), systemRegionName()));
}

QString Library::regionInUse() const
{
    return InnerTube::region();
}

QString Library::regionInUseName() const
{
    return countryName(InnerTube::region());
}

QString Library::systemRegionName() const
{
    return countryName(InnerTube::systemRegion());
}

// 720p is the most a music video usually is, and the most a machine without
// hardware decoding can carry; 360 is there for when it cannot.
int Library::videoQuality() const
{
    const int stored = settingValue(kVideoHeightKey).toInt();
    return stored > 0 ? stored : 720;
}

void Library::setVideoQuality(int height)
{
    const int wanted = height >= 1080 ? 1080 : (height <= 360 ? 360 : 720);
    if (wanted == videoQuality())
        return;
    setSetting(kVideoHeightKey, QString::number(wanted));
    Q_EMIT videoQualityChanged();
}

QString Library::countryName(const QString &code) const
{
    const QLocale::Territory territory = QLocale::codeToTerritory(code);
    return territory == QLocale::AnyTerritory ? code : QLocale::territoryToString(territory);
}

// The countries YouTube Music serves, by name. Offering the rest would only
// break the app: it refuses them outright.
QVariantList Library::countries() const
{
    QList<std::pair<QString, QString>> found;   // name, code
    QSet<QString> seen;
    const QList<QLocale> locales = QLocale::matchingLocales(QLocale::AnyLanguage, QLocale::AnyScript,
                                                            QLocale::AnyTerritory);
    for (const QLocale &locale : locales) {
        const QString code = QLocale::territoryToCode(locale.territory());
        if (code.size() != 2 || seen.contains(code) || !InnerTube::servedRegions().contains(code))
            continue;
        seen.insert(code);
        found.append({ QLocale::territoryToString(locale.territory()), code });
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.first.localeAwareCompare(b.first) < 0;
    });

    QVariantList list;
    list.reserve(found.size());
    for (const auto &[name, code] : std::as_const(found))
        list.append(QVariantMap{ { QStringLiteral("code"), code }, { QStringLiteral("name"), name } });
    return list;
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
    reloadPlaylistSongs();
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
    reloadPlaylistSongs();
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
        reloadPlaylistSongs();
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
    reloadPlaylistSongs();
    Q_EMIT notice(QStringLiteral("Removed from %1").arg(playlistName(playlistId)));
}

// The playlist ends numbered 0, 1, 2… in its new order, rather than the one
// row given a new number: positions written before could repeat (a playlist
// filled in one go) or skip (a song taken out), and a move among repeats
// would not hold once the list was read back ordered by position. Only the
// rows whose number changes are written, though: once the numbers are dense,
// those between `from` and `to` — two for a step up or down — where writing
// every row made each step of a held Alt+Down cost the whole playlist.
void Library::movePlaylistEntry(int playlistId, int entryId, int toIndex)
{
    QSqlDatabase db = AppDatabase::connection();
    QSqlQuery read(db);
    read.prepare(QStringLiteral(
        "SELECT id, position FROM playlist_tracks WHERE playlist_id = ? ORDER BY position ASC, id ASC"));
    read.addBindValue(playlistId);
    if (!read.exec())
        return;
    // Each entry, with the position it has stored.
    QList<std::pair<int, int>> entries;
    int from = -1;
    while (read.next()) {
        const int id = read.value(0).toInt();
        if (id == entryId)
            from = int(entries.size());
        entries.append({ id, read.value(1).toInt() });
    }
    if (from < 0)
        return;
    const int to = qBound(0, toIndex, int(entries.size()) - 1);
    if (to == from)
        return;
    entries.move(from, to);

    db.transaction();
    QSqlQuery write(db);
    write.prepare(QStringLiteral("UPDATE playlist_tracks SET position = ? WHERE id = ?"));
    bool ok = true;
    for (int position = 0; position < entries.size() && ok; ++position) {
        if (entries.at(position).second == position)
            continue;
        write.addBindValue(position);
        write.addBindValue(entries.at(position).first);
        ok = write.exec();
    }
    if (!ok) {
        qWarning("Monolist: could not reorder a playlist: %s", qPrintable(write.lastError().text()));
        db.rollback();
        return;
    }
    db.commit();
    markUpdated(playlistId);

    // The open list moves its one row, so a long playlist is not drawn again
    // for every song dragged; were it out of step, it is read afresh. The
    // mosaic on its card and in the sidebar is its first four covers, so the
    // playlists are read again only when those have changed.
    bool mosaicChanged = true;
    if (playlistId == m_openPlaylistId) {
        if (m_playlistTracks.get(from).value(QStringLiteral("entryId")).toInt() == entryId
                && m_playlistTracks.rowCount() == entries.size()) {
            m_playlistTracks.move(from, to);
            const QStringList artworks = PlaylistModel::artworksFor(playlistId);
            mosaicChanged = artworks != m_playlist.value(QStringLiteral("artworks")).toStringList();
            if (mosaicChanged) {
                m_playlist.insert(QStringLiteral("artworks"), artworks);
                Q_EMIT playlistChanged();
            }
        } else {
            reloadOpenPlaylist();
        }
    }
    if (mosaicChanged)
        m_playlists.reload();
}

QVariantList Library::playlistTracksFor(int playlistId) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT video_id, title, artist, album, artwork, duration_ms, is_video, id FROM playlist_tracks"
        " WHERE playlist_id = ? ORDER BY position ASC, id ASC"));
    q.addBindValue(playlistId);
    QVariantList list;
    if (!q.exec())
        return list;
    const QList<SearchResultModel::Item> items = readSongs(q, /*withEntryId=*/true);
    list.reserve(items.size());
    for (const SearchResultModel::Item &item : items)
        list.append(SearchResultModel::toMap(item));
    return list;
}

void Library::reloadPlaylistSongs()
{
    m_playlistSongIds.clear();
    QSqlQuery q(AppDatabase::connection());
    if (q.exec(QStringLiteral("SELECT DISTINCT video_id FROM playlist_tracks WHERE video_id <> ''"))) {
        while (q.next())
            m_playlistSongIds.insert(q.value(0).toString());
    }
    touch();
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
        "SELECT video_id, title, artist, album, artwork, duration_ms, is_video, id FROM playlist_tracks"
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
        "SELECT source_id, title, artist, album, artwork, duration_ms, is_video FROM tracks"
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
    if (storeLike(track, liked))
        Q_EMIT notice(liked ? QStringLiteral("Added to Liked songs") : QStringLiteral("Removed from Liked songs"));
}

bool Library::storeLike(const QVariantMap &track, bool liked)
{
    const QString videoId = trackVideoId(track);
    if (videoId.isEmpty() || liked == isLiked(videoId))
        return false;

    QSqlQuery find(AppDatabase::connection());
    find.prepare(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE source_id = ?"));
    find.addBindValue(videoId);
    const bool inLibrary = find.exec() && find.next() && find.value(0).toInt() > 0;

    // Named for the recommender's record below, taken before an unlike deletes
    // the row: a heart pressed in a list can hand over less than the player's.
    QString title = track.value(QStringLiteral("title")).toString();
    QString artist = track.value(QStringLiteral("artist")).toString();
    if (title.isEmpty() || artist.isEmpty()) {
        QSqlQuery named(AppDatabase::connection());
        named.prepare(QStringLiteral("SELECT title, artist FROM tracks WHERE source_id = ? LIMIT 1"));
        named.addBindValue(videoId);
        if (named.exec() && named.next()) {
            if (title.isEmpty())
                title = named.value(0).toString();
            if (artist.isEmpty())
                artist = named.value(1).toString();
        }
    }

    // Whether it is a music video: a row that already knows keeps knowing,
    // whatever list the heart was pressed in.
    const int isVideo = track.value(QStringLiteral("isVideo")).toBool() ? 1 : 0;
    QSqlQuery write(AppDatabase::connection());
    if (liked && inLibrary) {
        write.prepare(QStringLiteral(
            "UPDATE tracks SET favourite = 1, liked_at = datetime('now'),"
            " is_video = MAX(is_video, ?) WHERE source_id = ?"));
        write.addBindValue(isVideo);
        write.addBindValue(videoId);
    } else if (liked) {
        write.prepare(QStringLiteral(
            "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id,"
            " artwork, favourite, liked_at, is_video)"
            " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, ?, ?, '', ?, ?, 1,"
            " datetime('now'), ?"));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("title")).toString()));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("artist")).toString()));
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString()));
        write.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong());
        write.addBindValue(videoId);
        write.addBindValue(AppDatabase::text(track.value(QStringLiteral("artwork")).toString()));
        write.addBindValue(isVideo);
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
        return false;
    }

    // What the recommender learns from, recorded here because every heart in
    // the app arrives here — the player's, a track list's, a menu's. It used
    // to be recorded by the player alone, so a like from a list never counted
    // and un-hearting a song in Liked songs never took a like back. An unlike
    // is a tombstone that cancels the like before it.
    if (!title.isEmpty()) {
        QSqlQuery event(AppDatabase::connection());
        event.prepare(QStringLiteral(
            "INSERT INTO play_events (kind, video_id, title, artist) VALUES (?, ?, ?, ?)"));
        event.addBindValue(liked ? QStringLiteral("like") : QStringLiteral("unliked"));
        event.addBindValue(videoId);
        event.addBindValue(title);
        event.addBindValue(AppDatabase::text(artist));
        event.exec();
    }

    m_tracks.reload();
    reloadLiked();
    touch();
    Q_EMIT likesChanged();
    return true;
}

bool Library::isInLibrary(const QString &videoId) const
{
    return !videoId.isEmpty() && (m_likedIds.contains(videoId) || m_playlistSongIds.contains(videoId));
}

// Every copy of the song the user keeps: the like, and each playlist's entry
// for it, matched by video id. A downloaded file stays, and so does the
// library row that plays it; deleting a file is Remove download's to do.
void Library::removeFromLibrary(const QVariantMap &track)
{
    const QString videoId = trackVideoId(track);
    if (!isInLibrary(videoId))
        return;

    storeLike(track, false);

    QList<int> playlistIds;
    QSqlQuery which(AppDatabase::connection());
    which.prepare(QStringLiteral("SELECT DISTINCT playlist_id FROM playlist_tracks WHERE video_id = ?"));
    which.addBindValue(videoId);
    if (which.exec()) {
        while (which.next())
            playlistIds.append(which.value(0).toInt());
    }
    if (!playlistIds.isEmpty()) {
        QSqlQuery drop(AppDatabase::connection());
        drop.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE video_id = ?"));
        drop.addBindValue(videoId);
        if (!drop.exec())
            qWarning("Monolist: could not remove from playlists: %s", qPrintable(drop.lastError().text()));
        for (const int playlistId : std::as_const(playlistIds))
            markUpdated(playlistId);
        m_playlists.reload();
        if (playlistIds.contains(m_openPlaylistId))
            reloadOpenPlaylist();
    }
    reloadPlaylistSongs();
    Q_EMIT notice(QStringLiteral("Removed from your library"));
}

void Library::copyLink(const QString &url)
{
    if (url.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(url);
        Q_EMIT notice(QStringLiteral("Link copied"));
    }
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
        // A card from an artist's page or discography names nobody on its
        // own line ("Album • 2019"): whose page it was on (Catalog's owner).
        if (artist.isEmpty())
            artist = page.value(QStringLiteral("owner")).toString();
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
        "SELECT video_id, title, artist, album, artwork, duration_ms, is_video FROM recent"
        " ORDER BY played_at DESC, rowid DESC LIMIT 200"));
    m_history.replace(readSongs(q, /*withEntryId=*/false));
}

void Library::clearHistory()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("DELETE FROM recent"));
    q.exec(QStringLiteral("DELETE FROM history"));
    // And the stored visitor id, by which YouTube could still link what is
    // played next to what was just cleared.
    InnerTube::forgetVisitorData();
    // JioSaavn's remembered answers name every song played or queued next:
    // a cache, but one that would outlive the history it mirrors.
    q.exec(QStringLiteral("DELETE FROM saavn_matches"));
    reloadHistory();
    Q_EMIT historyChanged();
    Q_EMIT notice(QStringLiteral("History cleared"));
}

// As Clear history does for everything, for one song: its row in Recently
// played and the library's record of its plays. What the recommender learnt
// from them is kept, as it is by Clear history.
void Library::removeFromHistory(const QString &videoId)
{
    if (videoId.isEmpty())
        return;
    QSqlQuery recent(AppDatabase::connection());
    recent.prepare(QStringLiteral("DELETE FROM recent WHERE video_id = ?"));
    recent.addBindValue(videoId);
    if (!recent.exec() || recent.numRowsAffected() == 0)
        return;
    QSqlQuery plays(AppDatabase::connection());
    plays.prepare(QStringLiteral(
        "DELETE FROM history WHERE track_id IN (SELECT id FROM tracks WHERE source_id = ?)"));
    plays.addBindValue(videoId);
    plays.exec();
    reloadHistory();
    Q_EMIT historyChanged();
    Q_EMIT notice(QStringLiteral("Removed from your history"));
}
