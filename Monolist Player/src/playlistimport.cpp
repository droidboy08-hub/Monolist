#include "playlistimport.h"

#include "appdatabase.h"
#include "innertube.h"
#include "jiosaavn.h"
#include "library.h"

#include <QClipboard>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRandomGenerator>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

#include <algorithm>
#include <limits>

namespace {

// Where the hour's and the day's searches are counted, between launches.
const QString kBudgetKey = QStringLiteral("import.search_budget");
// Where YouTube Music's refusals are kept, between launches and imports:
// "<rest until, UTC ms> <refusals in a row> <what it said>".
const QString kRestKey = QStringLiteral("import.youtube_rest");
// Why a song a stop left is not found: it was never asked for.
const QString kNotSearched = QStringLiteral("Not searched for: the import was stopped");
// A file bigger than this is not a playlist's CSV.
constexpr qint64 kMaxFileBytes = 20LL * 1024 * 1024;
// A paused job's pause doubles with each refusal in a row, to a day at most.
constexpr qint64 kMaxPauseMs = 24LL * 60 * 60 * 1000;
// A guess at how long a song takes to find before any has been timed: the
// prototype's 68 searches for 66 songs, 2-3 s apart (REPORT.md).
constexpr double kGuessSecsPerSong = 3.5;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// "14:35", or "Sat 14:35" when it is not today.
QString clockText(qint64 ms)
{
    const QDateTime at = QDateTime::fromMSecsSinceEpoch(ms);
    const QLocale locale;
    const QString time = locale.toString(at.time(), QLocale::ShortFormat);
    if (at.date() == QDate::currentDate())
        return time;
    return locale.toString(at.date(), QStringLiteral("ddd")) + QLatin1Char(' ') + time;
}

QString minutesText(qint64 ms)
{
    const qint64 minutes = (ms + 59999) / 60000;
    if (minutes < 60)
        return minutes == 1 ? QStringLiteral("a minute") : QStringLiteral("%1 minutes").arg(minutes);
    const qint64 hours = (minutes + 59) / 60;
    return hours == 1 ? QStringLiteral("an hour") : QStringLiteral("%1 hours").arg(hours);
}

QString lengthText(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString toJson(const QVariantMap &map)
{
    if (map.isEmpty())
        return {};
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QVariantMap fromJson(const QString &text)
{
    if (text.isEmpty())
        return {};
    return QJsonDocument::fromJson(text.toUtf8()).object().toVariantMap();
}

QVariantMap candidateMap(const ImportMatch::Candidate &candidate, const ImportMatch::Verdict &verdict)
{
    return {
        { QStringLiteral("videoId"), candidate.videoId },
        { QStringLiteral("title"), candidate.title },
        { QStringLiteral("artist"), candidate.artistLine },
        { QStringLiteral("album"), candidate.album },
        { QStringLiteral("artwork"), candidate.artwork },
        { QStringLiteral("durationMs"), qint64(candidate.durationSec) * 1000 },
        { QStringLiteral("isVideo"), candidate.isVideo },
        { QStringLiteral("videoType"), candidate.videoType },
        { QStringLiteral("confidence"), ImportMatch::confidenceName(verdict.confidence) },
        { QStringLiteral("reason"), verdict.reason },
    };
}

QString platformOfKind(const QString &kind)
{
    return kind.section(QLatin1Char('-'), 0, 0);
}

bool youtubeKind(const QString &kind)
{
    const QString platform = platformOfKind(kind);
    return platform == QLatin1String("youtube") || platform == QLatin1String("ytmusic");
}

QVariantMap previewMap(const QString &state, const Import::Link &link, const Import::Page &page)
{
    QVariantList sample;
    for (int i = 0; i < page.rows.size() && i < 5; ++i) {
        const Import::Row &row = page.rows.at(i);
        sample.append(QVariantMap{
            { QStringLiteral("title"), row.title },
            { QStringLiteral("artist"), row.artists.join(QStringLiteral(", ")) },
            { QStringLiteral("durationMs"), row.durationMs },
        });
    }
    return {
        { QStringLiteral("state"), state },
        { QStringLiteral("link"), link.url },
        { QStringLiteral("platform"), link.platform },
        { QStringLiteral("type"), link.type },
        { QStringLiteral("label"), link.label },
        { QStringLiteral("name"), page.name },
        { QStringLiteral("owner"), page.owner },
        { QStringLiteral("artwork"), page.artwork },
        { QStringLiteral("count"), qMax(page.total, int(page.rows.size())) },
        { QStringLiteral("readable"), page.cursor.isEmpty() ? int(page.rows.size()) : qMax(page.total, int(page.rows.size())) },
        { QStringLiteral("warnings"), QVariant(page.warnings) },
        { QStringLiteral("error"), page.error },
        { QStringLiteral("sample"), sample },
    };
}

} // namespace

// ------------------------------------------------------------------ rows

ImportRowModel::ImportRowModel(QObject *parent)
    : QAbstractListModel(parent) {}

int ImportRowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant ImportRowModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const Item &item = m_items.at(index.row());
    switch (role) {
    case RowIdRole:      return item.rowId;
    case PositionRole:   return item.position;
    case TitleRole:      return item.title;
    case ArtistRole:     return item.artist;
    case AlbumRole:      return item.album;
    case DurationMsRole: return item.durationMs;
    case StatusRole:     return item.status;
    case VideoIdRole:    return item.videoId;
    case ReasonRole:     return item.reason;
    case CandidateRole:  return item.candidate;
    default:             return {};
    }
}

QHash<int, QByteArray> ImportRowModel::roleNames() const
{
    return {
        { RowIdRole, "rowId" }, { PositionRole, "position" }, { TitleRole, "title" }, { ArtistRole, "artist" },
        { AlbumRole, "album" }, { DurationMsRole, "durationMs" }, { StatusRole, "status" },
        { VideoIdRole, "videoId" }, { ReasonRole, "reason" }, { CandidateRole, "candidate" },
    };
}

QVariantMap ImportRowModel::get(int index) const
{
    QVariantMap map;
    if (index < 0 || index >= m_items.size())
        return map;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromLatin1(it.value()), data(this->index(index), it.key()));
    return map;
}

void ImportRowModel::reset(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    m_index.clear();
    for (int i = 0; i < m_items.size(); ++i)
        m_index.insert(m_items.at(i).rowId, i);
    endResetModel();
    Q_EMIT countChanged();
}

void ImportRowModel::append(const QList<Item> &items)
{
    QList<Item> fresh;
    for (const Item &item : items) {
        if (m_index.contains(item.rowId))
            update(item);
        else
            fresh.append(item);
    }
    if (fresh.isEmpty())
        return;
    beginInsertRows(QModelIndex(), int(m_items.size()), int(m_items.size() + fresh.size() - 1));
    for (const Item &item : std::as_const(fresh)) {
        m_index.insert(item.rowId, int(m_items.size()));
        m_items.append(item);
    }
    endInsertRows();
    Q_EMIT countChanged();
}

void ImportRowModel::update(const Item &item)
{
    const int at = m_index.value(item.rowId, -1);
    if (at < 0)
        return;
    m_items[at] = item;
    Q_EMIT dataChanged(index(at), index(at));
}

// ------------------------------------------------------------------ jobs

struct PlaylistImport::Job {
    int id = 0;
    int playlistId = 0;
    QString kind;         // a link's kind ("spotify-playlist"), or "list" or "csv"
    QString url;
    QString name;
    int total = 0;
    QString state;        // running, resting, waiting, paused, done, cancelled
    // "<rows read>\t<the source's own cursor>"; the source's part empty
    // once it is all read (read_done).
    QString cursor;
    bool readDone = false;
    int failures = 0;     // refusals in a row
    QString note;
    qint64 resumeAt = 0;  // UTC ms; 0 for none
    Import::Link link;
};

struct PlaylistImport::RowData {
    int id = 0;
    int jobId = 0;
    int position = 0;
    QString title;
    QStringList artists;
    QString album;
    qint64 durationMs = 0;
    QString isrc;
    QString sourceId;
    bool eitherWay = false;
    QString status;
    QString videoId;
    QString reason;
    QVariantMap candidate;
    int searches = 0;
};

// One song being searched for: its searches in turn, and the best so far.
struct PlaylistImport::Run {
    int jobId = 0;
    RowData row;
    ImportMatch::Wanted wanted;
    QStringList keys;
    QList<ImportMatch::Search> searches;
    int index = 0;
    bool haveBest = false;
    ImportMatch::Candidate best;
    ImportMatch::Verdict bestVerdict;
    int bestOrder = 0;
    // Why nothing, should nothing be found: the first normal search's top
    // result and what was wrong with it.
    QString whyNot;
    qint64 startedMs = 0;
    int asked = 0;
};

PlaylistImport::PlaylistImport(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
{
    m_clock.start();
    m_innerTube = new InnerTube(this, /*warmUp=*/false);
    // No cookies, and so never the account, on anything the import asks.
    m_innerTube->setCookieless(true);
    m_reader = new Import::Reader(m_innerTube, this);
    m_reader->setGap(m_timing.pageGapMs, m_timing.pageJitterMs);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &PlaylistImport::step);
}

PlaylistImport::~PlaylistImport()
{
    // What is out is dropped; the job stays as it is in the database, and
    // goes on at the next launch.
    ++m_epoch;
}

void PlaylistImport::setTiming(const Timing &timing)
{
    m_timing = timing;
    m_reader->setGap(timing.pageGapMs, timing.pageJitterMs);
}

void PlaylistImport::setTestServer(const QString &base)
{
    Import::Reader::setTestServer(base);
}

