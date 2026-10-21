#pragma once

#include <QByteArray>
#include <QList>
#include <QPointer>
#include <QString>
#include <QVariant>

class Library;
class Recommender;

// The user's library as one file, and that file merged back in: what
// Settings' Export and Import write and read, and what the Google Drive
// backup (DriveSync) merges on every device it reaches.
//
// The file, monolist-library.json (UTF-8, compact JSON), holds what is the
// user's own and means the same on any computer: liked songs, playlists and
// their songs, the albums and playlists saved from YouTube Music, every play
// and turn-down (play_events, the newest 100,000), and what was taken out on
// purpose (sync_removed, the tombstones). Never a download or any file path,
// a cache, the YouTube Music account's mirror (ytm_*), an import's state, the
// places opened lately, a setting, the profile's name, a secret, or anything
// that names the computer it was written on.
//
// A merge is a union with deletions. For each thing the newest of the two
// copies and its tombstone decide, and two copies as new as each other are
// told apart by their content, so either side picks the same one: merging
// both ways leaves both libraries the same, and merging the same file twice
// changes nothing the second time. It is applied in one transaction, and
// never deletes anything a newer tombstone in either library does not cover.
// A playlist's songs are merged as a set, not taken whole from the newer
// copy: a song added on one computer stays, whatever was changed on another,
// unless it was taken out (a tombstone of its own) after it was added.
//
// No time can be from the future of the computer reading it. One written by
// a clock that ran ahead would otherwise win every comparison until real time
// caught up (a Clear history dated tomorrow would empty History everywhere
// until then), so anything dated later than this computer's clock allows is
// left out of a file read and of a file written, and waits until it is due.
class LibraryBackup
{
public:
    static constexpr int kVersion = 1;
    // Plays kept in a file, the newest; turn-downs and likes always go.
    static constexpr int kMaxPlays = 100000;
    // Larger than any library this app could write (100,000 plays are about
    // 25 MB): a file this size is something else.
    static constexpr qint64 kMaxBytes = 128 * 1024 * 1024;
    // How far ahead of this computer's clock a time may be and still count:
    // two clocks that are each within DriveSync's five minutes of Google's
    // are at most ten apart. Anything later was written by a clock that is
    // wrong.
    static constexpr int kFutureSlackSecs = 10 * 60;

    // The tombstones' kinds (sync_removed.kind).
    static const QString kRemovedLike;
    static const QString kRemovedPlaylist;
    // A song taken out of a playlist; its key is entryKey()'s.
    static const QString kRemovedPlaylistEntry;
    static const QString kRemovedSaved;
    static const QString kRemovedHistory;
    static const QString kRemovedHistoryCleared;
    static const QString kRemovedTurnDown;

    // Times are the database's own ('YYYY-MM-DD HH:MM:SS', UTC) everywhere
    // here; the file has ISO-8601 with a Z. Empty means not known, which
    // counts as the oldest.
    struct Song {
        QString videoId;
        QString title;
        QString artist;
        QString album;
        QString artwork;           // a web address, or empty: never a path
        qint64 durationMs = 0;
        bool isVideo = false;
        QString at;                // liked (a like) or added (a playlist's)
    };
    struct Playlist {
        QString uid;
        QString name;
        bool pinned = false;
        QString createdAt;
        QString updatedAt;
        QList<Song> entries;       // in order
    };
    struct Saved {
        QString browseId;
        QString title;
        QString artist;
        QString year;
        QString format;            // ALBUM, SINGLE, EP or PLAYLIST
        QString artwork;
        QString savedAt;
    };
    struct Play {
        QString kind;              // play, like, unliked, notInterested, notInterestedArtist
        QString videoId;
        QString title;
        QString artist;
        QString startedAt;
        qint64 trackMs = 0;
        qint64 listenedMs = 0;
        bool completed = false;
        bool skipped = false;
        QVariant label;            // a double, or null: "not long enough to say"
        QString source;
        int repeatInSession = 0;
    };
    struct Removed {
        QString kind;
        QString key;
        QString at;
    };
    struct Data {
        QString writtenAt;
        QList<Song> likes;
        QList<Playlist> playlists;
        QList<Saved> saved;
        QList<Play> plays;
        QList<Removed> removed;
        // Items parse() left out for being dated later than this computer's
        // clock allows (kFutureSlackSecs): written by a clock that was ahead.
        int leftOut = 0;
    };

