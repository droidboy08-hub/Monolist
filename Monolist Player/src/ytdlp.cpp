#include "ytdlp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QString g_executableOverride;

// Download output is requested in fixed, parse-friendly shapes rather than
// scraped from yt-dlp's human-readable log, which changes between releases:
//   MONOLIST_PROGRESS <received> <total> <total estimate> <bytes/s> <eta s>
//   MONOLIST_STEP <post-processor> <started|finished>
//   MONOLIST_META <JSON: title, artist, album, duration, thumbnail, ...>
//   MONOLIST_FILE <final path, after every post-processor has run>
const QLatin1String kProgressMarker("MONOLIST_PROGRESS ");
const QLatin1String kStepMarker("MONOLIST_STEP ");
const QLatin1String kMetaMarker("MONOLIST_META ");
const QLatin1String kFileMarker("MONOLIST_FILE ");

const char *kProgressTemplate =
    "download:MONOLIST_PROGRESS %(progress.downloaded_bytes)s %(progress.total_bytes)s "
    "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s";
const char *kStepTemplate = "postprocess:MONOLIST_STEP %(progress.postprocessor)s %(progress.status)s";
// JSON is ASCII-escaped by default, so titles survive whatever the pipe does.
const char *kMetaTemplate = "after_move:MONOLIST_META %(.{title,artist,uploader,channel,album,duration,thumbnail})j";
const char *kFileTemplate = "after_move:MONOLIST_FILE %(filepath)s";

// Bounded, so a long download cannot grow the buffer without limit. Only the
// tail matters: it holds the error, if there is one.
constexpr qsizetype kMaxStderr = 64 * 1024;

// yt-dlp prints "NA" for fields it cannot fill yet, and floats for sizes it has
// only estimated.
double toNumber(const QString &field)
{
    bool ok = false;
    const double value = field.toDouble(&ok);
    return ok ? value : -1.0;
}

bool isMarker(const QString &line)
{
    return line.startsWith(kProgressMarker) || line.startsWith(kStepMarker)
        || line.startsWith(kMetaMarker) || line.startsWith(kFileMarker);
}

} // namespace

// ---------------------------------------------------------------- YtDlpRequest

YtDlpRequest::YtDlpRequest(QObject *parent)
    : QObject(parent)
{
}

YtDlpRequest::~YtDlpRequest()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

bool YtDlpRequest::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void YtDlpRequest::start(const QString &program, const QStringList &arguments, bool expectJson)
{
    m_expectJson = expectJson;
    m_process = new QProcess(this);
    m_process->setProgram(program);
    m_process->setArguments(arguments);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    // Piped output is written in the ANSI code page unless told otherwise,
    // which turns any title or path outside it into question marks. The
    // --encoding flag covers yt-dlp's own output; these cover Python's.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    m_process->setProcessEnvironment(environment);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &YtDlpRequest::handleStdout);
    // Drain stderr as it arrives. Left unread, a verbose run fills the pipe
    // buffer and yt-dlp blocks writing to it, which looks like a hang.
    connect(m_process, &QProcess::readyReadStandardError, this, &YtDlpRequest::handleStderr);
    connect(m_process, &QProcess::finished, this, &YtDlpRequest::handleFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            settleFailed(QStringLiteral("yt-dlp could not be started. Check that it is installed."));
    });

    m_process->start();
}

void YtDlpRequest::handleStdout()
{
    m_stdout.append(m_process->readAllStandardOutput());
    if (m_expectJson)
        return;

    // Download mode: consume complete lines.
    for (qsizetype newline = m_stdout.indexOf('\n'); newline >= 0; newline = m_stdout.indexOf('\n')) {
        const QString line = QString::fromUtf8(m_stdout.left(newline)).trimmed();
        m_stdout.remove(0, newline + 1);
        handleLine(line);
    }
}

