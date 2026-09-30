#pragma once

#include <QDir>
#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

#include "downloadmodels.h"
#include "ytdlp.h"

class QNetworkAccessManager;
class SaavnDownload;

// Offline library, ported from Melody's download service.
//
// Melody had to route every byte through a public CORS proxy, buffer the whole
// file in memory, base64 it, and hand it to the Capacitor filesystem — all of
// which existed purely to work around browser restrictions. None of that
// applies here: yt-dlp writes straight to disk, and FFmpeg tags the file and
// embeds its cover art on the way. What carries over is the model around it —
// a queue, per-item progress, a stable on-disk location, and a database row so
// the offline set survives restarts.
//
// A download moves through DownloadQueueModel (queued, downloading, processing,
// or failed) and, once its file is on disk, into DownloadLibraryModel and the
// library's track list. The state every track row shows comes from stateFor(),
// which answers from memory; QML asks again whenever `revision` changes. That
// is every row of every list at once, so it moves only when some download's
// state does; a running download's percentage moves `progressRevision`, which
// only the rows showing a running download follow.
class DownloadManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(DownloadQueueModel *queue READ queue CONSTANT)
    Q_PROPERTY(DownloadLibraryModel *library READ library CONSTANT)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY queueChanged)
    Q_PROPERTY(int queuedCount READ queuedCount NOTIFY queueChanged)
    Q_PROPERTY(int storedCount READ storedCount NOTIFY libraryChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory CONSTANT)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY optionsChanged)
    Q_PROPERTY(bool skipNonMusic READ skipNonMusic WRITE setSkipNonMusic NOTIFY optionsChanged)
    // Not fixed at launch: the tools can be installed or updated from Settings
    // while the app runs, and downloads should work the moment they are.
    Q_PROPERTY(bool available READ available NOTIFY toolsChanged)
    Q_PROPERTY(bool canConvert READ canConvert NOTIFY toolsChanged)
    // What to do about a missing yt-dlp, in this platform's terms.
    Q_PROPERTY(QString installHint READ installHint CONSTANT)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
    Q_PROPERTY(int progressRevision READ progressRevision NOTIFY progressRevisionChanged)
