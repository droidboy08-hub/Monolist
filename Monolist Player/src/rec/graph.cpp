#include "graph.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <atomic>

namespace Rec {
namespace {

// Qt binds a null QString as SQL NULL, and `a = NULL` is never true. A caller
// passing a default-constructed mbid would then match nothing, where Swift,
// which has no null string, bound '' and matched whatever '' matches.
QString text(const QString &value)
{
    return value.isNull() ? QStringLiteral("") : value;
}

// Swift's split(separator:) drops empty pieces, so "-HR" means country "HR"
// there. Qt keeps empty pieces unless it is told not to.
QString countryOf(const QString &code)
{
    const QStringList parts = code.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? code : parts.first();
}

QString globalCode() { return QStringLiteral("global"); }

// An SQLite URI for a file, written by hand. QUrl::fromLocalFile turns a UNC
// path like //psf/Home/x into file://psf/Home/x — "psf" as a host — and SQLite
// refuses any host but localhost. Four slashes keep the authority empty and
// hand SQLite the //psf/Home/x it can open; a drive path gets the usual three.
// Only the three characters SQLite's URI parser gives meaning to are escaped.
QString sqliteUri(const QString &file)
{
    QString path = QDir::fromNativeSeparators(file);
    path.replace(QLatin1Char('%'), QStringLiteral("%25"));
    path.replace(QLatin1Char('?'), QStringLiteral("%3f"));
    path.replace(QLatin1Char('#'), QStringLiteral("%23"));
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));        // Z:/x -> /Z:/x
    return QStringLiteral("file://") + path;   // file:///Z:/x, file:////psf/x
}

// Connection names are process-wide. Two Graphs alive at once (a test beside
// the app's, or a replacement built before the old one is gone) must not share
// one, because a second addDatabase under the same name replaces the first
// connection under its owner.
std::atomic<quint64> g_instances{0};

} // namespace

// ------------------------------------------------------------------- lifetime

Graph::Graph()
    : m_prefix(QStringLiteral("rec.graph.%1.").arg(++g_instances))
{
}

Graph::~Graph()
{
    closeAll();
}

bool Graph::open(const QString &directory)
{
    closeAll();
    m_files.clear();

    const QDir dir(directory);
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("*.sqlite")},
                                                    QDir::Files | QDir::Readable);
    // completeBaseName, because iOS drops only the last extension; the shipped
    // names have one either way.
    for (const QFileInfo &info : entries)
        m_files.insert(info.completeBaseName(), info.absoluteFilePath());

    m_open = !m_files.isEmpty();
    return m_open;
}

bool Graph::isOpen() const
{
    return m_open;
}

QStringList Graph::installedCodes() const
{
    QStringList codes = m_files.keys();
    std::sort(codes.begin(), codes.end());
    return codes;
}

void Graph::closeAll()
{
    for (const QString &name : std::as_const(m_connections))
        QSqlDatabase::removeDatabase(name);
    m_connections.clear();
    m_thread = nullptr;
}

QString Graph::connection(const QString &code) const
{
    Q_ASSERT_X(!m_thread || m_thread == QThread::currentThread(), "Rec::Graph",
               "a Graph must be used on the thread that first queried it");

    const auto known = m_connections.constFind(code);
    if (known != m_connections.constEnd())
        return known.value();

    // Only files open() listed. Swift likewise looks the file up before
    // opening, and a read-only open of a missing file would fail anyway.
    const auto file = m_files.constFind(code);
    if (file == m_files.constEnd())
        return QString();

    const QString name = m_prefix + code;
    bool opened = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        // Three options, each closing a hole the review measured:
        //
        // READONLY alone does not stop SQLite writing beside the file: a shard
        // saved in WAL mode gets a -shm and a -wal created next to it and left
        // behind, which breaks the promise that nothing in GraphData is ever
        // touched. `immutable=1` tells SQLite the file cannot change, so it
        // opens no journal and takes no locks at all — and needs a URI.
        //
        // BUSY_TIMEOUT=0 because Qt's driver otherwise waits five seconds on a
        // locked file, per query. A shard held by another program stalled one
        // "Popular in" read for minutes; Swift gives up at once and moves down
        // the chain, and so does this now.
        db.setConnectOptions(QStringLiteral(
            "QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI;QSQLITE_BUSY_TIMEOUT=0"));
        db.setDatabaseName(sqliteUri(file.value()) + QStringLiteral("?immutable=1"));
        opened = db.open();
    }
    if (!opened) {
        QSqlDatabase::removeDatabase(name);
        return QString();
    }

    m_connections.insert(code, name);
    if (!m_thread)
        m_thread = QThread::currentThread();
    return name;
}

// ---------------------------------------------------------------------- chain