void YtDlpRequest::handleStderr()
{
    const QByteArray chunk = m_process->readAllStandardError();
    if (m_expectJson) {
        m_stderr.append(chunk);
    } else {
        // --print puts yt-dlp in quiet mode, which sends its screen output,
        // post-processor progress included, to stderr. Pick the markers out and
        // keep everything else for the error message.
        m_stderrLine.append(chunk);
        for (qsizetype newline = m_stderrLine.indexOf('\n'); newline >= 0;
             newline = m_stderrLine.indexOf('\n')) {
            const QByteArray raw = m_stderrLine.left(newline + 1);
            m_stderrLine.remove(0, newline + 1);
            const QString line = QString::fromUtf8(raw).trimmed();
            if (isMarker(line))
                handleLine(line);
            else
                m_stderr.append(raw);
        }
    }
    if (m_stderr.size() > kMaxStderr)
        m_stderr.remove(0, m_stderr.size() - kMaxStderr);
}

void YtDlpRequest::handleLine(const QString &line)
{
    if (line.startsWith(kProgressMarker)) {
        const QStringList parts = line.mid(kProgressMarker.size()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 5)
            return;
        const qint64 received = qint64(toNumber(parts.at(0)));
        qint64 total = qint64(toNumber(parts.at(1)));
        if (total <= 0)
            total = qint64(toNumber(parts.at(2)));
        Q_EMIT progress(received, total, toNumber(parts.at(3)), int(toNumber(parts.at(4))));
    } else if (line.startsWith(kStepMarker)) {
        const QStringList parts = line.mid(kStepMarker.size()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts.at(1) == QLatin1String("started"))
            Q_EMIT postProcessing(parts.at(0));
    } else if (line.startsWith(kMetaMarker)) {
        m_metadata = QJsonDocument::fromJson(line.mid(kMetaMarker.size()).toUtf8()).object().toVariantMap();
    } else if (line.startsWith(kFileMarker)) {
        m_destinationPath = line.mid(kFileMarker.size()).trimmed();
    }
}

void YtDlpRequest::handleFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_settled)
        return;

    // finished() can arrive with data still sitting in the pipes: readyRead is
    // not guaranteed to have delivered every last chunk first. A --dump-json
    // response is ~600 KB, so losing the tail meant the JSON failed to parse.
    handleStdout();
    handleStderr();
    if (!m_expectJson && !m_stdout.isEmpty()) {
        handleLine(QString::fromUtf8(m_stdout).trimmed());   // a last line without a newline
        m_stdout.clear();
    }

    if (status == QProcess::CrashExit) {
        settleFailed(QStringLiteral("yt-dlp terminated unexpectedly."));
        return;
    }

    if (exitCode != 0) {
        // Prefer yt-dlp's own "ERROR: [youtube] <id>: <reason>", reduced to the
        // reason; otherwise the last thing it said.
        const QStringList lines = QString::fromUtf8(m_stderr + m_stderrLine)
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QString reason;
        for (auto it = lines.crbegin(); it != lines.crend() && reason.isEmpty(); ++it) {
            const qsizetype at = it->indexOf(QLatin1String("ERROR:"));
            if (at >= 0)
                reason = it->mid(at + 6).trimmed();
        }
        if (reason.isEmpty() && !lines.isEmpty())
            reason = lines.last().trimmed();
        static const QRegularExpression extractorPrefix(QStringLiteral(R"(^\[[^\]]+\]\s*[\w-]+:\s*)"));
        reason.remove(extractorPrefix);
        settleFailed(reason.isEmpty() ? QStringLiteral("yt-dlp exited with code %1.").arg(exitCode)
                                      : reason);
        return;
    }

    m_settled = true;

    // Said even on success: a resolve that worked while complaining about a
    // missing JavaScript runtime or a skipped PO token is the warning before
    // the failure, and it is the only notice we get.
    if (!m_stderr.isEmpty())
        qWarning("yt-dlp: %s", QString::fromUtf8(m_stderr).trimmed().toUtf8().constData());

    if (m_expectJson) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(m_stdout, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            m_settled = false;
            settleFailed(QStringLiteral("Could not parse yt-dlp output: %1").arg(parseError.errorString()));
            return;
        }
        Q_EMIT succeededJson(document);
    } else {
        Q_EMIT finishedFile(m_destinationPath, m_metadata);
    }

    deleteLater();
}

