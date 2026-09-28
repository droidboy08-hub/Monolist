#include "downloadselftest.h"

#include "downloadmanager.h"
#include "saavndownload.h"
#include "streamresolver.h"
#include "ytdlp.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

// One line per check, and a count at the end, as in connectionselftest.cpp.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qPrintable(text)); }

    int finish()
    {
        qWarning("%s: %d checks, %d failed", m_prefix, m_count, m_failed);
        return m_failed;
    }

private:
    const char *m_prefix;
    int m_count = 0;
    int m_failed = 0;
};

// An environment variable put back as it was, set or not, when the scope ends.
class SavedVariable
{
public:
    explicit SavedVariable(const char *name)
        : m_name(name), m_set(qEnvironmentVariableIsSet(name)), m_value(qgetenv(name)) {}
    ~SavedVariable()
    {
        if (m_set)
            qputenv(m_name, m_value);
        else
            qunsetenv(m_name);
    }
    SavedVariable(const SavedVariable &) = delete;
    SavedVariable &operator=(const SavedVariable &) = delete;

private:
    const char *m_name;
    bool m_set;
    QByteArray m_value;
};

// An invented file. Its content does not matter, only that it is not empty,
// as a finished download never is.
bool make(const QDir &dir, const QString &name)
{
    QFile file(dir.filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(QByteArray(4096, 'x')) == 4096;
}

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

struct Leftover {
    QString name;
    bool goes;          // deleted by the cleanup
    const char *what;
};

// Runs the event loop until `done` holds or `timeoutMs` passes.
bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

// A stand-in for JioSaavn's CDN and YouTube's thumbnails. Paths:
//   /song_320.mp4            the song, whole
//   /thin_320.mp4            the same song at 96 kbps, under a 320 kbps name
//   /gone_320.mp4            404, as a withdrawn file
//   /held_320.mp4            the headers and the first half, then nothing more
//   /vi/<id>/<name>.jpg      the cover, or 404 for an id in `noCover`
class CdnStandIn : public QObject
{
public:
    QByteArray song;
    QByteArray thin;
    QByteArray cover;
    QSet<QString> noCover;
    QStringList asked;

    bool listen()
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }
    void closeAll()
    {
        for (const QPointer<QTcpSocket> &socket : std::as_const(m_held)) {
            if (socket)
                socket->abort();
        }
        m_held.clear();
    }

private:
    void serve(QTcpSocket *socket)
    {
        auto head = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, head]() {
            head->append(socket->readAll());
            if (!head->contains("\r\n\r\n"))
                return;
            QObject::disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            const QString path = QString::fromLatin1(head->left(head->indexOf('\r')).split(' ').value(1));
            asked << path;
            answer(socket, path);
        });
    }

    void answer(QTcpSocket *socket, const QString &path)
    {
        const auto send = [socket](const QByteArray &status, const QByteArray &type, const QByteArray &body,
                                   qint64 length) {
            socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: "
                          + QByteArray::number(length) + "\r\nConnection: close\r\n\r\n" + body);
        };
        if (path == QLatin1String("/song_320.mp4")) {
            send("200 OK", "audio/mp4", song, song.size());
        } else if (path == QLatin1String("/thin_320.mp4")) {
            send("200 OK", "audio/mp4", thin, thin.size());
        } else if (path == QLatin1String("/held_320.mp4")) {
            send("200 OK", "audio/mp4", song.left(song.size() / 2), song.size());
            m_held << socket;
            return;   // left open, the rest never coming
        } else if (path.startsWith(QLatin1String("/vi/"))
                   && !noCover.contains(path.section(QLatin1Char('/'), 2, 2))) {
            send("200 OK", "image/jpeg", cover, cover.size());
        } else {
            send("404 Not Found", "text/plain", "missing", 7);
        }
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QList<QPointer<QTcpSocket>> m_held;
};

// FFmpeg, run to the end; its output on failure.
bool runTool(const QString &program, const QStringList &args, QString *output)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    const bool ok = process.waitForFinished(120000) && process.exitStatus() == QProcess::NormalExit
                    && process.exitCode() == 0;
    if (output)
        *output = QString::fromUtf8(process.readAll()).trimmed();
    return ok;
}

