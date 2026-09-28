#include "saavndownload.h"
#include "jiosaavn.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>
#include <QUrl>

namespace {

// A desktop browser, as mpv fetches the same links.
const QByteArray kUserAgent = QByteArrayLiteral(
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/149.0.0.0 Safari/537.36");
// Silence this long and the transfer is given up on (a whole song usually
// takes well under a second).
constexpr int kTransferTimeoutMs = 30000;
// FFmpeg only rewraps the sound, or re-encodes it for MP3, and embeds a cover.
constexpr int kTagTimeoutMs = 120000;
// A cover smaller than this is YouTube's grey "no thumbnail" picture, or not
// a picture at all.
constexpr qint64 kSmallestCover = 2000;
// Below this average the file is not JioSaavn's 320 kbps rendition (which
// averages about 321 with its container), whatever its link says.
constexpr int kMinKbps = 256;

bool s_allowLocal = false;

// Where bytes may come from: JioSaavn's CDN for the song, YouTube's image
// host for its cover, and for the self-test only, this computer.
bool allowedHost(const QUrl &url)
{
    if (Saavn::isCdnLink(url.toString()))
        return true;
    const QString host = url.host().toLower();
    if (url.scheme() == QLatin1String("https") && host == QLatin1String("i.ytimg.com"))
        return true;
    return s_allowLocal && url.scheme() == QLatin1String("http") && host == QLatin1String("127.0.0.1");
}

QNetworkRequest requestFor(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    // Followed only within the hosts above: a redirect elsewhere is refused.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);
    return request;
}

void guardRedirects(QNetworkReply *reply)
{
    QObject::connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl &to) {
        if (allowedHost(reply->url().resolved(to))) {
            Q_EMIT reply->redirectAllowed();
            return;
        }
        reply->abort();
    });
}

int statusOf(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

} // namespace

void SaavnDownload::setAllowLocalForTest(bool allow)
{
    s_allowLocal = allow;
}

SaavnDownload::SaavnDownload(const Job &job, QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_job(job)
    , m_ffmpegPath(YtDlp::ffmpegPath())
    , m_network(network)
{
}

SaavnDownload::~SaavnDownload()
{
    m_done = true;
    stopTransfers();
}

QString SaavnDownload::extension() const
{
    // MP3 only where FFmpeg can make one; without it the file is saved as
    // it came, as yt-dlp saves one then.
    return m_job.options.format == DownloadOptions::Format::Mp3 && !m_ffmpegPath.isEmpty()
               ? QStringLiteral("mp3")
               : QStringLiteral("m4a");
}

QString SaavnDownload::partPath() const
{
    return QDir(m_job.directory).filePath(m_job.stem + QStringLiteral(".m4a.part"));
}

QString SaavnDownload::coverPath() const
{
    return QDir(m_job.directory).filePath(m_job.stem + QStringLiteral(".jpg"));
}

QString SaavnDownload::tempPath() const
{
    return QDir(m_job.directory).filePath(m_job.stem + QStringLiteral(".temp.") + extension());
}

QString SaavnDownload::finalPath() const
{
    return QDir(m_job.directory).filePath(m_job.stem + QLatin1Char('.') + extension());
}

void SaavnDownload::start()
{
    const QUrl url(m_job.url);
    if (!allowedHost(url) || m_job.stem.isEmpty()) {
        fail(QStringLiteral("not a JioSaavn link"));
        return;
    }
    QDir().mkpath(m_job.directory);
    m_file.setFileName(partPath());
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("could not write %1: %2").arg(QDir::toNativeSeparators(partPath()), m_file.errorString()));
        return;
    }
    m_received = 0;
    m_clock.start();
    QNetworkReply *reply = m_network->get(requestFor(url));
    reply->setReadBufferSize(1 << 20);
    guardRedirects(reply);
    m_reply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_done || reply != m_reply)
            return;
        // An error page is not the song: read and dropped, and finished says
        // what went wrong.
        if (statusOf(reply) >= 400) {
            reply->readAll();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (m_file.write(chunk) != chunk.size()) {
            fail(QStringLiteral("could not write the file: %1").arg(m_file.errorString()));
            return;
        }
        m_received += chunk.size();
        const qint64 total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        const double seconds = qMax<qint64>(1, m_clock.elapsed()) / 1000.0;
        const double speed = m_received / seconds;
        const int eta = total > 0 && speed > 0 ? int((total - m_received) / speed) : -1;
        Q_EMIT progress(m_received, total > 0 ? total : -1, speed, eta);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_done || reply != m_reply)
            return;
        m_reply = nullptr;
        const int status = statusOf(reply);
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            fail(status >= 400 ? QStringLiteral("JioSaavn's CDN answered HTTP %1").arg(status)
                               : QStringLiteral("JioSaavn's CDN: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty() && m_file.write(rest) != rest.size()) {
            fail(QStringLiteral("could not write the file: %1").arg(m_file.errorString()));
            return;
        }
        m_received += rest.size();
        m_file.close();
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const qint64 total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (m_received <= 0 || type.startsWith(QLatin1String("text/"))) {
            fail(QStringLiteral("JioSaavn's CDN sent no sound (%1 bytes of %2)")
                     .arg(m_received).arg(type.isEmpty() ? QStringLiteral("nothing named") : type));
            return;
        }
        if (total > 0 && m_received != total) {
            fail(QStringLiteral("the file was cut short (%1 of %2 bytes)").arg(m_received).arg(total));
            return;
        }
        // What was promised is what is kept: a link named for 320 kbps has
        // been seen to serve 98, worse than YouTube's own, which yt-dlp then
        // fetches instead.
        const int kbps = m_job.durationMs > 0 ? int(m_received * 8 / m_job.durationMs) : 0;
        if (m_job.durationMs > 0 && kbps < kMinKbps) {
            fail(QStringLiteral("JioSaavn's file averages %1 kbps, not the %2 its link names")
                     .arg(kbps).arg(m_job.kbps > 0 ? m_job.kbps : 320));
            return;
        }
        qInfo("downloads: %s: %lld bytes from JioSaavn (%d kbps named, %d averaged) in %lld ms",
              qPrintable(m_job.videoId), static_cast<long long>(m_received), m_job.kbps, kbps,
              static_cast<long long>(m_clock.elapsed()));
        const bool canConvert = !m_ffmpegPath.isEmpty();
        if (canConvert && m_job.options.embedArtwork && !m_job.coverUrl.isEmpty())
            fetchCover(m_job.coverUrl, /*fallback=*/true);
        else
            tag();
    });
}