    // What a merge changed in this library.
    struct Counts {
        int likesAdded = 0;
        int likesRemoved = 0;
        int playlistsAdded = 0;
        int playlistsUpdated = 0;
        int playlistsRemoved = 0;
        int savedAdded = 0;
        int savedRemoved = 0;
        // Listens added (kind play), and the other rows of play_events: the
        // likes and unlikes the recommender learns from, and turn-downs.
        int playsAdded = 0;
        int eventsAdded = 0;
        // A play both had, given the other's later numbers (a listen that
        // was still going when that copy was made), and the like.
        int playsUpdated = 0;
        int turnDownsRemoved = 0;
        // Copies kept here that took the other side's details, as new as
        // each other: a like's title, a saved album's cover.
        int detailsUpdated = 0;
        int tombstonesAdded = 0;
        int historyRemoved = 0;
        int historyAdded = 0;
        // Not a change: what the file held that was dated later than now,
        // and so not brought in (Data::leftOut), for the toast to say.
        int futureLeftOut = 0;
        bool changed() const;
        // For the toast: "Imported: 3 playlists, 41 liked songs, 2,310 plays
        // added; 1 playlist changed", or that there was nothing new.
        QString summary() const;
        // For the log: counts only.
        QString logLine() const;
    };

    explicit LibraryBackup(Library *library);
    // Told of turn-downs a merge brings or takes away; optional.
    void setRecommender(Recommender *recommender);

    // The library as it is now, in the file's canonical order, without what
    // is dated later than this computer's clock allows.
    Data snapshot() const;
    // A file, as written: compact JSON.
    static QByteArray toJson(const Data &data);
    // What the file holds, without when or where it was written: two files
    // with the same hash hold the same library, so the Drive backup can tell
    // an upload that would change nothing.
    static QByteArray contentHash(const Data &data);
    // A file read and checked. False, with a sentence for the user, for
    // anything that is not a Monolist library, one from a newer Monolist or
    // a damaged one; items it cannot use are left out, keys it does not know
    // are ignored, and so is anything dated later than now allows (counted
    // in Data::leftOut): dropped, never moved to now, since a play's time is
    // what makes it itself, and a moved one would be another play at every
    // merge.
    static bool parse(const QByteArray &bytes, Data *data, QString *error);
    // `incoming` merged into this library, in one transaction, then the
    // library and the recommender told. False, with nothing changed and a
    // sentence for the user, when the database refused.
    bool merge(const Data &incoming, Counts *counts, QString *error);

    // Export and Import: the snapshot written to `path` (QSaveFile, so a
    // failure leaves any file there as it was), and a file read and merged.
    bool exportTo(const QString &path, Data *written, QString *error);
    bool importFrom(const QString &path, Counts *counts, QString *error);

    // — the tombstones, written by whatever takes something out —
    // Something taken out on purpose, now; the later time is kept, unless
    // the one there is dated later than now allows (this computer's clock
    // ran ahead once), which the user's taking it out now replaces.
    static void recordRemoval(const QString &kind, const QString &key);
    // A like or a save made in the very second it was undone: that
    // tombstone goes, since the newer has to be strictly later to win.
    static void withdrawRemoval(const QString &kind, const QString &key);
    // A turn-down's key: the same row on every device, by what it names and
    // when (a play_events row's id is only this database's). `kind` is
    // notInterested (by its video id, or its title and artist when it has
    // none) or notInterestedArtist (by the artist).
    static QString turnDownKey(const QString &kind, const QString &videoId, const QString &title,
                               const QString &artist, const QString &startedAt);
    // A song taken out of a playlist: the playlist's uid and the song's
    // video id. A song added to it later than this is in it again.
    static QString entryKey(const QString &playlistUid, const QString &videoId);

    // The database's time as the file has it, and back; empty when it
    // cannot be read.
    static QString toFileTime(const QString &databaseTime);
    static QString fromFileTime(const QString &fileTime);
    // The latest time that can be true now: this computer's clock, plus
    // kFutureSlackSecs, as the database writes times.
    static QString latestTrueTime();

private:
    Library *m_library = nullptr;
    QPointer<Recommender> m_recommender;
};