void YtDlpRequest::settleFailed(const QString &reason)
{
    if (m_settled)
        return;
    m_settled = true;
    Q_EMIT failed(reason);
    deleteLater();
}

void YtDlpRequest::cancel()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    settleFailed(QStringLiteral("Cancelled."));
}

// ----------------------------------------------------------------------- YtDlp

void YtDlp::setExecutableOverride(const QString &path)
{
    g_executableOverride = path;
}

QStringList YtDlp::toolDirectories()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList directories;
    const QString fromEnvironment = qEnvironmentVariable("MONOLIST_TOOLS_DIR");
    if (!fromEnvironment.isEmpty())
        directories.append(fromEnvironment);
    // tools/yt-dlp holds the unpacked yt-dlp: its exe beside its _internal
    // runtime, which starts far faster than the single-file build.
    directories << appDir.filePath(QStringLiteral("tools"))
                << appDir.filePath(QStringLiteral("tools/yt-dlp"))
                << appDir.absolutePath();
    return directories;
}

QString YtDlp::findTool(const QString &name)
{
    // findExecutable appends .exe and the other PATHEXT extensions on Windows.
    const QString bundled = QStandardPaths::findExecutable(name, toolDirectories());
    return bundled.isEmpty() ? QStandardPaths::findExecutable(name) : bundled;
}

QString YtDlp::ffmpegPath()
{
    return findTool(QStringLiteral("ffmpeg"));
}

QString YtDlp::denoPath()
{
    return findTool(QStringLiteral("deno"));
}

YtDlp::Invocation YtDlp::locate()
{
    Invocation invocation;

    const auto accept = [&invocation](const QString &program, const QStringList &prefix = {}) {
        invocation.program = program;
        invocation.prefixArgs = prefix;
        invocation.valid = true;
    };

    if (!g_executableOverride.isEmpty() && QFileInfo::exists(g_executableOverride)) {
        accept(g_executableOverride);
        return invocation;
    }

    const QString found = findTool(QStringLiteral("yt-dlp"));
    if (!found.isEmpty()) {
        accept(found);
        return invocation;
    }

    // Last resort: the Python module, which is how pip installs it when the
    // scripts directory is not on PATH.
    for (const QString &python : { QStringLiteral("python3"), QStringLiteral("python") }) {
        const QString interpreter = QStandardPaths::findExecutable(python);
        if (!interpreter.isEmpty()) {
            accept(interpreter, { QStringLiteral("-m"), QStringLiteral("yt_dlp") });
            return invocation;
        }
    }

    return invocation;
}

bool YtDlp::isAvailable()
{
    return locate().valid;
}

QStringList YtDlp::invocationCommand()
{
    const Invocation invocation = locate();
    if (!invocation.valid)
        return {};
    return QStringList{ invocation.program } + invocation.prefixArgs;
}

QString YtDlp::resolvedDescription()
{
    const Invocation invocation = locate();
    if (!invocation.valid)
        return QStringLiteral("not found");
    return (QStringList() << invocation.program << invocation.prefixArgs).join(QLatin1Char(' '));
}

QString YtDlp::normaliseToUrl(const QString &videoIdOrUrl)
{
    if (videoIdOrUrl.startsWith(QLatin1String("http://"))
        || videoIdOrUrl.startsWith(QLatin1String("https://")))
        return videoIdOrUrl;
    return QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoIdOrUrl);
}

QString YtDlp::cleanArtist(const QString &channel)
{
    static const QRegularExpression channelSuffix(QStringLiteral(R"((?:\s+-\s+Topic|VEVO)$)"));
    QString artist = channel.trimmed();
    artist.remove(channelSuffix);
    return artist.trimmed();
}

