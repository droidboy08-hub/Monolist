#include "appinfo.h"

#include "appdatabase.h"
#include "buildinfo.h"
#include "ytdlp.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

namespace {

constexpr int kFeedTimeoutMs = 10000;
// A tool that will not answer --version in this long is not going to. Nothing
// waits on the answer, so this can be generous: yt-dlp is a Python program,
// and started cold under x64 emulation it has taken several seconds.
constexpr int kVersionTimeoutMs = 20000;

// The components whose versions are read, as they are named in the list.
const QString kYtDlp = QStringLiteral("yt-dlp");
const QString kFfmpeg = QStringLiteral("FFmpeg");
const QString kDeno = QStringLiteral("Deno");
// The setup script fetches and unpacks several tools; it is allowed to take
// its time, but not forever.
constexpr int kToolsTimeoutMs = 10 * 60 * 1000;

QVariantMap component(const QString &name, const QString &version, const QString &role)
{
    return QVariantMap{
        { QStringLiteral("name"), name },
        // Empty means "not installed", which the row shows rather than hides:
        // a missing FFmpeg is why downloads cannot be tagged, and a missing
        // yt-dlp is why some tracks will not play.
        { QStringLiteral("version"), version },
        { QStringLiteral("role"), role }
    };
}

// "1.2.10" sorts after "1.2.9", which a string comparison gets wrong. Anything
// that is not a number compares as text, so "2026.08.19" works as well as
// "0.1" and a date-shaped tag beats a smaller one.
int compareVersions(const QString &left, const QString &right)
{
    const QStringList a = left.split(QLatin1Char('.'));
    const QStringList b = right.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < qMax(a.size(), b.size()); ++i) {
        const QString pa = i < a.size() ? a.at(i) : QString();
        const QString pb = i < b.size() ? b.at(i) : QString();
        bool okA = false;
        bool okB = false;
        const int na = pa.toInt(&okA);
        const int nb = pb.toInt(&okB);
        if (okA && okB) {
            if (na != nb)
                return na < nb ? -1 : 1;
        } else if (pa != pb) {
            return pa < pb ? -1 : 1;
        }
    }
    return 0;
}

} // namespace

AppInfo::AppInfo(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

QString AppInfo::version() const     { return QStringLiteral(MONOLIST_VERSION); }
QString AppInfo::buildNumber() const { return QStringLiteral(MONOLIST_BUILD_NUMBER); }
QString AppInfo::commit() const      { return QStringLiteral(MONOLIST_COMMIT); }
QString AppInfo::buildDate() const   { return QStringLiteral(MONOLIST_BUILD_DATE); }
bool AppInfo::modified() const       { return MONOLIST_DIRTY; }
QString AppInfo::qtVersion() const   { return QStringLiteral(QT_VERSION_STR); }

QString AppInfo::fullVersion() const
{
    QString text = QStringLiteral("%1 (build %2)").arg(version(), buildNumber());
    if (modified())
        text += QStringLiteral(" +changes");
    return text;
}

// ------------------------------------------------------------------ settings

QString AppInfo::updateFeed()
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(QStringLiteral("update.feed"));
    if (query.exec() && query.next())
        return query.value(0).toString();
    return {};
}

void AppInfo::setUpdateFeed(const QString &url)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(QStringLiteral("update.feed"));
    query.addBindValue(AppDatabase::text(url));
    query.exec();
}

// ---------------------------------------------------------------- components

// Every tool is asked in its own process, all at once, and nothing waits for
// them. This used to run each in turn on the interface thread, and because the
// Settings page was built at launch, every start of the app sat behind three
// processes — seconds of them, since yt-dlp is a Python start.
void AppInfo::refreshComponents()
{
    // A newer read replaces an older one: after a tool update, what the old
    // binaries are still saying is no longer the truth.
    ++m_versionRound;
    for (const QPointer<QProcess> &process : std::as_const(m_versionProcesses)) {
        if (process)
            process->kill();
    }
    m_versionProcesses.clear();
    m_versionAnswers.clear();

    // Held by this function while it starts them, so a tool that fails to
    // start at once cannot publish the list before the others are asked.
    m_versionsPending = 1;
    const QStringList ytdlp = YtDlp::invocationCommand();
    if (!ytdlp.isEmpty())
        askVersion(kYtDlp, ytdlp.first(), ytdlp.mid(1) + QStringList{ QStringLiteral("--version") });
    askVersion(kFfmpeg, YtDlp::ffmpegPath(), { QStringLiteral("-version") });
    askVersion(kDeno, YtDlp::denoPath(), { QStringLiteral("--version") });
    if (--m_versionsPending == 0)
        publishComponents();
}

