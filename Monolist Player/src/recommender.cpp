#include "recommender.h"

#include "appdatabase.h"
#include "mediaextractor.h"
#include "playbackcontroller.h"
#include "rec/catalog.h"
#include "rec/graph.h"
#include "rec/matchkey.h"
#include "rec/shelves.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace {

const QString kDataDirKey = QStringLiteral("rec.dataDir");
const QString kGraphDirKey = QStringLiteral("rec.graphDir");
const QString kHideExplicitKey = QStringLiteral("rec.hideExplicit");

// Enough to fill a shelf without making the page a list to scroll rather than
// a thing to look at.
constexpr int kPerShelf = 12;

QString storedSetting(const QString &key)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return {};
}

void storeSetting(const QString &key, const QString &value)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    query.addBindValue(AppDatabase::text(value));
    query.exec();
}

// QML hands back file URLs from its dialogs; everything here wants a path.
QString cleanPath(const QString &path)
{
    QString cleaned = path.trimmed();
    if (cleaned.startsWith(QLatin1String("file:///")))
        cleaned = QUrl(cleaned).toLocalFile();
    return cleaned;
}

// Which search result a suggestion becomes. The rules are the iOS
// TrackResolver's: a search for a song returns its live versions, covers,
// reaction videos and sped-up edits alongside it, and the first result is
// often one of those. They are passed over unless the suggestion itself asked
// for one. Where the length is known — graph recordings carry one — the
// first result within fifteen seconds of it wins, which separates the album
// cut from the eleven-minute extended one.
//
// When every result is "dirty" the unfiltered list is used rather than nothing:
// a cover is a worse answer than the song, but a better one than silence.
//
// Two things differ from iOS, both measured against thirty real presses. Its
// terms needed a space before them (" live", " remix"), so YouTube Music's own
// shape for these — "In the End (Live)", "Tum Hi Ho (Remix By …)" — went
// straight through. Here a title is reduced to its words first, so a term
// matches as a word wherever it sits and never inside another ("Alive"), and
// karaoke and instrumental versions join the list; seven sat within fifteen
// seconds of the song they would have stood in for.
//
// And a remix in the wanted title switched the whole list off. Now a term is
// waived only for a suggestion that itself contains it: asking for a remix still
// refuses the live version, and "Cover Me in Sunshine" is not a cover of itself.
QString wordsOf(const QString &title)
{
    QString words;
    words.reserve(title.size());
    for (const QChar c : title.toLower())
        words += c.isLetterOrNumber() ? c : QLatin1Char(' ');
    return QLatin1Char(' ') + words.simplified() + QLatin1Char(' ');
}

int pickResult(const QList<InnerTube::Track> &results, const QString &wantedTitle, qint64 wantedMs)
{
    static const QStringList reject = {
        QStringLiteral(" live "), QStringLiteral(" cover "), QStringLiteral(" reaction "),
        QStringLiteral(" sped up "), QStringLiteral(" slowed "), QStringLiteral(" nightcore "),
        QStringLiteral(" full album "), QStringLiteral(" full movie "),
        QStringLiteral(" hour version "), QStringLiteral(" 8d audio "), QStringLiteral(" remix "),
        QStringLiteral(" karaoke "), QStringLiteral(" instrumental ")
    };
    const QString wanted = wordsOf(wantedTitle);
    const auto dirty = [&](const QString &title) {
        const QString words = wordsOf(title);
        for (const QString &term : reject) {
            if (words.contains(term) && !wanted.contains(term))
                return true;
        }
        return false;
    };

    QList<int> pool;
    for (int i = 0; i < results.size(); ++i) {
        if (!dirty(results.at(i).title))
            pool.append(i);
    }
    if (pool.isEmpty()) {
        for (int i = 0; i < results.size(); ++i)
            pool.append(i);
    }
    if (pool.isEmpty())
        return -1;

    if (wantedMs > 0) {
        for (int i : pool) {
            const qint64 got = results.at(i).durationMs;
            if (got > 0 && qAbs(got - wantedMs) <= 15000)
                return i;
        }
    }
    return pool.first();
}

QString regionDisplayName(const QString &code)
{
    const QLocale::Territory territory = QLocale::codeToTerritory(code);
    if (territory == QLocale::AnyTerritory)
        return code;
    return QLocale::territoryToString(territory);
}

// One suggestion as QML has it. `row` is the catalogue row, which See all
// hands back so the rows already listed are recognised as rows, not only as
// names.
QVariantMap rowMap(const Rec::Suggestion &row)
{
    return QVariantMap{
        { QStringLiteral("title"), row.title },
        { QStringLiteral("artist"), row.artist },
        { QStringLiteral("lengthMs"), row.lengthMs },
        { QStringLiteral("row"), row.row }
    };
}

QString titleOf(const QVariantMap &row)
{
    return row.value(QStringLiteral("title")).toString();
}

QString artistOf(const QVariantMap &row)
{
    return row.value(QStringLiteral("artist")).toString();
}

// What pendingKey names a row by.
QString keyOf(const QVariantMap &row)
{
    return titleOf(row) + QLatin1Char('\n') + artistOf(row);
}

// How many rows each See all page adds: two screens or so.
constexpr int kMorePage = 24;