void PlaylistImport::load()
{
    // YouTube's continuation tokens are not worth keeping across a launch:
    // a list not read to its end is read again from its first page, and the
    // rows already kept stay as they are (each by its place in the list).
    {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("SELECT id, kind, cursor FROM import_jobs WHERE read_done = 0"
                                 " AND state NOT IN ('done', 'cancelled')"));
        if (q.exec()) {
            QList<int> restart;
            while (q.next()) {
                if (youtubeKind(q.value(1).toString()))
                    restart.append(q.value(0).toInt());
            }
            for (const int id : std::as_const(restart)) {
                QSqlQuery reset(AppDatabase::connection());
                reset.prepare(QStringLiteral("UPDATE import_jobs SET cursor = '0\t' WHERE id = ?"));
                reset.addBindValue(id);
                reset.exec();
            }
        }
    }
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id, state FROM import_jobs WHERE state NOT IN ('done', 'cancelled') ORDER BY id DESC"));
    int unfinished = 0;
    while (q.next()) {
        if (m_shownJob == 0)
            m_shownJob = q.value(0).toInt();
        ++unfinished;
    }
    if (m_shownJob == 0) {
        QSqlQuery latest(AppDatabase::connection());
        if (latest.exec(QStringLiteral("SELECT MAX(id) FROM import_jobs")) && latest.next())
            m_shownJob = latest.value(0).toInt();
    }
    if (m_shownJob > 0)
        showJob(m_shownJob);
    refreshActive();
    if (unfinished > 0) {
        qInfo("import: %d import(s) left unfinished; going on in %d ms", unfinished, m_timing.resumeDelayMs);
        setBusy(true);
        scheduleStep(m_timing.resumeDelayMs);
    } else if (const qint64 rest = restingUntil(); rest > 0) {
        // A rest that outlived the imports still keeps the covers waiting.
        qInfo("import: YouTube Music refused a little while ago; resting until %s", qPrintable(clockText(rest)));
        setBusy(true);
        scheduleStep(rest - nowMs() + 50);
    }
}

// ------------------------------------------------------------------ preview

QVariantMap PlaylistImport::parse(const QString &text) const
{
    const Import::Link link = Import::parseLink(text);
    return {
        { QStringLiteral("ok"), link.ok },
        { QStringLiteral("platform"), link.platform },
        { QStringLiteral("type"), link.type },
        { QStringLiteral("kind"), link.ok ? link.kind() : QString() },
        { QStringLiteral("label"), link.label },
        { QStringLiteral("reason"), link.reason },
        { QStringLiteral("looksLikeList"), link.looksLikeList },
    };
}

void PlaylistImport::preview(const QString &text)
{
    const Import::Link link = Import::parseLink(text);
    const quint64 token = ++m_previewToken;
    if (!link.ok) {
        m_previewReady = false;
        Import::Page page;
        page.error = link.reason.isEmpty() ? QStringLiteral("Paste a playlist's or an album's link.") : link.reason;
        m_previewed = previewMap(QStringLiteral("failed"), link, page);
        Q_EMIT previewedChanged();
        return;
    }
    // A list imported before, or being imported: said first, before it is
    // imported twice by mistake.
    const auto withWarning = [this](QVariantMap map, const Import::Link &of) {
        const QString before = importedBefore(of);
        if (!before.isEmpty()) {
            QStringList warnings = map.value(QStringLiteral("warnings")).toStringList();
            warnings.prepend(before);
            map.insert(QStringLiteral("warnings"), warnings);
        }
        return map;
    };
    // The same list again, as the dialog opens on it twice: read once.
    if (m_previewReady && m_previewLink.kind() == link.kind() && m_previewLink.id == link.id
        && m_previewLink.songId == link.songId) {
        m_previewed = withWarning(previewMap(QStringLiteral("ready"), m_previewLink, m_previewPage), m_previewLink);
        Q_EMIT previewedChanged();
        return;
    }
    // YouTube Music refused a little while ago: not asked again, even for
    // this, until the rest is over.
    if (const qint64 rest = restingUntil(); rest > 0 && youtubeKind(link.kind())) {
        m_previewReady = false;
        Import::Page page;
        page.error = QStringLiteral("YouTube Music asked Monolist to slow down a little while ago, so Monolist "
                                    "doesn't ask it anything until %1. Try this link then.").arg(clockText(rest));
        m_previewed = previewMap(QStringLiteral("failed"), link, page);
        Q_EMIT previewedChanged();
        return;
    }
    m_previewReady = false;
    m_previewed = previewMap(QStringLiteral("loading"), link, Import::Page());
    Q_EMIT previewedChanged();
    m_reader->first(link, [this, token, link, withWarning](const Import::Page &page) {
        if (token != m_previewToken)
            return;   // another link was previewed meanwhile
        m_previewLink = link;
        m_previewPage = page;
        // Songs in hand, or more to read (a Deezer playlist's header alone).
        m_previewReady = page.error.isEmpty() && (!page.rows.isEmpty() || !page.cursor.isEmpty());
        if (page.error.isEmpty() && youtubeKind(link.kind()))
            answered();
        m_previewed = previewMap(m_previewReady ? QStringLiteral("ready") : QStringLiteral("failed"), link, page);
        if (m_previewReady)
            m_previewed = withWarning(m_previewed, link);
        if (!m_previewReady && page.error.isEmpty())
            m_previewed.insert(QStringLiteral("error"), QStringLiteral("There are no songs to import there."));
        Q_EMIT previewedChanged();
    });
}

void PlaylistImport::previewList(const QString &text)
{
    ++m_previewToken;
    Import::Link link;
    link.ok = true;
    link.platform = QStringLiteral("list");
    link.type = QStringLiteral("list");
    link.label = QStringLiteral("Pasted list");
    m_previewLink = link;
    m_previewPage = Import::readList(text);
    m_previewReady = m_previewPage.error.isEmpty();
    m_previewed = previewMap(m_previewReady ? QStringLiteral("ready") : QStringLiteral("failed"), link, m_previewPage);
    Q_EMIT previewedChanged();
}

void PlaylistImport::previewFile(const QString &fileUrl)
{
    ++m_previewToken;
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    Import::Link link;
    link.ok = true;
    link.platform = QStringLiteral("csv");
    link.type = QStringLiteral("csv");
    link.label = QStringLiteral("CSV file");
    link.url = path;
    QFile file(path);
    Import::Page page;
    if (QFileInfo(path).size() > kMaxFileBytes) {
        page.error = QStringLiteral("That file is too big to be a playlist.");
    } else if (!file.open(QIODevice::ReadOnly)) {
        page.error = QStringLiteral("That file can't be opened: %1").arg(file.errorString());
    } else {
        page = Import::readCsv(file.readAll(), QFileInfo(path).completeBaseName());
    }
    m_previewLink = link;
    m_previewPage = page;
    m_previewReady = page.error.isEmpty();
    m_previewed = previewMap(m_previewReady ? QStringLiteral("ready") : QStringLiteral("failed"), link, page);
    Q_EMIT previewedChanged();
}

void PlaylistImport::clearPreview()
{
    ++m_previewToken;
    m_previewReady = false;
    m_previewLink = Import::Link();
    m_previewPage = Import::Page();
    m_previewed.clear();
    Q_EMIT previewedChanged();
}

// ------------------------------------------------------------------ start

int PlaylistImport::start(const QString &requestedName)
{
    if (!m_previewReady || !m_library)
        return 0;
    const Import::Link link = m_previewLink;
    const Import::Page page = m_previewPage;
    QString name = requestedName.trimmed();
    if (name.isEmpty())
        name = page.name.trimmed().isEmpty() ? QStringLiteral("Imported playlist") : page.name.trimmed();

    const int playlistId = m_library->createPlaylist(name);
    if (playlistId <= 0) {
        Q_EMIT notice(QStringLiteral("The playlist could not be made, so nothing was imported."));
        return 0;
    }
    const bool list = link.platform == QLatin1String("list") || link.platform == QLatin1String("csv");
    Job job;
    job.playlistId = playlistId;
    job.kind = list ? link.platform : link.kind();
    job.url = link.url;
    job.name = name;
    job.state = QStringLiteral("running");
    const int read = int(qMin<qsizetype>(page.rows.size(), Import::kMaxRows));
    job.readDone = page.cursor.isEmpty() || read >= Import::kMaxRows;
    // What it is counted against: the source's own count while there is
    // more to read; once all is read, the songs read. Spotify's 434 is said
    // in the preview, but only its first 100 are imported.
    job.total = job.readDone ? read : qMax(page.total, read);
    job.cursor = job.readDone ? QString::number(read) : QString::number(read) + QLatin1Char('\t') + page.cursor;
    job.link = link;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO import_jobs (playlist_id, kind, url, name, total, state, cursor, read_done, created_at, updated_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(job.playlistId);
    q.addBindValue(job.kind);
    q.addBindValue(AppDatabase::text(job.url));
    q.addBindValue(job.name);
    q.addBindValue(job.total);
    q.addBindValue(job.state);
    q.addBindValue(job.cursor);
    q.addBindValue(job.readDone ? 1 : 0);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        qWarning("import: could not keep the import: %s", qPrintable(q.lastError().text()));
        Q_EMIT notice(QStringLiteral("The import could not be started."));
        return 0;
    }
    job.id = q.lastInsertId().toInt();
    insertRows(job, page.rows.mid(0, read), 0);
    qInfo("import: #%d started: %s, %d song(s) read of %d, into playlist %d", job.id, qPrintable(job.kind), read,
          job.total, job.playlistId);

    clearPreview();
    m_shownJob = job.id;
    showJob(job.id);
    refreshActive();
    setBusy(true);
    // A YouTube list's songs are its own: in at once.
    settleFromSource(job);
    scheduleStep(0);
    return job.id;
}

// ------------------------------------------------------------------ the pump

void PlaylistImport::setBusy(bool busy)
{
    if (busy == m_busy)
        return;
    m_busy = busy;
    qInfo("import: %s", busy ? "busy" : "idle");
    Q_EMIT busyChanged();
}

void PlaylistImport::scheduleStep(qint64 inMs)
{
    inMs = qBound<qint64>(0, inMs, kMaxPauseMs);
    if (m_timer.isActive() && m_timer.remainingTime() >= 0 && m_timer.remainingTime() <= inMs)
        return;
    m_timer.start(int(inMs));
}

