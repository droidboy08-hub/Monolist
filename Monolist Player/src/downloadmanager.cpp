#include "downloadmanager.h"
#include "appdatabase.h"
#include "saavndownload.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QVariant>

namespace {

const QString kFormatKey = QStringLiteral("download_format");
const QString kSkipNonMusicKey = QStringLiteral("download_skip_non_music");
// The folder the listener chose; empty or missing for the default.
const QString kDirectoryKey = QStringLiteral("download_dir");
// How stale a look for the tools may be before it is taken again. Not on every
// call: "Download all" enqueues a whole album in one go, and a tool that is not
// bundled is looked for along the whole of PATH.
constexpr qint64 kToolsRecheckMs = 3000;
// A burst of changes to the queue (a whole playlist queued, three downloads
// moving on) is written once.
constexpr int kQueueSaveDelayMs = 1000;
bool g_heldForTest = false;

bool isKnownFormat(const QString &format)
{
    return format == QLatin1String("original") || format == QLatin1String("m4a")
        || format == QLatin1String("mp3");
}

// Whether a file is one yt-dlp has not finished with, judged by what follows
// "[<id>]" in its name, in lower case. A finished download is "[<id>].<ext>"
// and nothing else, so none of these is ever one: a download under way (.part,
// .ytdl, and .part-Frag<n> for a fragment), one stream of several waiting to
// be merged (.f<format>.<ext>), a post-processor's working copy (.temp.<ext>),
// the chapter list FFmpeg reads (.meta), and the thumbnail fetched to embed.
// ".orig.<ext>" is left out on purpose: it is the untouched original that
// yt-dlp moves aside while it converts, complete audio in its own right.
bool isIntermediate(const QString &tail)
{
    if (tail.endsWith(QLatin1String(".part")) || tail.endsWith(QLatin1String(".ytdl"))
        || tail.contains(QLatin1String(".part-frag")))
        return true;
    static const QRegularExpression stream(QStringLiteral(R"(^\.f[0-9a-z_-]+\.[0-9a-z]+$)"));
    if (stream.match(tail).hasMatch())
        return true;
    if (tail == QLatin1String(".temp") || tail.startsWith(QLatin1String(".temp."))
        || tail == QLatin1String(".meta"))
        return true;
    return tail == QLatin1String(".jpg") || tail == QLatin1String(".jpeg")
        || tail == QLatin1String(".png") || tail == QLatin1String(".webp");
}

} // namespace

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent)
    , m_directory(chooseDirectory())
    , m_available(YtDlp::isAvailable())
    , m_canConvert(!YtDlp::ffmpegPath().isEmpty())
{
    m_toolsChecked.start();
    // A folder chosen in Settings, unless one is named outright for a test.
    const QString chosen = settingValue(kDirectoryKey, QString());
    if (!chosen.isEmpty() && qEnvironmentVariable("MONOLIST_DOWNLOAD_DIR").isEmpty()) {
        m_directory = QDir::cleanPath(chosen);
        m_customDirectory = true;
    }
    QDir().mkpath(m_directory);

    const QString format = settingValue(kFormatKey, m_format);
    if (isKnownFormat(format))
        m_format = format;
    m_skipNonMusic = settingValue(kSkipNonMusicKey, QStringLiteral("1")) != QLatin1String("0");

    loadStored();
    m_library.reload();

    m_queueSave.setSingleShot(true);
    m_queueSave.setInterval(kQueueSaveDelayMs);
    connect(&m_queueSave, &QTimer::timeout, this, &DownloadManager::saveQueue);
    restoreQueue();
}

DownloadManager::~DownloadManager()
{
    // Written first, while the ones running still count as running: they
    // are queued again at the next launch, ahead of the rest.
    m_queueSave.stop();
    saveQueue();
    // Stop without the usual failure handling: nothing may start in their place.
    m_pending.clear();
    m_saavnAsking.clear();
    const auto requests = m_requests;
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        if (!it.value())
            continue;
        disconnect(it.value(), nullptr, this, nullptr);
        it.value()->cancel();
        // yt-dlp's partial file stays, for the next launch to carry on from
        // (it continues a .part where it stopped) rather than start again.
        // Should that launch not finish it, a failure or a cancel clears it
        // as it clears any, and a finished download its stale ones.
        if (!m_queueRestored)
            removePartialFiles(it.key());
    }
    // A JioSaavn download stops (FFmpeg with it) as it is deleted, before
    // its files go.
    const auto saavn = m_saavnRequests;
    for (auto it = saavn.cbegin(); it != saavn.cend(); ++it) {
        if (!it.value())
            continue;
        disconnect(it.value(), nullptr, this, nullptr);
        delete it.value().data();
        removePartialFiles(it.key());
    }
}

// ------------------------------------------------------------------- options

void DownloadManager::setFormat(const QString &format)
{
    if (format == m_format || !isKnownFormat(format))
        return;
    m_format = format;
    setSettingValue(kFormatKey, format);
    Q_EMIT optionsChanged();
}

void DownloadManager::setSkipNonMusic(bool skip)
{
    if (skip == m_skipNonMusic)
        return;
    m_skipNonMusic = skip;
    setSettingValue(kSkipNonMusicKey, skip ? QStringLiteral("1") : QStringLiteral("0"));
    Q_EMIT optionsChanged();
}