// What ffprobe reads from a file: its streams and its tags.
struct Probe {
    QString audioCodec;
    bool cover = false;
    int coverWidth = 0;
    int coverHeight = 0;
    QVariantMap tags;
    QString error;
};

Probe probe(const QString &ffprobe, const QString &path)
{
    Probe result;
    QString output;
    if (!runTool(ffprobe, { QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_streams"),
                            QStringLiteral("-show_format"), QStringLiteral("-of"), QStringLiteral("json"), path },
                 &output)) {
        result.error = output.left(300);
        return result;
    }
    const QJsonObject root = QJsonDocument::fromJson(output.toUtf8()).object();
    for (const QJsonValue &value : root.value(QStringLiteral("streams")).toArray()) {
        const QJsonObject stream = value.toObject();
        const QString type = stream.value(QStringLiteral("codec_type")).toString();
        if (type == QLatin1String("audio") && result.audioCodec.isEmpty())
            result.audioCodec = stream.value(QStringLiteral("codec_name")).toString();
        if (type == QLatin1String("video")
            && stream.value(QStringLiteral("disposition")).toObject().value(QStringLiteral("attached_pic")).toInt() == 1) {
            result.cover = true;
            result.coverWidth = stream.value(QStringLiteral("width")).toInt();
            result.coverHeight = stream.value(QStringLiteral("height")).toInt();
        }
    }
    const QJsonObject tags = root.value(QStringLiteral("format")).toObject().value(QStringLiteral("tags")).toObject();
    for (auto it = tags.begin(); it != tags.end(); ++it)
        result.tags.insert(it.key().toLower(), it.value().toString());
    return result;
}

} // namespace