void PlaylistImport::step()
{
    if (m_inFlight)
        return;   // its answer steps again
    if (m_run) {
        runSearch();
        return;
    }
    Job job;
    if (!nextJob(&job)) {
        m_activeJob = 0;
        return;
    }
    m_activeJob = job.id;
    if (m_library && m_library->playlistName(job.playlistId).isNull()) {
        finishJob(job, QStringLiteral("cancelled"), QStringLiteral("The playlist was deleted, so the import stopped."));
        scheduleStep(0);
        return;
    }
    if (!job.readDone)
        readMore(job);
    else
        findNext(job);
}

// The job to work on: imports go one at a time, in the order they were
// started. One resting or waiting holds the ones after it too (YouTube is
// as tired of them as of it); one the user paused does not. A rest on
// YouTube Music's refusal holds them all, whichever was refused: stopping
// that one, or starting another, does not cut it short.
bool PlaylistImport::nextJob(Job *job)
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id FROM import_jobs WHERE state IN ('running', 'resting', 'waiting', 'paused')"
                          " ORDER BY id"));
    QList<int> ids;
    while (q.next())
        ids.append(q.value(0).toInt());
    const qint64 now = nowMs();
    const qint64 rest = restingUntil();
    for (const int id : std::as_const(ids)) {
        Job candidate;
        if (!loadJob(id, &candidate))
            continue;
        if (candidate.state == QLatin1String("paused") && candidate.resumeAt == 0)
            continue;   // the user's pause
        const qint64 own = candidate.state == QLatin1String("running") ? 0 : candidate.resumeAt;
        if (rest > now && rest > own) {
            hold(candidate, rest);
            setBusy(true);
            scheduleStep(rest - now + 50);
            return false;
        }
        if (candidate.state != QLatin1String("running")) {
            if (candidate.resumeAt > now) {
                setBusy(true);
                scheduleStep(candidate.resumeAt - now + 50);
                return false;
            }
            qInfo("import: #%d goes on after %s", candidate.id, qPrintable(candidate.state));
            candidate.state = QStringLiteral("running");
            candidate.resumeAt = 0;
            candidate.note.clear();
            saveJob(candidate);
            refreshJob(candidate.id);
        }
        *job = candidate;
        setBusy(true);
        return true;
    }
    // None left to go on, but YouTube Music's rest is not over: the
    // recommender's covers, which ask it too, go on waiting with it.
    if (rest > now) {
        setBusy(true);
        scheduleStep(rest - now + 50);
        return false;
    }
    setBusy(false);
    return false;
}

// A job held by YouTube Music's rest: it says so, and until when. The job
// that was refused already does, in its own words.
void PlaylistImport::hold(Job &job, qint64 until)
{
    const QString state = m_refusals > 1 ? QStringLiteral("paused") : QStringLiteral("resting");
    if (job.state == state && job.resumeAt == until)
        return;
    job.state = state;
    job.resumeAt = until;
    job.note = restNote();
    qInfo("import: #%d waits out YouTube Music's rest, until %s", job.id, qPrintable(clockText(until)));
    saveJob(job);
    refreshJob(job.id);
}

void PlaylistImport::readMore(Job &job)
{
    const int offset = job.cursor.section(QLatin1Char('\t'), 0, 0).toInt();
    const QString sourceCursor = job.cursor.section(QLatin1Char('\t'), 1);
    m_inFlight = true;
    const quint64 epoch = m_epoch;
    const int jobId = job.id;
    const bool youtube = youtubeKind(job.kind);
    m_reader->next(job.link, sourceCursor, [this, epoch, jobId, offset, youtube](const Import::Page &page) {
        m_inFlight = false;
        Job job;
        if (epoch != m_epoch || !loadJob(jobId, &job) || job.state != QLatin1String("running")) {
            // Stopped or paused meanwhile: YouTube's refusal still counts.
            if (youtube && page.retryable && !page.error.isEmpty())
                refusedBy(page.error);
            scheduleStep(0);
            return;
        }
        if (!page.error.isEmpty()) {
            if (page.retryable) {
                failed(job, page.error, youtubeKind(job.kind));
            } else {
                // The rest of the list can't be read: what was read is found.
                qWarning("import: #%d: the rest of the list can't be read (%s)", jobId, qPrintable(page.error));
                job.readDone = true;
                job.cursor = QString::number(offset);
                job.total = offset;
                saveJob(job);
                refreshJob(jobId);
            }
            scheduleStep(0);
            return;
        }
        job.failures = 0;
        if (youtubeKind(job.kind))
            answered();
        const int room = qMax(0, Import::kMaxRows - offset);
        const QList<Import::Row> rows = page.rows.mid(0, room);
        insertRows(job, rows, offset);
        const int read = offset + int(rows.size());
        if (page.total > job.total)
            job.total = page.total;
        job.readDone = page.cursor.isEmpty() || read >= Import::kMaxRows || rows.isEmpty();
        job.cursor = job.readDone ? QString::number(read) : QString::number(read) + QLatin1Char('\t') + page.cursor;
        // All read: counted against what was read, not the source's count.
        if (job.readDone)
            job.total = read;
        saveJob(job);
        refreshJob(jobId);
        settleFromSource(job);
        scheduleStep(0);
    });
}

void PlaylistImport::findNext(Job &job)
{
    const QList<RowData> pending = rowsOf(job.id, QStringLiteral("status = 'pending'"), /*limit=*/1);
    if (pending.isEmpty()) {
        complete(job);
        scheduleStep(0);
        return;
    }
    const RowData &row = pending.first();
    if (!row.videoId.isEmpty()) {
        settleFromSource(job);
        scheduleStep(0);
        return;
    }
    if (m_indexedFor != job.id)
        buildLibraryIndex();
    m_indexedFor = job.id;

    const ImportMatch::Wanted wanted = wantedOf(row);
    const QStringList keys = cacheKeys(job, row);
    LibrarySong song;
    if (inLibrary(wanted, &song)) {
        const QVariantMap candidate{
            { QStringLiteral("videoId"), song.videoId }, { QStringLiteral("title"), song.title },
            { QStringLiteral("artist"), song.artist }, { QStringLiteral("album"), song.album },
            { QStringLiteral("artwork"), song.artwork },
            { QStringLiteral("durationMs"), qint64(song.durationSec) * 1000 },
            { QStringLiteral("isVideo"), song.isVideo }, { QStringLiteral("confidence"), QStringLiteral("sure") },
        };
        settle(job, row, QStringLiteral("found"), song.videoId, QStringLiteral("In your library already"), candidate);
        scheduleStep(0);
        return;
    }
    QVariantMap candidate;
    QString confidence;
    if (cached(keys, &candidate, &confidence) && !rejected(keys, candidate.value(QStringLiteral("videoId")).toString())) {
        settle(job, row, confidence == QLatin1String("likely") ? QStringLiteral("likely") : QStringLiteral("found"),
               candidate.value(QStringLiteral("videoId")).toString(), QStringLiteral("Matched before"), candidate);
        scheduleStep(0);
        return;
    }

    m_run = std::make_unique<Run>();
    m_run->jobId = job.id;
    m_run->row = row;
    m_run->wanted = wanted;
    m_run->keys = keys;
    m_run->searches = ImportMatch::searches(wanted);
    m_run->startedMs = m_clock.elapsed();
    runSearch();
}

void PlaylistImport::runSearch()
{
    if (!m_run)
        return;
    Job job;
    if (!loadJob(m_run->jobId, &job) || job.state != QLatin1String("running")) {
        m_run.reset();
        scheduleStep(0);
        return;
    }
    if (m_run->index >= m_run->searches.size()) {
        conclude();
        return;
    }
    // YouTube Music's rest: nothing is asked until it is over (nextJob says
    // so on the job).
    if (restingUntil() > 0) {
        m_run.reset();
        scheduleStep(0);
        return;
    }
    // The gap after the last search answered.
    const qint64 now = m_clock.elapsed();
    if (m_nextSearchAtMs >= 0 && now < m_nextSearchAtMs) {
        scheduleStep(m_nextSearchAtMs - now);
        return;
    }
    if (!budgetAllows(job)) {
        m_run.reset();
        scheduleStep(0);
        return;
    }
    const ImportMatch::Search search = m_run->searches.at(m_run->index);
    m_inFlight = true;
    ++m_run->asked;
    countSearch();
    Q_EMIT searched(search.query, search.videos);
    const quint64 epoch = m_epoch;
    m_innerTube->searchAnswer(
        search.query, search.videos ? InnerTube::Filter::Videos : InnerTube::Filter::Songs,
        [this, epoch, search](const QJsonObject &root, const QString &error) {
            m_inFlight = false;
            const int jitter = m_timing.searchJitterMs > 0
                                   ? int(QRandomGenerator::global()->bounded(m_timing.searchJitterMs + 1)) : 0;
            m_nextSearchAtMs = m_clock.elapsed() + m_timing.searchGapMs + jitter;
            const bool refusal = !error.isEmpty() || !root.contains(QLatin1String("contents"));
            if (epoch != m_epoch || !m_run) {
                // Asked before a stop or a pause: what YouTube Music said
                // still counts, for every import.
                if (refusal)
                    refusedBy(Import::youtubeTrouble(error));
                else
                    answered();
                scheduleStep(0);
                return;
            }
            Job job;
            if (!loadJob(m_run->jobId, &job)) {
                m_run.reset();
                scheduleStep(0);
                return;
            }
            // A refusal, a page that is not an answer, or an answer with
            // nothing at all in it: YouTube is saying something.
            if (refusal) {
                if (!error.isEmpty())
                    qWarning("import: a search: %s", qPrintable(error));
                m_run.reset();
                failed(job, Import::youtubeTrouble(error), /*youtube=*/true);
                scheduleStep(0);
                return;
            }
            answered();
            if (job.failures > 0) {
                job.failures = 0;
                saveJob(job);
            }
            Run &run = *m_run;
            QList<ImportMatch::Candidate> candidates;
            const QList<InnerTube::Track> tracks = InnerTube::parseSearch(root);
            for (int i = 0; i < tracks.size(); ++i) {
                // An ISRC search's first result is the only one worth
                // anything: when the user said Not this to it, the search
                // found nothing, rather than its junk moving up into its place.
                if (search.isrc && i > 0)
                    break;
                if (!rejected(run.keys, tracks.at(i).videoId))
                    candidates.append(ImportMatch::fromTrack(tracks.at(i), search.videos));
            }
            const ImportMatch::Pick pick = ImportMatch::pick(run.wanted, candidates, search);
            if (run.whyNot.isEmpty() && !search.isrc) {
                if (candidates.isEmpty()) {
                    run.whyNot = QStringLiteral("YouTube Music found nothing for \"%1\"").arg(search.query);
                } else {
                    const ImportMatch::Verdict top = ImportMatch::judge(run.wanted, candidates.first());
                    run.whyNot = QStringLiteral("The closest, \"%1\" by %2: %3")
                                     .arg(candidates.first().title, candidates.first().artistLine,
                                          top.reason.isEmpty() ? ImportMatch::confidenceName(top.confidence)
                                                               : top.reason);
                }
            }
            if (pick.index >= 0) {
                const ImportMatch::Candidate &found = candidates.at(pick.index);
                const int order = run.index * 100 + pick.index;
                if (!run.haveBest
                    || ImportMatch::better(pick.verdict.confidence, found.videoType, order,
                                           run.bestVerdict.confidence, run.best.videoType, run.bestOrder)) {
                    run.haveBest = true;
                    run.best = found;
                    run.bestVerdict = pick.verdict;
                    run.bestOrder = order;
                }
            }
            ++run.index;
            // Enough: sure or likely ends it; with no length to go by, so
            // does one to check, which another search could not better.
            const ImportMatch::Confidence best = run.haveBest ? run.bestVerdict.confidence : ImportMatch::Confidence::None;
            if (best == ImportMatch::Confidence::Sure || best == ImportMatch::Confidence::Likely
                || (best == ImportMatch::Confidence::Check && run.wanted.durationSec <= 0)
                || run.index >= run.searches.size()) {
                conclude();
                return;
            }
            scheduleStep(0);
        },
        /*retries=*/0);
}