const QString kNotInterested = QStringLiteral("notInterested");
const QString kNotInterestedArtist = QStringLiteral("notInterestedArtist");

} // namespace

// ------------------------------------------------------------------- worker

RecommenderWorker::RecommenderWorker(QObject *parent)
    : QObject(parent)
{
}

// Runs on the worker thread (the QThread's finished() deletes it there), which
// matters for the graph: its SQLite connections were opened on this thread and
// have to be closed on it.
RecommenderWorker::~RecommenderWorker()
{
    delete m_graph;
    delete m_catalog;
}

void RecommenderWorker::load(const QString &catalogueDirectory, const QString &graphDirectory)
{
    if (Rec::stopRequested())
        return;                        // quitting: nothing will read it
    delete m_catalog;
    m_catalog = nullptr;
    delete m_graph;
    m_graph = nullptr;

    // The two are independent: a country chart needs no catalogue, and the
    // catalogue needs no country. Each is loaded if it can be, and the page
    // shows whatever that allows.
    QString problem;
    if (!catalogueDirectory.isEmpty()) {
        auto *catalogue = new Rec::Catalog;
        if (catalogue->load(catalogueDirectory)) {
            m_catalog = catalogue;
        } else {
            delete catalogue;
            problem = QStringLiteral(
                "%1 does not hold a catalogue: the four embeat_v1_*.bin files are missing or unreadable.")
                          .arg(catalogueDirectory);
        }
    }

    if (!graphDirectory.isEmpty() && QDir(graphDirectory).exists()) {
        auto *graph = new Rec::Graph;
        if (graph->open(graphDirectory))
            m_graph = graph;
        else
            delete graph;
    }

    const int rows = m_catalog ? m_catalog->count() : 0;
    const int shards = m_graph ? int(m_graph->installedCodes().size()) : 0;
    if (!m_catalog && !m_graph && problem.isEmpty()) {
        problem = QStringLiteral("No catalogue is set, so there is nothing to recommend from yet.");
    }
    Q_EMIT loaded(m_catalog || m_graph, rows, shards, problem);
}

void RecommenderWorker::build(quint64 generation, const QVector<Rec::PlayEvent> &history,
                              const Rec::Exclusions &exclude, quint64 rotation, const QString &region,
                              const QString &regionName, int perShelf, bool hideExplicit)
{
    // The builders check this between scans as well, and return early; a page
    // cut short that way is nobody's to show, so it is dropped here, unsent.
    if (Rec::stopRequested())
        return;

    QVector<Rec::Shelf> shelves;
    if (m_catalog) {
        const Rec::TasteProfile taste =
            Rec::buildTaste(*m_catalog, history, QDateTime::currentDateTimeUtc());
        shelves = Rec::buildShelves(*m_catalog, taste, history, perShelf, m_graph, region, hideExplicit,
                                    exclude, rotation);
    }

    if (m_graph && !Rec::stopRequested()) {
        QVector<Rec::Shelf> regional = Rec::buildRegionShelves(*m_graph, m_catalog, region, regionName,
                                                               history, perShelf, hideExplicit,
                                                               exclude, rotation);
        // The country shelves are built from the graph, apart from the rest,
        // so nothing stopped them repeating a song a personal shelf above had
        // already offered. Same text key as every other shelf uses.
        QSet<quint64> above;
        for (const Rec::Shelf &shelf : std::as_const(shelves)) {
            for (const Rec::Suggestion &row : shelf.rows)
                above.insert(Rec::strictKey(row.title, row.artist));
        }
        for (Rec::Shelf &shelf : regional) {
            shelf.rows.erase(std::remove_if(shelf.rows.begin(), shelf.rows.end(),
                                            [&above](const Rec::Suggestion &row) {
                                                return above.contains(
                                                    Rec::strictKey(row.title, row.artist));
                                            }),
                             shelf.rows.end());
        }
        regional.erase(std::remove_if(regional.begin(), regional.end(),
                                      [](const Rec::Shelf &shelf) { return shelf.rows.size() < 4; }),
                       regional.end());
        if (!regional.isEmpty()) {
            // The country chart is a better place to start than the
            // catalogue's worldwide most-played, so it takes that shelf's place
            // rather than sitting beneath it.
            shelves.erase(std::remove_if(shelves.begin(), shelves.end(),
                                         [](const Rec::Shelf &shelf) {
                                             return shelf.kind == QLatin1String("popular");
                                         }),
                          shelves.end());
            shelves += regional;
        }
    }
    if (Rec::stopRequested())
        return;

    QVariantList out;
    bool personal = false;
    for (const Rec::Shelf &shelf : std::as_const(shelves)) {
        if (shelf.kind != QLatin1String("popular") && shelf.kind != QLatin1String("region"))
            personal = true;
        QVariantList rows;
        for (const Rec::Suggestion &row : shelf.rows)
            rows.append(rowMap(row));
        out.append(QVariantMap{
            { QStringLiteral("title"), shelf.title },
            { QStringLiteral("reason"), shelf.reason },
            { QStringLiteral("kind"), shelf.kind },
            { QStringLiteral("rows"), rows },
            // Whether See all has anywhere to go from here.
            { QStringLiteral("more"), shelf.anchor.kind != Rec::Anchor::None }
        });
    }
    // Kept, anchors and all, for See all to continue from: the page exactly
    // as sent, so an index means the same shelf on both threads.
    m_page = shelves;
    m_pageHistory = history;
    m_pageGeneration = generation;
    Q_EMIT built(generation, out, personal);
}

