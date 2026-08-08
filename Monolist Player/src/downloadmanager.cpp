#include "downloadmanager.h"
#include "appdatabase.h"
#include "ytdlp.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent)
{
    // Same convention Melody settled on: a named folder inside the user's real
    // Music directory, so downloads survive reinstalls and are visible to other
    // players rather than buried in app data.
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    m_directory = QDir(music.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           : music)
                      .filePath(QStringLiteral("Monolist"));
    QDir().mkpath(m_directory);
}

DownloadManager::~DownloadManager()
{
    for (const QPointer<YtDlpRequest> &request : std::as_const(m_requests)) {
        if (request)
            request->cancel();
    }
}

QString DownloadManager::sanitiseStem(const QString &videoId, const QString &title)
{
    // The id is the identity; the title is only there to make the folder
    // browsable. Strip anything a filesystem might object to.
    static const QRegularExpression illegal(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    QString stem = title;
    stem.replace(illegal, QStringLiteral("_"));
    stem = stem.simplified().left(80).trimmed();
    if (stem.isEmpty())
        stem = QStringLiteral("track");
    return QStringLiteral("%1 [%2]").arg(stem, videoId);
}

void DownloadManager::enqueue(const QString &videoId,
                              const QString &title,
                              const QString &artist,
                              const QString &artwork,
                              qint64 durationMs)
{
    if (videoId.isEmpty() || isDownloaded(videoId) || isPending(videoId))
        return;

    PendingItem item;
    item.videoId = videoId;
    item.title = title;
    item.artist = artist;
    item.artwork = artwork;
    item.durationMs = durationMs;

    m_queue.enqueue(item);
    m_progress.insert(videoId, 0.0);
    Q_EMIT queueChanged();
    Q_EMIT progressChanged(videoId, 0.0);
    pump();
}

void DownloadManager::pump()
{
    while (!m_queue.isEmpty() && m_active.size() < kMaxConcurrent)
        begin(m_queue.dequeue());
    Q_EMIT queueChanged();
}

void DownloadManager::begin(const PendingItem &item)
{
    m_active.insert(item.videoId, item);

    YtDlpRequest *request = YtDlp::download(item.videoId,
                                            m_directory,
                                            sanitiseStem(item.videoId, item.title),
                                            this);
    m_requests.insert(item.videoId, request);
    const QString videoId = item.videoId;

    connect(request, &YtDlpRequest::progress, this,
            [this, videoId](qint64 received, qint64 total) {
                const qreal fraction = total > 0
                    ? qBound(0.0, qreal(received) / qreal(total), 1.0)
                    : 0.0;
                m_progress.insert(videoId, fraction);
                Q_EMIT progressChanged(videoId, fraction);
            });

    connect(request, &YtDlpRequest::finishedFile, this,
            [this, videoId](const QString &reportedPath) {
                const PendingItem item = m_active.value(videoId);

                // yt-dlp reports the pre-remux name when it hands off to
                // ffmpeg, so trust the extension we asked for over the log line.
                QString path = QDir(m_directory).filePath(
                    sanitiseStem(videoId, item.title) + QStringLiteral(".m4a"));
                if (!QFileInfo::exists(path) && QFileInfo::exists(reportedPath))
                    path = reportedPath;

                if (!QFileInfo::exists(path)) {
                    finish(videoId);
                    Q_EMIT failed(videoId, QStringLiteral("Download finished but no file was written."));
                    return;
                }

                recordStored(item, path);
                m_progress.insert(videoId, 1.0);
                Q_EMIT progressChanged(videoId, 1.0);
                finish(videoId);
                Q_EMIT completed(videoId, path);
                Q_EMIT libraryChanged();
            });

    connect(request, &YtDlpRequest::failed, this,
            [this, videoId](const QString &reason) {
                finish(videoId);
                Q_EMIT failed(videoId, reason);
            });
}

void DownloadManager::finish(const QString &videoId)
{
    m_active.remove(videoId);
    m_requests.remove(videoId);
    m_progress.remove(videoId);
    pump();
}

void DownloadManager::recordStored(const PendingItem &item, const QString &path)
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
        "   bytes = excluded.bytes"));
    query.addBindValue(item.videoId);
    query.addBindValue(item.title);
    query.addBindValue(item.artist);
    query.addBindValue(item.artwork);
    query.addBindValue(item.durationMs);
    query.addBindValue(path);
    query.addBindValue(QFileInfo(path).size());
    query.exec();

    // Keep the main track list in step so a downloaded item is playable from
    // the library without a restart.
    QSqlQuery track(AppDatabase::connection());
    // No FROM on the outer SELECT: with one, MAX() would still yield a single
    // row even when the WHERE filtered everything out, and the guard would
    // insert a duplicate at position 0 instead of skipping.
    track.prepare(QStringLiteral(
        "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id, artwork, favourite)"
        " SELECT (SELECT COALESCE(MAX(position) + 1, 0) FROM tracks), ?, ?, '', ?, ?, ?, ?, 0"
        " WHERE NOT EXISTS (SELECT 1 FROM tracks WHERE source_id = ?)"));
    track.addBindValue(item.title);
    track.addBindValue(item.artist);
    track.addBindValue(item.durationMs);
    track.addBindValue(path);
    track.addBindValue(item.videoId);
    track.addBindValue(item.artwork);
    track.addBindValue(item.videoId);
    track.exec();
}