void PlaylistImport::conclude()
{
    if (!m_run)
        return;
    const std::unique_ptr<Run> run = std::move(m_run);
    Job job;
    if (!loadJob(run->jobId, &job)) {
        scheduleStep(0);
        return;
    }
    QString status = QStringLiteral("notfound");
    QString reason = run->whyNot.isEmpty() ? QStringLiteral("YouTube Music found nothing like it") : run->whyNot;
    QString videoId;
    QVariantMap candidate;
    if (run->haveBest) {
        candidate = candidateMap(run->best, run->bestVerdict);
        switch (run->bestVerdict.confidence) {
        case ImportMatch::Confidence::Sure:
            status = QStringLiteral("found");
            videoId = run->best.videoId;
            reason = QString();
            break;
        case ImportMatch::Confidence::Likely:
            status = QStringLiteral("likely");
            videoId = run->best.videoId;
            reason = run->bestVerdict.reason;
            break;
        case ImportMatch::Confidence::Check:
            status = QStringLiteral("check");
            reason = run->bestVerdict.reason;
            break;
        case ImportMatch::Confidence::None:
            break;
        }
    }
    RowData row = run->row;
    row.searches = run->asked;
    m_searchedRows += 1;
    m_searchedMs += m_clock.elapsed() - run->startedMs;
    settle(job, row, status, videoId, reason, candidate);
    if (status == QLatin1String("found") || status == QLatin1String("likely"))
        remember(run->keys, candidate, ImportMatch::confidenceName(run->bestVerdict.confidence));
    scheduleStep(0);
}

void PlaylistImport::settle(const Job &job, const RowData &row, const QString &status, const QString &videoId,
                            const QString &reason, const QVariantMap &candidate)
{
    RowData settled = row;
    settled.status = status;
    settled.videoId = videoId;
    settled.reason = reason;
    if (!candidate.isEmpty())
        settled.candidate = candidate;
    if (status == QLatin1String("found") || status == QLatin1String("likely")) {
        if (!addAt(job, settled, trackOf(settled.candidate))) {
            Job stopped = job;
            finishJob(stopped, QStringLiteral("cancelled"),
                      QStringLiteral("The playlist was deleted, so the import stopped."));
            return;
        }
        LibrarySong song;
        song.title = settled.candidate.value(QStringLiteral("title")).toString();
        song.artist = settled.candidate.value(QStringLiteral("artist")).toString();
        song.album = settled.candidate.value(QStringLiteral("album")).toString();
        song.artwork = settled.candidate.value(QStringLiteral("artwork")).toString();
        song.videoId = videoId;
        song.durationSec = int(settled.candidate.value(QStringLiteral("durationMs")).toLongLong() / 1000);
        song.isVideo = settled.candidate.value(QStringLiteral("isVideo")).toBool();
        noteInLibrary(song);
    }
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = ?, video_id = ?, reason = ?, candidate = ?, searches = ?"
                             " WHERE id = ?"));
    q.addBindValue(status);
    q.addBindValue(AppDatabase::text(videoId));
    q.addBindValue(AppDatabase::text(reason));
    q.addBindValue(AppDatabase::text(toJson(settled.candidate)));
    q.addBindValue(settled.searches);
    q.addBindValue(settled.id);
    q.exec();
    if (job.id == m_shownJob)
        m_rows.update(itemOf(settled));
    Q_EMIT rowSettled(job.id, settled.position, status);
    refreshJob(job.id);
}

// A YouTube list's rows: the songs themselves, in at once and in order.
void PlaylistImport::settleFromSource(const Job &job)
{
    const QList<RowData> rows = rowsOf(job.id, QStringLiteral("status = 'pending' AND video_id <> ''"));
    if (rows.isEmpty() || !m_library)
        return;
    QVariantList tracks;
    for (const RowData &row : rows)
        tracks.append(trackOf(row.candidate));
    const QList<int> entries = m_library->addImported(job.playlistId, tracks);
    if (entries.isEmpty()) {
        Job stopped = job;
        finishJob(stopped, QStringLiteral("cancelled"), QStringLiteral("The playlist was deleted, so the import stopped."));
        return;
    }
    // A song newly added says nothing; one the playlist had already says so.
    // (An empty reason, never a null one, which the NOT NULL column refuses.)
    const auto reasonFor = [&entries](int i) {
        return entries.value(i) > 0 ? QStringLiteral("") : QStringLiteral("Already in the playlist");
    };
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE import_rows SET status = 'found', reason = ? WHERE id = ?"));
    for (int i = 0; i < rows.size(); ++i) {
        q.addBindValue(reasonFor(i));
        q.addBindValue(rows.at(i).id);
        if (!q.exec())
            qWarning("import: #%d: row %d could not be kept as found: %s", job.id, rows.at(i).position,
                     qPrintable(q.lastError().text()));
    }
    db.commit();
    QList<ImportRowModel::Item> items;
    for (int i = 0; i < rows.size(); ++i) {
        RowData row = rows.at(i);
        row.status = QStringLiteral("found");
        row.reason = reasonFor(i);
        items.append(itemOf(row));
        Q_EMIT rowSettled(job.id, row.position, row.status);
    }
    if (job.id == m_shownJob)
        m_rows.append(items);
    refreshJob(job.id);
}

// A refusal. YouTube Music's (`youtube`) is counted for every import at once,
// and rests them all; the count goes on until it answers again, whichever
// import asks next. A source's (Spotify, JioSaavn, Deezer, Apple Music) is
// this job's alone. Either way: the first in a row rests a quarter of an
// hour, the second pauses an hour, and each after that twice as long, to a
// day.
void PlaylistImport::failed(Job &job, const QString &why, bool youtube)
{
    int refusals = 0;
    qint64 until = 0;
    if (youtube) {
        until = refusedBy(why);
        refusals = m_refusals;
    } else {
        refusals = job.failures + 1;
        until = nowMs() + (refusals <= 1 ? m_timing.restMs : pauseFor(refusals));
    }
    job.failures = refusals;
    job.resumeAt = until;
    if (refusals <= 1) {
        job.state = QStringLiteral("resting");
        job.note = QStringLiteral("%1, so the import rests for %2 and tries again at %3.")
                       .arg(why, minutesText(m_timing.restMs), clockText(job.resumeAt));
    } else {
        job.state = QStringLiteral("paused");
        job.note = QStringLiteral("%1 again, so the import is paused; it goes on by itself at %2.")
                       .arg(why, clockText(job.resumeAt));
    }
    qWarning("import: #%d %s: %s", job.id, qPrintable(job.state), qPrintable(why));
    saveJob(job);
    refreshJob(job.id);
}

qint64 PlaylistImport::refusedBy(const QString &why)
{
    loadRest();
    ++m_refusals;
    m_restUntilMs = nowMs() + (m_refusals <= 1 ? m_timing.restMs : pauseFor(m_refusals));
    m_restWhy = why;
    saveRest();
    qWarning("import: YouTube Music refused (%d in a row): %s; nothing is asked of it until %s", m_refusals,
             qPrintable(why), qPrintable(clockText(m_restUntilMs)));
    return m_restUntilMs;
}

qint64 PlaylistImport::pauseFor(int refusals) const
{
    qint64 pause = m_timing.pauseMs;
    for (int i = 2; i < refusals && pause < kMaxPauseMs; ++i)
        pause *= 2;
    return qMin(pause, kMaxPauseMs);
}

qint64 PlaylistImport::restingUntil()
{
    loadRest();
    const qint64 now = nowMs();
    if (m_restUntilMs <= now)
        return 0;
    // A clock put back makes a rest lie further ahead than any can be.
    return qMin(m_restUntilMs, now + kMaxPauseMs);
}

QString PlaylistImport::restNote() const
{
    const QString why = m_restWhy.isEmpty() ? QStringLiteral("YouTube Music refused a search") : m_restWhy;
    const QString at = clockText(qMin(m_restUntilMs, nowMs() + kMaxPauseMs));
    if (m_refusals <= 1)
        return QStringLiteral("%1, so every import rests until %2.").arg(why, at);
    return QStringLiteral("%1 again, so every import is paused; they go on by themselves at %2.").arg(why, at);
}