DownloadOptions DownloadManager::options() const
{
    DownloadOptions options;
    if (m_format == QLatin1String("m4a"))
        options.format = DownloadOptions::Format::M4a;
    else if (m_format == QLatin1String("mp3"))
        options.format = DownloadOptions::Format::Mp3;
    options.skipNonMusic = m_skipNonMusic;
    return options;
}

QString DownloadManager::settingValue(const QString &key, const QString &fallback) const
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next())
        return query.value(0).toString();
    return fallback;
}

void DownloadManager::setSettingValue(const QString &key, const QString &value)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    // Never NULL, which the table refuses: an empty value clears a setting.
    query.addBindValue(AppDatabase::text(value));
    query.exec();
}

// --------------------------------------------------------------------- queue

QString DownloadManager::fileStem(const QString &videoId, const QString &title, const QString &artist)
{
    // "<Artist> - <Title> [<id>]". The id is the identity and keeps names
    // unique; the rest makes the folder browsable in any other player. Most
    // video titles already carry the artist, so it is only added when missing.
    QString name = title.trimmed();
    const QString performer = YtDlp::cleanArtist(artist);
    if (!performer.isEmpty() && !name.contains(performer, Qt::CaseInsensitive))
        name = performer + QStringLiteral(" - ") + name;

    static const QRegularExpression illegal(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    name.replace(illegal, QStringLiteral("_"));
    name = name.simplified().left(120).trimmed();
    // Windows refuses names that end in a dot or a space.
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("track");
    return QStringLiteral("%1 [%2]").arg(name, videoId);
}

void DownloadManager::touch()
{
    ++m_revision;
    Q_EMIT revisionChanged();
    // Every change of state comes through here, so the queue is written
    // after each, a moment later.
    if (m_queueRestored)
        m_queueSave.start();
}

bool DownloadManager::setDownloadDirectory(const QString &folder)
{
    const auto refuse = [this](const QString &why) {
        m_directoryNote = why;
        Q_EMIT directoryChanged();
        return false;
    };
    // Running downloads write where they began, and clear up there.
    if (activeCount() > 0)
        return refuse(QStringLiteral("Wait for the downloads under way to finish, then choose the folder again."));

    QString path = folder.trimmed();
    if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
        path = QUrl(path).toLocalFile();
    const bool reset = path.isEmpty();
    path = QDir::cleanPath(QDir(reset ? chooseDirectory() : path).absolutePath());

    // Written to once, to be sure it can be.
    if (!QDir().mkpath(path))
        return refuse(QStringLiteral("That folder could not be made."));
    {
        QFile probe(QDir(path).filePath(QStringLiteral(".monolist-write-test")));
        if (!probe.open(QIODevice::WriteOnly))
            return refuse(QStringLiteral("Monolist cannot save files in that folder."));
        probe.close();
        probe.remove();
    }

    // The partial files kept for the queue in the old folder: the queue now
    // downloads into the new one, from the start.
    const QString old = m_directory;
    if (QDir(old) != QDir(path)) {
        for (const QString &videoId : std::as_const(m_pending))
            removeLeftovers(old, videoId, filesFor(old, videoId), QString());
    }

    m_directory = path;
    m_customDirectory = !reset;
    setSettingValue(kDirectoryKey, reset ? QString() : path);
    const int found = relinkStored();
    loadStored();
    m_library.reload();
    m_directoryNote = found == 0 ? QString()
                    : found == 1 ? QStringLiteral("1 download that had gone missing was found in this folder.")
                                 : QStringLiteral("%1 downloads that had gone missing were found in this folder.").arg(found);
    qInfo("downloads: now saved in %s%s", qUtf8Printable(QDir::toNativeSeparators(path)),
          found > 0 ? qPrintable(QStringLiteral(", %1 found there again").arg(found)) : "");
    touch();
    Q_EMIT directoryChanged();
    Q_EMIT libraryChanged();
    return true;
}

int DownloadManager::relinkStored()
{
    QList<QPair<QString, QString>> missing;   // video id, the path it had
    QSqlQuery query(AppDatabase::connection());
    if (!query.exec(QStringLiteral("SELECT video_id, file_path FROM downloads")))
        return 0;
    while (query.next()) {
        if (!QFileInfo::exists(query.value(1).toString()))
            missing.append({ query.value(0).toString(), query.value(1).toString() });
    }
    int found = 0;
    for (const auto &[videoId, oldPath] : std::as_const(missing)) {
        const QString path = findWrittenFile(videoId);
        if (path.isEmpty())
            continue;
        QSqlQuery row(AppDatabase::connection());
        row.prepare(QStringLiteral("UPDATE downloads SET file_path = ? WHERE video_id = ?"));
        row.addBindValue(path);
        row.addBindValue(videoId);
        row.exec();
        // The library's row plays the file, as recordStored set it.
        QSqlQuery track(AppDatabase::connection());
        track.prepare(QStringLiteral("UPDATE tracks SET source_url = ? WHERE source_id = ? AND source_url = ?"));
        track.addBindValue(path);
        track.addBindValue(videoId);
        track.addBindValue(oldPath);
        track.exec();
        ++found;
    }
    return found;
}

void DownloadManager::setHeldForTest(bool held)
{
    g_heldForTest = held;
}

void DownloadManager::saveQueueNow()
{
    m_queueSave.stop();
    saveQueue();
}

void DownloadManager::saveQueue()
{
    if (!m_queueRestored)
        return;
    // In the order they will run next time: those running now, then the
    // queue as it stands, then the failed ones, which wait for a retry.
    QList<const DownloadQueueModel::Item *> order;
    const QList<DownloadQueueModel::Item> &items = m_queue.items();
    for (const DownloadQueueModel::Item &item : items) {
        if (item.state != DownloadQueueModel::State::Failed && !m_pending.contains(item.videoId))
            order.append(&item);
    }
    for (const QString &videoId : std::as_const(m_pending)) {
        if (const DownloadQueueModel::Item *item = m_queue.find(videoId))
            order.append(item);
    }
    for (const DownloadQueueModel::Item &item : items) {
        if (item.state == DownloadQueueModel::State::Failed)
            order.append(&item);
    }

    QSqlDatabase db = AppDatabase::connection();
    db.transaction();
    QSqlQuery clear(db);
    clear.exec(QStringLiteral("DELETE FROM download_queue"));
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO download_queue"
        " (video_id, position, title, artist, album, artwork, duration_ms, is_video, failed, error)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    int position = 0;
    for (const DownloadQueueModel::Item *item : std::as_const(order)) {
        insert.addBindValue(item->videoId);
        insert.addBindValue(position++);
        insert.addBindValue(AppDatabase::text(item->title));
        insert.addBindValue(AppDatabase::text(item->artist));
        insert.addBindValue(AppDatabase::text(item->album));
        insert.addBindValue(AppDatabase::text(item->artwork));
        insert.addBindValue(item->durationMs);
        insert.addBindValue(item->isVideo ? 1 : 0);
        insert.addBindValue(item->state == DownloadQueueModel::State::Failed ? 1 : 0);
        insert.addBindValue(AppDatabase::text(item->error));
        if (!insert.exec())
            qWarning("downloads: the queue could not be kept: %s", qPrintable(insert.lastError().text()));
    }
    db.commit();
}

void DownloadManager::restoreQueue()
{
    if (m_queueRestored || !m_available)
        return;
    m_queueRestored = true;

    QSqlQuery query(AppDatabase::connection());
    if (!query.exec(QStringLiteral(
            "SELECT video_id, title, artist, album, artwork, duration_ms, is_video, failed, error"
            " FROM download_queue ORDER BY position"))) {
        return;
    }
    int queued = 0;
    int failed = 0;
    while (query.next()) {
        const QString videoId = query.value(0).toString();
        const QString title = query.value(1).toString();
        const QString artist = query.value(2).toString();
        const QString album = query.value(3).toString();
        const QString artwork = query.value(4).toString();
        const qint64 durationMs = query.value(5).toLongLong();
        const bool isVideo = query.value(6).toInt() != 0;
        // Finished since it was written (the app closed a moment after):
        // nothing to do.
        if (videoId.isEmpty() || m_stored.contains(videoId) || isPending(videoId))
            continue;
        if (query.value(7).toInt() != 0) {
            // Failed: back as it was, waiting for a retry, not tried again
            // unasked.
            DownloadQueueModel::Item item;
            item.videoId = videoId;
            item.title = title;
            item.artist = artist;
            item.album = album;
            item.artwork = artwork;
            item.durationMs = durationMs;
            item.isVideo = isVideo;
            item.state = DownloadQueueModel::State::Failed;
            item.error = query.value(8).toString();
            m_queue.upsert(item);
            ++failed;
        } else if (queueOne(videoId, title, artist, album, artwork, durationMs, isVideo)) {
            ++queued;
        }
    }
    if (queued + failed == 0)
        return;
    qInfo("downloads: %d queued again from the last launch, %d failed ones kept for a retry", queued, failed);
    touch();
    pump();
}

void DownloadManager::touchProgress()
{
    ++m_progressRevision;
    Q_EMIT progressRevisionChanged();
}

void DownloadManager::refreshTools()
{
    m_toolsChecked.start();
    const bool available = YtDlp::isAvailable();
    const bool canConvert = !YtDlp::ffmpegPath().isEmpty();
    if (available == m_available && canConvert == m_canConvert)
        return;
    m_available = available;
    m_canConvert = canConvert;
    Q_EMIT toolsChanged();
    // yt-dlp found at last: the queue the last launch left can run now.
    restoreQueue();
}

void DownloadManager::refreshToolsIfStale()
{
    if (m_toolsChecked.elapsed() > kToolsRecheckMs)
        refreshTools();
}

void DownloadManager::enqueue(const QString &videoId,
                              const QString &title,
                              const QString &artist,
                              const QString &artwork,
                              qint64 durationMs,
                              bool isVideo,
                              const QString &album)
{
    // What was found at launch is not the last word: tools put in place since
    // then are used rather than refused until a restart, and one taken away
    // is noticed here.
    refreshToolsIfStale();
    if (!queueOne(videoId, title, artist, album, artwork, durationMs, isVideo))
        return;
    touch();
    pump();
}

void DownloadManager::enqueueAll(const QVariantList &tracks)
{
    refreshToolsIfStale();
    bool queued = false;
    for (const QVariant &value : tracks) {
        const QVariantMap track = value.toMap();
        queued |= queueOne(track.value(QStringLiteral("sourceId")).toString(),
                           track.value(QStringLiteral("title")).toString(),
                           track.value(QStringLiteral("artist")).toString(),
                           track.value(QStringLiteral("album")).toString(),
                           track.value(QStringLiteral("artwork")).toString(),
                           track.value(QStringLiteral("durationMs")).toLongLong(),
                           track.value(QStringLiteral("isVideo")).toBool());
    }
    if (!queued)
        return;
    touch();
    pump();
}

bool DownloadManager::queueOne(const QString &videoId, const QString &title, const QString &artist,
                               const QString &album, const QString &artwork, qint64 durationMs, bool isVideo)
{
    if (videoId.isEmpty() || !m_available || m_stored.contains(videoId) || isPending(videoId))
        return false;

    DownloadQueueModel::Item item;
    item.videoId = videoId;
    item.title = title.trimmed();   // may be empty: yt-dlp's metadata fills it in
    item.artist = YtDlp::cleanArtist(artist);
    item.album = album.trimmed();
    item.artwork = artwork;
    item.durationMs = durationMs;
    item.isVideo = isVideo;
    m_queue.upsert(item);   // replaces a failed attempt at the same track
    m_pending.append(videoId);
    return true;
}

void DownloadManager::pump()
{
    if (g_heldForTest) {
        Q_EMIT queueChanged();
        return;
    }
    // Playback first: while a song someone is waiting for resolves, a new
    // download waits for it, since its yt-dlp and the song's would each run
    // several times slower side by side. Rarely for long: a song resolves in
    // a fifth of a second unless it has gone to yt-dlp, and never past 20 s.
    // Downloads already running are left alone; one stopped would start
    // again from nothing.
    if (!m_pending.isEmpty() && activeCount() < kMaxConcurrent && YtDlp::playbackResolving()) {
        if (!m_waitingOnPlayback) {
            m_waitingOnPlayback = true;
            qInfo("downloads: %d waiting while a song resolves", int(m_pending.size()));
            YtDlp::whenPlaybackResolved(this, [this]() {
                m_waitingOnPlayback = false;
                pump();
            });
        }
        Q_EMIT queueChanged();
        return;
    }
    bool began = false;
    while (!m_pending.isEmpty() && activeCount() < kMaxConcurrent) {
        begin(m_pending.takeFirst());
        began = true;
    }
    // Waiting has become downloading, which the rows show: they are told
    // here, since what follows only moves the percentage.
    if (began)
        touch();
    Q_EMIT queueChanged();
}

void DownloadManager::begin(const QString &videoId)
{
    const DownloadQueueModel::Item *queued = m_queue.find(videoId);
    if (!queued)
        return;

    DownloadQueueModel::Item item = *queued;
    item.state = DownloadQueueModel::State::Downloading;
    item.progress = 0.0;
    item.received = 0;
    item.total = -1;
    item.speed = -1.0;
    item.eta = -1;
    item.step.clear();
    item.error.clear();
    m_queue.upsert(item);

    // What is already there for this track, before a byte is written: should
    // this attempt fail, its cleanup may take only what the attempt wrote.
    // Taken once, before JioSaavn's copy and before yt-dlp should that fail.
    m_before.insert(videoId, filesFor(m_directory, videoId));

    // With High sound quality, JioSaavn's copy where it certainly has this
    // recording; asked by title, so a song without one goes to yt-dlp. On
    // Standard the finder says no without asking anyone.
    if (m_saavnFinder && !item.title.isEmpty()) {
        m_saavnAsking.insert(videoId);
        m_saavnFinder(videoId, item.title, item.artist, item.album, item.durationMs,
                      [this, videoId](const SaavnCopy &copy) { saavnAnswered(videoId, copy); });
        return;
    }
    startYtDlp(videoId);
}

void DownloadManager::startYtDlp(const QString &videoId)
{
    const DownloadQueueModel::Item *queued = m_queue.find(videoId);
    if (!queued) {
        m_before.remove(videoId);
        pump();
        return;
    }
    // Without a title the stem is left to yt-dlp, which names the file from
    // the metadata it fetches.
    const QString stem = queued->title.isEmpty() ? QString() : fileStem(videoId, queued->title, queued->artist);
    YtDlpRequest *request = YtDlp::download(videoId, m_directory, stem, options(), this);
    m_requests.insert(videoId, request);

    connect(request, &YtDlpRequest::progress, this,
            [this, videoId](qint64 received, qint64 total, double speed, int eta) {
                noteProgress(videoId, received, total, speed, eta);
            });
    connect(request, &YtDlpRequest::postProcessing, this, [this, videoId](const QString &step) {
        noteStep(videoId, step);
    });
    connect(request, &YtDlpRequest::finishedFile, this,
            [this, videoId](const QString &path, const QVariantMap &metadata) {
                complete(videoId, path, metadata);
            });
    connect(request, &YtDlpRequest::failed, this, [this, videoId](const QString &reason) {
        fail(videoId, reason);
    });
}

void DownloadManager::saavnAnswered(const QString &videoId, const SaavnCopy &copy)
{
    if (!m_saavnAsking.remove(videoId))
        return;   // cancelled while JioSaavn was asked
    if (copy.url.isEmpty()) {
        qInfo("downloads: %s from YouTube: %s", qPrintable(videoId), qUtf8Printable(copy.reason));
        startYtDlp(videoId);
        return;
    }
    startSaavn(videoId, copy);
}

void DownloadManager::startSaavn(const QString &videoId, const SaavnCopy &copy)
{
    const DownloadQueueModel::Item *queued = m_queue.find(videoId);
    if (!queued) {
        m_before.remove(videoId);
        pump();
        return;
    }
    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    SaavnDownload::Job job;
    job.videoId = videoId;
    job.url = copy.url;
    job.kbps = copy.kbps;
    job.directory = m_directory;
    job.stem = fileStem(videoId, queued->title, queued->artist);
    job.title = queued->title;
    job.artist = queued->artist;
    // YouTube Music's name for the album where the list gave one, as a
    // download through yt-dlp is tagged; JioSaavn's where it did not.
    job.album = queued->album.isEmpty() ? copy.album : queued->album;
    job.coverUrl = m_coverTemplate.arg(videoId);
    job.options = options();
    job.durationMs = queued->durationMs > 0 ? queued->durationMs : qint64(copy.durationSec) * 1000;

    auto *download = new SaavnDownload(job, m_network, this);
    m_saavnRequests.insert(videoId, download);
    connect(download, &SaavnDownload::progress, this,
            [this, videoId](qint64 received, qint64 total, double speed, int eta) {
                noteProgress(videoId, received, total, speed, eta);
            });
    connect(download, &SaavnDownload::postProcessing, this, [this, videoId](const QString &step) {
        noteStep(videoId, step);
    });
    connect(download, &SaavnDownload::finishedFile, this,
            [this, videoId, download](const QString &path, const QVariantMap &metadata) {
                m_saavnRequests.remove(videoId);
                download->deleteLater();
                qInfo("downloads: %s saved from JioSaavn as %s", qPrintable(videoId),
                      qUtf8Printable(QDir::toNativeSeparators(path)));
                complete(videoId, path, metadata);
            });
    connect(download, &SaavnDownload::failed, this, [this, videoId, download](const QString &reason) {
        m_saavnRequests.remove(videoId);
        download->deleteLater();
        saavnFailed(videoId, reason);
    });
    qInfo("downloads: %s from JioSaavn at %d kbps", qPrintable(videoId), copy.kbps);
    download->start();
}

// JioSaavn's copy fell through: whatever it wrote goes, and yt-dlp downloads
// the song as it would have. A cancel is a cancel, not a failure.
void DownloadManager::saavnFailed(const QString &videoId, const QString &reason)
{
    if (m_cancelling.contains(videoId)) {
        fail(videoId, reason);
        return;
    }
    // Only what this attempt wrote: its names are all unfinished ones, and
    // anything that was there before it began stays (ROADMAP F13).
    const QStringList removed = removeLeftovers(m_directory, videoId, m_before.value(videoId), m_stored.value(videoId));
    qWarning("downloads: %s: JioSaavn's copy failed (%s)%s; downloading it through yt-dlp instead",
             qPrintable(videoId), qUtf8Printable(reason),
             removed.isEmpty() ? "" : qPrintable(QStringLiteral(", %1 left-over file(s) removed").arg(removed.size())));
    if (const DownloadQueueModel::Item *current = m_queue.find(videoId)) {
        DownloadQueueModel::Item item = *current;
        item.state = DownloadQueueModel::State::Downloading;
        item.progress = 0.0;
        item.received = 0;
        item.total = -1;
        item.speed = -1.0;
        item.eta = -1;
        item.step.clear();
        m_queue.upsert(item);
        touch();
    }
    startYtDlp(videoId);
}

void DownloadManager::noteProgress(const QString &videoId, qint64 received, qint64 total, double speed, int eta)
{
    const DownloadQueueModel::Item *current = m_queue.find(videoId);
    if (!current)
        return;
    DownloadQueueModel::Item item = *current;
    const qreal fraction = total > 0 ? qBound(0.0, qreal(received) / qreal(total), 1.0) : item.progress;
    // Progress lines arrive many times a second; repaint on whole-percent
    // steps.
    if (item.state == DownloadQueueModel::State::Downloading && fraction < 1.0
        && qAbs(fraction - item.progress) < 0.01)
        return;
    // Back from processing (a second stream, say) is a new state for the
    // rows; another percent is only a new percentage.
    const bool stateChanged = item.state != DownloadQueueModel::State::Downloading;
    item.state = DownloadQueueModel::State::Downloading;
    item.progress = fraction;
    item.received = received;
    item.total = total;
    item.speed = speed;
    item.eta = eta;
    m_queue.upsert(item);
    if (stateChanged)
        touch();
    else
        touchProgress();
    Q_EMIT progressChanged(videoId, fraction);
}

void DownloadManager::noteStep(const QString &videoId, const QString &step)
{
    const DownloadQueueModel::Item *current = m_queue.find(videoId);
    if (!current)
        return;
    // Some steps run before a single byte arrives (reading metadata,
    // fetching SponsorBlock segments, converting the thumbnail). They are
    // part of starting, which the row already says, not of finishing.
    if (current->received <= 0 && current->progress <= 0.0)
        return;
    DownloadQueueModel::Item item = *current;
    item.state = DownloadQueueModel::State::Processing;
    item.progress = 1.0;
    item.step = step;
    m_queue.upsert(item);
    touch();
}

void DownloadManager::complete(const QString &videoId, const QString &reportedPath,
                               const QVariantMap &metadata)
{
    m_requests.remove(videoId);
    const DownloadQueueModel::Item *current = m_queue.find(videoId);
    if (!current) {
        m_before.remove(videoId);
        pump();
        return;
    }
    DownloadQueueModel::Item item = *current;

    // Fill whatever the caller did not know from what yt-dlp fetched. Its
    // artist field is the performer on YouTube Music tracks; the uploader is
    // the fallback, cleaned of channel suffixes.
    if (item.title.isEmpty())
        item.title = metadata.value(QStringLiteral("title")).toString();
    if (item.title.isEmpty())
        item.title = videoId;
    if (item.artist.isEmpty()) {
        QString artist = metadata.value(QStringLiteral("artist")).toString();
        if (artist.isEmpty())
            artist = metadata.value(QStringLiteral("uploader")).toString();
        if (artist.isEmpty())
            artist = metadata.value(QStringLiteral("channel")).toString();
        item.artist = YtDlp::cleanArtist(artist);
    }
    if (item.durationMs <= 0)
        item.durationMs = qint64(metadata.value(QStringLiteral("duration")).toDouble() * 1000.0);
    if (item.artwork.isEmpty())
        item.artwork = metadata.value(QStringLiteral("thumbnail")).toString();

    // yt-dlp reports the final path once every post-processor has run. Should
    // that line ever go missing, look for the file by its video id instead.
    QString path = reportedPath;
    if (path.isEmpty() || !QFileInfo::exists(path))
        path = findWrittenFile(videoId);
    if (path.isEmpty()) {
        fail(videoId, QStringLiteral("The download finished, but no file was written."));
        return;
    }
    m_before.remove(videoId);
    // A partial file an earlier launch left that this one did not carry on
    // from (another format, say) has nothing left to finish.
    removeLeftovers(m_directory, videoId, filesFor(m_directory, videoId), path);

    recordStored(item, path);
    m_stored.insert(videoId, path);
    m_queue.remove(videoId);
    m_library.reload();
    touch();
    Q_EMIT progressChanged(videoId, 1.0);
    Q_EMIT completed(videoId, path);
    Q_EMIT libraryChanged();
    pump();
}

void DownloadManager::fail(const QString &videoId, const QString &reason)
{
    m_requests.remove(videoId);
    const bool cancelled = m_cancelling.remove(videoId);

    removePartialFiles(videoId);
    if (const DownloadQueueModel::Item *current = m_queue.find(videoId)) {
        DownloadQueueModel::Item item = *current;
        if (cancelled) {
            m_queue.remove(videoId);
        } else {
            item.state = DownloadQueueModel::State::Failed;
            item.error = reason;
            item.speed = -1.0;
            item.eta = -1;
            m_queue.upsert(item);
        }
    }

    touch();
    if (!cancelled)
        Q_EMIT failed(videoId, reason);
    pump();
}

void DownloadManager::cancel(const QString &videoId)
{
    if (m_pending.removeAll(videoId) > 0) {
        m_queue.remove(videoId);
        touch();
        Q_EMIT queueChanged();
        return;
    }

    // Still asking JioSaavn: nothing written, nothing running.
    if (m_saavnAsking.remove(videoId)) {
        m_before.remove(videoId);
        m_queue.remove(videoId);
        touch();
        Q_EMIT queueChanged();
        pump();
        return;
    }

    if (QPointer<SaavnDownload> download = m_saavnRequests.value(videoId); download) {
        m_cancelling.insert(videoId);
        download->cancel();   // its failed() handler clears the entry
        return;
    }

    if (QPointer<YtDlpRequest> request = m_requests.value(videoId); request) {
        m_cancelling.insert(videoId);
        request->cancel();   // the failed() handler clears the entry
        return;
    }

    if (m_queue.find(videoId)) {   // a failed attempt being dismissed
        m_queue.remove(videoId);
        touch();
        Q_EMIT queueChanged();
    }
}

void DownloadManager::retry(const QString &videoId)
{
    const DownloadQueueModel::Item *current = m_queue.find(videoId);
    if (!current || current->state != DownloadQueueModel::State::Failed || isPending(videoId))
        return;
    DownloadQueueModel::Item item = *current;
    item.state = DownloadQueueModel::State::Queued;
    item.error.clear();
    item.progress = 0.0;
    m_queue.upsert(item);
    m_pending.append(videoId);
    touch();
    pump();
}

void DownloadManager::remove(const QString &videoId)
{
    cancel(videoId);

    QString path = m_stored.take(videoId);
    if (path.isEmpty()) {
        QSqlQuery lookup(AppDatabase::connection());
        lookup.prepare(QStringLiteral("SELECT file_path FROM downloads WHERE video_id = ?"));
        lookup.addBindValue(videoId);
        if (lookup.exec() && lookup.next())
            path = lookup.value(0).toString();
    }
    if (!path.isEmpty())
        QFile::remove(path);

    QSqlQuery row(AppDatabase::connection());
    row.prepare(QStringLiteral("DELETE FROM downloads WHERE video_id = ?"));
    row.addBindValue(videoId);
    row.exec();

    // The library row came from the download. A favourite keeps its place and
    // goes back to streaming; anything else leaves with the file.
    QSqlQuery drop(AppDatabase::connection());
    drop.prepare(QStringLiteral("DELETE FROM tracks WHERE source_id = ? AND favourite = 0"));
    drop.addBindValue(videoId);
    drop.exec();
    QSqlQuery keep(AppDatabase::connection());
    keep.prepare(QStringLiteral("UPDATE tracks SET source_url = '' WHERE source_id = ?"));
    keep.addBindValue(videoId);
    keep.exec();

    m_library.reload();
    touch();
    Q_EMIT libraryChanged();
}

// --------------------------------------------------------------------- state

QString DownloadManager::stateFor(const QString &videoId) const
{
    if (videoId.isEmpty())
        return {};
    if (const DownloadQueueModel::Item *item = m_queue.find(videoId))
        return DownloadQueueModel::stateName(item->state);
    if (m_stored.contains(videoId))
        return QStringLiteral("done");
    return {};
}

qreal DownloadManager::progressFor(const QString &videoId) const
{
    if (const DownloadQueueModel::Item *item = m_queue.find(videoId))
        return item->progress;
    return m_stored.contains(videoId) ? 1.0 : 0.0;
}

bool DownloadManager::isDownloaded(const QString &videoId) const
{
    return m_stored.contains(videoId);
}

QVariantMap DownloadManager::downloadCounts(const QVariantList &tracks) const
{
    int songs = 0, done = 0, pending = 0, failed = 0;
    for (const QVariant &value : tracks) {
        const QString videoId = value.toMap().value(QStringLiteral("sourceId")).toString();
        if (videoId.isEmpty())
            continue;
        ++songs;
        if (m_stored.contains(videoId)) {
            ++done;
        } else if (isPending(videoId)) {
            ++pending;
        } else if (const DownloadQueueModel::Item *item = m_queue.find(videoId);
                   item && item->state == DownloadQueueModel::State::Failed) {
            ++failed;
        }
    }
    return { { QStringLiteral("songs"), songs }, { QStringLiteral("done"), done },
             { QStringLiteral("pending"), pending }, { QStringLiteral("failed"), failed } };
}

// As retry(), but the rows are told once at the end, as in enqueueAll.
void DownloadManager::retryFailed(const QVariantList &tracks)
{
    bool queued = false;
    for (const QVariant &value : tracks) {
        const QString videoId = value.toMap().value(QStringLiteral("sourceId")).toString();
        const DownloadQueueModel::Item *current = m_queue.find(videoId);
        if (!current || current->state != DownloadQueueModel::State::Failed || isPending(videoId))
            continue;
        DownloadQueueModel::Item item = *current;
        item.state = DownloadQueueModel::State::Queued;
        item.error.clear();
        item.progress = 0.0;
        m_queue.upsert(item);
        m_pending.append(videoId);
        queued = true;
    }
    if (!queued)
        return;
    touch();
    pump();
}

bool DownloadManager::isPending(const QString &videoId) const
{
    return m_requests.contains(videoId) || m_saavnRequests.contains(videoId) || m_saavnAsking.contains(videoId)
           || m_pending.contains(videoId);
}

QString DownloadManager::localPathFor(const QString &videoId) const
{
    const QString path = m_stored.value(videoId);
    // Deleted outside the app since launch: fall back to streaming.
    return (!path.isEmpty() && QFileInfo::exists(path)) ? path : QString();
}

void DownloadManager::openDownloadFolder() const
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory));
}

