#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QVariantMap>

class YtDlpRequest;

// Offline library, ported from Melody's download service.
//
// Melody had to route every byte through a public CORS proxy, buffer the whole
// file in memory, base64 it, and hand it to the Capacitor filesystem — all of
// which existed purely to work around browser restrictions. None of that
// applies here: yt-dlp writes straight to disk, so the entire tunnel/base64/
// blob-URL chain is gone. What carries over is the model around it — a queue,
// per-item progress, a stable on-disk location, and a database row so the
// offline set survives restarts.
class DownloadManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY queueChanged)
    Q_PROPERTY(int queuedCount READ queuedCount NOTIFY queueChanged)
    Q_PROPERTY(int storedCount READ storedCount NOTIFY libraryChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory CONSTANT)
public:
    explicit DownloadManager(QObject *parent = nullptr);
    ~DownloadManager() override;

    int activeCount() const { return int(m_active.size()); }
    int queuedCount() const { return int(m_queue.size()); }
    int storedCount() const;
    QString downloadDirectory() const { return m_directory; }

    // Queue a track for offline use. Re-queuing something already stored is a
    // no-op, so the UI can call this from a simple toggle.
    Q_INVOKABLE void enqueue(const QString &videoId,
                             const QString &title,
                             const QString &artist,
                             const QString &artwork = QString(),
                             qint64 durationMs = 0);

    Q_INVOKABLE void cancel(const QString &videoId);
    Q_INVOKABLE void remove(const QString &videoId);        // deletes the file too

    Q_INVOKABLE bool isDownloaded(const QString &videoId) const;
    Q_INVOKABLE bool isPending(const QString &videoId) const;
    Q_INVOKABLE qreal progressFor(const QString &videoId) const;
    Q_INVOKABLE QString localPathFor(const QString &videoId) const;
    Q_INVOKABLE QVariantList storedTracks() const;

Q_SIGNALS:
    void progressChanged(const QString &videoId, qreal progress);
    void completed(const QString &videoId, const QString &path);
    void failed(const QString &videoId, const QString &reason);
    void queueChanged();
    void libraryChanged();

private:
    struct PendingItem {
        QString videoId;
        QString title;
        QString artist;
        QString artwork;
        qint64 durationMs = 0;
    };

    void pump();
    void begin(const PendingItem &item);
    void recordStored(const PendingItem &item, const QString &path);
    void finish(const QString &videoId);
    static QString sanitiseStem(const QString &videoId, const QString &title);

    // Three at a time: enough to keep a slow instance from stalling the queue,
    // low enough to avoid tripping rate limits.
    static constexpr int kMaxConcurrent = 3;

    QString m_directory;
    QQueue<PendingItem> m_queue;
    QHash<QString, PendingItem> m_active;
    QHash<QString, QPointer<YtDlpRequest>> m_requests;
    QHash<QString, qreal> m_progress;
};