// YouTube Music answered: its refusals are forgotten. Only an answer does
// this; never a stop, a start or a resume.
void PlaylistImport::answered()
{
    loadRest();
    if (m_refusals == 0 && m_restUntilMs == 0)
        return;
    qInfo("import: YouTube Music answers again; %d refusal(s) forgotten", m_refusals);
    m_refusals = 0;
    m_restUntilMs = 0;
    m_restWhy.clear();
    saveRest();
}

void PlaylistImport::loadRest()
{
    if (m_restLoaded || !m_library)
        return;
    m_restLoaded = true;
    const QString value = m_library->settingValue(kRestKey);
    m_restUntilMs = value.section(QLatin1Char(' '), 0, 0).toLongLong();
    m_refusals = value.section(QLatin1Char(' '), 1, 1).toInt();
    m_restWhy = value.section(QLatin1Char(' '), 2).trimmed();
}

void PlaylistImport::saveRest()
{
    if (!m_library)
        return;
    m_library->setSetting(kRestKey, m_refusals == 0 && m_restUntilMs == 0
                                        ? QString()
                                        : QStringLiteral("%1 %2 %3").arg(m_restUntilMs).arg(m_refusals).arg(m_restWhy));
}

void PlaylistImport::complete(Job &job)
{
    finishJob(job, QStringLiteral("done"), QString());
}

void PlaylistImport::finishJob(Job &job, const QString &state, const QString &note)
{
    if (m_run && m_run->jobId == job.id)
        m_run.reset();
    if (m_activeJob == job.id) {
        ++m_epoch;
        m_activeJob = 0;
    }
    // Deleted while it ran: there is nothing left to review, and nowhere to
    // review it.
    const bool gone = m_library && m_library->playlistName(job.playlistId).isNull();
    if (state == QLatin1String("cancelled")) {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("UPDATE import_rows SET status = 'notfound', reason = ?"
                                 " WHERE job_id = ? AND status = 'pending'"));
        q.addBindValue(kNotSearched);
        q.addBindValue(job.id);
        q.exec();
        if (job.id == m_shownJob)
            showJob(job.id);
    }
    job.state = state;
    job.note = note;
    job.resumeAt = 0;
    saveJob(job);
    {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("UPDATE import_jobs SET finished_at = ? WHERE id = ?"));
        q.addBindValue(QDateTime::currentSecsSinceEpoch());
        q.addBindValue(job.id);
        q.exec();
    }
    refreshJob(job.id);

    const QVariantMap counts = jobMap(job);
    const int added = counts.value(QStringLiteral("found")).toInt() + counts.value(QStringLiteral("likely")).toInt();
    const int all = counts.value(QStringLiteral("read")).toInt();
    const int check = counts.value(QStringLiteral("check")).toInt();
    const int unasked = counts.value(QStringLiteral("notSearched")).toInt();
    const int missing = counts.value(QStringLiteral("notFound")).toInt() - unasked;
    QString summary;
    if (gone)
        summary = QStringLiteral("‘%1’ was deleted, so its import stopped").arg(job.name);
    else if (state == QLatin1String("done") && check == 0 && missing == 0)
        summary = QStringLiteral("Imported ‘%1’: all %2 songs").arg(job.name).arg(all);
    else
        summary = QStringLiteral("%1 ‘%2’: %3 of %4").arg(state == QLatin1String("done") ? QStringLiteral("Imported")
                                                                                                    : QStringLiteral("Stopped"),
                                                                   job.name)
                      .arg(added).arg(all);
    QStringList rest;
    if (check > 0)
        rest << QStringLiteral("%1 to check").arg(check);
    if (missing > 0)
        rest << QStringLiteral("%1 not found").arg(missing);
    if (unasked > 0)
        rest << QStringLiteral("%1 not looked for").arg(unasked);
    if (!rest.isEmpty() && !gone)
        summary += QStringLiteral(" — ") + rest.join(QStringLiteral(", "));
    qInfo("import: #%d %s: %s", job.id, qPrintable(state), qPrintable(summary));
    Q_EMIT finished(job.id, summary);
}

// ------------------------------------------------------------------ the budget

void PlaylistImport::loadBudget()
{
    if (m_budgetLoaded || !m_library)
        return;
    m_budgetLoaded = true;
    const QStringList parts = m_library->settingValue(kBudgetKey).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() == 4) {
        m_hourStart = parts.at(0).toLongLong();
        m_hourCount = parts.at(1).toInt();
        m_dayStart = parts.at(2).toLongLong();
        m_dayCount = parts.at(3).toInt();
    }
}

bool PlaylistImport::budgetAllows(Job &job)
{
    loadBudget();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // A clock put back makes a window's start lie ahead: it starts afresh.
    if (now - m_hourStart >= 3600 || now < m_hourStart) {
        m_hourStart = now;
        m_hourCount = 0;
    }
    if (now - m_dayStart >= 86400 || now < m_dayStart) {
        m_dayStart = now;
        m_dayCount = 0;
    }
    qint64 until = 0;
    QString why;
    if (m_dayCount >= m_timing.perDay) {
        until = (m_dayStart + 86400) * 1000;
        why = QStringLiteral("Monolist searches YouTube Music at most %1 times a day").arg(m_timing.perDay);
    } else if (m_hourCount >= m_timing.perHour) {
        until = (m_hourStart + 3600) * 1000;
        why = QStringLiteral("Monolist searches YouTube Music at most %1 times an hour").arg(m_timing.perHour);
    }
    if (until == 0)
        return true;
    job.state = QStringLiteral("waiting");
    job.resumeAt = until;
    job.note = QStringLiteral("%1, so the import goes on at %2.").arg(why, clockText(until));
    qInfo("import: #%d waits: %s", job.id, qPrintable(why));
    saveJob(job);
    refreshJob(job.id);
    return false;
}

void PlaylistImport::countSearch()
{
    loadBudget();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - m_hourStart >= 3600 || now < m_hourStart) {
        m_hourStart = now;
        m_hourCount = 0;
    }
    if (now - m_dayStart >= 86400 || now < m_dayStart) {
        m_dayStart = now;
        m_dayCount = 0;
    }
    ++m_hourCount;
    ++m_dayCount;
    if (m_library)
        m_library->setSetting(kBudgetKey, QStringLiteral("%1 %2 %3 %4").arg(m_hourStart).arg(m_hourCount)
                                              .arg(m_dayStart).arg(m_dayCount));
}

// ------------------------------------------------------------------ the user's

void PlaylistImport::pause(int jobId)
{
    const int id = jobId > 0 ? jobId : (m_activeJob > 0 ? m_activeJob : m_shownJob);
    Job job;
    if (!loadJob(id, &job) || job.state == QLatin1String("done") || job.state == QLatin1String("cancelled"))
        return;
    if (m_activeJob == id) {
        ++m_epoch;
        m_run.reset();
        m_activeJob = 0;
    }
    job.state = QStringLiteral("paused");
    job.resumeAt = 0;
    job.note = QStringLiteral("Paused.");
    saveJob(job);
    refreshJob(id);
    qInfo("import: #%d paused", id);
    scheduleStep(0);
}

void PlaylistImport::resume(int jobId)
{
    const int id = jobId > 0 ? jobId : m_shownJob;
    Job job;
    if (!loadJob(id, &job) || job.state == QLatin1String("done") || job.state == QLatin1String("cancelled")
        || job.state == QLatin1String("running"))
        return;
    job.state = QStringLiteral("running");
    job.resumeAt = 0;
    job.failures = 0;
    job.note.clear();
    saveJob(job);
    refreshJob(id);
    qInfo("import: #%d resumed", id);
    setBusy(true);
    scheduleStep(0);
}

void PlaylistImport::cancel(int jobId)
{
    const int id = jobId > 0 ? jobId : (m_activeJob > 0 ? m_activeJob : m_shownJob);
    Job job;
    if (!loadJob(id, &job) || job.state == QLatin1String("done") || job.state == QLatin1String("cancelled"))
        return;
    finishJob(job, QStringLiteral("cancelled"), QStringLiteral("Stopped; the songs found are kept."));
    scheduleStep(0);
}

void PlaylistImport::showJob(int jobId)
{
    m_shownJob = jobId;
    QList<ImportRowModel::Item> items;
    for (const RowData &row : rowsOf(jobId))
        items.append(itemOf(row));
    m_rows.reset(items);
    Job job;
    m_jobMap = loadJob(jobId, &job) ? jobMap(job) : QVariantMap();
    Q_EMIT jobChanged();
}

int PlaylistImport::jobForPlaylist(int playlistId) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT MAX(id) FROM import_jobs WHERE playlist_id = ?"));
    q.addBindValue(playlistId);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

QString PlaylistImport::importedBefore(const Import::Link &link) const
{
    if (!link.ok || link.id.isEmpty() || !m_library)
        return {};
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT url, state, playlist_id FROM import_jobs WHERE kind = ? ORDER BY id DESC"));
    q.addBindValue(link.kind());
    if (!q.exec())
        return {};
    const QString what = link.type == QLatin1String("album") ? QStringLiteral("album")
                         : link.type == QLatin1String("song") ? QStringLiteral("song") : QStringLiteral("playlist");
    while (q.next()) {
        const Import::Link other = Import::parseLink(q.value(0).toString());
        if (other.id != link.id || other.songId != link.songId)
            continue;
        // Its playlist deleted since: as good as never imported.
        const QString name = m_library->playlistName(q.value(2).toInt());
        if (name.isNull())
            continue;
        const QString state = q.value(1).toString();
        if (state != QLatin1String("done") && state != QLatin1String("cancelled"))
            return QStringLiteral("This %1 is being imported now, into ‘%2’; importing it again makes a second "
                                  "playlist.").arg(what, name);
        return QStringLiteral("This %1 was imported before, into ‘%2’; importing it again makes a second playlist.")
            .arg(what, name);
    }
    return {};
}

QVariantMap PlaylistImport::jobInfo(int jobId) const
{
    Job job;
    return loadJob(jobId, &job) ? jobMap(job) : QVariantMap();
}