void DownloadManager::cancel(const QString &videoId)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).videoId == videoId) {
            m_queue.removeAt(i);
            m_progress.remove(videoId);
            Q_EMIT queueChanged();
            return;
        }
    }

    if (QPointer<YtDlpRequest> request = m_requests.value(videoId); request)
        request->cancel();   // the failed() handler clears the active entry
}

void DownloadManager::remove(const QString &videoId)
{
    cancel(videoId);

    const QString path = localPathFor(videoId);
    if (!path.isEmpty())
        QFile::remove(path);

    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("DELETE FROM downloads WHERE video_id = ?"));
    query.addBindValue(videoId);
    query.exec();

    QSqlQuery track(AppDatabase::connection());
    track.prepare(QStringLiteral("DELETE FROM tracks WHERE source_id = ?"));
    track.addBindValue(videoId);
    track.exec();

    Q_EMIT libraryChanged();
}

bool DownloadManager::isDownloaded(const QString &videoId) const
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT file_path FROM downloads WHERE video_id = ?"));
    query.addBindValue(videoId);
    if (!query.exec() || !query.next())
        return false;
    // A row whose file has been deleted from disk is not a download.
    return QFileInfo::exists(query.value(0).toString());
}

bool DownloadManager::isPending(const QString &videoId) const
{
    if (m_active.contains(videoId))
        return true;
    for (const PendingItem &item : m_queue) {
        if (item.videoId == videoId)
            return true;
    }
    return false;
}

qreal DownloadManager::progressFor(const QString &videoId) const
{
    return m_progress.value(videoId, isDownloaded(videoId) ? 1.0 : 0.0);
}

QString DownloadManager::localPathFor(const QString &videoId) const
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT file_path FROM downloads WHERE video_id = ?"));
    query.addBindValue(videoId);
    if (!query.exec() || !query.next())
        return QString();
    const QString path = query.value(0).toString();
    return QFileInfo::exists(path) ? path : QString();
}

int DownloadManager::storedCount() const
{
    QSqlQuery query(AppDatabase::connection());
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM downloads")) && query.next())
        return query.value(0).toInt();
    return 0;
}

QVariantList DownloadManager::storedTracks() const
{
    QVariantList results;
    QSqlQuery query(AppDatabase::connection());
    query.exec(QStringLiteral(
        "SELECT video_id, title, artist, artwork, duration_ms, file_path, bytes"
        " FROM downloads ORDER BY downloaded_at DESC"));
    while (query.next()) {
        results.append(QVariantMap{
            { QStringLiteral("videoId"),    query.value(0) },
            { QStringLiteral("title"),      query.value(1) },
            { QStringLiteral("artist"),     query.value(2) },
            { QStringLiteral("artwork"),    query.value(3) },
            { QStringLiteral("durationMs"), query.value(4) },
            { QStringLiteral("filePath"),   query.value(5) },
            { QStringLiteral("bytes"),      query.value(6) }
        });
    }
    return results;
}
