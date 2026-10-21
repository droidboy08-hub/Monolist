#include "librarybackup.h"

#include "appdatabase.h"
#include "library.h"
#include "recommender.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <functional>

const QString LibraryBackup::kRemovedLike = QStringLiteral("like");
const QString LibraryBackup::kRemovedPlaylist = QStringLiteral("playlist");
const QString LibraryBackup::kRemovedPlaylistEntry = QStringLiteral("playlistEntry");
const QString LibraryBackup::kRemovedSaved = QStringLiteral("saved");
const QString LibraryBackup::kRemovedHistory = QStringLiteral("history");
const QString LibraryBackup::kRemovedHistoryCleared = QStringLiteral("historyCleared");
const QString LibraryBackup::kRemovedTurnDown = QStringLiteral("turnDown");

namespace {

using Song = LibraryBackup::Song;
using Playlist = LibraryBackup::Playlist;
using Saved = LibraryBackup::Saved;
using Play = LibraryBackup::Play;
using Removed = LibraryBackup::Removed;
using Data = LibraryBackup::Data;

const QString kFormat = QStringLiteral("monolist-library");
const QString kPlay = QStringLiteral("play");
const QString kNotInterested = QStringLiteral("notInterested");
const QString kNotInterestedArtist = QStringLiteral("notInterestedArtist");

// What a refused file is answered with, in the toast.
QString notALibrary()
{
    return QStringLiteral("That file isn't a Monolist library, so nothing was brought in.");
}

QString tooNew()
{
    return QStringLiteral("That library was saved by a newer Monolist. Update Monolist, then bring it in again.");
}

QString damaged()
{
    return QStringLiteral("That Monolist library is damaged, so nothing was brought in.");
}

QString refusedByDatabase()
{
    return QStringLiteral("Monolist couldn't write that into your library, so nothing was changed.");
}

// The kinds play_events holds that mean the same on any device. Anything
// else in a file is from a Monolist that knows more, and is left out.
const QSet<QString> &playKinds()
{
    static const QSet<QString> kinds{ kPlay, QStringLiteral("like"), QStringLiteral("unliked"), kNotInterested,
                                      kNotInterestedArtist };
    return kinds;
}

const QSet<QString> &removedKinds()
{
    static const QSet<QString> kinds{ LibraryBackup::kRemovedLike, LibraryBackup::kRemovedPlaylist,
                                      LibraryBackup::kRemovedPlaylistEntry, LibraryBackup::kRemovedSaved,
                                      LibraryBackup::kRemovedHistory, LibraryBackup::kRemovedHistoryCleared,
                                      LibraryBackup::kRemovedTurnDown };
    return kinds;
}

bool isTurnDown(const QString &kind)
{
    return kind == kNotInterested || kind == kNotInterestedArtist;
}

QDateTime readDatabaseTime(const QString &text)
{
    // 'YYYY-MM-DD HH:MM:SS', as datetime('now') writes it; anything after
    // the seconds (a fraction) is not this app's and is dropped. An ISO time
    // ('T', and a zone) is the file's, read by fromFileTime.
    if (text.size() < 19 || text.at(10) != QLatin1Char(' '))
        return {};
    const QDate date = QDate::fromString(text.left(10), Qt::ISODate);
    const QTime time = QTime::fromString(text.mid(11, 8), QStringLiteral("HH:mm:ss"));
    if (!date.isValid() || !time.isValid())
        return {};
    return QDateTime(date, time, QTimeZone(QTimeZone::UTC));
}

QString databaseNow()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// A cover the file may carry: a web address. A downloaded song's cover can
// be a file on this computer, which means nothing anywhere else.
QString webAddress(const QString &artwork)
{
    return artwork.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
                   || artwork.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
               ? artwork
               : QString();
}

QString textOf(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

qint64 numberOf(const QJsonObject &object, const QString &key)
{
    const double value = object.value(key).toDouble();
    if (!std::isfinite(value) || value <= 0)
        return 0;
    return qint64(std::min(value, 9.0e15));
}

bool flagOf(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isBool() ? value.toBool() : value.toDouble() != 0;
}

// A label as play_events keeps it: a number, or null.
QVariant labelOf(const QVariant &value)
{
    if (value.isNull() || !value.isValid())
        return {};
    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok && std::isfinite(number) ? QVariant(number) : QVariant();
}

QJsonValue labelJson(const QVariant &label)
{
    return label.isNull() || !label.isValid() ? QJsonValue(QJsonValue::Null) : QJsonValue(label.toDouble());
}

// ------------------------------------------------------------ the file's form

QJsonObject songJson(const Song &song, const QString &atKey)
{
    return QJsonObject{
        { QStringLiteral("videoId"), song.videoId },
        { QStringLiteral("title"), song.title },
        { QStringLiteral("artist"), song.artist },
        { QStringLiteral("album"), song.album },
        { QStringLiteral("artwork"), song.artwork },
        { QStringLiteral("durationMs"), song.durationMs },
        { QStringLiteral("isVideo"), song.isVideo },
        { atKey, LibraryBackup::toFileTime(song.at) }
    };
}

QJsonObject playlistJson(const Playlist &playlist)
{
    QJsonArray entries;
    for (const Song &entry : playlist.entries)
        entries.append(songJson(entry, QStringLiteral("addedAt")));
    return QJsonObject{
        { QStringLiteral("uid"), playlist.uid },
        { QStringLiteral("name"), playlist.name },
        { QStringLiteral("pinned"), playlist.pinned },
        { QStringLiteral("createdAt"), LibraryBackup::toFileTime(playlist.createdAt) },
        { QStringLiteral("updatedAt"), LibraryBackup::toFileTime(playlist.updatedAt) },
        { QStringLiteral("entries"), entries }
    };
}

QJsonObject savedJson(const Saved &saved)
{
    return QJsonObject{
        { QStringLiteral("browseId"), saved.browseId },
        { QStringLiteral("title"), saved.title },
        { QStringLiteral("artist"), saved.artist },
        { QStringLiteral("year"), saved.year },
        { QStringLiteral("format"), saved.format },
        { QStringLiteral("artwork"), saved.artwork },
        { QStringLiteral("savedAt"), LibraryBackup::toFileTime(saved.savedAt) }
    };
}

QJsonObject playJson(const Play &play)
{
    return QJsonObject{
        { QStringLiteral("kind"), play.kind },
        { QStringLiteral("videoId"), play.videoId },
        { QStringLiteral("title"), play.title },
        { QStringLiteral("artist"), play.artist },
        { QStringLiteral("startedAt"), LibraryBackup::toFileTime(play.startedAt) },
        { QStringLiteral("trackMs"), play.trackMs },
        { QStringLiteral("listenedMs"), play.listenedMs },
        { QStringLiteral("completed"), play.completed },
        { QStringLiteral("skipped"), play.skipped },
        { QStringLiteral("label"), labelJson(play.label) },
        { QStringLiteral("source"), play.source },
        { QStringLiteral("repeatInSession"), play.repeatInSession }
    };
}

QJsonObject removedJson(const Removed &removed)
{
    return QJsonObject{
        { QStringLiteral("kind"), removed.kind },
        { QStringLiteral("key"), removed.key },
        { QStringLiteral("at"), LibraryBackup::toFileTime(removed.at) }
    };
}

// The library itself, without the header: what contentHash() hashes.
QJsonObject sectionsJson(const Data &data)
{
    QJsonArray likes, playlists, saved, plays, removed;
    for (const Song &song : data.likes)
        likes.append(songJson(song, QStringLiteral("likedAt")));
    for (const Playlist &playlist : data.playlists)
        playlists.append(playlistJson(playlist));
    for (const Saved &item : data.saved)
        saved.append(savedJson(item));
    for (const Play &play : data.plays)
        plays.append(playJson(play));
    for (const Removed &item : data.removed)
        removed.append(removedJson(item));
    return QJsonObject{
        { QStringLiteral("likes"), likes },
        { QStringLiteral("playlists"), playlists },
        { QStringLiteral("saved"), saved },
        { QStringLiteral("plays"), plays },
        { QStringLiteral("removed"), removed }
    };
}

// Each thing's content, as the file has it, for telling apart two copies as
// new as each other: QJsonObject keeps its keys sorted, so the same content
// is always the same bytes, on either device.
QByteArray formOf(const Song &song)
{
    return QJsonDocument(songJson(song, QStringLiteral("at"))).toJson(QJsonDocument::Compact);
}

QByteArray formOf(const Playlist &playlist)
{
    return QJsonDocument(playlistJson(playlist)).toJson(QJsonDocument::Compact);
}

QByteArray formOf(const Saved &saved)
{
    return QJsonDocument(savedJson(saved)).toJson(QJsonDocument::Compact);
}

// Whether their copy takes the place of mine: the later time, and between
// two as new as each other the greater content, so that both devices, each
// asking this the other way round, keep the same one.
bool replaces(const QString &theirsAt, const QString &mineAt, const QByteArray &theirs, const QByteArray &mine)
{
    return theirsAt != mineAt ? theirsAt > mineAt : theirs > mine;
}

// Whether a time can be true yet: not later than LibraryBackup::latestTrueTime()
// (`ceiling`). Not known ('') is the oldest, and always can.
bool due(const QString &at, const QString &ceiling)
{
    return at <= ceiling;
}

// One playlist from two copies of it, the same whichever device asks.
//
// Its songs are a set. A song either copy has stays, unless it was taken out
// of this playlist (a playlistEntry tombstone, `removedAt`) later than it was
// last added: so a song added on one computer is never lost to an edit made
// on another that had not seen it yet (a rename, a pin, another song), and a
// song taken out does not come back from a copy that still had it. Of two
// copies of one song, the one added last is kept, with its time, since that
// time is what keeps it in.
//
// The name, the pin and the order are the newer copy's (`winner`, the one
// replaces() picks), and a song only the other copy has goes in just after
// the song it followed there (or first, if it was first): what was added at
// the end on two computers ends up at the end on both.
Playlist mergedPlaylist(const Playlist &winner, const Playlist *other,
                        const std::function<QString(const QString &videoId)> &removedAt)
{
    QHash<QString, Song> latest;
    const auto offer = [&latest](const Song &song) {
        const auto it = latest.find(song.videoId);
        if (it == latest.end())
            latest.insert(song.videoId, song);
        else if (replaces(song.at, it->at, formOf(song), formOf(*it)))
            *it = song;
    };
    for (const Song &entry : winner.entries)
        offer(entry);
    if (other) {
        for (const Song &entry : other->entries)
            offer(entry);
    }
    const auto kept = [&](const QString &videoId) {
        const QString removed = removedAt(videoId);
        return removed.isEmpty() || latest.value(videoId).at > removed;
    };

    QStringList order;
    QSet<QString> placed;
    for (const Song &entry : winner.entries) {
        if (placed.contains(entry.videoId) || !kept(entry.videoId))
            continue;
        order << entry.videoId;
        placed.insert(entry.videoId);
    }
    if (other) {
        QString previous;   // the song before, in the other copy's order, that is placed
        for (const Song &entry : other->entries) {
            if (placed.contains(entry.videoId)) {
                previous = entry.videoId;
                continue;
            }
            if (!kept(entry.videoId))
                continue;
            order.insert(previous.isEmpty() ? 0 : order.indexOf(previous) + 1, entry.videoId);
            placed.insert(entry.videoId);
            previous = entry.videoId;
        }
    }

    Playlist merged = winner;
    merged.entries.clear();
    merged.entries.reserve(order.size());
    for (const QString &videoId : std::as_const(order))
        merged.entries.append(latest.value(videoId));
    return merged;
}

// A play is the same row on every device by what it was and when; a row
// with no video id (a song known by its name only) is told by the name too.
QString identityOf(const Play &play)
{
    QString identity = play.kind + QLatin1Char('\x1f') + play.videoId + QLatin1Char('\x1f') + play.startedAt;
    if (play.videoId.isEmpty())
        identity += QLatin1Char('\x1f') + play.title + QLatin1Char('\x1f') + play.artist;
    return identity;
}

QString pickText(const QString &a, const QString &b)
{
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    return std::max(a, b);
}

// Two copies of one play: whatever either heard, the most of it. A listen is
// written as it starts and finished later (its playhead, its label), so one
// copy can be older than the other, never different. Symmetric, so both
// devices end with the same row.
Play mergedPlay(const Play &a, const Play &b)
{
    Play merged = a;
    merged.title = pickText(a.title, b.title);
    merged.artist = pickText(a.artist, b.artist);
    merged.source = pickText(a.source, b.source);
    merged.trackMs = std::max(a.trackMs, b.trackMs);
    merged.listenedMs = std::max(a.listenedMs, b.listenedMs);
    merged.completed = a.completed || b.completed;
    merged.skipped = a.skipped || b.skipped;
    merged.repeatInSession = std::max(a.repeatInSession, b.repeatInSession);
    if (a.label.isNull())
        merged.label = b.label;
    else if (!b.label.isNull())
        merged.label = std::max(a.label.toDouble(), b.label.toDouble());
    return merged;
}

bool samePlay(const Play &a, const Play &b)
{
    const bool sameLabel = a.label.isNull() == b.label.isNull()
                           && (a.label.isNull() || a.label.toDouble() == b.label.toDouble());
    return a.title == b.title && a.artist == b.artist && a.source == b.source && a.trackMs == b.trackMs
           && a.listenedMs == b.listenedMs && a.completed == b.completed && a.skipped == b.skipped
           && a.repeatInSession == b.repeatInSession && sameLabel;
}

QString removedId(const QString &kind, const QString &key)
{
    return kind + QLatin1Char('\n') + key;
}

// One copy of each thing, the one a merge would keep, in an order that
// depends on nothing but the content: a library written twice, or by two
// devices holding the same, is the same file.
void canonicalise(Data &data)
{
    {
        QHash<QString, Song> byId;
        for (const Song &song : std::as_const(data.likes)) {
            auto it = byId.find(song.videoId);
            if (it == byId.end())
                byId.insert(song.videoId, song);
            else if (replaces(song.at, it->at, formOf(song), formOf(*it)))
                *it = song;
        }
        data.likes = byId.values();
        std::sort(data.likes.begin(), data.likes.end(), [](const Song &a, const Song &b) {
            return a.at != b.at ? a.at > b.at : a.videoId < b.videoId;
        });
    }
    {
        QHash<QString, Playlist> byUid;
        for (const Playlist &playlist : std::as_const(data.playlists)) {
            auto it = byUid.find(playlist.uid);
            if (it == byUid.end())
                byUid.insert(playlist.uid, playlist);
            else if (replaces(playlist.updatedAt, it->updatedAt, formOf(playlist), formOf(*it)))
                *it = playlist;
        }
        data.playlists = byUid.values();
        std::sort(data.playlists.begin(), data.playlists.end(), [](const Playlist &a, const Playlist &b) {
            return a.createdAt != b.createdAt ? a.createdAt < b.createdAt : a.uid < b.uid;
        });
    }
    {
        QHash<QString, Saved> byId;
        for (const Saved &saved : std::as_const(data.saved)) {
            auto it = byId.find(saved.browseId);
            if (it == byId.end())
                byId.insert(saved.browseId, saved);
            else if (replaces(saved.savedAt, it->savedAt, formOf(saved), formOf(*it)))
                *it = saved;
        }
        data.saved = byId.values();
        std::sort(data.saved.begin(), data.saved.end(), [](const Saved &a, const Saved &b) {
            return a.savedAt != b.savedAt ? a.savedAt > b.savedAt : a.browseId < b.browseId;
        });
    }
    {
        QHash<QString, Play> byIdentity;
        byIdentity.reserve(data.plays.size());
        for (const Play &play : std::as_const(data.plays)) {
            const QString identity = identityOf(play);
            auto it = byIdentity.find(identity);
            if (it == byIdentity.end())
                byIdentity.insert(identity, play);
            else
                *it = mergedPlay(*it, play);
        }
        data.plays = byIdentity.values();
        // By time, then by what makes each one itself, field by field: a
        // file can hold 100,000, and building their identities over again
        // for every comparison would cost seconds.
        std::sort(data.plays.begin(), data.plays.end(), [](const Play &a, const Play &b) {
            if (a.startedAt != b.startedAt)
                return a.startedAt < b.startedAt;
            if (a.kind != b.kind)
                return a.kind < b.kind;
            if (a.videoId != b.videoId)
                return a.videoId < b.videoId;
            if (a.title != b.title)
                return a.title < b.title;
            return a.artist < b.artist;
        });
    }
    {
        QHash<QString, Removed> byId;
        for (const Removed &removed : std::as_const(data.removed)) {
            const QString id = removedId(removed.kind, removed.key);
            auto it = byId.find(id);
            if (it == byId.end())
                byId.insert(id, removed);
            else if (removed.at > it->at)
                it->at = removed.at;
        }
        data.removed = byId.values();
        std::sort(data.removed.begin(), data.removed.end(), [](const Removed &a, const Removed &b) {
            return a.kind != b.kind ? a.kind < b.kind : a.key < b.key;
        });
    }
}

// --------------------------------------------------------- the library read

// Columns: video id, title, artist, album, artwork, duration, is_video, time.
Song songFrom(const QSqlQuery &query, int first = 0)
{
    Song song;
    song.videoId = query.value(first).toString();
    song.title = query.value(first + 1).toString();
    song.artist = query.value(first + 2).toString();
    song.album = query.value(first + 3).toString();
    song.artwork = webAddress(query.value(first + 4).toString());
    song.durationMs = std::max<qint64>(0, query.value(first + 5).toLongLong());
    song.isVideo = query.value(first + 6).toInt() != 0;
    song.at = LibraryBackup::fromFileTime(query.value(first + 7).toString());
    return song;
}

// Columns: kind, video id, title, artist, started, track, listened,
// completed, skipped, label, source, repeat.
Play playFrom(const QSqlQuery &query, int first = 0)
{
    Play play;
    play.kind = query.value(first).toString();
    play.videoId = query.value(first + 1).toString();
    play.title = query.value(first + 2).toString();
    play.artist = query.value(first + 3).toString();
    play.startedAt = LibraryBackup::fromFileTime(query.value(first + 4).toString());
    play.trackMs = std::max<qint64>(0, query.value(first + 5).toLongLong());
    play.listenedMs = std::max<qint64>(0, query.value(first + 6).toLongLong());
    play.completed = query.value(first + 7).toInt() != 0;
    play.skipped = query.value(first + 8).toInt() != 0;
    play.label = labelOf(query.value(first + 9));
    play.source = query.value(first + 10).toString();
    play.repeatInSession = query.value(first + 11).toInt();
    return play;
}

const QString kPlayColumns = QStringLiteral(
    "kind, video_id, title, artist, started_at, track_ms, listened_ms, completed, skipped, label, source,"
    " repeat_in_session");

struct LocalPlaylist {
    int id = 0;
    Playlist playlist;
    QList<int> entryIds;           // beside playlist.entries
};

QList<LocalPlaylist> readPlaylists(QSqlDatabase db)
{
    QList<LocalPlaylist> lists;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT id, uid, name, pinned, created_at, updated_at FROM playlists WHERE uid <> ''")))
        return lists;
    while (query.next()) {
        LocalPlaylist local;
        local.id = query.value(0).toInt();
        local.playlist.uid = query.value(1).toString();
        local.playlist.name = query.value(2).toString();
        local.playlist.pinned = query.value(3).toInt() != 0;
        local.playlist.createdAt = LibraryBackup::fromFileTime(query.value(4).toString());
        local.playlist.updatedAt = LibraryBackup::fromFileTime(query.value(5).toString());
        lists.append(local);
    }
    QSqlQuery entries(db);
    entries.prepare(QStringLiteral(
        "SELECT video_id, title, artist, album, artwork, duration_ms, is_video, added_at, id FROM playlist_tracks"
        " WHERE playlist_id = ? AND video_id <> '' ORDER BY position ASC, id ASC"));
    for (LocalPlaylist &local : lists) {
        entries.addBindValue(local.id);
        if (!entries.exec())
            continue;
        while (entries.next()) {
            local.playlist.entries.append(songFrom(entries));
            local.entryIds.append(entries.value(8).toInt());
        }
    }
    return lists;
}

// The statements a merge writes with, and whether every one has worked: the
// first failure stops the rest, and the whole merge is rolled back.
class Writer
{
public:
    explicit Writer(QSqlDatabase db)
        : m_db(db) {}

    QSqlQuery prepare(const QString &sql)
    {
        QSqlQuery query(m_db);
        if (!query.prepare(sql))
            fail(query);
        return query;
    }

    bool exec(QSqlQuery &query)
    {
        if (!m_ok)
            return false;
        if (!query.exec())
            fail(query);
        return m_ok;
    }

    bool ok() const { return m_ok; }
    QString failure() const { return m_failure; }

private:
    void fail(const QSqlQuery &query)
    {
        if (m_ok)
            m_failure = query.lastError().text();
        m_ok = false;
    }

    QSqlDatabase m_db;
    bool m_ok = true;
    QString m_failure;
};

void bindSong(QSqlQuery &query, const Song &song)
{
    query.addBindValue(AppDatabase::text(song.title));
    query.addBindValue(AppDatabase::text(song.artist));
    query.addBindValue(AppDatabase::text(song.album));
    query.addBindValue(AppDatabase::text(song.artwork));
    query.addBindValue(song.durationMs);
    query.addBindValue(song.isVideo ? 1 : 0);
}

QString plural(int count, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(QLocale().toString(count),
                                       QString::fromUtf8(count == 1 ? one : many));
}

} // namespace

// ----------------------------------------------------------------- Counts

bool LibraryBackup::Counts::changed() const
{
    return likesAdded || likesRemoved || playlistsAdded || playlistsUpdated || playlistsRemoved || savedAdded
           || savedRemoved || playsAdded || eventsAdded || playsUpdated || turnDownsRemoved || detailsUpdated
           || tombstonesAdded || historyRemoved || historyAdded;
}

QString LibraryBackup::Counts::summary() const
{
    // What was added, what was changed, and what was taken out, each said:
    // a playlist that only took songs from the file is not a new one, and a
    // History emptied by a clearing made elsewhere is named, not left to be
    // found.
    QStringList added;
    if (playlistsAdded > 0)
        added << plural(playlistsAdded, "playlist", "playlists");
    if (likesAdded > 0)
        added << plural(likesAdded, "liked song", "liked songs");
    if (savedAdded > 0)
        added << plural(savedAdded, "saved album or playlist", "saved albums and playlists");
    if (playsAdded > 0)
        added << plural(playsAdded, "play", "plays");
    QStringList removed;
    if (playlistsRemoved > 0)
        removed << plural(playlistsRemoved, "playlist", "playlists");
    if (likesRemoved > 0)
        removed << plural(likesRemoved, "liked song", "liked songs");
    if (savedRemoved > 0)
        removed << plural(savedRemoved, "saved album or playlist", "saved albums and playlists");
    if (historyRemoved > 0)
        removed << plural(historyRemoved, "song from History", "songs from History");

    QStringList parts;
    if (!added.isEmpty())
        parts << added.join(QStringLiteral(", ")) + QStringLiteral(" added");
    if (playlistsUpdated > 0)
        parts << plural(playlistsUpdated, "playlist", "playlists") + QStringLiteral(" changed");
    if (!removed.isEmpty())
        parts << removed.join(QStringLiteral(", ")) + QStringLiteral(" removed, as they were deleted since");
    // Left out, rather than brought in at a time that cannot be true yet:
    // said, since the cause is a clock the user can set right.
    const QString clock = QStringLiteral(" dated later than now (that computer's clock may be wrong)");
    if (!parts.isEmpty()) {
        if (futureLeftOut > 0) {
            parts << plural(futureLeftOut, "thing", "things")
                         + (futureLeftOut == 1 ? QStringLiteral(" left out, as it was") : QStringLiteral(" left out, as they were"))
                         + clock;
        }
        return QStringLiteral("Imported: ") + parts.join(QStringLiteral("; "));
    }
    if (futureLeftOut > 0) {
        return QStringLiteral("Nothing was brought in: %1 in that file %2%3")
            .arg(plural(futureLeftOut, "thing", "things"), futureLeftOut == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                 clock);
    }
    return changed() ? QStringLiteral("Imported: your library is up to date with that file")
                     : QStringLiteral("Nothing new: your library already has everything in that file");
}

QString LibraryBackup::Counts::logLine() const
{
    return QStringLiteral("likes +%1 -%2, playlists +%3 ~%4 -%5, saved +%6 -%7, plays +%8 ~%9, other events +%10,"
                          " turn-downs -%11, details %12, tombstones +%13, history +%14 -%15, left out (future) %16")
        .arg(likesAdded).arg(likesRemoved).arg(playlistsAdded).arg(playlistsUpdated).arg(playlistsRemoved)
        .arg(savedAdded).arg(savedRemoved).arg(playsAdded).arg(playsUpdated).arg(eventsAdded)
        .arg(turnDownsRemoved).arg(detailsUpdated).arg(tombstonesAdded).arg(historyAdded).arg(historyRemoved)
        .arg(futureLeftOut);
}

// ------------------------------------------------------------------ times

QString LibraryBackup::toFileTime(const QString &databaseTime)
{
    const QDateTime when = readDatabaseTime(databaseTime);
    return when.isValid() ? when.toString(Qt::ISODate) : QString();
}

QString LibraryBackup::fromFileTime(const QString &fileTime)
{
    QDateTime when = readDatabaseTime(fileTime);
    if (!when.isValid() && !fileTime.isEmpty()) {
        when = QDateTime::fromString(fileTime, Qt::ISODateWithMs);
        // A time with no zone is UTC, as everything here is.
        if (when.isValid() && when.timeSpec() == Qt::LocalTime)
            when.setTimeZone(QTimeZone(QTimeZone::UTC));
    }
    return when.isValid() ? when.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
}

QString LibraryBackup::latestTrueTime()
{
    return QDateTime::currentDateTimeUtc().addSecs(kFutureSlackSecs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// ------------------------------------------------------------- tombstones

void LibraryBackup::recordRemoval(const QString &kind, const QString &key)
{
    // The later of the two is kept, as merges keep it; but one dated later
    // than now allows came from this computer's clock running ahead, and
    // would win over everything the user does until real time caught up,
    // so taking the thing out again now puts it at now.
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT INTO sync_removed (kind, key, at) VALUES (?, ?, datetime('now'))"
        " ON CONFLICT(kind, key) DO UPDATE SET at = CASE WHEN sync_removed.at > ? THEN excluded.at"
        " ELSE MAX(sync_removed.at, excluded.at) END"));
    query.addBindValue(kind);
    query.addBindValue(AppDatabase::text(key));
    query.addBindValue(latestTrueTime());
    if (!query.exec())
        qWarning("backup: could not keep a %s as taken out: %s", qPrintable(kind), qPrintable(query.lastError().text()));
}

void LibraryBackup::withdrawRemoval(const QString &kind, const QString &key)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("DELETE FROM sync_removed WHERE kind = ? AND key = ? AND at >= datetime('now')"));
    query.addBindValue(kind);
    query.addBindValue(AppDatabase::text(key));
    query.exec();
}

