#include "recommender.h"

#include "appdatabase.h"
#include "playbackcontroller.h"
#include "rec/catalog.h"
#include "rec/shelves.h"

#include <QDateTime>
#include <QDir>
#include <QSqlQuery>
#include <QVariantMap>

namespace {

const QString kDataDirKey = QStringLiteral("rec.dataDir");

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

} // namespace

// ------------------------------------------------------------------- worker

RecommenderWorker::RecommenderWorker(QObject *parent)
    : QObject(parent)
{
}

RecommenderWorker::~RecommenderWorker()
{
    delete m_catalog;
}

void RecommenderWorker::load(const QString &directory)
{
    delete m_catalog;
    m_catalog = nullptr;

    if (directory.isEmpty()) {
        Q_EMIT loaded(false, 0, QStringLiteral(
            "No catalogue is set, so there is nothing to recommend from yet."));
        return;
    }
    if (!QDir(directory).exists()) {
        Q_EMIT loaded(false, 0, QStringLiteral("There is no folder at %1.").arg(directory));
        return;
    }

    auto *catalogue = new Rec::Catalog;
    if (!catalogue->load(directory)) {
        delete catalogue;
        Q_EMIT loaded(false, 0, QStringLiteral(
            "%1 does not hold a catalogue: the four embeat_v1_*.bin files are missing or unreadable.")
                                    .arg(directory));
        return;
    }
    m_catalog = catalogue;
    Q_EMIT loaded(true, m_catalog->count(), QString());
}

void RecommenderWorker::build(const QVector<Rec::PlayEvent> &history, int perShelf)
{
    if (!m_catalog) {
        Q_EMIT built({}, false);
        return;
    }

    const Rec::TasteProfile taste =
        Rec::buildTaste(*m_catalog, history, QDateTime::currentDateTimeUtc());
    const QVector<Rec::Shelf> shelves =
        Rec::buildShelves(*m_catalog, taste, history, perShelf);

    QVariantList out;
    bool personal = false;
    for (const Rec::Shelf &shelf : shelves) {
        if (shelf.kind != QLatin1String("popular"))
            personal = true;
        QVariantList rows;
        for (const Rec::Suggestion &row : shelf.rows) {
            rows.append(QVariantMap{
                { QStringLiteral("title"), row.title },
                { QStringLiteral("artist"), row.artist }
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
            [this](bool ok, int rows, const QString &message) {
                m_rows = ok ? rows : 0;
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
            });

    m_thread.start();

    // A search of its own, so a suggestion resolving never cancels whatever
    // the listener is typing into the search field.
    connect(&m_innerTube, &InnerTube::searchFinished, this,
            [this](const QString &query, const QList<InnerTube::Track> &tracks) {
                if (query != m_pendingQuery)
                    return;
                m_pendingQuery.clear();
                if (tracks.isEmpty() || !m_player) {
                    Q_EMIT notice(QStringLiteral("Couldn't find “%1” to play").arg(m_pendingTitle));
                    return;
                }
                const InnerTube::Track &track = tracks.first();
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
    QMetaObject::invokeMethod(m_worker, "load", Qt::QueuedConnection,
                              Q_ARG(QString, dataDirectory()));
    m_busy = true;
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
    QString cleaned = path.trimmed();
    // QML file dialogs hand back a URL; the catalogue wants a path.
    if (cleaned.startsWith(QLatin1String("file:///")))
        cleaned = QUrl(cleaned).toLocalFile();
    if (cleaned == dataDirectory())
        return;
    storeSetting(kDataDirKey, cleaned);
    m_shelves.clear();
    m_rows = 0;
    setState(true, QString());
    Q_EMIT shelvesChanged();
    QMetaObject::invokeMethod(m_worker, "load", Qt::QueuedConnection, Q_ARG(QString, cleaned));
}

void Recommender::refresh()
{
    if (m_busy || m_rows == 0)
        return;
    // Read here, on the thread that owns the database connection: a Qt SQL
    // connection belongs to one thread and using it from another is undefined
    // rather than merely unwise.
    const QVector<Rec::PlayEvent> history = Rec::readPlayEvents();
    setState(true, m_message);
    QMetaObject::invokeMethod(m_worker, "build", Qt::QueuedConnection,
                              Q_ARG(QVector<Rec::PlayEvent>, history),
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