// Passed on every call, so the companions are found even when they are not on
// PATH: next to the app, or in a tools folder.
QStringList YtDlp::commonArguments()
{
    QStringList args = {
        QStringLiteral("--ignore-config"),
        // Warnings are kept deliberately. yt-dlp says why it is about to fail
        // in a warning, not an error — "PO Token … will be skipped", "ensure
        // you have a supported JavaScript runtime" — and with --no-warnings on
        // those never reached us, which is most of why a track that would not
        // play was so hard to diagnose.
        QStringLiteral("--encoding"), QStringLiteral("utf-8"),
        QStringLiteral("--socket-timeout"), QStringLiteral("15"),
        // Bounded. The defaults retry enough to outlast any patience, and the
        // resolver now gives up at 12s anyway; failing fast leaves time for
        // the tier below.
        QStringLiteral("--retries"), QStringLiteral("2"),
        QStringLiteral("--extractor-retries"), QStringLiteral("1")
    };
    const QString deno = denoPath();
    if (!deno.isEmpty())
        args << QStringLiteral("--js-runtimes") << QStringLiteral("deno:") + QDir::toNativeSeparators(deno);
    const QString ffmpeg = ffmpegPath();
    if (!ffmpeg.isEmpty()) {
        // The directory, so yt-dlp picks up ffprobe from beside it too.
        args << QStringLiteral("--ffmpeg-location")
             << QDir::toNativeSeparators(QFileInfo(ffmpeg).absolutePath());
    }
    return args;
}

YtDlpRequest *YtDlp::run(const QStringList &args, bool expectJson, QObject *parent)
{
    auto *request = new YtDlpRequest(parent);
    const Invocation invocation = locate();
    if (!invocation.valid) {
        // Report asynchronously so callers can connect before anything fires.
        QMetaObject::invokeMethod(request, [request]() {
            request->settleFailed(QStringLiteral(
                "yt-dlp was not found. Run scripts/setup-windows.ps1, install it with "
                "`pip install yt-dlp`, or place the binary next to the application."));
        }, Qt::QueuedConnection);
        return request;
    }

    request->start(invocation.program, invocation.prefixArgs + commonArguments() + args, expectJson);
    return request;
}

YtDlpRequest *YtDlp::search(const QString &query, int limit, QObject *parent)
{
    const QStringList args = {
        QStringLiteral("ytsearch%1:%2").arg(QString::number(qBound(1, limit, 50)), query),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--flat-playlist")
    };
    return run(args, /*expectJson=*/true, parent);
}

YtDlpRequest *YtDlp::resolveAudio(const QString &videoIdOrUrl, QObject *parent)
{
    const QStringList args = {
        normaliseToUrl(videoIdOrUrl),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--no-playlist"),
        // Usually Opus at ~130-160 kb/s, which beats the ~128 kb/s AAC stream;
        // mpv plays either directly.
        QStringLiteral("-f"), QStringLiteral("bestaudio/best")
    };
    return run(args, /*expectJson=*/true, parent);
}

YtDlpRequest *YtDlp::resolveVideo(const QString &videoIdOrUrl, int maxHeight, QObject *parent)
{
    const int cap = qBound(240, maxHeight, 2160);
    // H.264 first, and only then whatever else there is. Left to itself
    // YouTube offers AV1, which every desktop can decode in software and few
    // can decode in hardware: on a modest machine — or a virtual one — the
    // picture never arrives. H.264 is the one codec with hardware decoding
    // everywhere the app runs. After that, a stream with both in one file,
    // then anything at all.
    const QString format = QStringLiteral(
        "bv*[vcodec^=avc1][height<=?%1]+ba"
        "/b[height<=?%1][vcodec!=none][acodec!=none]"
        "/bv*[height<=?%1]+ba"
        "/b").arg(cap);
    const QStringList args = {
        normaliseToUrl(videoIdOrUrl),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("-f"), format
    };
    return run(args, /*expectJson=*/true, parent);
}