QString LibraryBackup::turnDownKey(const QString &kind, const QString &videoId, const QString &title,
                                   const QString &artist, const QString &startedAt)
{
    const QString when = toFileTime(fromFileTime(startedAt));
    if (kind == kNotInterestedArtist)
        return QStringLiteral("artist:") + artist + QLatin1Char('|') + when;
    if (videoId.isEmpty())
        return QStringLiteral("song:") + title + QLatin1Char('|') + artist + QLatin1Char('|') + when;
    return videoId + QLatin1Char('|') + when;
}

QString LibraryBackup::entryKey(const QString &playlistUid, const QString &videoId)
{
    return playlistUid + QLatin1Char('|') + videoId;
}

// ---------------------------------------------------------- LibraryBackup

LibraryBackup::LibraryBackup(Library *library)
    : m_library(library) {}

void LibraryBackup::setRecommender(Recommender *recommender)
{
    m_recommender = recommender;
}

LibraryBackup::Data LibraryBackup::snapshot() const
{
    Data data;
    data.writtenAt = databaseNow();
    // What this computer's clock wrote while it ran ahead is kept here, but
    // goes in no file until it is due: another computer would leave it out
    // all the same, and the two would never hold the same library.
    const QString ceiling = latestTrueTime();

    QSqlDatabase db = AppDatabase::connection();
    QSqlQuery query(db);
    // A row of tracks is a like, a download, or both: only the likes, and
    // never the file (source_url).
    if (query.exec(QStringLiteral(
            "SELECT source_id, title, artist, album, artwork, duration_ms, is_video, liked_at FROM tracks"
            " WHERE favourite = 1 AND source_id <> ''"))) {
        while (query.next()) {
            const Song song = songFrom(query);
            if (due(song.at, ceiling))
                data.likes.append(song);
        }
    }
    // The order in the sidebar is this computer's (position): not kept.
    for (const LocalPlaylist &local : readPlaylists(db)) {
        if (!due(local.playlist.updatedAt, ceiling) || !due(local.playlist.createdAt, ceiling))
            continue;
        Playlist playlist = local.playlist;
        playlist.entries.removeIf([&ceiling](const Song &entry) { return !due(entry.at, ceiling); });
        data.playlists.append(playlist);
    }
    if (query.exec(QStringLiteral(
            "SELECT browse_id, title, artist, year, format, artwork, saved_at FROM albums WHERE browse_id <> ''"))) {
        while (query.next()) {
            Saved saved;
            saved.browseId = query.value(0).toString();
            saved.title = query.value(1).toString();
            saved.artist = query.value(2).toString();
            saved.year = query.value(3).toString();
            saved.format = query.value(4).toString();
            saved.artwork = webAddress(query.value(5).toString());
            saved.savedAt = fromFileTime(query.value(6).toString());
            if (due(saved.savedAt, ceiling))
                data.saved.append(saved);
        }
    }
    // Every turn-down and like there is, which are few and say the most;
    // then the plays, the newest, up to kMaxPlays in all.
    if (query.exec(QStringLiteral("SELECT %1 FROM play_events WHERE kind <> 'play'").arg(kPlayColumns))) {
        while (query.next()) {
            const Play play = playFrom(query);
            if (playKinds().contains(play.kind) && !play.startedAt.isEmpty() && due(play.startedAt, ceiling))
                data.plays.append(play);
        }
    }
    query.prepare(QStringLiteral("SELECT %1 FROM play_events WHERE kind = 'play' AND started_at <= ?"
                                 " ORDER BY started_at DESC, id DESC LIMIT ?").arg(kPlayColumns));
    query.addBindValue(ceiling);
    query.addBindValue(std::max<qsizetype>(0, kMaxPlays - data.plays.size()));
    if (query.exec()) {
        while (query.next()) {
            const Play play = playFrom(query);
            if (!play.startedAt.isEmpty())
                data.plays.append(play);
        }
    }
    if (query.exec(QStringLiteral("SELECT kind, key, at FROM sync_removed"))) {
        while (query.next()) {
            Removed removed{ query.value(0).toString(), query.value(1).toString(),
                             fromFileTime(query.value(2).toString()) };
            if (removedKinds().contains(removed.kind) && !removed.at.isEmpty() && due(removed.at, ceiling))
                data.removed.append(removed);
        }
    }
    canonicalise(data);
    return data;
}

