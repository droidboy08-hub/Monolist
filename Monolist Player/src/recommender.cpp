#include "recommender.h"

#include "appdatabase.h"
#include "playbackcontroller.h"
#include "rec/catalog.h"
#include "rec/graph.h"
#include "rec/shelves.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSqlQuery>
#include <QUrl>
#include <QVariantMap>

#include <utility>

namespace {

const QString kDataDirKey = QStringLiteral("rec.dataDir");
const QString kGraphDirKey = QStringLiteral("rec.graphDir");

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

void RecommenderWorker::build(const QVector<Rec::PlayEvent> &history, const QString &region,
                              const QString &regionName, int perShelf)
{
    QVector<Rec::Shelf> shelves;
    if (m_catalog) {
        const Rec::TasteProfile taste =
            Rec::buildTaste(*m_catalog, history, QDateTime::currentDateTimeUtc());
        shelves = Rec::buildShelves(*m_catalog, taste, history, perShelf);
    }

    if (m_graph) {
        const QVector<Rec::Shelf> regional =
            Rec::buildRegionShelves(*m_graph, m_catalog, region, regionName, history, perShelf);
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

    QVariantList out;
    bool personal = false;
    for (const Rec::Shelf &shelf : shelves) {
        if (shelf.kind != QLatin1String("popular") && shelf.kind != QLatin1String("region"))
            personal = true;
        QVariantList rows;
        for (const Rec::Suggestion &row : shelf.rows) {
            rows.append(QVariantMap{
                { QStringLiteral("title"), row.title },
                { QStringLiteral("artist"), row.artist },
                { QStringLiteral("lengthMs"), row.lengthMs }
            });
        }
        out.append(QVariantMap{
            { QStringLiteral("title"), shelf.title },
            { QStringLiteral("reason"), shelf.reason },
            { QStringLiteral("kind"), shelf.kind },
            { QStringLiteral("rows"), rows }
        });
    }
    Q_EMIT built(out, personal);
}

// -------------------------------------------------------------- recommender

Recommender::Recommender(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<Rec::PlayEvent>>("QVector<Rec::PlayEvent>");

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
            });
    connect(m_worker, &RecommenderWorker::built, this,
            [this](const QVariantList &shelves, bool personal) {
                m_shelves = shelves;
                m_personal = personal;
                setState(false, m_message);
                Q_EMIT shelvesChanged();
                if (std::exchange(m_refreshQueued, false))
                    refresh();
            });

    m_thread.start();

    // A search of its own, so a suggestion resolving never cancels whatever
    // the listener is typing into the search field.
    connect(&m_innerTube, &InnerTube::searchFinished, this,
            [this](const QString &query, const QList<InnerTube::Track> &tracks) {
                if (query != m_pendingQuery)
                    return;
                m_pendingQuery.clear();
                const int picked = pickResult(tracks, m_pendingTitle, m_pendingLengthMs);
                if (picked < 0 || !m_player) {
                    Q_EMIT notice(QStringLiteral("Couldn't find “%1” to play").arg(m_pendingTitle));
                    return;
                }
                const InnerTube::Track &track = tracks.at(picked);
                m_player->playSource(track.videoId, track.title, track.artist, track.artwork,
                                     track.durationMs, track.album, /*isVideo=*/false,
                                     QStringLiteral("explore"));
            });
    connect(&m_innerTube, &InnerTube::searchFailed, this,
            [this](const QString &query, const QString &reason) {
                if (query != m_pendingQuery)
                    return;
                m_pendingQuery.clear();
                Q_EMIT notice(QStringLiteral("Couldn't play “%1”: %2").arg(m_pendingTitle, reason));
            });

    // Off the critical path: whatever is set is loaded in the background while
    // the rest of the app starts.
    reload();
}

Recommender::~Recommender()
{
    m_thread.quit();
    m_thread.wait(2000);
}

void Recommender::setPlayer(PlaybackController *player)
{
    m_player = player;
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
    m_shelves.clear();
    m_rows = 0;
    m_graphShards = 0;
    Q_EMIT shelvesChanged();
    setState(true, QString());
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
    const QVector<Rec::PlayEvent> history = Rec::readPlayEvents();
    const QString region = InnerTube::region();
    setState(true, m_message);
    QMetaObject::invokeMethod(m_worker, "build", Qt::QueuedConnection,
                              Q_ARG(QVector<Rec::PlayEvent>, history),
                              Q_ARG(QString, region),
                              Q_ARG(QString, regionDisplayName(region)),
                              Q_ARG(int, kPerShelf));
}

void Recommender::play(int shelfIndex, int rowIndex)
{
    if (shelfIndex < 0 || shelfIndex >= m_shelves.size())
        return;
    const QVariantList rows = m_shelves.at(shelfIndex).toMap()
                                  .value(QStringLiteral("rows")).toList();
    if (rowIndex < 0 || rowIndex >= rows.size())
        return;

    const QVariantMap row = rows.at(rowIndex).toMap();
    const QString title = row.value(QStringLiteral("title")).toString();
    const QString artist = row.value(QStringLiteral("artist")).toString();
    // Graph recordings know their length; catalogue rows do not, and say -1.
    m_pendingLengthMs = row.value(QStringLiteral("lengthMs"), -1).toLongLong();

    // The catalogue knows the name; YouTube knows where the sound is. One
    // search, and from there it is an ordinary track like any other.
    m_pendingTitle = title;
    m_pendingQuery = artist.isEmpty() ? title : artist + QLatin1Char(' ') + title;
    m_innerTube.search(m_pendingQuery, InnerTube::Filter::Songs);
}

void Recommender::setState(bool busy, const QString &message)
{
    if (busy == m_busy && message == m_message)
        return;
    m_busy = busy;
    m_message = message;
    Q_EMIT stateChanged();
}
