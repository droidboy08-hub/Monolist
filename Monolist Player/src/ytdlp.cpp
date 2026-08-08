#include "ytdlp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QString g_executableOverride;

// Progress lines are requested in a fixed, parse-friendly shape rather than
// scraping yt-dlp's human-readable bar, which changes between releases.
const char *kProgressTemplate = "download:MONOLIST_PROGRESS %(progress.downloaded_bytes)s %(progress.total_bytes)s %(progress.total_bytes_estimate)s";

qint64 toBytes(const QString &field)
{
    bool ok = false;
    const qint64 value = field.toLongLong(&ok);
    return ok ? value : -1;
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

    connect(m_process, &QProcess::readyReadStandardOutput, this, &YtDlpRequest::handleStdout);
    connect(m_process, &QProcess::finished, this, &YtDlpRequest::handleFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            settleFailed(QStringLiteral("yt-dlp could not be started — check that it is installed."));
    });

    m_process->start();
}

void YtDlpRequest::handleStdout()
{
    const QByteArray chunk = m_process->readAllStandardOutput();
    m_stdout.append(chunk);

    if (m_expectJson)
        return;

    // Download mode: consume complete lines, looking for progress and the
    // final destination path.
    while (true) {
        const int newline = m_stdout.indexOf('\n');
        if (newline < 0)
            break;
        const QString line = QString::fromUtf8(m_stdout.left(newline)).trimmed();
        m_stdout.remove(0, newline + 1);

        if (line.startsWith(QLatin1String("MONOLIST_PROGRESS"))) {
            const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() >= 3) {
                const qint64 received = toBytes(parts.at(1));
                qint64 total = toBytes(parts.at(2));
                if (total <= 0 && parts.size() >= 4)
                    total = toBytes(parts.at(3));
                Q_EMIT progress(received, total);
            }
        } else if (line.startsWith(QLatin1String("[Merger] Merging formats into"))
                   || line.startsWith(QLatin1String("[ExtractAudio] Destination:"))
                   || line.startsWith(QLatin1String("[download] Destination:"))) {
            static const QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
            const QRegularExpressionMatch match = quoted.match(line);
            if (match.hasMatch())
                m_destinationPath = match.captured(1);
            else
                m_destinationPath = line.section(QLatin1Char(':'), 1).trimmed();
        }
    }
}

void YtDlpRequest::handleFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_settled)
        return;

    if (status == QProcess::CrashExit) {
        settleFailed(QStringLiteral("yt-dlp terminated unexpectedly."));
        return;
    }

    if (exitCode != 0) {
        const QString stderrText = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        settleFailed(stderrText.isEmpty()
                         ? QStringLiteral("yt-dlp exited with code %1.").arg(exitCode)
                         : stderrText.section(QLatin1Char('\n'), -1));
        return;
    }

    m_settled = true;

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
        Q_EMIT finishedFile(m_destinationPath);
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

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (!onPath.isEmpty()) {
        accept(onPath);
        return invocation;
    }

    // A copy shipped alongside the application, for portable installs.
#ifdef Q_OS_WIN
    const QString bundledName = QStringLiteral("yt-dlp.exe");
#else
    const QString bundledName = QStringLiteral("yt-dlp");
#endif
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(bundledName);
    if (QFileInfo::exists(bundled)) {
        accept(bundled);
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

YtDlpRequest *YtDlp::run(const QStringList &args, bool expectJson, QObject *parent)
{
    auto *request = new YtDlpRequest(parent);
    const Invocation invocation = locate();
    if (!invocation.valid) {
        // Report asynchronously so callers can connect before anything fires.
        QMetaObject::invokeMethod(request, [request]() {
            request->settleFailed(QStringLiteral(
                "yt-dlp was not found. Install it with `pip install yt-dlp`, or place "
                "the binary next to the application."));
        }, Qt::QueuedConnection);
        return request;
    }

    request->start(invocation.program, invocation.prefixArgs + args, expectJson);
    return request;
}

YtDlpRequest *YtDlp::search(const QString &query, int limit, QObject *parent)
{
    const QStringList args = {
        QStringLiteral("ytsearch%1:%2").arg(qBound(1, limit, 50)).arg(query),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("--ignore-config"),
        QStringLiteral("--socket-timeout"), QStringLiteral("15")
    };
    return run(args, /*expectJson=*/true, parent);
}

YtDlpRequest *YtDlp::resolveAudio(const QString &videoIdOrUrl, QObject *parent)
{
    const QStringList args = {
        normaliseToUrl(videoIdOrUrl),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("--ignore-config"),
        // Prefer a plain m4a track: one stream, no remux needed to start playing.
        QStringLiteral("-f"), QStringLiteral("bestaudio[ext=m4a]/bestaudio/best"),
        QStringLiteral("--socket-timeout"), QStringLiteral("15")
    };
    return run(args, /*expectJson=*/true, parent);
}

YtDlpRequest *YtDlp::download(const QString &videoIdOrUrl,
                              const QString &destinationDir,
                              const QString &fileStem,
                              QObject *parent)
{
    QDir().mkpath(destinationDir);
    const QString outputTemplate = QDir(destinationDir).filePath(fileStem + QStringLiteral(".%(ext)s"));

    const QStringList args = {
        normaliseToUrl(videoIdOrUrl),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("--ignore-config"),
        QStringLiteral("--newline"),
        QStringLiteral("-f"), QStringLiteral("bestaudio[ext=m4a]/bestaudio/best"),
        QStringLiteral("--extract-audio"),
        QStringLiteral("--audio-format"), QStringLiteral("m4a"),
        QStringLiteral("--audio-quality"), QStringLiteral("0"),
        QStringLiteral("--embed-thumbnail"),
        QStringLiteral("--embed-metadata"),
        QStringLiteral("--progress-template"), QString::fromUtf8(kProgressTemplate),
        QStringLiteral("--socket-timeout"), QStringLiteral("15"),
        QStringLiteral("-o"), outputTemplate
    };
    return run(args, /*expectJson=*/false, parent);
}