QByteArray LibraryBackup::toJson(const Data &data)
{
    // Nothing that names the computer: the file goes to the user's Drive and
    // wherever they put an export, and no reader has any use for a name.
    QJsonObject root = sectionsJson(data);
    root.insert(QStringLiteral("format"), kFormat);
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("writtenAt"), toFileTime(data.writtenAt));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray LibraryBackup::contentHash(const Data &data)
{
    return QCryptographicHash::hash(QJsonDocument(sectionsJson(data)).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256)
        .toHex();
}

bool LibraryBackup::parse(const QByteArray &bytes, Data *data, QString *error)
{
    const auto refuse = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (bytes.size() > kMaxBytes)
        return refuse(notALibrary());
    QByteArray text = bytes;
    if (text.startsWith("\xEF\xBB\xBF"))
        text.remove(0, 3);    // a byte-order mark, which an editor may add
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return refuse(notALibrary());
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != kFormat)
        return refuse(notALibrary());
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() < 1 || version.toDouble() != std::floor(version.toDouble()))
        return refuse(notALibrary());
    if (version.toDouble() > kVersion)
        return refuse(tooNew());
    const QStringList sections{ QStringLiteral("likes"), QStringLiteral("playlists"), QStringLiteral("saved"),
                                QStringLiteral("plays"), QStringLiteral("removed") };
    for (const QString &section : sections) {
        if (root.contains(section) && !root.value(section).isArray())
            return refuse(damaged());
    }

    Data parsed;
    parsed.writtenAt = fromFileTime(textOf(root, QStringLiteral("writtenAt")));
    // Anything dated later than this computer's clock allows was written by
    // a clock that ran ahead, and would win every comparison until real time
    // caught up, then hold on to that win: left out, and counted.
    const QString ceiling = latestTrueTime();
    const auto future = [&ceiling, &parsed](const QString &at) {
        if (due(at, ceiling))
            return false;
        ++parsed.leftOut;
        return true;
    };

    const auto songOf = [](const QJsonObject &object, const QString &atKey) {
        Song song;
        song.videoId = textOf(object, QStringLiteral("videoId"));
        song.title = textOf(object, QStringLiteral("title"));
        song.artist = textOf(object, QStringLiteral("artist"));
        song.album = textOf(object, QStringLiteral("album"));
        song.artwork = webAddress(textOf(object, QStringLiteral("artwork")));
        song.durationMs = numberOf(object, QStringLiteral("durationMs"));
        song.isVideo = flagOf(object, QStringLiteral("isVideo"));
        song.at = fromFileTime(textOf(object, atKey));
        return song;
    };

    for (const QJsonValue &value : root.value(QStringLiteral("likes")).toArray()) {
        const Song song = songOf(value.toObject(), QStringLiteral("likedAt"));
        if (!song.videoId.isEmpty() && !future(song.at))
            parsed.likes.append(song);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("playlists")).toArray()) {
        const QJsonObject object = value.toObject();
        Playlist playlist;
        playlist.uid = textOf(object, QStringLiteral("uid"));
        if (playlist.uid.isEmpty())
            continue;
        playlist.name = textOf(object, QStringLiteral("name"));
        playlist.pinned = flagOf(object, QStringLiteral("pinned"));
        playlist.createdAt = fromFileTime(textOf(object, QStringLiteral("createdAt")));
        playlist.updatedAt = fromFileTime(textOf(object, QStringLiteral("updatedAt")));
        // A playlist changed "later" than now would be the newer copy at
        // every merge, its name, pin and order winning over every edit.
        if (future(playlist.updatedAt) || future(playlist.createdAt))
            continue;
        for (const QJsonValue &entry : object.value(QStringLiteral("entries")).toArray()) {
            const Song song = songOf(entry.toObject(), QStringLiteral("addedAt"));
            if (!song.videoId.isEmpty() && !future(song.at))
                playlist.entries.append(song);
        }
        parsed.playlists.append(playlist);
    }
    static const QSet<QString> formats{ QStringLiteral("ALBUM"), QStringLiteral("SINGLE"), QStringLiteral("EP"),
                                        QStringLiteral("PLAYLIST") };
    for (const QJsonValue &value : root.value(QStringLiteral("saved")).toArray()) {
        const QJsonObject object = value.toObject();
        Saved saved;
        saved.browseId = textOf(object, QStringLiteral("browseId"));
        if (saved.browseId.isEmpty())
            continue;
        saved.title = textOf(object, QStringLiteral("title"));
        saved.artist = textOf(object, QStringLiteral("artist"));
        saved.year = textOf(object, QStringLiteral("year"));
        saved.format = textOf(object, QStringLiteral("format")).toUpper();
        if (!formats.contains(saved.format))
            saved.format = QStringLiteral("ALBUM");
        saved.artwork = webAddress(textOf(object, QStringLiteral("artwork")));
        saved.savedAt = fromFileTime(textOf(object, QStringLiteral("savedAt")));
        if (!future(saved.savedAt))
            parsed.saved.append(saved);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("plays")).toArray()) {
        const QJsonObject object = value.toObject();
        Play play;
        play.kind = textOf(object, QStringLiteral("kind"));
        play.startedAt = fromFileTime(textOf(object, QStringLiteral("startedAt")));
        // A play is told apart from another by its time, so one without is
        // no play at all.
        if (!playKinds().contains(play.kind) || play.startedAt.isEmpty() || future(play.startedAt))
            continue;
        play.videoId = textOf(object, QStringLiteral("videoId"));
        play.title = textOf(object, QStringLiteral("title"));
        play.artist = textOf(object, QStringLiteral("artist"));
        play.trackMs = numberOf(object, QStringLiteral("trackMs"));
        play.listenedMs = numberOf(object, QStringLiteral("listenedMs"));
        play.completed = flagOf(object, QStringLiteral("completed"));
        play.skipped = flagOf(object, QStringLiteral("skipped"));
        const QJsonValue label = object.value(QStringLiteral("label"));
        play.label = label.isDouble() && std::isfinite(label.toDouble()) ? QVariant(label.toDouble()) : QVariant();
        play.source = textOf(object, QStringLiteral("source"));
        play.repeatInSession = flagOf(object, QStringLiteral("repeatInSession")) ? 1 : 0;
        parsed.plays.append(play);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("removed")).toArray()) {
        const QJsonObject object = value.toObject();
        Removed removed{ textOf(object, QStringLiteral("kind")), textOf(object, QStringLiteral("key")),
                         fromFileTime(textOf(object, QStringLiteral("at"))) };
        if (!removedKinds().contains(removed.kind) || removed.at.isEmpty())
            continue;
        if (removed.kind == kRemovedHistoryCleared)
            removed.key.clear();
        else if (removed.key.isEmpty())
            continue;
        // A deletion dated "tomorrow" would take away every like, play and
        // History row up to then, again and again: it waits until it is due.
        if (future(removed.at))
            continue;
        parsed.removed.append(removed);
    }
    canonicalise(parsed);
    if (data)
        *data = parsed;
    return true;
}

