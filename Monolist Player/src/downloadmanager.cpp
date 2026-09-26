#include "downloadmanager.h"
#include "appdatabase.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
// How stale a look for the tools may be before enqueue looks again. Not on
// every call: "Download all" enqueues a whole album in one go, and a tool that
// is not bundled is looked for along the whole of PATH.
constexpr qint64 kToolsRecheckMs = 3000;

bool isKnownFormat(const QString &format)
{
    return format == QLatin1String("original") || format == QLatin1String("m4a")
        || format == QLatin1String("mp3");
}

} // namespace

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent)
    , m_available(YtDlp::isAvailable())
    , m_canConvert(!YtDlp::ffmpegPath().isEmpty())
{
    m_toolsChecked.start();

    // Same convention Melody settled on: a named folder inside the user's real
    // Music directory, so downloads survive reinstalls and are visible to other
    // players rather than buried in app data.
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    m_directory = QDir(music.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           : music)
                      .filePath(QStringLiteral("Monolist"));
    QDir().mkpath(m_directory);

    const QString format = settingValue(kFormatKey, m_format);
    if (isKnownFormat(format))
        m_format = format;
    m_skipNonMusic = settingValue(kSkipNonMusicKey, QStringLiteral("1")) != QLatin1String("0");

    loadStored();
    m_library.reload();
}

DownloadManager::~DownloadManager()
{
    // Stop without the usual failure handling: nothing may start in their place.
    m_pending.clear();
    const auto requests = m_requests;
    for (auto it = requests.cbegin(); it != requests.cend(); ++it) {
        if (!it.value())
            continue;
        disconnect(it.value(), nullptr, this, nullptr);
        it.value()->cancel();
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
    query.addBindValue(value);
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
}

void DownloadManager::enqueue(const QString &videoId,
                              const QString &title,
                              const QString &artist,
                              const QString &artwork,
                              qint64 durationMs)
{
    if (videoId.isEmpty())
        return;
    // What was found at launch is not the last word: tools put in place since
    // then are used rather than refused until a restart.
    if (m_toolsChecked.elapsed() > kToolsRecheckMs)
        refreshTools();
    if (!m_available || m_stored.contains(videoId) || isPending(videoId))
        return;

    DownloadQueueModel::Item item;
    item.videoId = videoId;
    item.title = title.trimmed();   // may be empty: yt-dlp's metadata fills it in
    item.artist = YtDlp::cleanArtist(artist);
    item.artwork = artwork;
    item.durationMs = durationMs;
    m_queue.upsert(item);   // replaces a failed attempt at the same track
    m_pending.append(videoId);
    touch();
    pump();
}

void DownloadManager::pump()
{
    while (!m_pending.isEmpty() && m_requests.size() < kMaxConcurrent)
        begin(m_pending.takeFirst());
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

    // Without a title the stem is left to yt-dlp, which names the file from
    // the metadata it fetches.
    const QString stem = item.title.isEmpty() ? QString() : fileStem(videoId, item.title, item.artist);
    YtDlpRequest *request = YtDlp::download(videoId, m_directory, stem, options(), this);
    m_requests.insert(videoId, request);

    connect(request, &YtDlpRequest::progress, this,
            [this, videoId](qint64 received, qint64 total, double speed, int eta) {
                const DownloadQueueModel::Item *current = m_queue.find(videoId);
                if (!current)
                    return;
                DownloadQueueModel::Item item = *current;
                const qreal fraction = total > 0 ? qBound(0.0, qreal(received) / qreal(total), 1.0)
                                                 : item.progress;
                // Progress lines arrive many times a second; repaint on
                // whole-percent steps.
                if (item.state == DownloadQueueModel::State::Downloading && fraction < 1.0
                    && qAbs(fraction - item.progress) < 0.01)
                    return;
                item.state = DownloadQueueModel::State::Downloading;
                item.progress = fraction;
                item.received = received;
                item.total = total;
                item.speed = speed;
                item.eta = eta;
                m_queue.upsert(item);
                touch();
                Q_EMIT progressChanged(videoId, fraction);
            });

    connect(request, &YtDlpRequest::postProcessing, this, [this, videoId](const QString &step) {
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
    });

    connect(request, &YtDlpRequest::finishedFile, this,
            [this, videoId](const QString &path, const QVariantMap &metadata) {
                complete(videoId, path, metadata);
            });

    connect(request, &YtDlpRequest::failed, this, [this, videoId](const QString &reason) {
        fail(videoId, reason);
    });
}

void DownloadManager::complete(const QString &videoId, const QString &reportedPath,
                               const QVariantMap &metadata)
{
    m_requests.remove(videoId);
    const DownloadQueueModel::Item *current = m_queue.find(videoId);
    if (!current) {
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

bool DownloadManager::isPending(const QString &videoId) const
{
    return m_requests.contains(videoId) || m_pending.contains(videoId);
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
// is stopped part-way. A finished download of the same track is never touched.
void DownloadManager::removePartialFiles(const QString &videoId) const
{
    const QString marker = QLatin1Char('[') + videoId + QStringLiteral("].");
    const QString kept = m_stored.value(videoId);
    const QFileInfoList files = QDir(m_directory).entryInfoList(QDir::Files | QDir::Hidden);
    for (const QFileInfo &file : files) {
        const QString path = file.absoluteFilePath();
        if (file.fileName().contains(marker) && path != kept)
            QFile::remove(path);
    }
}

void DownloadManager::recordStored(const DownloadQueueModel::Item &item, const QString &path)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral(
        "INSERT INTO downloads (video_id, title, artist, artwork, duration_ms, file_path, bytes)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(video_id) DO UPDATE SET"
        "   title = excluded.title,"
        "   artist = excluded.artist,"
        "   artwork = excluded.artwork,"
        "   duration_ms = excluded.duration_ms,"
        "   file_path = excluded.file_path,"
        "   bytes = excluded.bytes,"
        "   downloaded_at = datetime('now')"));
    query.addBindValue(item.videoId);
    query.addBindValue(AppDatabase::text(item.title));
    query.addBindValue(AppDatabase::text(item.artist));
    query.addBindValue(AppDatabase::text(item.artwork));
    query.addBindValue(item.durationMs);
    query.addBindValue(path);
    query.addBindValue(QFileInfo(path).size());
    if (!query.exec())
        qWarning("Monolist: could not record a download: %s", qPrintable(query.lastError().text()));

    // Keep the main track list in step so a downloaded item is playable from
    // the library without a restart: an existing row now points at the file,
    // and a new one is appended.
    QSqlQuery existing(AppDatabase::connection());
    existing.prepare(QStringLiteral("UPDATE tracks SET source_url = ? WHERE source_id = ?"));
    existing.addBindValue(path);
    existing.addBindValue(item.videoId);
    existing.exec();

    QSqlQuery track(AppDatabase::connection());
    // No FROM on the outer SELECT: with one, MAX() would still yield a single
    // row even when the WHERE filtered everything out, and the guard would
    // insert a duplicate at position 0 instead of skipping.
    track.prepare(QStringLiteral(
        "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id, artwork, favourite)"
        " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, '', ?, ?, ?, ?, 0"
        " WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE source_id = ?)"));
    track.addBindValue(AppDatabase::text(item.title));
    track.addBindValue(AppDatabase::text(item.artist));
    track.addBindValue(item.durationMs);
    track.addBindValue(path);
    track.addBindValue(item.videoId);
    track.addBindValue(AppDatabase::text(item.artwork));
    track.addBindValue(item.videoId);
    if (!track.exec())
        qWarning("Monolist: could not add a download to the library: %s", qPrintable(track.lastError().text()));
}