int runDownloadCleanupSelfTest()
{
    Checks t("cleanup");
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test opens the downloads on the database there"));
        return t.finish();
    }
    // Inside the data folder, and gone again when the test ends.
    QTemporaryDir scratch(QDir(data).filePath(QStringLiteral("cleanup-test-XXXXXX")));
    if (!scratch.isValid()) {
        t.check(false, QStringLiteral("a scratch folder"), scratch.errorString());
        return t.finish();
    }
    const QDir dir(scratch.path());
    t.note(QStringLiteral("scratch folder %1").arg(QDir::toNativeSeparators(dir.path())));

    // — where downloads go —
    {
        SavedVariable keepNamed("MONOLIST_DOWNLOAD_DIR");
        qunsetenv("MONOLIST_DOWNLOAD_DIR");
        {
            // What every test run gets: a scratch database, nothing else set.
            DownloadManager manager;
            const QString expected = QDir(data).absoluteFilePath(QStringLiteral("downloads"));
            t.check(samePath(manager.downloadDirectory(), expected) && QFileInfo(expected).isDir(),
                    QStringLiteral("MONOLIST_DATA_DIR alone: downloads go to \"downloads\" inside it"),
                    manager.downloadDirectory());
        }

        const QString named = dir.filePath(QStringLiteral("named folder"));
        qputenv("MONOLIST_DOWNLOAD_DIR", QFile::encodeName(QDir::toNativeSeparators(named)));
        {
            DownloadManager manager;
            t.check(samePath(manager.downloadDirectory(), named) && QFileInfo(named).isDir(),
                    QStringLiteral("MONOLIST_DOWNLOAD_DIR: downloads go there, made if missing"),
                    manager.downloadDirectory());
        }
        qunsetenv("MONOLIST_DOWNLOAD_DIR");

        // Named only: nothing is made or written there.
        SavedVariable keepData("MONOLIST_DATA_DIR");
        qunsetenv("MONOLIST_DATA_DIR");
        const QString real = DownloadManager::chooseDirectory();
        t.check(QFileInfo(real).fileName() == QLatin1String("Monolist")
                    && !real.startsWith(QDir::cleanPath(QDir(data).absolutePath()), Qt::CaseInsensitive),
                QStringLiteral("neither set: <Music>/Monolist, the user's own folder"), real);
    }

    // — what a failed or cancelled download may delete —
    //
    // One track, "Selftest001", downloaded again after the database lost
    // track of the first download: its finished files are in the folder
    // already, beside another track's. The attempt writes yt-dlp's usual
    // working files, then fails.
    const QString id = QStringLiteral("Selftest001");   // eleven characters, like a video id
    const QString song = QStringLiteral("Selftest Artist - New Song [Selftest001]");
    const QString old = QStringLiteral("An Old Title [Selftest001]");

    const QList<Leftover> before = {
        { song + QStringLiteral(".m4a"), false, "finished earlier under the same name, the database knowing nothing of it" },
        { old + QStringLiteral(".opus"), false, "finished earlier under an older title" },
        { old + QStringLiteral(".mp3"), false, "finished earlier, as MP3" },
        { old + QStringLiteral(".webm"), false, "finished earlier, audio as YouTube serves it" },
        { old + QStringLiteral(".orig.m4a"), false, "the original yt-dlp sets aside while converting" },
        { old + QStringLiteral(".webm.part"), true, "a partial download from a run that crashed" },
        { old + QStringLiteral(".webp"), true, "a thumbnail left behind" },
        { QStringLiteral("Another Song [Selftest002].opus"), false, "another track's download" },
        { QStringLiteral("Another Song [Selftest002].webm.part"), false, "another track's download, under way" },
        { QStringLiteral("Shouting [SELFTEST001].opus"), false, "another video: ids are case-sensitive" },
        { QStringLiteral("Selftest001 notes.txt"), false, "the id without its brackets" },
        { QStringLiteral("Unrelated.mp3"), false, "not a download" },
    };
    const QList<Leftover> during = {
        { song + QStringLiteral(".webm.part"), true, "the download under way" },
        { song + QStringLiteral(".webm.ytdl"), true, "yt-dlp's record for resuming" },
        { song + QStringLiteral(".f251.webm"), true, "one stream of a merge" },
        { song + QStringLiteral(".f140.m4a.part-Frag7"), true, "a fragment" },
        { song + QStringLiteral(".temp.m4a"), true, "FFmpeg's working copy" },
        { song + QStringLiteral(".meta"), true, "the chapter list for FFmpeg" },
        { song + QStringLiteral(".webp"), true, "the thumbnail as fetched" },
        { song + QStringLiteral(".jpg"), true, "the thumbnail converted to embed" },
        { song + QStringLiteral(".webm"), true, "this attempt's download, not yet converted" },
        { song + QStringLiteral(".mp3"), true, "FFmpeg's output, cut off when the attempt stopped" },
        { QStringLiteral("Held By The Database [Selftest001].m4a"), false, "the file the database holds" },
        { QStringLiteral("Another Song [Selftest002].f251.webm"), false, "another track's, written meanwhile" },
    };

    bool made = true;
    for (const Leftover &file : before)
        made = make(dir, file.name) && made;
    const QSet<QString> seen = DownloadManager::filesFor(dir.path(), id);
    for (const Leftover &file : during)
        made = make(dir, file.name) && made;
    t.check(made, QStringLiteral("%1 invented files written").arg(before.size() + during.size()));
    t.check(seen.size() == 7 && seen.contains(song + QStringLiteral(".m4a"))
                && seen.contains(old + QStringLiteral(".webm.part"))
                && !seen.contains(QStringLiteral("Shouting [SELFTEST001].opus")),
            QStringLiteral("at the start, the seven files of this track that are already there are noted"),
            QStringList(seen.values()).join(QStringLiteral(", ")));

    // As yt-dlp reports a path on Windows, and as the database stores it.
    const QString kept = QDir::toNativeSeparators(dir.filePath(QStringLiteral("Held By The Database [Selftest001].m4a")));
    const QStringList removed = DownloadManager::removeLeftovers(dir.path(), id, seen, kept);

    int expected = 0;
    for (const QList<Leftover> *list : { &before, &during }) {
        const bool earlier = list == &before;
        for (const Leftover &file : *list) {
            expected += file.goes;
            const bool there = dir.exists(file.name);
            t.check(there != file.goes && removed.contains(file.name) == file.goes,
                    QStringLiteral("%1 %2: %3%4").arg(file.goes ? QStringLiteral("removed") : QStringLiteral("kept   "),
                                                      file.name, QString::fromLatin1(file.what),
                                                      earlier ? QStringLiteral(" (there before)") : QString()),
                    there ? QStringLiteral("still there") : QStringLiteral("gone"));
        }
    }
    t.check(removed.size() == expected,
            QStringLiteral("%1 removed in all, and nothing else").arg(expected),
            removed.join(QStringLiteral(", ")));

    // With no record of the start, which a download that ran always has:
    // everything counts as having been there, and only yt-dlp's own
    // leftovers go.
    {
        const QString again = QStringLiteral("Selftest Artist - Retry [Selftest003]");
        const bool ok = make(dir, again + QStringLiteral(".opus")) && make(dir, again + QStringLiteral(".opus.part"))
                        && make(dir, again + QStringLiteral(".temp.opus"));
        const QStringList gone = DownloadManager::removeLeftovers(
            dir.path(), QStringLiteral("Selftest003"),
            DownloadManager::filesFor(dir.path(), QStringLiteral("Selftest003")), QString());
        t.check(ok && dir.exists(again + QStringLiteral(".opus")) && !dir.exists(again + QStringLiteral(".opus.part"))
                    && !dir.exists(again + QStringLiteral(".temp.opus")) && gone.size() == 2,
                QStringLiteral("no record of the start: the finished .opus stays, .part and .temp go"),
                gone.join(QStringLiteral(", ")));
    }

    return t.finish();
}