QStringList Graph::shardChain(const QString &region) const
{
    const QString code = region.toUpper();
    if (code == QLatin1String("ZZ") || code.isEmpty())
        return {globalCode()};

    const bool subdivision = code.contains(QLatin1Char('-'));
    const QString country = subdivision ? countryOf(code) : code;
    QStringList chain = subdivision ? QStringList{code, country} : QStringList{code};

    const QString prefix = country + QLatin1Char('-');
    QStringList extra;
    for (auto it = m_files.constBegin(); it != m_files.constEnd(); ++it) {
        if (it.key().startsWith(prefix) && !chain.contains(it.key()))
            extra.append(it.key());
    }
    std::sort(extra.begin(), extra.end());
    chain += extra;
    chain.append(globalCode());
    return chain;
}

// -------------------------------------------------------------------- queries

QVector<GraphArtist> Graph::artists(const QString &region, int limit) const
{
    QVector<GraphArtist> out;
    QSet<QString> seen;
    const QString wanted = region.toUpper();

    for (const QString &shard : shardChain(region)) {
        const QString name = connection(shard);
        if (name.isEmpty())
            continue;

        QSqlQuery q(QSqlDatabase::database(name, false));
        q.setForwardOnly(true);
        if (!q.prepare(QStringLiteral(
                "SELECT mbid, name, region, popularity, confidence"
                " FROM artists"
                " WHERE (? = 'ZZ')"
                " OR region = ?"
                " OR region LIKE (? || '-%')"
                " ORDER BY popularity DESC"
                " LIMIT ?")))
            continue;

        // The country bind is what lets "IN" reach "IN-HR" rows inside the
        // Indian shard, and a subdivision's country-wide rows inside its own.
        const QString bind = shard == globalCode() ? QStringLiteral("ZZ") : wanted;
        q.addBindValue(text(bind));
        q.addBindValue(text(bind));
        q.addBindValue(text(countryOf(bind)));
        q.addBindValue(limit);
        if (!q.exec())
            continue;

        while (q.next()) {
            const QString mbid = q.value(0).toString();
            if (!seen.contains(mbid)) {
                seen.insert(mbid);
                GraphArtist a;
                a.mbid = mbid;
                a.name = q.value(1).toString();
                a.region = q.value(2).toString();
                a.popularity = q.value(3).toInt();
                a.confidence = q.value(4).toDouble();
                out.append(a);
            }
            // Checked after every row, duplicates included, as iOS does, so
            // the walk ends at exactly the same row there and here.
            if (out.size() >= limit)
                return out;
        }
    }
    return out;
}

QVector<GraphNeighbour> Graph::neighbours(const QString &mbid, const QString &region, int limit) const
{
    QVector<GraphNeighbour> out;
    QSet<QString> seen;

    for (const QString &shard : shardChain(region)) {
        const QString name = connection(shard);
        if (name.isEmpty())
            continue;

        QSqlQuery q(QSqlDatabase::database(name, false));
        q.setForwardOnly(true);
        // An edge can hold the artist at either end, so both ends are read.
        // ListenBrainz edges go first because they come from what people play
        // together. Structural ones come from MusicBrainz links and shared tags
        // and nearly all score 0.38, so their score barely ranks them.
        if (!q.prepare(QStringLiteral(
                "SELECT CASE WHEN a = ? THEN b ELSE a END, score, source"
                " FROM edges"
                " WHERE a = ? OR b = ?"
                " ORDER BY CASE source WHEN 'listenbrainz' THEN 0 ELSE 1 END, score DESC"
                " LIMIT ?")))
            continue;

        q.addBindValue(text(mbid));
        q.addBindValue(text(mbid));
        q.addBindValue(text(mbid));
        q.addBindValue(limit);
        if (!q.exec())
            continue;

        while (q.next()) {
            const QString other = q.value(0).toString();
            if (seen.contains(other))
                continue;
            seen.insert(other);
            GraphNeighbour n;
            n.mbid = other;
            n.score = q.value(1).toDouble();
            n.source = q.value(2).toString();
            out.append(n);
        }
    }
    return out;
}

QVector<GraphTrack> Graph::tracks(const QString &artistMbid, const QString &region, int limit) const
{
    // From the whole chain and before any tracks, as iOS does, so every track
    // carries the name iOS would show even when another shard has the tracks.
    const QString artist = artistName(artistMbid, region);

    for (const QString &shard : shardChain(region)) {
        const QString name = connection(shard);
        if (name.isEmpty())
            continue;

        QSqlQuery q(QSqlDatabase::database(name, false));
        q.setForwardOnly(true);
        if (!q.prepare(QStringLiteral(
                "SELECT recording_mbid, artist_mbid, title, length_ms, popularity"
                " FROM tracks WHERE artist_mbid = ?"
                " ORDER BY popularity DESC LIMIT ?")))
            continue;

        q.addBindValue(text(artistMbid));
        q.addBindValue(limit);
        if (!q.exec())
            continue;

        QVector<GraphTrack> rows;
        while (q.next()) {
            GraphTrack t;
            t.recordingMbid = q.value(0).toString();
            t.artistMbid = q.value(1).toString();
            t.artistName = artist;
            t.title = q.value(2).toString();
            // Kept apart from 0: the resolver skips its +-15 s duration check
            // only when the length is unknown (TrackResolver.pick).
            t.lengthMs = q.isNull(3) ? -1 : q.value(3).toLongLong();
            t.popularity = q.value(4).toInt();
            rows.append(t);
        }
        if (!rows.isEmpty())
            return rows;
    }
    return {};
}