void DownloadManager::revealFile(const QString &videoId) const
{
    const QString path = localPathFor(videoId);
    if (path.isEmpty()) {
        openDownloadFolder();
        return;
    }
#if defined(Q_OS_WIN)
    // explorer wants the path quoted inside its own argument, which QProcess's
    // quoting would wrap a second time.
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(path)));
    explorer.startDetached();
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

// ------------------------------------------------------------------- storage

void DownloadManager::loadStored()
{
    m_stored.clear();
    QSqlQuery query(AppDatabase::connection());
    if (!query.exec(QStringLiteral("SELECT video_id, file_path FROM downloads")))
        return;
    while (query.next()) {
        const QString path = query.value(1).toString();
        // A missing file is not a download, but the row stays: the folder may
        // be on a drive that is simply not attached right now.
        if (QFileInfo::exists(path))
            m_stored.insert(query.value(0).toString(), path);
    }
}

// Every file a download writes carries "[<video id>]." in its name, whatever
// the title turned out to be. Matched as a plain substring rather than a QDir
// name filter, which would read the brackets as a wildcard character class.
QString DownloadManager::findWrittenFile(const QString &videoId) const
{
    const QString marker = QLatin1Char('[') + videoId + QStringLiteral("].");
    const QFileInfoList files = QDir(m_directory).entryInfoList(QDir::Files, QDir::Time);
    for (const QFileInfo &file : files) {
        const QString suffix = file.suffix().toLower();
        if (file.fileName().contains(marker)
            && suffix != QLatin1String("part") && suffix != QLatin1String("ytdl")
            && suffix != QLatin1String("jpg") && suffix != QLatin1String("webp")
            && suffix != QLatin1String("png"))
            return file.absoluteFilePath();
    }
    return {};
}

