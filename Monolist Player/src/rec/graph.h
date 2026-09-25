#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class QThread;

namespace Rec {

// `popularity` is the number of MusicBrainz user ratings, not listens. It
// ranks artists within a shard and says nothing about how often anyone plays
// them.
struct GraphArtist {
    QString mbid;
    QString name;
    // The artist's own region, which need not be the shard's: IN-HR.sqlite
    // holds IN-PB and IN-DL artists too, and every global row says "ZZ".
    QString region;
    int popularity = 0;
    double confidence = 0;
};

struct GraphTrack {
    QString recordingMbid;
    QString artistMbid;
    QString artistName;
    QString title;
    qint64 lengthMs = -1;   // -1 when the shard has NULL
    int popularity = 0;     // ratings again, per recording
};

struct GraphNeighbour {
    QString mbid;
    double score = 0;
    QString source;         // "listenbrainz" or "structural"
};

// The Explore graph: artists, their recordings and who sounds like whom, from
// the MusicBrainz + ListenBrainz shards in GraphData/. One SQLite file per chart
// country or subdivision (US, JP, IN-HR) plus global. Ported from
// ExploreGraphStore.swift, and it has to give the same rows in the same order:
// the same shard chain, the same SQL, the same binds.
//
// Read-only and in place. The shards are licensed for non-commercial use and
// ship exactly as they were built, so nothing here may write to them. Every
// connection is opened with SQLITE_OPEN_READONLY, which also means a missing
// file fails to open instead of being created empty. iOS copies the shards
// into Application Support on every launch because of a sandbox rule the
// desktop does not have.
//
// Lazy. open() only lists the directory. A shard is connected the first time a
// query needs it, so only the shards on the chains actually asked about are
// ever touched. The files can sit on a network share, where connecting all 72
// up front would pay a round trip per file for shards most sessions never read.
//
// ONE THREAD ONLY. A Qt SQL connection belongs to the thread that created it
// and refuses to work anywhere else. Construct, query and destroy a Graph on
// the same worker thread. That thread gives the serialisation the iOS actor
// gave. Nothing here is safe to call concurrently, and it must not run on the
// GUI thread either, because a cold shard read over a share can block.
//
// Failures read as "no data". A shard that will not open or a query that fails
// is skipped and the chain moves on, as on iOS, so a damaged file costs its own
// rows and nothing else.
class Graph
{
public:
    Graph();
    ~Graph();                                       // closes every connection it opened
    // Copying would duplicate connection names that are removed on destruction.
    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;

    // Records which <CODE>.sqlite files `directory` holds. Opens none of them.
    // False when there are none. Calling it again closes whatever the previous
    // directory had open.
    bool open(const QString &directory);
    bool isOpen() const;
    QStringList installedCodes() const;             // sorted

    // Shards to read for `region`, most specific first and global last.
    //
    // Callers stop as soon as they have enough rows, so this order decides what
    // the listener sees. When global led, its WHERE matched every artist it
    // holds, it filled every limit, and every region showed the same worldwide
    // list. Global stays at the back to top up a thin country shard.
    //
    // Installed subdivisions follow their country because nothing else reaches
    // them. The region picker offers countries only, so without this, India
    // never read the Haryana or Punjab shard.
    QStringList shardChain(const QString &region) const;

    // Up to `limit` distinct artists, most rated first within each shard, and
    // the walk stops as soon as it has `limit`. The global shard is queried as
    // "ZZ", which its WHERE treats as "everyone": it tops the country up and is
    // not filtered by it.
    QVector<GraphArtist> artists(const QString &region, int limit) const;

    // The first shard on the chain that has ANY tracks for the artist answers
    // alone. Later shards are not consulted, even when it returns fewer than
    // `limit`. That is iOS behaviour, kept so both give the same list.
    QVector<GraphTrack> tracks(const QString &artistMbid, const QString &region, int limit) const;

    // Similar artists through edges in either direction, with ListenBrainz
    // before structural and then by score within each shard.
    //
    // `limit` applies PER SHARD, not in total, exactly as on iOS. Every shard
    // on the chain adds up to `limit` new neighbours, so the result can be
    // longer than `limit` and is ordered shard by shard. Callers slice it:
    // GraphChannelProvider asks for 12 and uses 8.
    QVector<GraphNeighbour> neighbours(const QString &mbid, const QString &region, int limit) const;

    // First name any shard on the chain has for `mbid`. Null when none knows
    // the artist. Empty but not null when a shard has the row and no name,
    // which keeps Swift's nil and "" apart.
    QString artistName(const QString &mbid, const QString &region) const;

private:
    // Name of an open connection to `code`, connecting on first use. Empty when
    // the shard is not installed or will not open. A failed open is not
    // remembered, just as iOS retries on the next call.
    //
    // Only this name is kept between calls, never a QSqlDatabase or QSqlQuery.
    // Handles live only inside the query functions, so when the destructor
    // removes the connections nothing can still hold one, and Qt has no
    // "connection still in use" to warn about.
    QString connection(const QString &code) const;
    void closeAll();

    QString m_prefix;                               // unique to this instance
    bool m_open = false;
    QHash<QString, QString> m_files;                // code -> absolute path
    mutable QHash<QString, QString> m_connections;  // code -> connection name
    mutable QThread *m_thread = nullptr;            // whoever connected first
};

} // namespace Rec