QVariantMap PlaylistImport::review(int playlistId) const
{
    QVariantMap result;
    QVariantList check;
    QVariantList notFound;
    QVariantList notSearched;
    const int jobId = jobForPlaylist(playlistId);
    Job job;
    if (jobId > 0 && loadJob(jobId, &job)) {
        for (const RowData &row : rowsOf(jobId, QStringLiteral("status IN ('check', 'notfound')"))) {
            if (row.status == QLatin1String("check"))
                check.append(rowMap(row));
            else if (row.reason == kNotSearched)
                notSearched.append(rowMap(row));
            else
                notFound.append(rowMap(row));
        }
        result.insert(QStringLiteral("name"), job.name);
        result.insert(QStringLiteral("state"), jobMap(job).value(QStringLiteral("state")));
    }
    result.insert(QStringLiteral("jobId"), jobId);
    result.insert(QStringLiteral("check"), check);
    result.insert(QStringLiteral("notFound"), notFound);
    result.insert(QStringLiteral("notSearched"), notSearched);
    return result;
}

void PlaylistImport::lookForStopped(int jobId)
{
    Job job;
    if (!loadJob(jobId, &job) || (job.state != QLatin1String("done") && job.state != QLatin1String("cancelled")))
        return;
    if (m_library && m_library->playlistName(job.playlistId).isNull())
        return;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = 'pending', reason = '' WHERE job_id = ?"
                             " AND status = 'notfound' AND reason = ?"));
    q.addBindValue(job.id);
    q.addBindValue(kNotSearched);
    if (!q.exec())
        return;
    const int again = q.numRowsAffected();
    if (again <= 0 && job.readDone)
        return;
    // A YouTube list stopped before it was all read: read again from its
    // first page, its continuation being stale by now; the rows kept stay.
    if (!job.readDone && youtubeKind(job.kind))
        job.cursor = QStringLiteral("0\t");
    job.state = QStringLiteral("running");
    job.note.clear();
    job.resumeAt = 0;
    job.failures = 0;
    saveJob(job);
    QSqlQuery reopen(AppDatabase::connection());
    reopen.prepare(QStringLiteral("UPDATE import_jobs SET finished_at = 0 WHERE id = ?"));
    reopen.addBindValue(job.id);
    reopen.exec();
    qInfo("import: #%d looks for the %d song(s) its stop left", job.id, again);
    m_shownJob = job.id;
    showJob(job.id);
    refreshJob(job.id);
    setBusy(true);
    // A YouTube list's songs are its own: in at once.
    settleFromSource(job);
    scheduleStep(0);
}

void PlaylistImport::dismissStopped(int jobId)
{
    Job job;
    if (!loadJob(jobId, &job))
        return;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = 'dismissed' WHERE job_id = ? AND status = 'notfound'"
                             " AND reason = ?"));
    q.addBindValue(job.id);
    q.addBindValue(kNotSearched);
    q.exec();
    if (job.id == m_shownJob)
        showJob(job.id);
    refreshJob(job.id);
}

int PlaylistImport::estimateSeconds(int songs) const
{
    return int(qMin<qint64>(estimate(songs, kGuessSecsPerSong), std::numeric_limits<int>::max()));
}

// A few seconds a song, and no faster than the caps let it go: at least a
// search a song (unless the library has it or it was matched before), at
// most perHour of them in an hour and perDay in a day, each counted from
// the first search in it, as budgetAllows counts them.
qint64 PlaylistImport::estimate(qint64 songs, double perSong) const
{
    if (songs <= 0)
        return 0;
    const qint64 perHour = qMax(1, m_timing.perHour);
    const qint64 perDay = qMax<qint64>(1, m_timing.perDay);
    const auto withinDay = [&](qint64 n) {
        const qint64 hours = (n - 1) / perHour;
        return hours * 3600 + qint64(double(n - hours * perHour) * perSong);
    };
    const qint64 days = (songs - 1) / perDay;
    return days * 86400 + withinDay(songs - days * perDay);
}

bool PlaylistImport::useCandidate(int rowId)
{
    RowData row;
    Job job;
    if (!rowById(rowId, &row, &job) || row.candidate.value(QStringLiteral("videoId")).toString().isEmpty())
        return false;
    QVariantMap candidate = row.candidate;
    candidate.insert(QStringLiteral("confidence"), QStringLiteral("sure"));
    if (!addAt(job, row, trackOf(candidate)))
        return false;
    row.status = QStringLiteral("found");
    row.videoId = candidate.value(QStringLiteral("videoId")).toString();
    row.reason = QStringLiteral("Chosen by you");
    row.candidate = candidate;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = ?, video_id = ?, reason = ?, candidate = ? WHERE id = ?"));
    q.addBindValue(row.status);
    q.addBindValue(row.videoId);
    q.addBindValue(row.reason);
    q.addBindValue(toJson(row.candidate));
    q.addBindValue(row.id);
    q.exec();
    remember(cacheKeys(job, row), candidate, QStringLiteral("sure"));
    if (job.id == m_shownJob)
        m_rows.update(itemOf(row));
    refreshJob(job.id);
    Q_EMIT notice(QStringLiteral("Added to %1").arg(job.name));
    return true;
}

bool PlaylistImport::useSong(int rowId, const QVariantMap &track)
{
    RowData row;
    Job job;
    const QString videoId = track.value(QStringLiteral("sourceId")).toString().isEmpty()
                                ? track.value(QStringLiteral("videoId")).toString()
                                : track.value(QStringLiteral("sourceId")).toString();
    if (videoId.isEmpty() || !rowById(rowId, &row, &job))
        return false;
    QVariantMap candidate{
        { QStringLiteral("videoId"), videoId },
        { QStringLiteral("title"), track.value(QStringLiteral("title")) },
        { QStringLiteral("artist"), track.value(QStringLiteral("artist")) },
        { QStringLiteral("album"), track.value(QStringLiteral("album")) },
        { QStringLiteral("artwork"), track.value(QStringLiteral("artwork")) },
        { QStringLiteral("durationMs"), track.value(QStringLiteral("durationMs")).toLongLong() },
        { QStringLiteral("isVideo"), track.value(QStringLiteral("isVideo")).toBool() },
        { QStringLiteral("confidence"), QStringLiteral("sure") },
    };
    row.candidate = candidate;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET candidate = ? WHERE id = ?"));
    q.addBindValue(toJson(candidate));
    q.addBindValue(row.id);
    q.exec();
    return useCandidate(rowId);
}

void PlaylistImport::notThis(int rowId)
{
    RowData row;
    Job job;
    if (!rowById(rowId, &row, &job))
        return;
    const QString videoId = row.candidate.value(QStringLiteral("videoId")).toString();
    const QStringList keys = cacheKeys(job, row);
    if (!videoId.isEmpty()) {
        QSqlDatabase db = AppDatabase::connection();
        db.transaction();
        for (const QString &key : keys) {
            QSqlQuery no(db);
            no.prepare(QStringLiteral("INSERT OR REPLACE INTO import_matches (key, video_id, confidence, matcher,"
                                      " matched_at) VALUES (?, ?, 'rejected', ?, ?)"));
            no.addBindValue(key + QStringLiteral("#not:") + videoId);
            no.addBindValue(videoId);
            no.addBindValue(ImportMatch::version());
            no.addBindValue(QDateTime::currentSecsSinceEpoch());
            no.exec();
            QSqlQuery forget(db);
            forget.prepare(QStringLiteral("DELETE FROM import_matches WHERE key = ? AND video_id = ?"));
            forget.addBindValue(key);
            forget.addBindValue(videoId);
            forget.exec();
        }
        db.commit();
    }
    row.status = QStringLiteral("notfound");
    row.videoId.clear();
    row.reason = QStringLiteral("Not this song, you said");
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = ?, video_id = '', reason = ? WHERE id = ?"));
    q.addBindValue(row.status);
    q.addBindValue(row.reason);
    q.addBindValue(row.id);
    q.exec();
    if (job.id == m_shownJob)
        m_rows.update(itemOf(row));
    refreshJob(job.id);
}

void PlaylistImport::dismiss(int rowId)
{
    RowData row;
    Job job;
    if (!rowById(rowId, &row, &job))
        return;
    row.status = QStringLiteral("dismissed");
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_rows SET status = 'dismissed' WHERE id = ?"));
    q.addBindValue(row.id);
    q.exec();
    if (job.id == m_shownJob)
        m_rows.update(itemOf(row));
    refreshJob(job.id);
}

QString PlaylistImport::searchTextFor(int rowId) const
{
    RowData row;
    Job job;
    if (!rowById(rowId, &row, &job))
        return {};
    return (ImportMatch::bareTitle(row.title) + QLatin1Char(' ') + row.artists.value(0)).simplified();
}

int PlaylistImport::copyNotFound(int jobId)
{
    // The ones the review counts under Not found: not those a stop left
    // unasked, which it lists apart.
    QStringList lines;
    for (const RowData &row : rowsOf(jobId, QStringLiteral("status = 'notfound'"))) {
        if (row.reason == kNotSearched)
            continue;
        QString line = row.artists.isEmpty() ? row.title
                                             : row.artists.join(QStringLiteral(", ")) + QStringLiteral(" - ") + row.title;
        if (row.durationMs > 0)
            line += QStringLiteral(" (%1)").arg(lengthText(row.durationMs));
        lines.append(line);
    }
    if (lines.isEmpty())
        return 0;
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()); app && app->clipboard())
        app->clipboard()->setText(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
    Q_EMIT notice(lines.size() == 1 ? QStringLiteral("Copied the song not found")
                                    : QStringLiteral("Copied %1 songs not found").arg(lines.size()));
    return int(lines.size());
}

// ------------------------------------------------------------------ storage

bool PlaylistImport::loadJob(int jobId, Job *job) const
{
    if (jobId <= 0)
        return false;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT id, playlist_id, kind, url, name, total, state, cursor, read_done, failures, note,"
                             " resume_at FROM import_jobs WHERE id = ?"));
    q.addBindValue(jobId);
    if (!q.exec() || !q.next())
        return false;
    job->id = q.value(0).toInt();
    job->playlistId = q.value(1).toInt();
    job->kind = q.value(2).toString();
    job->url = q.value(3).toString();
    job->name = q.value(4).toString();
    job->total = q.value(5).toInt();
    job->state = q.value(6).toString();
    job->cursor = q.value(7).toString();
    job->readDone = q.value(8).toBool();
    job->failures = q.value(9).toInt();
    job->note = q.value(10).toString();
    job->resumeAt = q.value(11).toLongLong();
    if (job->kind == QLatin1String("list") || job->kind == QLatin1String("csv")) {
        job->link = Import::Link();
        job->link.ok = true;
        job->link.platform = job->kind;
        job->link.type = job->kind;
        job->link.label = job->kind == QLatin1String("list") ? QStringLiteral("Pasted list") : QStringLiteral("CSV file");
        job->link.url = job->url;
    } else {
        job->link = Import::parseLink(job->url);
    }
    return true;
}