public:
    explicit DownloadManager(QObject *parent = nullptr);
    ~DownloadManager() override;

    DownloadQueueModel *queue() { return &m_queue; }
    DownloadLibraryModel *library() { return &m_library; }
    int activeCount() const
    {
        return int(m_requests.size() + m_saavnRequests.size() + m_saavnAsking.size());
    }

    // JioSaavn's copy of a song about to be downloaded, when there is one it
    // is certain of (StreamResolver::findSaavn, wired in main.cpp): with High
    // sound quality on, a song it has is saved from it as 320 kbps AAC, named
    // and tagged as yt-dlp's download would be, and yt-dlp takes over should
    // anything about that fail. `done` must be called once, later, with an
    // empty url for "none"; on Standard it is, without JioSaavn being asked.
    struct SaavnCopy {
        QString url;
        int kbps = 0;
        QString album;      // JioSaavn's, for the tags where the app knows none
        int durationSec = 0;   // JioSaavn's length, where the app knows none
        QString reason;     // why none
    };
    using SaavnFinder = std::function<void(const QString &videoId, const QString &title, const QString &artist,
                                           const QString &album, qint64 durationMs,
                                           std::function<void(const SaavnCopy &)> done)>;
    void setSaavnFinder(SaavnFinder finder) { m_saavnFinder = std::move(finder); }
    // For --saavn-download-test: where the cover comes from, "%1" being the
    // video id (YouTube's own thumbnail otherwise).
    void setCoverTemplateForTest(const QString &pattern) { m_coverTemplate = pattern; }
    int queuedCount() const { return int(m_pending.size()); }
    int storedCount() const { return int(m_stored.size()); }
    QString downloadDirectory() const { return QDir::toNativeSeparators(m_directory); }

    // "original" (the best stream, never re-encoded), "m4a" or "mp3".
    QString format() const { return m_format; }
    void setFormat(const QString &format);
    bool skipNonMusic() const { return m_skipNonMusic; }
    void setSkipNonMusic(bool skip);

    bool available() const { return m_available; }     // yt-dlp was found
    bool canConvert() const { return m_canConvert; }   // FFmpeg too: tags, cover art, trimming
    QString installHint() const { return YtDlp::installHint(); }
    int revision() const { return m_revision; }
    int progressRevision() const { return m_progressRevision; }

    // Looks for yt-dlp and FFmpeg again. Run after the setup script updates
    // the tools.
    Q_INVOKABLE void refreshTools();
    // The same, unless they were looked for a moment ago. Run from enqueue,
    // when the window is returned to and when Downloads is opened: while
    // yt-dlp is missing every download control is hidden, so enqueue alone
    // would never find one installed since.
    Q_INVOKABLE void refreshToolsIfStale();

    // Queue a track for offline use. Re-queuing something already stored or in
    // flight is a no-op, so the interface can call this from a simple toggle.
    // `isVideo` is kept with the copy, so a music video played from Downloads
    // still offers its picture (streamed, since the file is the sound alone).
    Q_INVOKABLE void enqueue(const QString &videoId,
                             const QString &title,
                             const QString &artist,
                             const QString &artwork = QString(),
                             qint64 durationMs = 0,
                             bool isVideo = false,
                             const QString &album = QString());
    // A whole list at once (Download all): track maps as the lists hand the
    // player (sourceId, title, artist, album, artwork, durationMs, isVideo). The rows
    // are told once at the end rather than once a song, which for a long
    // playlist was every row asking again for every song queued.
    Q_INVOKABLE void enqueueAll(const QVariantList &tracks);

    Q_INVOKABLE void cancel(const QString &videoId);   // also dismisses a failed entry
    Q_INVOKABLE void retry(const QString &videoId);
    Q_INVOKABLE void remove(const QString &videoId);   // deletes the file too

    // "", "queued", "downloading", "processing", "failed" or "done".
    Q_INVOKABLE QString stateFor(const QString &videoId) const;
    Q_INVOKABLE qreal progressFor(const QString &videoId) const;
    Q_INVOKABLE bool isDownloaded(const QString &videoId) const;
    Q_INVOKABLE bool isPending(const QString &videoId) const;
    // Where a list's songs, in enqueueAll's shape, stand: { songs, done,
    // pending, failed }. What a page's "Download all" says. Songs with no
    // source cannot be downloaded, so they are not counted at all.
    Q_INVOKABLE QVariantMap downloadCounts(const QVariantList &tracks) const;
    // Every song in the list whose download failed, queued again.
    Q_INVOKABLE void retryFailed(const QVariantList &tracks);
    Q_INVOKABLE QString localPathFor(const QString &videoId) const;

    Q_INVOKABLE void openDownloadFolder() const;
    Q_INVOKABLE void revealFile(const QString &videoId) const;

    // Where downloads are written: MONOLIST_DOWNLOAD_DIR when it is set;
    // otherwise "downloads" inside MONOLIST_DATA_DIR when that is set, so a
    // test run with a scratch database never writes to the real Music folder;
    // otherwise <Music>/Monolist.
    static QString chooseDirectory();

    // The names of the files in `directory` that belong to `videoId`: every
    // file a download writes carries "[<video id>]." in its name. Taken as a
    // download starts, so that its cleanup can tell what it wrote itself.
    static QSet<QString> filesFor(const QString &directory, const QString &videoId);

    // Clears up after a download of `videoId` that failed or was cancelled,
    // and returns the names it deleted. yt-dlp's partial and intermediate
    // files go, whenever they were written. Anything else goes only if this
    // download wrote it (it is not in `before`), and `kept`, the file the
    // database holds, never does. So a finished file from an earlier
    // download survives even when the database has forgotten it. Public for
    // --download-cleanup-test.
    static QStringList removeLeftovers(const QString &directory, const QString &videoId,
                                       const QSet<QString> &before, const QString &kept);

    // For --download-queue-test: nothing starts while held, so what is
    // queued stays queued. Set before a manager is made, since one puts the
    // last launch's queue back as it is made.
    static void setHeldForTest(bool held);
    // The queue as it stands, written now rather than a moment later.
    void saveQueueNow();