// yt-dlp leaves .part, .ytdl, thumbnail and intermediate files behind when it
// is stopped part-way. A finished file of the same track is never touched,
// whether or not the database knows about it: after app data is reset, or
// when another run wrote to the same folder, the database is not the whole
// story, and a file it has no row for is still somebody's music.
void DownloadManager::removePartialFiles(const QString &videoId)
{
    // Without a record of what was there before (every download that ran
    // has one), count everything as having been there: only yt-dlp's own
    // leftovers go.
    const QSet<QString> before = m_before.contains(videoId) ? m_before.take(videoId)
                                                            : filesFor(m_directory, videoId);
    removeLeftovers(m_directory, videoId, before, m_stored.value(videoId));
}

QString DownloadManager::chooseDirectory()
{
    // Named outright: for a test, or for keeping downloads somewhere else.
    const QString named = qEnvironmentVariable("MONOLIST_DOWNLOAD_DIR");
    if (!named.isEmpty())
        return QDir::cleanPath(QDir(named).absolutePath());

    // A scratch database means a test or a trial run, and its downloads are
    // just as much scratch: beside that database, never among the user's
    // music, where a later cleanup or a re-download could meet them.
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (!data.isEmpty())
        return QDir::cleanPath(QDir(data).absoluteFilePath(QStringLiteral("downloads")));

    // Same convention Melody settled on: a named folder inside the user's real
    // Music directory, so downloads survive reinstalls and are visible to other
    // players rather than buried in app data.
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                : music)
        .filePath(QStringLiteral("Monolist"));
}