void RecommenderWorker::more(quint64 generation, int shelf, const QVariantList &already, int count,
                             const Rec::Exclusions &exclude, bool hideExplicit)
{
    if (Rec::stopRequested())
        return;
    QVariantList out;
    bool exhausted = true;
    if (generation == m_pageGeneration && shelf >= 0 && shelf < m_page.size()) {
        QVector<Rec::Suggestion> shown;
        shown.reserve(already.size());
        for (const QVariant &value : already) {
            const QVariantMap map = value.toMap();
            Rec::Suggestion row;
            row.title = titleOf(map);
            row.artist = artistOf(map);
            row.row = map.value(QStringLiteral("row"), -1).toInt();
            shown.append(row);
        }
        const QVector<Rec::Suggestion> rows = Rec::moreFrom(m_catalog, m_graph, m_page.at(shelf), m_pageHistory,
                                                            exclude, shown, count, hideExplicit);
        if (Rec::stopRequested())
            return;
        for (const Rec::Suggestion &row : rows)
            out.append(rowMap(row));
        exhausted = rows.size() < count;
    }
    Q_EMIT moreReady(generation, shelf, out, exhausted);
}

// -------------------------------------------------------------- recommender

Recommender::Recommender(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<Rec::PlayEvent>>("QVector<Rec::PlayEvent>");
    qRegisterMetaType<Rec::Exclusions>("Rec::Exclusions");

    // Read before the first build is asked for, below, so a launch never
    // shows one page of explicit titles before the setting catches up.
    m_hideExplicit = storedSetting(kHideExplicitKey) == QLatin1String("1");
    loadTurnedDown();

    m_worker = new RecommenderWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &RecommenderWorker::loaded, this,
            [this](bool ok, int rows, int graphShards, const QString &message) {
                m_rows = rows;
                m_graphShards = graphShards;
                setState(false, message);
                if (ok)
                    refresh();
                // Loads run in the order asked, so the last one answered is
                // the one the settings name now.
                m_loadsPending = qMax(0, m_loadsPending - 1);
                if (m_loadsPending == 0)
                    Q_EMIT dataLoaded();
            });
    connect(m_worker, &RecommenderWorker::built, this,
            [this](quint64 generation, const QVariantList &shelves, bool personal) {
                m_shelves = shelves;
                m_personal = personal;
                m_shownGeneration = generation;
                setState(false, m_message);
                Q_EMIT shelvesChanged();
                if (std::exchange(m_refreshQueued, false))
                    refresh();
            });
    connect(m_worker, &RecommenderWorker::moreReady, this,
            [this](quint64 generation, int shelf, const QVariantList &rows, bool exhausted) {
                if (generation != m_moreGeneration || shelf != m_more.value(QStringLiteral("shelf")).toInt())
                    return;   // a list since replaced
                m_moreLoading = false;
                // A page asked for from a page since rebuilt comes back empty:
                // its anchors are gone, and the list ends where it is.
                m_moreExhausted = exhausted;
                // Anything turned down while the page was being found.
                QVariantList added;
                for (const QVariant &value : rows) {
                    const QVariantMap row = value.toMap();
                    if (!unwanted(QString(), titleOf(row), artistOf(row)))
                        added.append(row);
                }
                m_moreRows += added;
                Q_EMIT moreStateChanged();
                if (!added.isEmpty())
                    Q_EMIT moreAppended(added);
            });

    m_thread.start();

    // Off the critical path: whatever is set is loaded in the background while
    // the rest of the app starts.
    reload();
}

// Destroying a QThread that is still running is fatal in Qt 6, and a build
// can outlast any timeout worth picking: the first one in a session reads
// every graph shard, from wherever they are stored, which may be a network
// share. So the worker is asked to stop — the builders check between scans —
// and then waited for until it has actually finished, which after the ask is
// at most the rest of one scan.
Recommender::~Recommender()
{
    // The player outlives this, and must not ask a filter that is gone.
    if (m_player)
        m_player->setRadioFilter({});
    m_thread.requestInterruption();
    m_thread.quit();
    m_thread.wait();
}

void Recommender::setPlayer(PlaybackController *player)
{
    if (m_player)
        m_player->setRadioFilter({});
    m_player = player;
    if (m_player) {
        m_player->setRadioFilter([this](const QString &videoId, const QString &title, const QString &artist) {
            return unwanted(videoId, title, artist);
        });
    }
}

QString Recommender::dataDirectory() const
{
    return storedSetting(kDataDirKey);
}

void Recommender::setDataDirectory(const QString &path)
{
    const QString cleaned = cleanPath(path);
    if (cleaned == dataDirectory())
        return;
    storeSetting(kDataDirKey, cleaned);
    reload();
}

QString Recommender::graphDirectory() const
{
    return storedSetting(kGraphDirKey);
}

void Recommender::setGraphDirectory(const QString &path)
{
    const QString cleaned = cleanPath(path);
    if (cleaned == graphDirectory())
        return;
    storeSetting(kGraphDirKey, cleaned);
    reload();
}