void AppInfo::askVersion(const QString &tool, const QString &program, const QStringList &arguments)
{
    if (program.isEmpty())
        return;   // not installed, which its row says

    auto *process = new QProcess(this);
    const int round = m_versionRound;
    m_versionProcesses.append(process);
    ++m_versionsPending;

    // Exactly one of the two handlers below runs: a process that failed to
    // start never finishes, and one that finished did start.
    const auto settle = [this, process, tool, round](bool answered) {
        process->deleteLater();
        if (round != m_versionRound)
            return;
        QString text;
        if (answered) {
            // FFmpeg writes its banner to stdout, yt-dlp to stdout, others to
            // stderr: take whichever spoke.
            text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
            if (text.isEmpty())
                text = QString::fromUtf8(process->readAllStandardError()).trimmed();
        }
        m_versionAnswers.insert(tool, text.section(QLatin1Char('\n'), 0, 0).trimmed());
        if (--m_versionsPending == 0)
            publishComponents();
    };
    connect(process, &QProcess::finished, this, [settle](int, QProcess::ExitStatus status) {
        // Killed for taking too long counts as no answer.
        settle(status == QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, this, [settle](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            settle(false);
    });
    QTimer::singleShot(kVersionTimeoutMs, process, [process]() {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });

    process->setProgram(program);
    process->setArguments(arguments);
    process->start();
}

void AppInfo::publishComponents()
{
    QVariantList list;

    list.append(component(QStringLiteral("Qt"), qtVersion(),
                          QStringLiteral("The interface, and everything drawn in it")));

    list.append(component(kYtDlp, m_versionAnswers.value(kYtDlp),
                          QStringLiteral("Resolves tracks the fast path cannot, and saves downloads")));

    QString ffmpegVersion = m_versionAnswers.value(kFfmpeg);
    // "ffmpeg version 7.1-full_build-www.gyan.dev Copyright (c) …"
    ffmpegVersion.remove(QStringLiteral("ffmpeg version "));
    ffmpegVersion = ffmpegVersion.section(QStringLiteral(" Copyright"), 0, 0).trimmed();
    list.append(component(kFfmpeg, ffmpegVersion,
                          QStringLiteral("Converts and tags what you download")));

    QString denoVersion = m_versionAnswers.value(kDeno);
    // "deno 2.9.7 (stable, release, x86_64-pc-windows-msvc)" — the build
    // triple is not what anyone is here to read.
    denoVersion.remove(QStringLiteral("deno "));
    denoVersion = denoVersion.section(QLatin1Char(' '), 0, 0);
    list.append(component(kDeno, denoVersion.trimmed(),
                          QStringLiteral("Runs YouTube's player script when yt-dlp needs it")));

    m_versionProcesses.clear();
    m_components = list;
    m_componentsKnown = true;
    Q_EMIT componentsChanged();

    // A report copied while these were being read is completed now — but only
    // if it is still what the clipboard holds. Anything copied since is the
    // user's, and replacing it seconds later would paste the wrong thing.
    if (!m_partialReport.isEmpty()) {
        const QString partial = std::exchange(m_partialReport, QString());
        QClipboard *clipboard = QGuiApplication::clipboard();
        // Compared without carriage returns: a platform clipboard may hand
        // text back with Windows line ends.
        const auto plain = [](QString text) { return text.remove(QLatin1Char('\r')); };
        if (clipboard && plain(clipboard->text()) == plain(partial))
            clipboard->setText(report());
    }
}

QString AppInfo::report() const
{
    // Qt is not named here: it is the first of the components below, and a
    // report that states a version twice invites the reader to wonder which
    // one is the real one.
    QString text = QStringLiteral("Monolist %1\ncommit %2, built %3\n")
                       .arg(fullVersion(), commit(), buildDate());
    if (!m_componentsKnown) {
        // Asked for before the tools answered: Qt needs no asking, and the
        // rest is said to be missing rather than left out without a word.
        text += QStringLiteral("Qt %1\n(other components: versions not read yet)\n").arg(qtVersion());
        return text;
    }
    for (const QVariant &entry : m_components) {
        const QVariantMap map = entry.toMap();
        text += QStringLiteral("%1 %2\n")
                    .arg(map.value(QStringLiteral("name")).toString(),
                         map.value(QStringLiteral("version")).toString().isEmpty()
                             ? QStringLiteral("(not installed)")
                             : map.value(QStringLiteral("version")).toString());
    }
    return text;
}

void AppInfo::copyReport()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;
    // Copied at once, whatever is known: a click that puts nothing on the
    // clipboard looks broken, and the answers can take many seconds. The
    // versions are what a report is for, though, so one copied before they
    // are in is completed when they arrive (publishComponents).
    const QString text = report();
    clipboard->setText(text);
    if (m_componentsKnown)
        return;
    m_partialReport = text;
    if (m_versionsPending == 0)
        refreshComponents();
}

// -------------------------------------------------------------- app updates

void AppInfo::setUpdate(int state, const QString &message, const QString &url)
{
    m_updateState = state;
    m_updateMessage = message;
    m_updateUrl = url;
    Q_EMIT updateChanged();
}

void AppInfo::checkForUpdate()
{
    if (m_updateState == Working)
        return;

    const QString feed = updateFeed();
    if (feed.isEmpty()) {
        // Said plainly rather than dressed up as a failure. There is no
        // release feed yet because the project has nowhere to publish to;
        // that is a fact about the project, not a fault in the check.
        setUpdate(NotConfigured,
                  QStringLiteral("No release feed is set, so there is nothing to check against yet."));
        return;
    }

    setUpdate(Working, QStringLiteral("Checking…"));

    QNetworkRequest request{ QUrl(feed) };
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Monolist/%1").arg(version()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(kFeedTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    m_updateReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_updateReply != reply)
            return;
        m_updateReply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            setUpdate(Failed, QStringLiteral("Could not reach the release feed: %1")
                                  .arg(reply->errorString()));
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QString tag = root.value(QStringLiteral("tag_name")).toString();
        const QString page = root.value(QStringLiteral("html_url")).toString();
        if (tag.isEmpty()) {
            setUpdate(Failed, QStringLiteral("The release feed did not name a version."));
            return;
        }
        if (tag.startsWith(QLatin1Char('v')))
            tag.remove(0, 1);

        if (compareVersions(version(), tag) < 0) {
            setUpdate(Available, QStringLiteral("Monolist %1 is available.").arg(tag), page);
        } else {
            setUpdate(UpToDate,
                      QStringLiteral("This is the newest release (%1).").arg(version()));
        }
    });
}