QString Graph::artistName(const QString &mbid, const QString &region) const
{
    for (const QString &shard : shardChain(region)) {
        const QString name = connection(shard);
        if (name.isEmpty())
            continue;

        QSqlQuery q(QSqlDatabase::database(name, false));
        q.setForwardOnly(true);
        if (!q.prepare(QStringLiteral("SELECT name FROM artists WHERE mbid = ? LIMIT 1")))
            continue;
        q.addBindValue(text(mbid));
        if (!q.exec())
            continue;
        if (q.next())
            return text(q.value(0).toString());
    }
    return QString();
}

GraphArtist Graph::findArtist(const QString &name, const QString &region) const
{
    const QString wanted = name.trimmed();
    if (wanted.isEmpty())
        return {};
    const QString cacheKey = wanted.toLower();
    const auto cached = m_found.constFind(cacheKey);
    if (cached != m_found.constEnd())
        return cached.value();

    // The listener's own country first, then every other country, and the
    // worldwide shard LAST — the opposite of shardChain, which puts it at the
    // end only because it is a top-up there.
    //
    // It matters here because an artist's edges are read from the shard they
    // were found in. A. R. Rahman is in the global shard as well as India's,
    // and found in global first his neighbours were Linkin Park and Hans
    // Zimmer; the global shard's edges are the generic worldwide ones. Found
    // in India's, they are India's. A fixed order also means the same name
    // always resolves to the same row.
    QStringList order;
    for (const QString &code : shardChain(region)) {
        if (code != QLatin1String("global"))
            order.append(code);
    }
    for (const QString &code : installedCodes()) {
        if (code != QLatin1String("global") && !order.contains(code))
            order.append(code);
    }
    order.append(QStringLiteral("global"));

    // Every country's copy is read, and the copy with the most edges in its
    // own shard wins; the worldwide shard only when no country has them.
    //
    // Why edges and not confidence. One artist is one MBID, but each shard
    // holds only the edges between artists that shard contains, so the copy
    // chosen decides which country the neighbours come from. Utada Hikaru is
    // in the US shard (born in New York, confidence 0.90) and the Japanese one
    // (0.80): confidence picked the US copy and a US listener got Linkin Park
    // and Evanescence. By edges it is 24 in Japan against 15 — the shard where
    // an artist's listening neighbourhood is richest is the one that knows
    // them. Ties go to higher confidence, then to the earlier shard in the
    // order above, so the listener's own country wins an even contest.
    struct Candidate {
        GraphArtist artist;
        int edges = 0;
    };
    const auto copyIn = [&](const QString &shard, Candidate *out) {
        const QString connectionName = connection(shard);
        if (connectionName.isEmpty())
            return false;
        const QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.prepare(QStringLiteral(
                "SELECT mbid, name, region, popularity, confidence FROM artists"
                " WHERE name = ? COLLATE NOCASE ORDER BY confidence DESC, popularity DESC, mbid LIMIT 1")))
            return false;
        q.addBindValue(wanted);
        if (!q.exec() || !q.next())
            return false;
        out->artist.mbid = text(q.value(0).toString());
        out->artist.name = text(q.value(1).toString());
        out->artist.region = text(q.value(2).toString());
        out->artist.popularity = q.value(3).toInt();
        out->artist.confidence = q.value(4).toDouble();

        QSqlQuery count(db);
        count.setForwardOnly(true);
        if (count.prepare(QStringLiteral("SELECT COUNT(*) FROM edges WHERE a = ? OR b = ?"))) {
            count.addBindValue(out->artist.mbid);
            count.addBindValue(out->artist.mbid);
            if (count.exec() && count.next())
                out->edges = count.value(0).toInt();
        }
        return true;
    };

    Candidate best;
    for (const QString &shard : std::as_const(order)) {
        if (shard == QLatin1String("global")) {
            if (!best.artist.mbid.isEmpty())
                break;                 // a country had them: global is only a last resort
        }
        Candidate candidate;
        if (!copyIn(shard, &candidate))
            continue;
        const bool better = best.artist.mbid.isEmpty()
            || candidate.edges > best.edges
            || (candidate.edges == best.edges && candidate.artist.confidence > best.artist.confidence);
        if (better)
            best = candidate;
    }
    m_found.insert(cacheKey, best.artist);
    return best.artist;
}

} // namespace Rec