void PlaylistImport::saveJob(const Job &job)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE import_jobs SET total = ?, state = ?, cursor = ?, read_done = ?, failures = ?,"
                             " note = ?, resume_at = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(job.total);
    q.addBindValue(job.state);
    q.addBindValue(AppDatabase::text(job.cursor));
    q.addBindValue(job.readDone ? 1 : 0);
    q.addBindValue(job.failures);
    q.addBindValue(AppDatabase::text(job.note));
    q.addBindValue(job.resumeAt);
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    q.addBindValue(job.id);
    if (!q.exec())
        qWarning("import: could not keep #%d: %s", job.id, qPrintable(q.lastError().text()));
}

QList<PlaylistImport::RowData> PlaylistImport::rowsOf(int jobId, const QString &where, int limit) const
{
    QList<RowData> rows;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT id, job_id, position, title, artists, album, duration_ms, isrc, source_id,"
                             " either_way, status, video_id, reason, candidate, searches FROM import_rows"
                             " WHERE job_id = ?%1 ORDER BY position%2")
                  .arg(where.isEmpty() ? QString() : QStringLiteral(" AND (") + where + QLatin1Char(')'),
                       limit > 0 ? QStringLiteral(" LIMIT %1").arg(limit) : QString()));
    q.addBindValue(jobId);
    if (!q.exec())
        return rows;
    while (q.next()) {
        RowData row;
        row.id = q.value(0).toInt();
        row.jobId = q.value(1).toInt();
        row.position = q.value(2).toInt();
        row.title = q.value(3).toString();
        row.artists = q.value(4).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        row.album = q.value(5).toString();
        row.durationMs = q.value(6).toLongLong();
        row.isrc = q.value(7).toString();
        row.sourceId = q.value(8).toString();
        row.eitherWay = q.value(9).toBool();
        row.status = q.value(10).toString();
        row.videoId = q.value(11).toString();
        row.reason = q.value(12).toString();
        row.candidate = fromJson(q.value(13).toString());
        row.searches = q.value(14).toInt();
        rows.append(row);
    }
    return rows;
}

bool PlaylistImport::rowById(int rowId, RowData *row, Job *job) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT job_id FROM import_rows WHERE id = ?"));
    q.addBindValue(rowId);
    if (!q.exec() || !q.next())
        return false;
    const int jobId = q.value(0).toInt();
    if (!loadJob(jobId, job))
        return false;
    for (const RowData &each : rowsOf(jobId, QStringLiteral("id = %1").arg(rowId))) {
        *row = each;
        return true;
    }
    return false;
}

int PlaylistImport::insertRows(Job &job, const QList<Import::Row> &rows, int from)
{
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    // A place already kept (a YouTube list read again from its first page)
    // is left as it is; anything else refused is an error, and said.
    q.prepare(QStringLiteral(
        "INSERT INTO import_rows (job_id, position, title, artists, album, duration_ms, isrc, source_id,"
        " either_way, status, video_id, candidate) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?, ?)"
        " ON CONFLICT(job_id, position) DO NOTHING"));
    int inserted = 0;
    for (int i = 0; i < rows.size(); ++i) {
        const Import::Row &row = rows.at(i);
        QVariantMap candidate;
        if (!row.videoId.isEmpty()) {
            candidate = {
                { QStringLiteral("videoId"), row.videoId },
                { QStringLiteral("title"), row.title },
                { QStringLiteral("artist"), row.artists.join(QStringLiteral(", ")) },
                { QStringLiteral("album"), row.album },
                { QStringLiteral("artwork"), row.artwork },
                { QStringLiteral("durationMs"), row.durationMs },
                { QStringLiteral("isVideo"), row.isVideo },
                { QStringLiteral("confidence"), QStringLiteral("sure") },
            };
        }
        q.addBindValue(job.id);
        q.addBindValue(from + i);
        q.addBindValue(AppDatabase::text(row.title));
        // A song with no artist (a title alone pasted, a CSV with no artist
        // column) joins to a null string: bound as it is, the row would be
        // ignored without a word.
        q.addBindValue(AppDatabase::text(row.artists.join(QLatin1Char('\n'))));
        q.addBindValue(AppDatabase::text(row.album));
        q.addBindValue(row.durationMs);
        q.addBindValue(AppDatabase::text(row.isrc));
        q.addBindValue(AppDatabase::text(row.sourceId));
        q.addBindValue(row.eitherWay ? 1 : 0);
        q.addBindValue(AppDatabase::text(row.videoId));
        q.addBindValue(AppDatabase::text(toJson(candidate)));
        if (!q.exec())
            qWarning("import: #%d: song %d (\"%s\") could not be kept: %s", job.id, from + i, qPrintable(row.title),
                     qPrintable(q.lastError().text()));
        else if (q.numRowsAffected() > 0)
            ++inserted;
    }
    db.commit();
    if (job.id == m_shownJob && !rows.isEmpty()) {
        QList<ImportRowModel::Item> items;
        for (const RowData &row : rowsOf(job.id, QStringLiteral("position >= %1 AND position < %2")
                                                     .arg(from).arg(from + rows.size())))
            items.append(itemOf(row));
        m_rows.append(items);
    }
    return inserted;
}

void PlaylistImport::refreshJob(int jobId)
{
    if (jobId == m_shownJob) {
        Job job;
        if (loadJob(jobId, &job)) {
            m_jobMap = jobMap(job);
            Q_EMIT jobChanged();
        }
    }
    refreshActive();
    touch();
}

// The import the sidebar shows: the first one not yet done, in the order
// they go, one paused by hand after any other; and how many more wait.
void PlaylistImport::refreshActive()
{
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral("SELECT id FROM import_jobs WHERE state NOT IN ('done', 'cancelled')"
                          " ORDER BY CASE WHEN state = 'paused' AND resume_at = 0 THEN 1 ELSE 0 END, id"));
    int first = 0;
    int unfinished = 0;
    while (q.next()) {
        if (first == 0)
            first = q.value(0).toInt();
        ++unfinished;
    }
    QVariantMap map;
    Job job;
    if (first > 0 && loadJob(first, &job)) {
        map = jobMap(job);
        map.insert(QStringLiteral("others"), unfinished - 1);
    }
    if (map == m_activeMap)
        return;
    m_activeMap = map;
    Q_EMIT activeJobChanged();
}

void PlaylistImport::touch()
{
    ++m_revision;
    Q_EMIT revisionChanged();
}

QVariantMap PlaylistImport::jobMap(const Job &job) const
{
    QHash<QString, int> counts;
    int read = 0;
    int notSearched = 0;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT status, reason = ?, COUNT(*) FROM import_rows WHERE job_id = ? GROUP BY 1, 2"));
    q.addBindValue(kNotSearched);
    q.addBindValue(job.id);
    if (q.exec()) {
        while (q.next()) {
            const QString status = q.value(0).toString();
            const int n = q.value(2).toInt();
            counts[status] += n;
            read += n;
            if (status == QLatin1String("notfound") && q.value(1).toInt() == 1)
                notSearched += n;
        }
    }
    QString state = job.state;
    if (state == QLatin1String("running")) {
        state = job.readDone ? QStringLiteral("finding") : QStringLiteral("reading");
        // Another import is ahead of it.
        QSqlQuery ahead(AppDatabase::connection());
        ahead.prepare(QStringLiteral("SELECT 1 FROM import_jobs WHERE id < ? AND (state IN ('running', 'resting',"
                                     " 'waiting') OR (state = 'paused' AND resume_at > 0)) LIMIT 1"));
        ahead.addBindValue(job.id);
        if (ahead.exec() && ahead.next())
            state = QStringLiteral("queued");
    }
    const int pending = counts.value(QStringLiteral("pending")) + (job.readDone ? 0 : qMax(0, job.total - read));
    const double perSong = m_searchedRows > 0 ? double(m_searchedMs) / double(m_searchedRows) / 1000.0
                                              : kGuessSecsPerSong;
    // A YouTube list's songs are not searched for: only its pages are read.
    qint64 secondsLeft = youtubeKind(job.kind) ? 0 : estimate(pending, perSong);
    if (job.resumeAt > 0)
        secondsLeft += qMax<qint64>(0, (job.resumeAt - nowMs()) / 1000);
    const bool done = job.state == QLatin1String("done") || job.state == QLatin1String("cancelled");
    return {
        { QStringLiteral("id"), job.id },
        { QStringLiteral("playlistId"), job.playlistId },
        { QStringLiteral("name"), job.name },
        { QStringLiteral("platform"), job.link.platform },
        { QStringLiteral("type"), job.link.type },
        { QStringLiteral("label"), job.link.label },
        { QStringLiteral("url"), job.url },
        { QStringLiteral("state"), state },
        { QStringLiteral("note"), job.note },
        { QStringLiteral("resumeAt"), job.resumeAt },
        { QStringLiteral("resumeText"), job.resumeAt > 0 ? clockText(job.resumeAt) : QString() },
        // All read, the songs read; until then, what the source says.
        { QStringLiteral("total"), job.readDone ? read : qMax(job.total, read) },
        { QStringLiteral("read"), read },
        { QStringLiteral("found"), counts.value(QStringLiteral("found")) },
        { QStringLiteral("likely"), counts.value(QStringLiteral("likely")) },
        { QStringLiteral("check"), counts.value(QStringLiteral("check")) },
        { QStringLiteral("notFound"), counts.value(QStringLiteral("notfound")) },
        { QStringLiteral("notSearched"), notSearched },
        { QStringLiteral("dismissed"), counts.value(QStringLiteral("dismissed")) },
        { QStringLiteral("pending"), pending },
        { QStringLiteral("done"), done },
        { QStringLiteral("playlistGone"), done && m_library && m_library->playlistName(job.playlistId).isNull() },
        { QStringLiteral("secondsLeft"), done ? 0 : secondsLeft },
    };
}