void Recommender::setHideExplicit(bool hide)
{
    if (hide == m_hideExplicit)
        return;
    m_hideExplicit = hide;
    storeSetting(kHideExplicitKey, hide ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT hideExplicitChanged();
    // The switch is part of what a page is built from, so this rebuilds it —
    // or, mid-build, queues the rebuild behind the one running.
    refresh();
}

void Recommender::useDirectories(const QString &catalogue, const QString &graph)
{
    storeSetting(kDataDirKey, cleanPath(catalogue));
    storeSetting(kGraphDirKey, cleanPath(graph));
    reload();
    // The folder fields follow even when the state line did not change.
    Q_EMIT stateChanged();
}

bool Recommender::release(const QString &folder)
{
    bool changed = false;
    if (isInside(dataDirectory(), folder)) {
        storeSetting(kDataDirKey, QString());
        changed = true;
    }
    if (isInside(graphDirectory(), folder)) {
        storeSetting(kGraphDirKey, QString());
        changed = true;
    }
    // A graph found beside the catalogue is inside the folder only when the
    // catalogue is, which the first test has already let go of.
    if (!changed)
        return false;
    reload();
    Q_EMIT stateChanged();
    return true;
}

bool Recommender::isInside(const QString &path, const QString &folder)
{
    if (path.trimmed().isEmpty() || folder.trimmed().isEmpty())
        return false;
    // The file systems these run on by default ignore case.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    const QString inner = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    const QString outer = QDir::cleanPath(QDir::fromNativeSeparators(folder.trimmed()));
    return inner.compare(outer, sensitivity) == 0
           || inner.startsWith(outer + QLatin1Char('/'), sensitivity);
}

QString Recommender::resolvedGraphDirectory() const
{
    const QString explicitDir = graphDirectory();
    if (!explicitDir.isEmpty())
        return explicitDir;
    // The iOS project keeps EmbeddingData and GraphData side by side, so a
    // catalogue pointed at is usually a graph found for free.
    const QString catalogue = dataDirectory();
    if (catalogue.isEmpty())
        return {};
    const QString beside = QDir(catalogue).absoluteFilePath(QStringLiteral("../GraphData"));
    return QFileInfo(beside).isDir() ? QDir::cleanPath(beside) : QString();
}

void Recommender::reload()
{
    m_builtFrom.clear();
    m_shelves.clear();
    m_rows = 0;
    m_graphShards = 0;
    Q_EMIT shelvesChanged();
    setState(true, QString());
    ++m_loadsPending;
    QMetaObject::invokeMethod(m_worker, "load", Qt::QueuedConnection,
                              Q_ARG(QString, dataDirectory()),
                              Q_ARG(QString, resolvedGraphDirectory()));
}

void Recommender::refresh()
{
    if (m_rows == 0 && m_graphShards == 0)
        return;
    if (m_busy) {
        m_refreshQueued = true;
        return;
    }
    // Both read here, on the main thread. A Qt SQL connection belongs to one
    // thread and using it from another is undefined rather than merely unwise;
    // and the region lives in a global the settings page writes from this one.
    const QString region = InnerTube::region();

    // Called every time Search is opened, so it has to be nothing when nothing
    // has happened. What a page depends on is the listening history, the
    // country, the "Hide explicit titles" switch, the songs the library keeps,
    // what was turned down, and the rotation, so those are summarised and
    // compared. Anything else — opening Search twice in a row — keeps the
    // page exactly as it is, which also means it never rearranges itself under
    // someone reading it.
    //
    // The newest event alone is not enough: a listen is written when it starts
    // and FINISHED by an update to the same row — the playhead, the label —
    // which moves no id. Skipping on while paused finishes a listen that way,
    // and the page stayed stale. The labelled count and the total heard catch
    // every such update.
    QSqlQuery summary(AppDatabase::connection());
    QString listened = QStringLiteral("0");
    if (summary.exec(QStringLiteral(
            "SELECT MAX(id), COUNT(label), TOTAL(listened_ms) FROM play_events")) && summary.next()) {
        listened = QStringLiteral("%1/%2/%3").arg(summary.value(0).toLongLong())
                      .arg(summary.value(1).toLongLong())
                      .arg(summary.value(2).toLongLong());
    }
    // The library by its rows rather than its songs, which would cost a text
    // key per song on every visit: a like, a playlist entry or a download
    // added or taken away changes a count or a sum of ids.
    QString owned = QStringLiteral("0");
    if (summary.exec(QStringLiteral(
            "SELECT (SELECT COUNT(*) FROM tracks), (SELECT TOTAL(id) FROM tracks),"
            " (SELECT COUNT(*) FROM playlist_tracks), (SELECT TOTAL(id) FROM playlist_tracks),"
            " (SELECT COUNT(*) FROM downloads), (SELECT TOTAL(rowid) FROM downloads)")) && summary.next()) {
        QStringList parts;
        for (int i = 0; i < 6; ++i)
            parts << QString::number(summary.value(i).toLongLong());
        owned = parts.join(QLatin1Char('/'));
    }
    // An undone "Not interested" deletes its row, which can put MAX(id) back
    // where it was; the sets themselves are what say so.
    quint64 turnedDown = quint64(m_turnedDownSongs.size()) << 32 | quint64(m_turnedDownArtists.size());
    for (const quint64 key : std::as_const(m_turnedDownSongs))
        turnedDown ^= key;
    for (const QString &artist : std::as_const(m_turnedDownArtists))
        turnedDown ^= Rec::fnv1a64(artist.toUtf8());
    const quint64 rotation = currentRotation();

    const QString fingerprint = listened + QLatin1Char('|') + region + QLatin1Char('|')
                                + (m_hideExplicit ? QLatin1Char('1') : QLatin1Char('0'))
                                + QLatin1Char('|') + owned + QLatin1Char('|') + QString::number(turnedDown)
                                + QLatin1Char('|') + QString::number(rotation);
    if (fingerprint == m_builtFrom && !m_shelves.isEmpty())
        return;
    m_builtFrom = fingerprint;

    const QVector<Rec::PlayEvent> history = Rec::readPlayEvents();
    // What the library keeps, by text key, as the catalogue has no ids: every
    // song liked or downloaded (both are rows of `tracks`), in a playlist, or
    // in the downloads folder.
    m_owned.clear();
    QSqlQuery kept(AppDatabase::connection());
    if (kept.exec(QStringLiteral("SELECT title, artist FROM tracks UNION SELECT title, artist FROM playlist_tracks"
                                 " UNION SELECT title, artist FROM downloads"))) {
        while (kept.next()) {
            const QString title = kept.value(0).toString();
            if (!title.isEmpty())
                m_owned.insert(Rec::strictKey(title, kept.value(1).toString()));
        }
    }

    setState(true, m_message);
    QMetaObject::invokeMethod(m_worker, "build", Qt::QueuedConnection,
                              Q_ARG(quint64, ++m_generation),
                              Q_ARG(QVector<Rec::PlayEvent>, history),
                              Q_ARG(Rec::Exclusions, exclusions()),
                              Q_ARG(quint64, rotation),
                              Q_ARG(QString, region),
                              Q_ARG(QString, regionDisplayName(region)),
                              Q_ARG(int, kPerShelf),
                              Q_ARG(bool, m_hideExplicit));
}

void Recommender::rebuild()
{
    // The next rotation, which is a different page even when nothing else
    // has changed: what REFRESH is for.
    ++m_turn;
    m_builtFrom.clear();
    refresh();
}

quint64 Recommender::currentRotation() const
{
    return Rec::rotationFor(QDateTime::currentSecsSinceEpoch() / kRotationPeriodSecs, m_turn);
}

Rec::Exclusions Recommender::exclusions() const
{
    Rec::Exclusions exclude;
    exclude.owned = m_owned;
    exclude.turnedDown = m_turnedDownSongs;
    exclude.artists = m_turnedDownArtists;
    return exclude;
}

QVariantList Recommender::rowsOf(int shelf) const
{
    if (shelf == -1)
        return m_moreRows;
    if (shelf < 0 || shelf >= m_shelves.size())
        return {};
    return m_shelves.at(shelf).toMap().value(QStringLiteral("rows")).toList();
}

// ------------------------------------------------------------- looking up

void Recommender::lookUp(const QVariantMap &row, Found done)
{
    const QString title = titleOf(row);
    const QString artist = artistOf(row);
    // Graph recordings know their length; catalogue rows do not, and say -1.
    const qint64 lengthMs = row.value(QStringLiteral("lengthMs"), -1).toLongLong();
    if (title.isEmpty()) {
        done(nullptr, QString());
        return;
    }
    // The catalogue knows the name; YouTube knows where the sound is. One
    // search, and from there it is an ordinary track like any other. A search
    // of its own, and one per ask, so a suggestion being looked up never
    // cancels whatever the listener is typing, or another look-up.
    const QString query = artist.isEmpty() ? title : artist + QLatin1Char(' ') + title;
    m_innerTube.searchTracks(query, InnerTube::Filter::Songs,
                             [title, lengthMs, done](const QList<InnerTube::Track> &tracks, const QString &error) {
                                 if (!error.isEmpty()) {
                                     done(nullptr, error);
                                     return;
                                 }
                                 const int picked = pickResult(tracks, title, lengthMs);
                                 done(picked >= 0 ? &tracks.at(picked) : nullptr, QString());
                             });
}

void Recommender::setPending(const QString &key)
{
    if (key == m_pendingKey)
        return;
    m_pendingKey = key;
    Q_EMIT pendingChanged();
}

// Only the look-up that set it clears it: a later one has its own row lit.
void Recommender::clearPending(const QString &key)
{
    if (key == m_pendingKey)
        setPending(QString());
}

void Recommender::play(const QVariantMap &row)
{
    // A press is a new wish: Play all stops feeding, and an earlier press
    // still being looked up is dropped when it answers.
    const quint64 token = ++m_playToken;
    m_feed = Feed();
    const QString key = keyOf(row);
    const QString title = titleOf(row);
    setPending(key);
    lookUp(row, [this, token, key, title](const InnerTube::Track *track, const QString &error) {
        clearPending(key);
        if (token != m_playToken)
            return;
        if (!track || !m_player) {
            Q_EMIT notice(error.isEmpty() ? QStringLiteral("Couldn't find “%1” to play").arg(title)
                                          : QStringLiteral("Couldn't play “%1”: %2").arg(title, error));
            return;
        }
        m_player->playSource(track->videoId, track->title, track->artist, track->artwork,
                             track->durationMs, track->album, /*isVideo=*/false,
                             QStringLiteral("explore"), track->primaryArtist);
    });
}

void Recommender::resolve(const QVariantMap &row, const QString &purpose)
{
    if (purpose.isEmpty())
        return;
    const quint64 token = ++m_resolveToken;
    const QString key = keyOf(row);
    const QString title = titleOf(row);
    setPending(key);
    lookUp(row, [this, token, key, title, purpose](const InnerTube::Track *track, const QString &error) {
        clearPending(key);
        if (token != m_resolveToken)
            return;
        if (!track) {
            Q_EMIT notice(error.isEmpty() ? QStringLiteral("Couldn't find “%1”").arg(title)
                                          : QStringLiteral("Couldn't find “%1”: %2").arg(title, error));
            return;
        }
        // Not a video, as for playing it: the catalogue lists recordings, and
        // a search for songs answers with songs.
        SearchResultModel::Item item = SearchResultModel::fromTrack(*track);
        item.isVideo = false;
        Q_EMIT resolved(purpose, SearchResultModel::toMap(item));
    });
}

void Recommender::playAll(int shelf)
{
    const QVariantList rows = rowsOf(shelf);
    if (rows.isEmpty())
        return;
    m_feed = Feed();
    m_feed.token = ++m_playToken;
    m_feed.rows = rows;
    feedNext();
}

// One name at a time, in the shelf's order: the first becomes the queue, and
// each after it joins the queue as it is found. Looked up one after another
// rather than all at once, so the songs arrive in order and a shelf of twelve
// is twelve polite requests rather than a burst.
void Recommender::feedNext()
{
    while (m_feed.next < m_feed.rows.size()) {
        const QVariantMap row = m_feed.rows.at(m_feed.next++).toMap();
        // Turned down since Play all was pressed.
        if (unwanted(QString(), titleOf(row), artistOf(row)))
            continue;
        const quint64 token = m_feed.token;
        const QString key = keyOf(row);
        // The first row shows it is being found, as a press would.
        if (!m_feed.started)
            setPending(key);
        lookUp(row, [this, token, key](const InnerTube::Track *track, const QString &) {
            clearPending(key);
            // Something else was pressed, or another Play all.
            if (token != m_playToken || token != m_feed.token)
                return;
            if (track && m_player) {
                if (!m_feed.started) {
                    m_player->playSource(track->videoId, track->title, track->artist, track->artwork,
                                         track->durationMs, track->album, /*isVideo=*/false,
                                         QStringLiteral("explore"), track->primaryArtist);
                    m_feed.queue = m_player->queueGeneration();
                    m_feed.started = true;
                } else if (m_player->queueGeneration() != m_feed.queue) {
                    // The listener has played something else since: the
                    // shelf's queue is gone, and nothing more joins theirs.
                    m_feed = Feed();
                    return;
                } else {
                    SearchResultModel::Item item = SearchResultModel::fromTrack(*track);
                    item.isVideo = false;
                    m_player->addToQueue(SearchResultModel::toMap(item));
                }
            }
            // A name YouTube Music does not know is passed over.
            feedNext();
        });
        return;
    }
    if (!m_feed.started && m_feed.token != 0 && m_feed.token == m_playToken)
        Q_EMIT notice(QStringLiteral("Couldn't find any of these songs to play"));
    m_feed = Feed();
}

// -------------------------------------------------------------- See all

// The page a shelf was on and its place there, then what it is — its kind and
// title — for when Back or Forward come to it after the page was rebuilt, and
// the same place holds another shelf, or none.
QString Recommender::moreKey(int shelf) const
{
    if (shelf < 0 || shelf >= m_shelves.size())
        return {};
    const QVariantMap source = m_shelves.at(shelf).toMap();
    return QStringLiteral("%1:%2:%3|%4").arg(QString::number(m_shownGeneration), QString::number(shelf),
                                            source.value(QStringLiteral("kind")).toString(),
                                            source.value(QStringLiteral("title")).toString());
}

void Recommender::openMore(const QString &key)
{
    // Back to a list already open: as it was left, scrolled and paged.
    if (key.isEmpty() || (!m_more.isEmpty() && m_more.value(QStringLiteral("key")).toString() == key))
        return;
    const qsizetype bar = key.indexOf(QLatin1Char('|'));
    const QStringList place = key.left(bar).split(QLatin1Char(':'));
    if (bar < 0 || place.size() != 3)
        return;
    const quint64 generation = place.at(0).toULongLong();
    const int index = place.at(1).toInt();
    const QString kind = place.at(2);
    const QString title = key.mid(bar + 1);
    const auto isIt = [this, &kind, &title](int candidate) {
        const QVariantMap map = m_shelves.at(candidate).toMap();
        return map.value(QStringLiteral("kind")).toString() == kind
               && map.value(QStringLiteral("title")).toString() == title;
    };
    int shelf = -1;
    if (generation == m_shownGeneration && index >= 0 && index < m_shelves.size() && isIt(index))
        shelf = index;
    // The page has been drawn again since: the same shelf, wherever it is on
    // the new one, from the top.
    for (int candidate = 0; shelf < 0 && candidate < m_shelves.size(); ++candidate) {
        if (isIt(candidate))
            shelf = candidate;
    }

    ++m_moreSerial;
    m_moreLoading = false;
    if (shelf < 0) {
        // Gone with the page it was on. Said so, rather than showing the
        // list left open, or another shelf's, under its name.
        m_more = QVariantMap{
            { QStringLiteral("title"), title },
            { QStringLiteral("shelf"), -1 },
            { QStringLiteral("key"), key },
            { QStringLiteral("gone"), true }
        };
        m_moreRows.clear();
        m_moreGeneration = 0;
        m_moreExhausted = true;
        Q_EMIT moreChanged();
        Q_EMIT moreStateChanged();
        return;
    }

    const QVariantMap source = m_shelves.at(shelf).toMap();
    m_more = QVariantMap{
        { QStringLiteral("title"), source.value(QStringLiteral("title")) },
        { QStringLiteral("reason"), source.value(QStringLiteral("reason")) },
        { QStringLiteral("kind"), source.value(QStringLiteral("kind")) },
        { QStringLiteral("shelf"), shelf },
        { QStringLiteral("key"), key }
    };
    m_moreRows = source.value(QStringLiteral("rows")).toList();
    m_moreGeneration = m_shownGeneration;
    m_moreExhausted = !source.value(QStringLiteral("more")).toBool();
    Q_EMIT moreChanged();
    Q_EMIT moreStateChanged();
    loadMore();
}

void Recommender::loadMore()
{
    if (m_more.isEmpty() || m_moreLoading || m_moreExhausted)
        return;
    m_moreLoading = true;
    Q_EMIT moreStateChanged();
    QMetaObject::invokeMethod(m_worker, "more", Qt::QueuedConnection,
                              Q_ARG(quint64, m_moreGeneration),
                              Q_ARG(int, m_more.value(QStringLiteral("shelf")).toInt()),
                              Q_ARG(QVariantList, m_moreRows),
                              Q_ARG(int, kMorePage),
                              Q_ARG(Rec::Exclusions, exclusions()),
                              Q_ARG(bool, m_hideExplicit));
}

// --------------------------------------------------------- not interested

bool Recommender::unwanted(const QString &videoId, const QString &title, const QString &artist) const
{
    if (!videoId.isEmpty() && m_turnedDownIds.contains(videoId))
        return true;
    if (!title.isEmpty() && m_turnedDownSongs.contains(Rec::strictKey(title, artist)))
        return true;
    return !m_turnedDownArtists.isEmpty() && !artist.isEmpty()
           && m_turnedDownArtists.contains(Rec::suggestionArtistKey(artist));
}

// Kept in play_events, where the taste profile already reads a song turned
// down as its strongest "no" (taste.cpp). One table, so there is no second
// list to fall out of step with the first, and a history carried to another
// device carries these with it.
void Recommender::loadTurnedDown()
{
    m_turnedDownSongs.clear();
    m_turnedDownIds.clear();
    m_turnedDownArtists.clear();
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT kind, video_id, title, artist FROM play_events WHERE kind IN (?, ?)"));
    query.addBindValue(kNotInterested);
    query.addBindValue(kNotInterestedArtist);
    if (!query.exec())
        return;
    while (query.next()) {
        const QString kind = query.value(0).toString();
        const QString videoId = query.value(1).toString();
        const QString title = query.value(2).toString();
        const QString artist = query.value(3).toString();
        if (kind == kNotInterested) {
            if (!title.isEmpty())
                m_turnedDownSongs.insert(Rec::strictKey(title, artist));
            if (!videoId.isEmpty())
                m_turnedDownIds.insert(videoId);
        } else if (!artist.isEmpty()) {
            m_turnedDownArtists.insert(Rec::suggestionArtistKey(artist));
        }
    }
}

// The label is iOS's for the same event (NotInterestedStore: label 0.0,
// source .notInterested), so an imported history means the same here.
qint64 Recommender::recordTurnDown(const QString &kind, const QString &videoId, const QString &title,
                                   const QString &artist)
{
    QSqlQuery insert(AppDatabase::connection());
    insert.prepare(QStringLiteral(
        "INSERT INTO play_events (kind, video_id, title, artist, source, label) VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(kind);
    insert.addBindValue(AppDatabase::text(videoId));
    insert.addBindValue(AppDatabase::text(title));
    insert.addBindValue(AppDatabase::text(artist));
    insert.addBindValue(kNotInterested);
    insert.addBindValue(kind == kNotInterested ? QVariant(0.0) : QVariant());
    if (!insert.exec())
        return 0;
    return insert.lastInsertId().toLongLong();
}

// Every row the test matches, off every shelf and the See all list, each
// noted with where it stood so Undo can put it back. The page is edited in
// place rather than rebuilt: a rebuild would redraw every shelf under the
// pointer to take one row away.
void Recommender::takeRows(Dismissal &dismissal, const std::function<bool(const QVariantMap &row)> &matches)
{
    for (int shelf = 0; shelf < m_shelves.size(); ++shelf) {
        QVariantMap map = m_shelves.at(shelf).toMap();
        const QVariantList rows = map.value(QStringLiteral("rows")).toList();
        QVariantList kept;
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows.at(i).toMap();
            if (matches(row))
                dismissal.taken.append({ shelf, i, row });
            else
                kept.append(row);
        }
        if (kept.size() == rows.size())
            continue;
        map.insert(QStringLiteral("rows"), kept);
        m_shelves[shelf] = map;
        Q_EMIT rowsEdited(shelf);
    }
    QVariantList kept;
    for (int i = 0; i < m_moreRows.size(); ++i) {
        const QVariantMap row = m_moreRows.at(i).toMap();
        if (matches(row))
            dismissal.taken.append({ -1, i, row });
        else
            kept.append(row);
    }
    if (kept.size() != m_moreRows.size()) {
        m_moreRows = kept;
        Q_EMIT rowsEdited(-1);
    }
}

void Recommender::notInterested(const QVariantMap &track)
{
    const QString title = track.value(QStringLiteral("title")).toString().trimmed();
    const QString artist = track.value(QStringLiteral("artist")).toString().trimmed();
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (title.isEmpty())
        return;
    Dismissal dismissal;
    dismissal.eventId = recordTurnDown(kNotInterested, videoId, title, artist);
    if (dismissal.eventId <= 0) {
        Q_EMIT notice(QStringLiteral("Couldn't save that — “%1” may be suggested again").arg(title));
        return;
    }
    loadTurnedDown();
    dismissal.generation = m_shownGeneration;
    dismissal.moreSerial = m_moreSerial;
    const quint64 key = Rec::strictKey(title, artist);
    takeRows(dismissal, [key](const QVariantMap &row) {
        return Rec::strictKey(titleOf(row), artistOf(row)) == key;
    });
    m_lastDismissal = dismissal;
    if (m_player)
        m_player->pruneRadio();
    Q_EMIT undoable(QStringLiteral("Won't suggest “%1” again").arg(title));
}

void Recommender::dontSuggestArtist(const QString &artist)
{
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+topic\s*$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QString name = QString(artist).remove(topic).trimmed();
    const QString key = Rec::suggestionArtistKey(name);
    if (name.isEmpty() || key.isEmpty())
        return;
    Dismissal dismissal;
    dismissal.eventId = recordTurnDown(kNotInterestedArtist, QString(), QString(), name);
    if (dismissal.eventId <= 0) {
        Q_EMIT notice(QStringLiteral("Couldn't save that — %1 may be suggested again").arg(name));
        return;
    }
    loadTurnedDown();
    dismissal.generation = m_shownGeneration;
    dismissal.moreSerial = m_moreSerial;
    takeRows(dismissal, [key](const QVariantMap &row) {
        return Rec::suggestionArtistKey(artistOf(row)) == key;
    });
    m_lastDismissal = dismissal;
    if (m_player)
        m_player->pruneRadio();
    Q_EMIT undoable(QStringLiteral("Won't suggest %1 again").arg(name));
}

void Recommender::undoNotInterested()
{
    const Dismissal dismissal = std::exchange(m_lastDismissal, Dismissal());
    if (dismissal.eventId <= 0)
        return;
    // Deleting the record is the undo: it was written a moment ago, by this,
    // and only a turn-down row with that id can go.
    QSqlQuery remove(AppDatabase::connection());
    remove.prepare(QStringLiteral("DELETE FROM play_events WHERE id = ? AND kind IN (?, ?)"));
    remove.addBindValue(dismissal.eventId);
    remove.addBindValue(kNotInterested);
    remove.addBindValue(kNotInterestedArtist);
    remove.exec();
    loadTurnedDown();

    // Back where they stood, in the order they were taken, so each index is
    // right when its turn comes. Only into the page and the list they were
    // taken from: a page rebuilt since is built without them, and the next
    // refresh builds it with them again.
    QSet<int> edited;
    QHash<int, QVariantList> rowsByShelf;
    for (const Taken &taken : dismissal.taken) {
        const bool samePage = taken.shelf >= 0 ? dismissal.generation == m_shownGeneration
                                                   && taken.shelf < m_shelves.size()
                                               : dismissal.moreSerial == m_moreSerial;
        if (!samePage)
            continue;
        if (!rowsByShelf.contains(taken.shelf))
            rowsByShelf.insert(taken.shelf, rowsOf(taken.shelf));
        QVariantList &rows = rowsByShelf[taken.shelf];
        rows.insert(std::min(qsizetype(taken.index), rows.size()), taken.row);
        edited.insert(taken.shelf);
    }
    for (const int shelf : std::as_const(edited)) {
        if (shelf == -1) {
            m_moreRows = rowsByShelf.value(shelf);
        } else {
            QVariantMap map = m_shelves.at(shelf).toMap();
            map.insert(QStringLiteral("rows"), rowsByShelf.value(shelf));
            m_shelves[shelf] = map;
        }
        Q_EMIT rowsEdited(shelf);
    }
}

void Recommender::setState(bool busy, const QString &message)
{
    if (busy == m_busy && message == m_message)
        return;
    m_busy = busy;
    m_message = message;
    Q_EMIT stateChanged();
}