QSet<QString> DownloadManager::filesFor(const QString &directory, const QString &videoId)
{
    QSet<QString> names;
    if (videoId.isEmpty())
        return names;
    const QString marker = QLatin1Char('[') + videoId + QStringLiteral("].");
    const QFileInfoList files = QDir(directory).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo &file : files) {
        if (file.fileName().contains(marker))
            names.insert(file.fileName());
    }
    return names;
}

QStringList DownloadManager::removeLeftovers(const QString &directory, const QString &videoId,
                                             const QSet<QString> &before, const QString &kept)
{
    QStringList removed;
    if (videoId.isEmpty())
        return removed;
    const QString marker = QLatin1Char('[') + videoId + QLatin1Char(']');
    const QFileInfo keptFile(kept);
    const QFileInfoList files = QDir(directory).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo &file : files) {
        const QString name = file.fileName();
        // The last one: the id closes the stem, and a title could in theory
        // carry the same text earlier on.
        const qsizetype at = name.lastIndexOf(marker + QLatin1Char('.'));
        if (at < 0)
            continue;
        if (!kept.isEmpty() && file == keptFile)
            continue;
        const QString tail = name.mid(at + marker.size()).toLower();
        if (!isIntermediate(tail) && before.contains(name))
            continue;   // finished before this attempt began
        if (QFile::remove(file.absoluteFilePath()))
            removed.append(name);
    }
    return removed;
}