void AppInfo::openUpdatePage()
{
    if (!m_updateUrl.isEmpty())
        QDesktopServices::openUrl(QUrl(m_updateUrl));
}

// ------------------------------------------------------------ tool updates

// The script that installed the bundled tools in the first place, if this
// build can still see it. A build running from where it was developed can; one
// installed somewhere else cannot, and says so instead of pretending.
QString AppInfo::setupScriptPath()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("scripts/setup-windows.ps1");
    const QString fromSource = QDir(QStringLiteral(MONOLIST_SOURCE_DIR)).filePath(name);
    if (QFileInfo::exists(fromSource))
        return QDir::toNativeSeparators(fromSource);
    const QString beside = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFileInfo::exists(beside))
        return QDir::toNativeSeparators(beside);
#endif
    return {};
}

bool AppInfo::canUpdateTools() const
{
    return !setupScriptPath().isEmpty();
}

void AppInfo::setTools(int state, const QString &message)
{
    m_toolsState = state;
    m_toolsMessage = message;
    Q_EMIT toolsChanged();
}

void AppInfo::updateTools()
{
    if (m_toolsState == Working)
        return;

    const QString script = setupScriptPath();
    if (script.isEmpty()) {
        setTools(Failed,
                 QStringLiteral("The setup script is not next to this build, so the tools "
                                "cannot be updated from here."));
        return;
    }

    setTools(Working, QStringLiteral("Fetching the newest tools…"));

    auto *process = new QProcess(this);
    m_toolsProcess = process;
    process->setProgram(QStringLiteral("powershell.exe"));
    process->setArguments({ QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                            QStringLiteral("-NonInteractive"),
                            QStringLiteral("-File"), script, QStringLiteral("-Update") });
    process->setProcessChannelMode(QProcess::MergedChannels);

    // The script prints a line per tool; show the last one, so a long update
    // says what it is doing rather than sitting on one message for minutes.
    connect(process, &QProcess::readyRead, this, [this, process]() {
        const QString chunk = QString::fromUtf8(process->readAll());
        const QStringList lines = chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            setTools(Working, lines.last().trimmed());
    });

    connect(process, &QProcess::finished, this,
            [this, process](int code, QProcess::ExitStatus status) {
                process->deleteLater();
                if (m_toolsProcess != process)
                    return;
                m_toolsProcess = nullptr;
                // Said whatever the exit code: a run that stopped at the third
                // tool may still have put the first two in place.
                Q_EMIT toolsUpdated();
                if (status == QProcess::CrashExit || code != 0) {
                    setTools(Failed, QStringLiteral("The update did not finish (exit %1). "
                                                    "The tools you already have are untouched.")
                                         .arg(code));
                    return;
                }
                setTools(UpToDate, QStringLiteral("The tools are up to date."));
                // Show what they are now, not what they were.
                refreshComponents();
            });

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_toolsProcess != process)
            return;
        m_toolsProcess = nullptr;
        setTools(Failed, QStringLiteral("PowerShell could not be started."));
    });

    process->start();
    QTimer::singleShot(kToolsTimeoutMs, this, [this, process]() {
        if (m_toolsProcess == process && process->state() != QProcess::NotRunning) {
            process->kill();
            setTools(Failed, QStringLiteral("The update took too long and was stopped."));
        }
    });
}
