#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include "ytdlp.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

// One song downloaded from JioSaavn rather than through yt-dlp: with High
// sound quality on, a song JioSaavn has as exactly the same recording is
// saved as its 320 kbps AAC, where YouTube's best is Opus at 130-160.
//
// The file comes out as yt-dlp's would: "<Artist> - <Title> [<id>].m4a" in
// the downloads folder, tagged with the title, artist and album the app
// knows, the YouTube link as its source, and YouTube's thumbnail cropped
// square as its cover, through FFmpeg exactly as yt-dlp embeds one. Every
// file written on the way carries a name DownloadManager's cleanup already
// treats as unfinished (".part", ".jpg", ".temp.<ext>"), so a failure or a
// cancel leaves nothing behind, and the finished name appears only once the
// file under it is whole (ROADMAP F13).
//
// JioSaavn's CDN sends a whole song in well under a second from here (35 MB/s
// median, measured on 10 songs in engine step JS) and paces nothing, so the
// file is fetched in one request.
class SaavnDownload : public QObject
{
    Q_OBJECT
public:
    struct Job {
        QString videoId;
        QString url;            // the stream, on JioSaavn's CDN
        int kbps = 0;
        QString directory;
        QString stem;           // "<Artist> - <Title> [<id>]"
        QString title;
        QString artist;
        QString album;
        QString coverUrl;       // the thumbnail yt-dlp would embed; empty for none
        DownloadOptions options;
        // The song's length, to tell what the file really is: 0 when unknown.
        qint64 durationMs = 0;
    };

    SaavnDownload(const Job &job, QNetworkAccessManager *network, QObject *parent = nullptr);
    ~SaavnDownload() override;

    void start();
    // Stops at once; failed() follows, and the caller cleans up.
    void cancel();

    // For --saavn-download-test: links on this computer (127.0.0.1) are let
    // through as well as JioSaavn's CDN and YouTube's thumbnails.
    static void setAllowLocalForTest(bool allow);

Q_SIGNALS:
    void progress(qint64 received, qint64 total, double speed, int etaSeconds);
    // "Metadata" and "EmbedThumbnail", as yt-dlp names its steps.
    void postProcessing(const QString &step);
    void finishedFile(const QString &path, const QVariantMap &metadata);
    void failed(const QString &reason);

private:
    QString partPath() const;
    QString coverPath() const;
    QString tempPath() const;
    QString finalPath() const;
    QString extension() const;
    void fetchCover(const QString &url, bool fallback);
    void tag();
    void finish(const QString &from);
    void fail(const QString &reason);
    void stopTransfers();

    Job m_job;
    QString m_ffmpegPath;   // looked for once, as the download begins
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<QProcess> m_ffmpeg;
    QFile m_file;
    QElapsedTimer m_clock;
    qint64 m_received = 0;
    bool m_haveCover = false;
    bool m_done = false;
};