Q_SIGNALS:
    void progressChanged(const QString &videoId, qreal progress);
    void completed(const QString &videoId, const QString &path);
    void failed(const QString &videoId, const QString &reason);
    void queueChanged();
    void libraryChanged();
    void optionsChanged();
    void revisionChanged();
    void progressRevisionChanged();
    void toolsChanged();

private:
    // Puts one track in the queue; false when it is stored, queued or running
    // already, or yt-dlp is missing. The caller tells the rows and pumps.
    bool queueOne(const QString &videoId, const QString &title, const QString &artist, const QString &album,
                  const QString &artwork, qint64 durationMs, bool isVideo);
    void pump();
    void begin(const QString &videoId);
    // The two ways a download runs: yt-dlp, and JioSaavn's copy, which falls
    // back to yt-dlp on any failure.
    void startYtDlp(const QString &videoId);
    void saavnAnswered(const QString &videoId, const SaavnCopy &copy);
    void startSaavn(const QString &videoId, const SaavnCopy &copy);
    void saavnFailed(const QString &videoId, const QString &reason);
    // What either reports as it goes.
    void noteProgress(const QString &videoId, qint64 received, qint64 total, double speed, int eta);
    void noteStep(const QString &videoId, const QString &step);
    void complete(const QString &videoId, const QString &reportedPath, const QVariantMap &metadata);
    void fail(const QString &videoId, const QString &reason);
    void recordStored(const DownloadQueueModel::Item &item, const QString &path);
    QString findWrittenFile(const QString &videoId) const;
    void removePartialFiles(const QString &videoId);
    void loadStored();
    // The downloads not finished, kept in download_queue so a quit does not
    // lose them: written a moment after any change, and as the app closes.
    // Put back once, at launch, or once yt-dlp is found if it was missing
    // then; nothing is written before that, so a launch without yt-dlp
    // leaves the last queue as it was.
    void saveQueue();
    void restoreQueue();
    void touch();
    void touchProgress();
    DownloadOptions options() const;
    QString settingValue(const QString &key, const QString &fallback) const;
    void setSettingValue(const QString &key, const QString &value);
    static QString fileStem(const QString &videoId, const QString &title, const QString &artist);

    // Three at a time: enough to keep one slow download from stalling the
    // queue, few enough to stay clear of rate limits.
    static constexpr int kMaxConcurrent = 3;

    DownloadQueueModel m_queue;
    DownloadLibraryModel m_library;
    QString m_directory;
    QStringList m_pending;                               // waiting to start, in order
    bool m_waitingOnPlayback = false;                    // pump() waits for a song to resolve
    QHash<QString, QPointer<YtDlpRequest>> m_requests;   // running through yt-dlp
    QHash<QString, QPointer<SaavnDownload>> m_saavnRequests;   // running from JioSaavn
    QSet<QString> m_saavnAsking;                         // waiting to hear whether JioSaavn has it
    SaavnFinder m_saavnFinder;
    QString m_coverTemplate = QStringLiteral("https://i.ytimg.com/vi/%1/maxresdefault.jpg");
    QNetworkAccessManager *m_network = nullptr;          // made for the first JioSaavn download
    QHash<QString, QString> m_stored;                    // video id -> file on disk
    QHash<QString, QSet<QString>> m_before;              // running: its files already there at the start
    QSet<QString> m_cancelling;
    QString m_format = QStringLiteral("original");
    bool m_skipNonMusic = true;
    bool m_available = false;
    bool m_canConvert = false;
    QElapsedTimer m_toolsChecked;   // since the tools were last looked for
    QTimer m_queueSave;
    bool m_queueRestored = false;
    int m_revision = 0;
    int m_progressRevision = 0;
};