int runSaavnDownloadSelfTest()
{
    Checks t("saavn-download");
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test downloads into a folder there and records it"));
        return t.finish();
    }
    QTemporaryDir scratch(QDir(data).filePath(QStringLiteral("saavn-download-test-XXXXXX")));
    if (!t.check(scratch.isValid(), QStringLiteral("a scratch folder in the data folder"), scratch.errorString()))
        return t.finish();
    const QDir dir(scratch.path());
    const QString downloads = dir.filePath(QStringLiteral("downloads"));
    SavedVariable keepNamed("MONOLIST_DOWNLOAD_DIR");
    qputenv("MONOLIST_DOWNLOAD_DIR", QFile::encodeName(QDir::toNativeSeparators(downloads)));
    t.note(QStringLiteral("downloading into %1").arg(QDir::toNativeSeparators(downloads)));

    // The song and its cover, made here: 20 s of pink noise (which fills
    // every bit it is given, as music does and a pure tone does not) as AAC
    // at 320 kbps in MP4, as JioSaavn serves its songs, and at 96 kbps for a
    // link that promises more than it serves; and a 16:9 picture like
    // YouTube's.
    const QString ffmpeg = YtDlp::ffmpegPath();
    const QString ffprobe = ffmpeg.isEmpty()
                                ? QString()
                                : QStandardPaths::findExecutable(QStringLiteral("ffprobe"),
                                                                 { QFileInfo(ffmpeg).absolutePath() });
    if (!t.check(!ffmpeg.isEmpty() && !ffprobe.isEmpty(), QStringLiteral("FFmpeg and ffprobe are here"), ffmpeg))
        return t.finish();
    const QString tone = dir.filePath(QStringLiteral("standin-song.mp4"));
    const QString thinTone = dir.filePath(QStringLiteral("standin-thin.mp4"));
    const QString picture = dir.filePath(QStringLiteral("standin-cover.jpg"));
    QString said;
    const auto makeSong = [&](const QString &bitrate, const QString &path) {
        return runTool(ffmpeg, { QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-f"),
                                 QStringLiteral("lavfi"), QStringLiteral("-i"),
                                 QStringLiteral("anoisesrc=duration=20:color=pink:sample_rate=44100:amplitude=0.3"),
                                 QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-c:a"), QStringLiteral("aac"),
                                 QStringLiteral("-b:a"), bitrate, QStringLiteral("-metadata"),
                                 QStringLiteral("title=JioSaavn's own title"), path },
                       &said);
    };
    const bool madeTone = makeSong(QStringLiteral("320k"), tone) && makeSong(QStringLiteral("96k"), thinTone);
    const bool madeCover = runTool(ffmpeg, { QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-f"),
                                             QStringLiteral("lavfi"), QStringLiteral("-i"),
                                             QStringLiteral("color=c=0x3366cc:s=1280x720:d=1"),
                                             QStringLiteral("-frames:v"), QStringLiteral("1"), picture },
                                   &said);
    if (!t.check(madeTone && madeCover, QStringLiteral("a stand-in song and cover made with FFmpeg"), said))
        return t.finish();

    CdnStandIn cdn;
    {
        QFile song(tone);
        QFile thin(thinTone);
        QFile cover(picture);
        if (song.open(QIODevice::ReadOnly))
            cdn.song = song.readAll();
        if (thin.open(QIODevice::ReadOnly))
            cdn.thin = thin.readAll();
        if (cover.open(QIODevice::ReadOnly))
            cdn.cover = cover.readAll();
    }
    if (!t.check(cdn.listen() && !cdn.song.isEmpty() && !cdn.thin.isEmpty() && !cdn.cover.isEmpty(),
                 QStringLiteral("a stand-in CDN on this computer")))
        return t.finish();
    t.note(QStringLiteral("stand-in song %1 kbps, thin one %2 kbps")
               .arg(cdn.song.size() * 8 / 20000).arg(cdn.thin.size() * 8 / 20000));
    SaavnDownload::setAllowLocalForTest(true);

    DownloadManager manager;
    t.check(QDir::cleanPath(QDir::fromNativeSeparators(manager.downloadDirectory())).compare(
                QDir::cleanPath(downloads), Qt::CaseInsensitive) == 0,
            QStringLiteral("the manager downloads into the scratch folder"), manager.downloadDirectory());
    manager.setCoverTemplateForTest(cdn.url(QStringLiteral("/vi/%1/maxresdefault.jpg")));
    manager.setFormat(QStringLiteral("original"));

    // What the finder is asked, and what it answers, by video id.
    QHash<QString, QString> links;
    QStringList askedFor;
    manager.setSaavnFinder([&](const QString &videoId, const QString &title, const QString &artist,
                               const QString &album, qint64 durationMs,
                               std::function<void(const DownloadManager::SaavnCopy &)> done) {
        askedFor << QStringLiteral("%1|%2|%3|%4|%5").arg(videoId, title, artist, album).arg(durationMs);
        DownloadManager::SaavnCopy copy;
        copy.url = links.value(videoId);
        copy.kbps = 320;
        copy.album = QStringLiteral("JioSaavn Album");
        copy.reason = copy.url.isEmpty() ? QStringLiteral("no match, for the test") : QString();
        QTimer::singleShot(0, [done, copy]() { done(copy); });
    });

    QHash<QString, QString> saved;
    QHash<QString, QString> failures;
    QObject::connect(&manager, &DownloadManager::completed, [&saved](const QString &id, const QString &path) {
        saved.insert(id, path);
    });
    QObject::connect(&manager, &DownloadManager::failed, [&failures](const QString &id, const QString &reason) {
        failures.insert(id, reason);
    });
    const auto filesOf = [&downloads](const QString &id) {
        QStringList names = QStringList(DownloadManager::filesFor(downloads, id).values());
        names.sort();
        return names;
    };

    // 1. The copy downloaded, named, tagged and covered as yt-dlp's would be.
    {
        const QString id = QStringLiteral("Selftest201");
        links.insert(id, cdn.url(QStringLiteral("/song_320.mp4")));
        manager.enqueue(id, QStringLiteral("New Song"), QStringLiteral("Selftest Artist"), QString(), 20000, false,
                        QStringLiteral("YouTube Music Album"));
        waitUntil([&]() { return saved.contains(id) || failures.contains(id); }, 60000);
        const QString path = saved.value(id);
        const QString expected = QDir(downloads).filePath(QStringLiteral("Selftest Artist - New Song [Selftest201].m4a"));
        t.check(!path.isEmpty(), QStringLiteral("JioSaavn's copy: downloaded"), failures.value(id));
        t.check(QDir::cleanPath(QDir::fromNativeSeparators(path)) == QDir::cleanPath(expected),
                QStringLiteral("named \"<Artist> - <Title> [<id>].m4a\", as yt-dlp names it"), path);
        t.check(filesOf(id) == QStringList{ QFileInfo(expected).fileName() },
                QStringLiteral("and nothing else of it is left in the folder"), filesOf(id).join(QStringLiteral(", ")));
        t.check(askedFor.value(0) == QStringLiteral("Selftest201|New Song|Selftest Artist|YouTube Music Album|20000"),
                QStringLiteral("JioSaavn was asked by title, artist, album and length"), askedFor.value(0));
        const Probe p = probe(ffprobe, expected);
        t.check(p.audioCodec == QLatin1String("aac"), QStringLiteral("the sound is JioSaavn's AAC, not re-encoded"),
                p.audioCodec + p.error);
        t.check(p.tags.value(QStringLiteral("title")) == QLatin1String("New Song")
                    && p.tags.value(QStringLiteral("artist")) == QLatin1String("Selftest Artist")
                    && p.tags.value(QStringLiteral("album")) == QLatin1String("YouTube Music Album"),
                QStringLiteral("tagged with the app's title, artist and album (JioSaavn's own tags dropped)"),
                QStringLiteral("%1 / %2 / %3").arg(p.tags.value(QStringLiteral("title")).toString(),
                                                   p.tags.value(QStringLiteral("artist")).toString(),
                                                   p.tags.value(QStringLiteral("album")).toString()));
        t.check(p.tags.value(QStringLiteral("comment")) == QStringLiteral("https://www.youtube.com/watch?v=") + id,
                QStringLiteral("the YouTube link in the comment, as yt-dlp writes it"),
                p.tags.value(QStringLiteral("comment")).toString());
        t.check(p.cover && p.coverWidth == 720 && p.coverHeight == 720,
                QStringLiteral("YouTube's thumbnail embedded as the cover, cropped to its centre square"),
                QStringLiteral("cover %1, %2x%3").arg(p.cover ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(p.coverWidth).arg(p.coverHeight));
        t.check(manager.localPathFor(id) == path && manager.isDownloaded(id),
                QStringLiteral("recorded as downloaded, and played from the file"), manager.localPathFor(id));
    }

    // 2. A link the CDN refuses: yt-dlp takes the song, and a finished file
    // that was there before stays (F13). The id is invented, so yt-dlp finds
    // nothing on YouTube and fails without downloading anything.
    {
        const QString id = QStringLiteral("Selftest202");
        const QString older = QStringLiteral("Selftest Artist - An Old Title [Selftest202].opus");
        QDir().mkpath(downloads);
        const bool madeOld = make(QDir(downloads), older);
        links.insert(id, cdn.url(QStringLiteral("/gone_320.mp4")));
        manager.enqueue(id, QStringLiteral("Refused Song"), QStringLiteral("Selftest Artist"), QString(), 20000);
        waitUntil([&]() { return saved.contains(id) || failures.contains(id); }, 90000);
        t.check(madeOld && failures.contains(id) && !saved.contains(id),
                QStringLiteral("refused: handed to yt-dlp, which fails on an id YouTube does not have"),
                failures.value(id, saved.value(id)));
        t.check(!failures.value(id).contains(QLatin1String("JioSaavn")),
                QStringLiteral("the failure is yt-dlp's own, not JioSaavn's"), failures.value(id));
        t.check(filesOf(id) == QStringList{ older },
                QStringLiteral("the finished file from before stays, and JioSaavn's partial file is gone"),
                filesOf(id).join(QStringLiteral(", ")));
        manager.cancel(id);   // dismiss the failed row
    }

    // 2b. A link named for 320 kbps that serves 96: not kept, yt-dlp's turn
    // (which again fails on the invented id, downloading nothing).
    {
        const QString id = QStringLiteral("Selftest207");
        links.insert(id, cdn.url(QStringLiteral("/thin_320.mp4")));
        manager.enqueue(id, QStringLiteral("Thin Song"), QStringLiteral("Selftest Artist"), QString(), 20000);
        waitUntil([&]() { return saved.contains(id) || failures.contains(id); }, 90000);
        t.check(cdn.asked.contains(QStringLiteral("/thin_320.mp4")) && failures.contains(id) && !saved.contains(id)
                    && filesOf(id).isEmpty(),
                QStringLiteral("a 320 kbps link serving 96: fetched, refused by its size, handed to yt-dlp, nothing left"),
                saved.value(id, failures.value(id)) + QLatin1Char(' ') + filesOf(id).join(QStringLiteral(", ")));
        manager.cancel(id);
    }

    // 3. No cover anywhere: the song still comes, uncovered.
    {
        const QString id = QStringLiteral("Selftest203");
        cdn.noCover.insert(id);
        links.insert(id, cdn.url(QStringLiteral("/song_320.mp4")));
        manager.enqueue(id, QStringLiteral("Bare Song"), QStringLiteral("Selftest Artist"), QString(), 20000);
        waitUntil([&]() { return saved.contains(id) || failures.contains(id); }, 60000);
        const Probe p = probe(ffprobe, saved.value(id));
        t.check(saved.contains(id) && !p.cover && p.tags.value(QStringLiteral("title")) == QLatin1String("Bare Song"),
                QStringLiteral("no thumbnail: saved and tagged, with no cover"),
                failures.value(id, p.error));
        t.check(cdn.asked.contains(QStringLiteral("/vi/Selftest203/maxresdefault.jpg"))
                    && cdn.asked.contains(QStringLiteral("/vi/Selftest203/hqdefault.jpg")),
                QStringLiteral("the largest thumbnail asked for, then the one every video has"));
        t.check(filesOf(id).size() == 1, QStringLiteral("and only the song is left"), filesOf(id).join(QStringLiteral(", ")));
    }

    // 4. Cancelled part-way: nothing of it stays, and nothing is said.
    {
        const QString id = QStringLiteral("Selftest204");
        links.insert(id, cdn.url(QStringLiteral("/held_320.mp4")));
        manager.enqueue(id, QStringLiteral("Held Song"), QStringLiteral("Selftest Artist"), QString(), 20000);
        const bool started = waitUntil([&]() { return manager.progressFor(id) > 0.2; }, 30000);
        manager.cancel(id);
        waitUntil([&]() { return manager.stateFor(id).isEmpty(); }, 5000);
        t.check(started && manager.stateFor(id).isEmpty() && !failures.contains(id) && !saved.contains(id),
                QStringLiteral("cancelled half-way through the transfer: the row goes, and no failure is told"),
                QStringLiteral("state \"%1\"").arg(manager.stateFor(id)));
        t.check(filesOf(id).isEmpty(), QStringLiteral("and its partial file is gone"),
                filesOf(id).join(QStringLiteral(", ")));
        cdn.closeAll();
    }

    // 5. MP3: re-encoded from JioSaavn's AAC, tagged and covered.
    {
        const QString id = QStringLiteral("Selftest205");
        manager.setFormat(QStringLiteral("mp3"));
        links.insert(id, cdn.url(QStringLiteral("/song_320.mp4")));
        manager.enqueue(id, QStringLiteral("Mp3 Song"), QStringLiteral("Selftest Artist"), QString(), 20000);
        waitUntil([&]() { return saved.contains(id) || failures.contains(id); }, 90000);
        const Probe p = probe(ffprobe, saved.value(id));
        t.check(saved.value(id).endsWith(QLatin1String("Selftest Artist - Mp3 Song [Selftest205].mp3"))
                    && p.audioCodec == QLatin1String("mp3") && p.cover
                    && p.tags.value(QStringLiteral("artist")) == QLatin1String("Selftest Artist")
                    && p.tags.value(QStringLiteral("album")) == QLatin1String("JioSaavn Album"),
                QStringLiteral("MP3 chosen: re-encoded, tagged (JioSaavn's album where the list had none), covered"),
                failures.value(id, saved.value(id) + QLatin1Char(' ') + p.audioCodec + p.error));
        manager.setFormat(QStringLiteral("original"));
    }

    // 6. On Standard, the resolver says "none" without asking JioSaavn.
    {
        StreamResolver resolver;
        QString reason;
        bool answered = false;
        Saavn::Target target;
        target.videoId = QStringLiteral("Selftest206");
        target.title = QStringLiteral("Any Song");
        target.artist = QStringLiteral("Selftest Artist");
        target.durationMs = 20000;
        resolver.findSaavn(target, &manager, [&](const StreamResolver::SaavnCopy &copy) {
            answered = true;
            reason = copy.url.isEmpty() ? copy.reason : QStringLiteral("a link: ") + copy.url;
        });
        const bool synchronous = answered;
        waitUntil([&]() { return answered; }, 2000);
        t.check(!resolver.saavnEnabled() && answered && !synchronous && reason.contains(QLatin1String("Standard")),
                QStringLiteral("Standard: findSaavn answers \"none\" at once, JioSaavn never asked"), reason);
    }

    SaavnDownload::setAllowLocalForTest(false);
    return t.finish();
}