// ------------------------------------------------------------------ matching's

void PlaylistImport::buildLibraryIndex()
{
    m_libraryIndex.clear();
    QSqlQuery q(AppDatabase::connection());
    q.exec(QStringLiteral(
        "SELECT title, artist, album, artwork, duration_ms, source_id, is_video FROM tracks WHERE source_id <> ''"
        " UNION ALL SELECT title, artist, album, artwork, duration_ms, video_id, is_video FROM playlist_tracks"
        " WHERE video_id <> ''"));
    while (q.next()) {
        LibrarySong song;
        song.title = q.value(0).toString();
        song.artist = q.value(1).toString();
        song.album = q.value(2).toString();
        song.artwork = q.value(3).toString();
        song.durationSec = int(q.value(4).toLongLong() / 1000);
        song.videoId = q.value(5).toString();
        song.isVideo = q.value(6).toBool();
        noteInLibrary(song);
    }
}

void PlaylistImport::noteInLibrary(const LibrarySong &song)
{
    if (song.videoId.isEmpty() || song.durationSec <= 0)
        return;
    const QString key = ImportMatch::titleKey(song.title, Saavn::splitArtists(song.artist));
    if (key.isEmpty())
        return;
    QList<LibrarySong> &list = m_libraryIndex[key];
    for (const LibrarySong &each : std::as_const(list)) {
        if (each.videoId == song.videoId)
            return;
    }
    list.append(song);
}

bool PlaylistImport::inLibrary(const ImportMatch::Wanted &wanted, LibrarySong *song) const
{
    if (wanted.durationSec <= 0)
        return false;
    QStringList keys{ ImportMatch::titleKey(wanted.title, wanted.artists) };
    if (wanted.eitherWay && !wanted.artists.isEmpty())
        keys.append(ImportMatch::titleKey(wanted.artists.first(), { wanted.title }));
    for (const QString &key : std::as_const(keys)) {
        for (const LibrarySong &each : m_libraryIndex.value(key)) {
            if (ImportMatch::sameSong(wanted, each.title, each.artist, each.durationSec, each.album)) {
                *song = each;
                return true;
            }
        }
    }
    return false;
}

QStringList PlaylistImport::cacheKeys(const Job &job, const RowData &row) const
{
    QStringList keys;
    if (!row.sourceId.isEmpty()) {
        const QString platform = row.sourceId.startsWith(QLatin1String("spotify:")) ? QStringLiteral("spotify")
                                                                                     : job.link.platform;
        keys.append(QStringLiteral("id:%1:%2").arg(platform, row.sourceId));
    }
    if (!row.isrc.isEmpty())
        keys.append(QStringLiteral("isrc:") + row.isrc.toUpper());
    // By title, version and artist, and length: "Yellow (Live)" is not a
    // song matched for "Yellow", whatever their lengths (none, in a pasted
    // list), nor a Telugu album's song its Hindi dub.
    keys.append(QStringLiteral("tal:%1|%2").arg(ImportMatch::songKey(wantedOf(row))).arg((row.durationMs + 500) / 1000));
    return keys;
}

bool PlaylistImport::cached(const QStringList &keys, QVariantMap *candidate, QString *confidence) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT video_id, title, artist, album, artwork, duration_ms, is_video, confidence"
                             " FROM import_matches WHERE key = ? AND matcher = ? AND confidence IN ('sure', 'likely')"));
    for (const QString &key : keys) {
        q.addBindValue(key);
        q.addBindValue(ImportMatch::version());
        if (!q.exec() || !q.next())
            continue;
        *candidate = {
            { QStringLiteral("videoId"), q.value(0).toString() }, { QStringLiteral("title"), q.value(1).toString() },
            { QStringLiteral("artist"), q.value(2).toString() }, { QStringLiteral("album"), q.value(3).toString() },
            { QStringLiteral("artwork"), q.value(4).toString() },
            { QStringLiteral("durationMs"), q.value(5).toLongLong() },
            { QStringLiteral("isVideo"), q.value(6).toBool() }, { QStringLiteral("confidence"), q.value(7).toString() },
        };
        *confidence = q.value(7).toString();
        return !candidate->value(QStringLiteral("videoId")).toString().isEmpty();
    }
    return false;
}

void PlaylistImport::remember(const QStringList &keys, const QVariantMap &candidate, const QString &confidence)
{
    const QString videoId = candidate.value(QStringLiteral("videoId")).toString();
    if (videoId.isEmpty())
        return;
    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO import_matches (key, video_id, title, artist, album, artwork,"
                             " duration_ms, is_video, confidence, matcher, matched_at)"
                             " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const QString &key : keys) {
        q.addBindValue(key);
        q.addBindValue(videoId);
        q.addBindValue(AppDatabase::text(candidate.value(QStringLiteral("title")).toString()));
        q.addBindValue(AppDatabase::text(candidate.value(QStringLiteral("artist")).toString()));
        q.addBindValue(AppDatabase::text(candidate.value(QStringLiteral("album")).toString()));
        q.addBindValue(AppDatabase::text(candidate.value(QStringLiteral("artwork")).toString()));
        q.addBindValue(candidate.value(QStringLiteral("durationMs")).toLongLong());
        q.addBindValue(candidate.value(QStringLiteral("isVideo")).toBool() ? 1 : 0);
        q.addBindValue(confidence);
        q.addBindValue(ImportMatch::version());
        q.addBindValue(QDateTime::currentSecsSinceEpoch());
        q.exec();
    }
    db.commit();
}

bool PlaylistImport::rejected(const QStringList &keys, const QString &videoId) const
{
    if (videoId.isEmpty())
        return false;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("SELECT 1 FROM import_matches WHERE key = ? AND confidence = 'rejected'"));
    for (const QString &key : keys) {
        q.addBindValue(key + QStringLiteral("#not:") + videoId);
        if (q.exec() && q.next())
            return true;
    }
    return false;
}

// Into the playlist at the song's place in the source's order: after the
// last song of an earlier row of this import. A song found in its turn goes
// at the end, which is that place; one the user chose later goes back
// among its neighbours.
bool PlaylistImport::addAt(const Job &job, const RowData &row, const QVariantMap &track)
{
    if (!m_library)
        return false;
    const QList<int> entries = m_library->addImported(job.playlistId, { track });
    if (entries.isEmpty())
        return false;   // the playlist is gone
    const int entry = entries.first();
    if (entry <= 0)
        return true;    // already there

    QSet<QString> earlier;
    QSqlQuery before(AppDatabase::connection());
    before.prepare(QStringLiteral("SELECT video_id FROM import_rows WHERE job_id = ? AND position < ?"
                                  " AND status IN ('found', 'likely') AND video_id <> ''"));
    before.addBindValue(job.id);
    before.addBindValue(row.position);
    if (before.exec()) {
        while (before.next())
            earlier.insert(before.value(0).toString());
    }
    QSqlQuery list(AppDatabase::connection());
    list.prepare(QStringLiteral("SELECT id, video_id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position, id"));
    list.addBindValue(job.playlistId);
    int index = 0;
    int at = -1;
    int place = 0;
    if (list.exec()) {
        while (list.next()) {
            if (list.value(0).toInt() == entry)
                at = index;
            else if (earlier.contains(list.value(1).toString()))
                place = index + 1;
            ++index;
        }
    }
    // The new entry counted in `place` only if it sat before an earlier one.
    if (at >= 0 && place > at)
        --place;
    if (at >= 0 && place != at)
        m_library->movePlaylistEntry(job.playlistId, entry, place);
    return true;
}

ImportMatch::Wanted PlaylistImport::wantedOf(const RowData &row)
{
    ImportMatch::Wanted wanted;
    wanted.title = row.title;
    wanted.artists = row.artists;
    wanted.album = row.album;
    wanted.durationSec = int((row.durationMs + 500) / 1000);
    wanted.isrc = row.isrc;
    wanted.eitherWay = row.eitherWay;
    return wanted;
}

ImportRowModel::Item PlaylistImport::itemOf(const RowData &row)
{
    ImportRowModel::Item item;
    item.rowId = row.id;
    item.position = row.position;
    item.title = row.title;
    item.artist = row.artists.join(QStringLiteral(", "));
    item.album = row.album;
    item.durationMs = row.durationMs;
    item.status = row.status;
    item.videoId = row.videoId;
    item.reason = row.reason;
    item.candidate = row.candidate;
    return item;
}

QVariantMap PlaylistImport::rowMap(const RowData &row)
{
    return {
        { QStringLiteral("rowId"), row.id },
        { QStringLiteral("position"), row.position },
        { QStringLiteral("title"), row.title },
        { QStringLiteral("artist"), row.artists.join(QStringLiteral(", ")) },
        { QStringLiteral("album"), row.album },
        { QStringLiteral("durationMs"), row.durationMs },
        { QStringLiteral("status"), row.status },
        { QStringLiteral("videoId"), row.videoId },
        { QStringLiteral("reason"), row.reason },
        { QStringLiteral("candidate"), row.candidate },
    };
}

// A candidate as a track with the app's roles, for the library.
QVariantMap PlaylistImport::trackOf(const QVariantMap &candidate)
{
    return {
        { QStringLiteral("sourceId"), candidate.value(QStringLiteral("videoId")) },
        { QStringLiteral("title"), candidate.value(QStringLiteral("title")) },
        { QStringLiteral("artist"), candidate.value(QStringLiteral("artist")) },
        { QStringLiteral("album"), candidate.value(QStringLiteral("album")) },
        { QStringLiteral("artwork"), candidate.value(QStringLiteral("artwork")) },
        { QStringLiteral("durationMs"), candidate.value(QStringLiteral("durationMs")).toLongLong() },
        { QStringLiteral("isVideo"), candidate.value(QStringLiteral("isVideo")).toBool() },
    };
}