// YouTube's own thumbnail, as yt-dlp embeds it: the largest, then the one
// every video has. A song without either keeps going, uncovered, as yt-dlp
// carries on past a thumbnail it cannot fetch.
void SaavnDownload::fetchCover(const QString &address, bool fallback)
{
    const QUrl url(address);
    if (!allowedHost(url)) {
        tag();
        return;
    }
    QNetworkReply *reply = m_network->get(requestFor(url));
    guardRedirects(reply);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, address, fallback]() {
        reply->deleteLater();
        if (m_done || reply != m_reply)
            return;
        m_reply = nullptr;
        const QByteArray image = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && statusOf(reply) < 400 && image.size() >= kSmallestCover) {
            QFile cover(coverPath());
            if (cover.open(QIODevice::WriteOnly | QIODevice::Truncate) && cover.write(image) == image.size()) {
                cover.close();
                m_haveCover = true;
                tag();
                return;
            }
        }
        QString next = address;
        next.replace(QLatin1String("maxresdefault"), QLatin1String("hqdefault"));
        if (fallback && next != address) {
            fetchCover(next, /*fallback=*/false);
            return;
        }
        qInfo("downloads: %s: no cover to embed (%s)", qPrintable(m_job.videoId),
              qPrintable(reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                                  : QStringLiteral("HTTP %1").arg(statusOf(reply))));
        tag();
    });
}