bool LibraryBackup::merge(const Data &given, Counts *countsOut, QString *error)
{
    Data incoming = given;
    canonicalise(incoming);
    Counts counts;
    QSqlDatabase db = AppDatabase::connection();
    if (!db.transaction()) {
        qWarning("backup: the library could not begin a merge: %s", qPrintable(db.lastError().text()));
        if (error)
            *error = refusedByDatabase();
        return false;
    }
    Writer writer(db);

    // — what was taken out: the union, the later time of each —
    // One this library holds dated later than now allows (written while this
    // computer's clock ran ahead) decides nothing here: it would unlike a
    // song liked since on another computer, or keep History empty, until
    // real time caught up. It stays until it is due, unless the other copy
    // has one for the same thing that can be true, which takes its place.
    const QString ceiling = latestTrueTime();
    QHash<QString, QString> removedAt;
    // The tombstones this merge brought or made later: History is cleared
    // by these only, once, so that one already applied here does not take
    // away again, at every merge, what has been played here since.
    QSet<QString> raised;
    {
        QSqlQuery read(db);
        if (read.exec(QStringLiteral("SELECT kind, key, at FROM sync_removed"))) {
            while (read.next()) {
                const QString at = fromFileTime(read.value(2).toString());
                if (due(at, ceiling))
                    removedAt.insert(removedId(read.value(0).toString(), read.value(1).toString()), at);
            }
        }
        QSqlQuery keep = writer.prepare(QStringLiteral(
            "INSERT INTO sync_removed (kind, key, at) VALUES (?, ?, ?)"
            " ON CONFLICT(kind, key) DO UPDATE SET at = excluded.at"
            " WHERE excluded.at > sync_removed.at OR sync_removed.at > ?"));
        for (const Removed &removed : std::as_const(incoming.removed)) {
            const QString id = removedId(removed.kind, removed.key);
            // parse() leaves out the other side's future ones; a Data made
            // here (a self-test's) is held to the same.
            if (removed.at <= removedAt.value(id) || !due(removed.at, ceiling))
                continue;
            keep.addBindValue(removed.kind);
            keep.addBindValue(AppDatabase::text(removed.key));
            keep.addBindValue(removed.at);
            keep.addBindValue(ceiling);
            writer.exec(keep);
            removedAt.insert(id, removed.at);
            raised.insert(id);
            ++counts.tombstonesAdded;
        }
    }
    const auto tombstone = [&removedAt](const QString &kind, const QString &key) {
        return removedAt.value(removedId(kind, key));
    };

    // — likes: the newest of the two and the unlike decides —
    {
        struct Track {
            bool liked = false;
            bool kept = false;     // a row there, liked or a download's
            Song song;
        };
        QHash<QString, Track> tracks;
        QSqlQuery read(db);
        if (read.exec(QStringLiteral(
                "SELECT source_id, title, artist, album, artwork, duration_ms, is_video, liked_at, favourite"
                " FROM tracks WHERE source_id <> ''"))) {
            while (read.next()) {
                Track &track = tracks[read.value(0).toString()];
                track.kept = true;
                if (read.value(8).toInt() != 0 && !track.liked) {
                    track.liked = true;
                    track.song = songFrom(read);
                }
            }
        }
        QHash<QString, const Song *> theirs;
        for (const Song &song : std::as_const(incoming.likes))
            theirs.insert(song.videoId, &song);
        QStringList ids = theirs.keys();
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it) {
            if (it->liked && !theirs.contains(it.key()))
                ids << it.key();
        }
        std::sort(ids.begin(), ids.end());

        QSqlQuery update = writer.prepare(QStringLiteral(
            "UPDATE tracks SET favourite = 1, liked_at = ?, title = ?, artist = ?, album = ?, artwork = ?,"
            " duration_ms = ?, is_video = ? WHERE source_id = ?"));
        QSqlQuery insert = writer.prepare(QStringLiteral(
            "INSERT INTO tracks (position, liked_at, title, artist, album, artwork, duration_ms, is_video,"
            " source_id, source_url, favourite)"
            " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, ?, ?, ?, ?, ?, ?, '', 1"));
        // As Library::storeLike unlikes: the row goes unless it plays a file.
        QSqlQuery drop = writer.prepare(QStringLiteral("DELETE FROM tracks WHERE source_id = ? AND source_url = ''"));
        QSqlQuery keepFile = writer.prepare(QStringLiteral(
            "UPDATE tracks SET favourite = 0, liked_at = '' WHERE source_id = ?"));
        for (const QString &id : std::as_const(ids)) {
            const Track track = tracks.value(id);
            const Song *mine = track.liked ? &track.song : nullptr;
            const Song *their = theirs.value(id);
            const bool theirsWins = their && (!mine || replaces(their->at, mine->at, formOf(*their), formOf(*mine)));
            const Song &winner = theirsWins ? *their : *mine;
            const QString removed = tombstone(kRemovedLike, id);
            // A like outlives its unlike only by being strictly later.
            if (removed.isEmpty() || winner.at > removed) {
                if (!theirsWins)
                    continue;
                QSqlQuery &write = track.kept ? update : insert;
                write.addBindValue(AppDatabase::text(winner.at));
                bindSong(write, winner);
                write.addBindValue(id);
                writer.exec(write);
                if (mine)
                    ++counts.detailsUpdated;
                else
                    ++counts.likesAdded;
            } else if (mine) {
                drop.addBindValue(id);
                writer.exec(drop);
                keepFile.addBindValue(id);
                writer.exec(keepFile);
                ++counts.likesRemoved;
            }
        }
    }

    // — saved albums and playlists, the same way —
    {
        QHash<QString, Saved> mine;
        QSqlQuery read(db);
        if (read.exec(QStringLiteral(
                "SELECT browse_id, title, artist, year, format, artwork, saved_at FROM albums WHERE browse_id <> ''"))) {
            while (read.next()) {
                Saved saved;
                saved.browseId = read.value(0).toString();
                saved.title = read.value(1).toString();
                saved.artist = read.value(2).toString();
                saved.year = read.value(3).toString();
                saved.format = read.value(4).toString();
                saved.artwork = webAddress(read.value(5).toString());
                saved.savedAt = fromFileTime(read.value(6).toString());
                mine.insert(saved.browseId, saved);
            }
        }
        QHash<QString, const Saved *> theirs;
        for (const Saved &saved : std::as_const(incoming.saved))
            theirs.insert(saved.browseId, &saved);
        QStringList ids = theirs.keys();
        for (auto it = mine.cbegin(); it != mine.cend(); ++it) {
            if (!theirs.contains(it.key()))
                ids << it.key();
        }
        std::sort(ids.begin(), ids.end());

        QSqlQuery update = writer.prepare(QStringLiteral(
            "UPDATE albums SET title = ?, artist = ?, year = ?, format = ?, artwork = ?, saved_at = ?"
            " WHERE browse_id = ?"));
        QSqlQuery insert = writer.prepare(QStringLiteral(
            "INSERT INTO albums (position, title, artist, year, format, artwork, saved_at, browse_id)"
            " VALUES (0, ?, ?, ?, ?, ?, ?, ?)"));
        QSqlQuery drop = writer.prepare(QStringLiteral("DELETE FROM albums WHERE browse_id = ?"));
        for (const QString &id : std::as_const(ids)) {
            const auto found = mine.constFind(id);
            const Saved *my = found != mine.cend() ? &found.value() : nullptr;
            const Saved *their = theirs.value(id);
            const bool theirsWins = their && (!my || replaces(their->savedAt, my->savedAt, formOf(*their), formOf(*my)));
            const Saved &winner = theirsWins ? *their : *my;
            const QString removed = tombstone(kRemovedSaved, id);
            if (removed.isEmpty() || winner.savedAt > removed) {
                if (!theirsWins)
                    continue;
                QSqlQuery &write = my ? update : insert;
                write.addBindValue(AppDatabase::text(winner.title));
                write.addBindValue(AppDatabase::text(winner.artist));
                write.addBindValue(AppDatabase::text(winner.year));
                write.addBindValue(AppDatabase::text(winner.format));
                write.addBindValue(AppDatabase::text(winner.artwork));
                write.addBindValue(AppDatabase::text(winner.savedAt));
                write.addBindValue(id);
                writer.exec(write);
                if (my)
                    ++counts.detailsUpdated;
                else
                    ++counts.savedAdded;
            } else if (my) {
                drop.addBindValue(id);
                writer.exec(drop);
                ++counts.savedRemoved;
            }
        }
    }

    // — playlists, by uid: their songs as a set, the rest the newer's, unless
    // deleted since —
    {
        QHash<QString, LocalPlaylist> mine;
        for (const LocalPlaylist &local : readPlaylists(db))
            mine.insert(local.playlist.uid, local);
        QHash<QString, const Playlist *> theirs;
        QStringList uids;
        for (const Playlist &playlist : std::as_const(incoming.playlists)) {
            theirs.insert(playlist.uid, &playlist);
            uids << playlist.uid;  // in the file's order, which new ones keep
        }
        QStringList onlyMine;
        for (auto it = mine.cbegin(); it != mine.cend(); ++it) {
            if (!theirs.contains(it.key()))
                onlyMine << it.key();
        }
        std::sort(onlyMine.begin(), onlyMine.end());
        uids += onlyMine;

        QSqlQuery dropEntries = writer.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE playlist_id = ?"));
        QSqlQuery dropList = writer.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
        // A playlist new here goes at the end of the sidebar, below the rest.
        QSqlQuery addList = writer.prepare(QStringLiteral(
            "INSERT INTO playlists (position, name, track_count, pinned, created_at, updated_at, uid)"
            " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM playlists), ?, 0, ?, ?, ?, ?"));
        QSqlQuery updateList = writer.prepare(QStringLiteral(
            "UPDATE playlists SET name = ?, pinned = ?, created_at = ?, updated_at = ? WHERE id = ?"));
        QSqlQuery addEntry = writer.prepare(QStringLiteral(
            "INSERT INTO playlist_tracks (title, artist, album, artwork, duration_ms, is_video, added_at,"
            " position, video_id, playlist_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        QSqlQuery updateEntry = writer.prepare(QStringLiteral(
            "UPDATE playlist_tracks SET title = ?, artist = ?, album = ?, artwork = ?, duration_ms = ?,"
            " is_video = ?, added_at = ?, position = ? WHERE id = ?"));
        QSqlQuery dropEntry = writer.prepare(QStringLiteral("DELETE FROM playlist_tracks WHERE id = ?"));

        for (const QString &uid : std::as_const(uids)) {
            const auto found = mine.constFind(uid);
            const LocalPlaylist *my = found != mine.cend() ? &found.value() : nullptr;
            const Playlist *their = theirs.value(uid);
            const QString newest = std::max(my ? my->playlist.updatedAt : QString(),
                                            their ? their->updatedAt : QString());
            const QString removed = tombstone(kRemovedPlaylist, uid);
            // Deleted after the last change either side knows of: gone, as
            // Library::deletePlaylist leaves it.
            if (!removed.isEmpty() && removed >= newest) {
                if (my) {
                    dropEntries.addBindValue(my->id);
                    writer.exec(dropEntries);
                    dropList.addBindValue(my->id);
                    writer.exec(dropList);
                    ++counts.playlistsRemoved;
                }
                continue;
            }
            // Both copies made one (mergedPlaylist): the newer's name, pin
            // and order, and every song either has that was not taken out
            // since it was added. One copy alone still loses the songs the
            // other's tombstones took out.
            const Playlist *winner = their;
            const Playlist *other = my ? &my->playlist : nullptr;
            if (!their || (my && !replaces(their->updatedAt, my->playlist.updatedAt, formOf(*their),
                                           formOf(my->playlist)))) {
                winner = &my->playlist;
                other = their;
            }
            const Playlist merged = mergedPlaylist(*winner, other, [&](const QString &videoId) {
                return tombstone(kRemovedPlaylistEntry, entryKey(uid, videoId));
            });
            if (my && formOf(merged) == formOf(my->playlist))
                continue;   // this library has it as both copies make it

            int playlistId = 0;
            if (!my) {
                addList.addBindValue(AppDatabase::text(merged.name));
                addList.addBindValue(merged.pinned ? 1 : 0);
                addList.addBindValue(AppDatabase::text(merged.createdAt));
                addList.addBindValue(AppDatabase::text(merged.updatedAt));
                addList.addBindValue(uid);
                if (!writer.exec(addList))
                    break;
                playlistId = addList.lastInsertId().toInt();
                ++counts.playlistsAdded;
            } else {
                playlistId = my->id;
                updateList.addBindValue(AppDatabase::text(merged.name));
                updateList.addBindValue(merged.pinned ? 1 : 0);
                updateList.addBindValue(AppDatabase::text(merged.createdAt));
                updateList.addBindValue(AppDatabase::text(merged.updatedAt));
                updateList.addBindValue(playlistId);
                writer.exec(updateList);
                ++counts.playlistsUpdated;
            }
            // The songs in their order, each with when it was added. A song
            // this playlist already holds keeps its row, so its entry id (what
            // an open playlist's rows name) stays the same.
            QHash<QString, QList<int>> rows;
            if (my) {
                for (int i = 0; i < my->playlist.entries.size(); ++i)
                    rows[my->playlist.entries.at(i).videoId].append(my->entryIds.at(i));
            }
            for (int position = 0; position < merged.entries.size(); ++position) {
                const Song &entry = merged.entries.at(position);
                QList<int> &reusable = rows[entry.videoId];
                QSqlQuery &write = reusable.isEmpty() ? addEntry : updateEntry;
                bindSong(write, entry);
                write.addBindValue(AppDatabase::text(entry.at));
                write.addBindValue(position);
                if (reusable.isEmpty()) {
                    write.addBindValue(entry.videoId);
                    write.addBindValue(playlistId);
                } else {
                    write.addBindValue(reusable.takeFirst());
                }
                writer.exec(write);
            }
            for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
                for (const int id : it.value()) {
                    dropEntry.addBindValue(id);
                    writer.exec(dropEntry);
                }
            }
        }
    }

    // — plays: the union, each once —
    QHash<QString, QList<const Play *>> newPlays;   // by video id, for History
    {
        struct LocalPlay {
            qint64 id = 0;
            Play play;
        };
        QHash<QString, QList<LocalPlay>> mine;
        QSqlQuery read(db);
        read.setForwardOnly(true);
        if (read.exec(QStringLiteral("SELECT id, %1 FROM play_events").arg(kPlayColumns))) {
            while (read.next()) {
                LocalPlay local{ read.value(0).toLongLong(), playFrom(read, 1) };
                if (!local.play.startedAt.isEmpty())
                    mine[identityOf(local.play)].append(local);
            }
        }
        QSqlQuery insert = writer.prepare(QStringLiteral(
            "INSERT INTO play_events (kind, video_id, title, artist, started_at, track_ms, listened_ms,"
            " completed, skipped, label, source, repeat_in_session) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        QSqlQuery update = writer.prepare(QStringLiteral(
            "UPDATE play_events SET title = ?, artist = ?, track_ms = ?, listened_ms = ?, completed = ?,"
            " skipped = ?, label = ?, source = ?, repeat_in_session = ? WHERE id = ?"));
        QSqlQuery drop = writer.prepare(QStringLiteral("DELETE FROM play_events WHERE id = ?"));
        // A turn-down taken back on either side, once it was made.
        const auto undone = [&tombstone](const Play &play) {
            if (!isTurnDown(play.kind))
                return false;
            const QString removed = tombstone(kRemovedTurnDown,
                                              turnDownKey(play.kind, play.videoId, play.title, play.artist,
                                                          play.startedAt));
            return !removed.isEmpty() && removed >= play.startedAt;
        };

        for (const Play &play : std::as_const(incoming.plays)) {
            if (undone(play))
                continue;
            const auto found = mine.find(identityOf(play));
            if (found != mine.end()) {
                for (LocalPlay &local : *found) {
                    const Play merged = mergedPlay(local.play, play);
                    if (samePlay(merged, local.play))
                        continue;
                    update.addBindValue(AppDatabase::text(merged.title));
                    update.addBindValue(AppDatabase::text(merged.artist));
                    update.addBindValue(merged.trackMs);
                    update.addBindValue(merged.listenedMs);
                    update.addBindValue(merged.completed ? 1 : 0);
                    update.addBindValue(merged.skipped ? 1 : 0);
                    update.addBindValue(merged.label);
                    update.addBindValue(AppDatabase::text(merged.source));
                    update.addBindValue(merged.repeatInSession);
                    update.addBindValue(local.id);
                    writer.exec(update);
                    local.play = merged;
                    ++counts.playsUpdated;
                }
                continue;
            }
            insert.addBindValue(play.kind);
            insert.addBindValue(AppDatabase::text(play.videoId));
            insert.addBindValue(AppDatabase::text(play.title));
            insert.addBindValue(AppDatabase::text(play.artist));
            insert.addBindValue(play.startedAt);
            insert.addBindValue(play.trackMs);
            insert.addBindValue(play.listenedMs);
            insert.addBindValue(play.completed ? 1 : 0);
            insert.addBindValue(play.skipped ? 1 : 0);
            insert.addBindValue(play.label);
            insert.addBindValue(AppDatabase::text(play.source));
            insert.addBindValue(play.repeatInSession);
            writer.exec(insert);
            if (play.kind != kPlay) {
                ++counts.eventsAdded;
                continue;
            }
            ++counts.playsAdded;
            if (!play.videoId.isEmpty())
                newPlays[play.videoId].append(&play);
        }
        for (auto it = mine.cbegin(); it != mine.cend(); ++it) {
            for (const LocalPlay &local : it.value()) {
                if (!undone(local.play))
                    continue;
                drop.addBindValue(local.id);
                writer.exec(drop);
                ++counts.turnDownsRemoved;
            }
        }
    }

    // — History, as the plays have it —
    // What History shows (recent) is not in the file: it is the plays, one
    // row a song. Clearing it, or taking a song out, is kept as a tombstone,
    // so it is cleared here too, and plays from before it stay out. Only by
    // a tombstone this merge brought (`raised`): one already here was
    // applied when it came, and History played here since is this library's.
    {
        const QString clearedId = removedId(kRemovedHistoryCleared, QString());
        const QString cleared = tombstone(kRemovedHistoryCleared, QString());
        if (!cleared.isEmpty() && raised.contains(clearedId)) {
            QSqlQuery clear = writer.prepare(QStringLiteral("DELETE FROM recent WHERE played_at <= ?"));
            clear.addBindValue(cleared);
            if (writer.exec(clear))
                counts.historyRemoved += clear.numRowsAffected();
            QSqlQuery legacy = writer.prepare(QStringLiteral("DELETE FROM history WHERE played_at <= ?"));
            legacy.addBindValue(cleared);
            writer.exec(legacy);
        }
        QSqlQuery forget = writer.prepare(QStringLiteral("DELETE FROM recent WHERE video_id = ? AND played_at <= ?"));
        const QString prefix = kRemovedHistory + QLatin1Char('\n');
        for (const QString &id : std::as_const(raised)) {
            if (!id.startsWith(prefix))
                continue;
            forget.addBindValue(id.mid(prefix.size()));
            forget.addBindValue(removedAt.value(id));
            if (writer.exec(forget))
                counts.historyRemoved += forget.numRowsAffected();
        }

        // The song's row as recordHistory keeps it: its last play and how
        // many, its details from the newest play. A row there already only
        // takes them when the play is newer than its own.
        QSqlQuery played = writer.prepare(QStringLiteral(
            "INSERT INTO recent (video_id, title, artist, duration_ms, played_at, play_count)"
            " VALUES (?, ?, ?, ?, ?, ?)"
            " ON CONFLICT(video_id) DO UPDATE SET"
            "   title = CASE WHEN excluded.played_at > recent.played_at AND excluded.title <> ''"
            "                THEN excluded.title ELSE recent.title END,"
            "   artist = CASE WHEN excluded.played_at > recent.played_at AND excluded.artist <> ''"
            "                 THEN excluded.artist ELSE recent.artist END,"
            "   duration_ms = CASE WHEN excluded.played_at > recent.played_at AND excluded.duration_ms > 0"
            "                      THEN excluded.duration_ms ELSE recent.duration_ms END,"
            "   played_at = MAX(recent.played_at, excluded.played_at),"
            "   play_count = recent.play_count + excluded.play_count"));
        QStringList videos = newPlays.keys();
        std::sort(videos.begin(), videos.end());
        for (const QString &videoId : std::as_const(videos)) {
            const QString gone = std::max(cleared, tombstone(kRemovedHistory, videoId));
            const Play *newest = nullptr;
            int count = 0;
            for (const Play *play : newPlays.value(videoId)) {
                if (play->startedAt <= gone)
                    continue;
                ++count;
                if (!newest || play->startedAt > newest->startedAt)
                    newest = play;
            }
            if (!newest)
                continue;
            played.addBindValue(videoId);
            played.addBindValue(AppDatabase::text(newest->title));
            played.addBindValue(AppDatabase::text(newest->artist));
            played.addBindValue(newest->trackMs);
            played.addBindValue(newest->startedAt);
            played.addBindValue(count);
            writer.exec(played);
            ++counts.historyAdded;
        }
    }

    if (!writer.ok() || !db.commit()) {
        qWarning("backup: the merge was refused and rolled back: %s",
                 qPrintable(writer.ok() ? db.lastError().text() : writer.failure()));
        db.rollback();
        if (error)
            *error = refusedByDatabase();
        return false;
    }
    qInfo("backup: merged: %s", qPrintable(counts.logLine()));
    if (counts.changed()) {
        if (m_library)
            m_library->reloadAll();
        if (m_recommender)
            m_recommender->reloadTurnedDown();
    }
    if (countsOut)
        *countsOut = counts;
    return true;
}

bool LibraryBackup::exportTo(const QString &path, Data *written, QString *error)
{
    const Data data = snapshot();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Couldn't save the file there: %1").arg(file.errorString());
        return false;
    }
    file.write(toJson(data));
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Couldn't save the file there: %1").arg(file.errorString());
        return false;
    }
    qInfo("backup: exported %d playlists, %d liked songs, %d saved, %d plays and turn-downs, %d tombstones",
          int(data.playlists.size()), int(data.likes.size()), int(data.saved.size()), int(data.plays.size()),
          int(data.removed.size()));
    if (written)
        *written = data;
    return true;
}

bool LibraryBackup::importFrom(const QString &path, Counts *counts, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Couldn't open that file: %1").arg(file.errorString());
        return false;
    }
    if (file.size() > kMaxBytes) {
        if (error)
            *error = notALibrary();
        return false;
    }
    Data data;
    if (!parse(file.readAll(), &data, error)) {
        qInfo("backup: a file was refused for import");
        return false;
    }
    if (data.leftOut > 0)
        qInfo("backup: %d things in the file were dated later than now and were left out", data.leftOut);
    if (!merge(data, counts, error))
        return false;
    if (counts)
        counts->futureLeftOut = data.leftOut;
    return true;
}