void DownloadManager::recordStored(const DownloadQueueModel::Item &item, const QString &path)
{
    // Whether it is a music video: as the list it was downloaded from said,
    // or as any copy of it the app already keeps says — the menus that queue
    // a download do not all pass it on.
    int isVideo = item.isVideo ? 1 : 0;
    QSqlQuery known(AppDatabase::connection());
    known.prepare(QStringLiteral(
        "SELECT MAX(v) FROM (SELECT is_video AS v FROM recent WHERE video_id = ?"
        " UNION ALL SELECT is_video FROM playlist_tracks WHERE video_id = ?"
        " UNION ALL SELECT is_video FROM tracks WHERE source_id = ?)"));
    known.addBindValue(item.videoId);
    known.addBindValue(item.videoId);
    known.addBindValue(item.videoId);
    if (known.exec() && known.next() && known.value(0).toInt() > 0)
        isVideo = 1;

    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT INTO downloads (video_id, title, artist, artwork, duration_ms, file_path, bytes, is_video)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(video_id) DO UPDATE SET"
        "   title = excluded.title,"
        "   artist = excluded.artist,"
        "   artwork = excluded.artwork,"
        "   duration_ms = excluded.duration_ms,"
        "   file_path = excluded.file_path,"
        "   bytes = excluded.bytes,"
        "   is_video = MAX(is_video, excluded.is_video),"
        "   downloaded_at = datetime('now')"));
    query.addBindValue(item.videoId);
    query.addBindValue(AppDatabase::text(item.title));
    query.addBindValue(AppDatabase::text(item.artist));
    query.addBindValue(AppDatabase::text(item.artwork));
    query.addBindValue(item.durationMs);
    query.addBindValue(path);
    query.addBindValue(QFileInfo(path).size());
    query.addBindValue(isVideo);
    if (!query.exec())
        qWarning("Monolist: could not record a download: %s", qPrintable(query.lastError().text()));

    // Keep the main track list in step so a downloaded item is playable from
    // the library without a restart: an existing row now points at the file,
    // and a new one is appended.
    QSqlQuery existing(AppDatabase::connection());
    existing.prepare(QStringLiteral(
        "UPDATE tracks SET source_url = ?, is_video = MAX(is_video, ?) WHERE source_id = ?"));
    existing.addBindValue(path);
    existing.addBindValue(isVideo);
    existing.addBindValue(item.videoId);
    existing.exec();

    QSqlQuery track(AppDatabase::connection());
    // No FROM on the outer SELECT: with one, MAX() would still yield a single
    // row even when the WHERE filtered everything out, and the guard would
    // insert a duplicate at position 0 instead of skipping.
    track.prepare(QStringLiteral(
        "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id, artwork,"
        " favourite, is_video)"
        " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, '', ?, ?, ?, ?, 0, ?"
        " WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE source_id = ?)"));
    track.addBindValue(AppDatabase::text(item.title));
    track.addBindValue(AppDatabase::text(item.artist));
    track.addBindValue(item.durationMs);
    track.addBindValue(path);
    track.addBindValue(item.videoId);
    track.addBindValue(AppDatabase::text(item.artwork));
    track.addBindValue(isVideo);
    track.addBindValue(item.videoId);
    if (!track.exec())
        qWarning("Monolist: could not add a download to the library: %s", qPrintable(track.lastError().text()));
}