YtDlpRequest *YtDlp::download(const QString &videoIdOrUrl,
                              const QString &destinationDir,
                              const QString &fileStem,
                              const DownloadOptions &options,
                              QObject *parent)
{
    QDir().mkpath(destinationDir);
    // -o is an output template, so a literal "%" (a title like "100% Pure")
    // has to be doubled to survive it. With no stem, yt-dlp names the file
    // from the metadata it fetches, in the same shape.
    const QString directory = QString(destinationDir).replace(QLatin1Char('%'), QStringLiteral("%%"));
    const QString stem = fileStem.isEmpty()
        ? QStringLiteral("%(artist,uploader|Unknown artist)s - %(title)s [%(id)s]")
        : QString(fileStem).replace(QLatin1Char('%'), QStringLiteral("%%"));
    const QString outputTemplate = QDir(directory).filePath(stem + QStringLiteral(".%(ext)s"));

    QStringList args = {
        normaliseToUrl(videoIdOrUrl),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--no-mtime"),       // "date added" is today, not the upload date
        QStringLiteral("--newline"),
        QStringLiteral("--progress"),       // --print implies --quiet; keep the progress lines
        QStringLiteral("--progress-template"), QString::fromLatin1(kProgressTemplate),
        QStringLiteral("--progress-template"), QString::fromLatin1(kStepTemplate),
        QStringLiteral("--print"), QString::fromLatin1(kMetaTemplate),
        QStringLiteral("--print"), QString::fromLatin1(kFileTemplate),
        QStringLiteral("-o"), outputTemplate
    };

    // Without FFmpeg nothing can be extracted, tagged or trimmed: the best
    // single-file audio stream is saved exactly as served.
    const bool canConvert = !ffmpegPath().isEmpty();

    switch (options.format) {
    case DownloadOptions::Format::Original:
        // Unwrap the audio from its container but never re-encode it: Opus
        // stays Opus (.opus), AAC stays AAC (.m4a).
        args << QStringLiteral("-f") << QStringLiteral("bestaudio/best");
        if (canConvert)
            args << QStringLiteral("-x") << QStringLiteral("--audio-format") << QStringLiteral("best");
        break;
    case DownloadOptions::Format::M4a:
        args << QStringLiteral("-f") << QStringLiteral("bestaudio[ext=m4a]/bestaudio/best");
        if (canConvert)
            args << QStringLiteral("-x") << QStringLiteral("--audio-format") << QStringLiteral("m4a");
        break;
    case DownloadOptions::Format::Mp3:
        args << QStringLiteral("-f") << QStringLiteral("bestaudio/best");
        if (canConvert) {
            args << QStringLiteral("-x") << QStringLiteral("--audio-format") << QStringLiteral("mp3")
                 << QStringLiteral("--audio-quality") << QStringLiteral("0");
        }
        break;
    }

    if (canConvert && options.embedMetadata) {
        // Tag the performer, not the channel: take yt-dlp's artist field when
        // it has one, and strip the "- Topic" / "VEVO" suffixes otherwise.
        args << QStringLiteral("--embed-metadata")
             << QStringLiteral("--parse-metadata")
             << QStringLiteral("%(artist,creator,uploader|)s:^(?P<meta_artist>.+?)(?: - Topic|VEVO)?$");
    }

    if (canConvert && options.embedArtwork) {
        // Video thumbnails are 16:9 and album art is square. Crop the centre,
        // which is where YouTube Music places the cover on its letterboxed
        // thumbnails.
        args << QStringLiteral("--embed-thumbnail")
             << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg")
             << QStringLiteral("--ppa")
             << QStringLiteral("ThumbnailsConvertor+FFmpeg_o:-c:v mjpeg -q:v 2 -vf crop=\"'min(iw,ih)':'min(iw,ih)'\"");
    }

    if (canConvert && options.skipNonMusic)
        args << QStringLiteral("--sponsorblock-remove") << QStringLiteral("music_offtopic");

    return run(args, /*expectJson=*/false, parent);
}