// The tags and the cover, by FFmpeg, into a working copy; the sound itself is
// only rewrapped (AAC stays AAC), or re-encoded for MP3 as yt-dlp does it.
void SaavnDownload::tag()
{
    if (m_done)
        return;
    const QString ffmpeg = m_ffmpegPath;
    if (ffmpeg.isEmpty()) {
        finish(partPath());   // saved as it came, as yt-dlp saves one without FFmpeg
        return;
    }
    Q_EMIT postProcessing(m_haveCover ? QStringLiteral("EmbedThumbnail") : QStringLiteral("Metadata"));
    const bool mp3 = extension() == QLatin1String("mp3");
    QStringList args{ QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-loglevel"),
                      QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), partPath() };
    if (m_haveCover)
        args << QStringLiteral("-i") << coverPath();
    args << QStringLiteral("-map") << QStringLiteral("0:a:0");
    if (m_haveCover)
        args << QStringLiteral("-map") << QStringLiteral("1:v:0");
    // JioSaavn's own tags go: the file says what the app knows of the song,
    // as a download through yt-dlp does.
    args << QStringLiteral("-map_metadata") << QStringLiteral("-1") << QStringLiteral("-map_chapters")
         << QStringLiteral("-1");
    if (mp3) {
        args << QStringLiteral("-c:a") << QStringLiteral("libmp3lame") << QStringLiteral("-q:a") << QStringLiteral("0")
             << QStringLiteral("-id3v2_version") << QStringLiteral("3");
    } else {
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    }
    if (m_haveCover) {
        // YouTube's thumbnails are 16:9 with the cover in the middle: the
        // centre square, as yt-dlp crops it for every download.
        args << QStringLiteral("-c:v") << QStringLiteral("mjpeg") << QStringLiteral("-q:v") << QStringLiteral("2")
             << QStringLiteral("-vf") << QStringLiteral("crop='min(iw,ih)':'min(iw,ih)'")
             << QStringLiteral("-disposition:v:0") << QStringLiteral("attached_pic");
        if (mp3) {
            args << QStringLiteral("-metadata:s:v") << QStringLiteral("title=Album cover")
                 << QStringLiteral("-metadata:s:v") << QStringLiteral("comment=Cover (front)");
        }
    }
    const QString link = QStringLiteral("https://www.youtube.com/watch?v=") + m_job.videoId;
    const auto meta = [&args](const QString &key, const QString &value) {
        if (!value.trimmed().isEmpty())
            args << QStringLiteral("-metadata") << key + QLatin1Char('=') + value.trimmed();
    };
    meta(QStringLiteral("title"), m_job.title);
    meta(QStringLiteral("artist"), m_job.artist);
    meta(QStringLiteral("album"), m_job.album);
    meta(QStringLiteral("purl"), link);
    meta(QStringLiteral("comment"), link);
    args << tempPath();

    auto *process = new QProcess(this);
    m_ffmpeg = process;
    process->setProgram(ffmpeg);
    process->setArguments(args);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (m_done || process != m_ffmpeg)
            return;
        m_ffmpeg = nullptr;
        const QString said = QString::fromUtf8(process->readAll()).trimmed().right(300);
        if (status != QProcess::NormalExit || code != 0 || QFileInfo(tempPath()).size() <= 0) {
            fail(QStringLiteral("FFmpeg could not tag the file (%1)")
                     .arg(said.isEmpty() ? QStringLiteral("exit %1").arg(code) : said));
            return;
        }
        finish(tempPath());
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_done || process != m_ffmpeg)
            return;
        fail(QStringLiteral("FFmpeg would not start"));
    });
    QTimer::singleShot(kTagTimeoutMs, process, [this, process]() {
        if (!m_done && process == m_ffmpeg && process->state() != QProcess::NotRunning)
            fail(QStringLiteral("FFmpeg took too long"));
    });
    process->start();
}

// The finished file under its own name, and only now: until this it has been
// a .part or a .temp, which a failure's cleanup removes. A copy of the same
// song already under that name (one the database had forgotten) is replaced
// only by this complete one.
void SaavnDownload::finish(const QString &from)
{
    if (m_done)
        return;
    const QString target = finalPath();
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        fail(QStringLiteral("could not replace %1, which may be in use").arg(QDir::toNativeSeparators(target)));
        return;
    }
    if (!QFile::rename(from, target)) {
        // Rare, with the folder just written to; a copy keeps the song.
        if (!QFile::copy(from, target)) {
            fail(QStringLiteral("could not put %1 in place").arg(QDir::toNativeSeparators(target)));
            return;
        }
        QFile::remove(from);
    }
    QFile::remove(partPath());
    QFile::remove(coverPath());
    m_done = true;
    QVariantMap metadata{ { QStringLiteral("title"), m_job.title },
                          { QStringLiteral("artist"), m_job.artist },
                          { QStringLiteral("album"), m_job.album },
                          { QStringLiteral("source"), QStringLiteral("jiosaavn") } };
    Q_EMIT finishedFile(target, metadata);
}

void SaavnDownload::cancel()
{
    fail(QStringLiteral("Cancelled"));
}

void SaavnDownload::fail(const QString &reason)
{
    if (m_done)
        return;
    m_done = true;
    stopTransfers();
    Q_EMIT failed(reason);
}

void SaavnDownload::stopTransfers()
{
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    // Gone before its files are, since they are deleted next.
    if (QProcess *process = m_ffmpeg) {
        m_ffmpeg = nullptr;
        disconnect(process, nullptr, this, nullptr);
        process->kill();
        process->waitForFinished(3000);
        process->deleteLater();
    }
    if (m_file.isOpen())
        m_file.close();
}
