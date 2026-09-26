#include "toolstore.h"
#include "appinfo.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>

#include <memory>

namespace {

// How each tool is published. Both are GitHub releases with a checksum file
// beside the archive; they differ in what the archive holds.
struct Tool {
    QString name;          // also the folder it is unpacked into
    QString repo;
    QString asset;         // the release archive for this Mac
    QString checksums;     // the file that lists its SHA-256
    QString unpacked;      // the executable's name inside the archive
};

// yt-dlp's macOS build unpacked (an executable beside its _internal folder)
// rather than the single file, which unpacks itself to a temporary folder on
// every run and so starts seconds slower. Deno is one file.
Tool tool(const QString &name)
{
    if (name == QLatin1String("yt-dlp")) {
        return { name, QStringLiteral("yt-dlp/yt-dlp"), QStringLiteral("yt-dlp_macos.zip"),
                 QStringLiteral("SHA2-256SUMS"), QStringLiteral("yt-dlp_macos") };
    }
    const QString arch = QSysInfo::currentCpuArchitecture() == QLatin1String("arm64")
        ? QStringLiteral("aarch64") : QStringLiteral("x86_64");
    const QString asset = QStringLiteral("deno-%1-apple-darwin.zip").arg(arch);
    return { QStringLiteral("deno"), QStringLiteral("denoland/deno"), asset,
             asset + QStringLiteral(".sha256sum"), QStringLiteral("deno") };
}

const QStringList kTools = { QStringLiteral("yt-dlp"), QStringLiteral("deno") };
const QString kManifest = QStringLiteral("tools.json");
const QString kCheckedKey = QStringLiteral("checked");
// "At most once a day", allowing for a Mac opened at slightly different times.
constexpr qint64 kCheckIntervalSecs = 20 * 60 * 60;
constexpr int kUnpackTimeoutMs = 3 * 60 * 1000;

// The manifest is written by the startup unpacking and by an update, which
// runs its unpacking on a worker thread.
QMutex g_manifestLock;

QJsonObject readManifest(const QString &directory)
{
    QFile file(QDir(directory).filePath(kManifest));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeManifestValue(const QString &key, const QString &value)
{
    QMutexLocker lock(&g_manifestLock);
    QJsonObject manifest = readManifest(ToolStore::directory());
    manifest.insert(key, value);
    QFile file(QDir(ToolStore::directory()).filePath(kManifest));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(manifest).toJson());
}

QString executablePath(const QString &name)
{
    return QDir(ToolStore::directory()).filePath(name + QLatin1Char('/') + name);
}

// The version installed, or empty when there is none that can run.
QString installedVersion(const QString &name)
{
    if (!QFileInfo(executablePath(name)).isExecutable())
        return {};
    QMutexLocker lock(&g_manifestLock);
    return readManifest(ToolStore::directory()).value(name).toString();
}

// Unpacks `archive` into tools/<name>, replacing what is there only once the
// new copy is complete: a failed unpack leaves the old one working. A yt-dlp
// still running from the old copy keeps its files until it exits.
bool unpack(const QString &name, const QString &archive, const QString &version, QString *error)
{
    const QDir root(ToolStore::directory());
    root.mkpath(QStringLiteral("."));
    const QString staging = root.filePath(QStringLiteral(".staging-") + name);
    const QString retired = root.filePath(QStringLiteral(".old-") + name);
    const QString target = root.filePath(name);
    QDir(staging).removeRecursively();
    QDir(retired).removeRecursively();

    // ditto keeps what a zip made on a Mac carries: permissions, symlinks
    // and the signatures of the files inside.
    QProcess ditto;
    ditto.start(QStringLiteral("/usr/bin/ditto"), { QStringLiteral("-x"), QStringLiteral("-k"),
                                                   archive, staging });
    if (!ditto.waitForFinished(kUnpackTimeoutMs) || ditto.exitStatus() != QProcess::NormalExit
        || ditto.exitCode() != 0) {
        ditto.kill();
        *error = QStringLiteral("Could not unpack %1: %2")
                     .arg(name, QString::fromUtf8(ditto.readAllStandardError()).trimmed());
        QDir(staging).removeRecursively();
        return false;
    }

    const Tool spec = tool(name);
    const QString unpacked = QDir(staging).filePath(spec.unpacked);
    const QString executable = QDir(staging).filePath(name);
    if (spec.unpacked != name && QFileInfo::exists(unpacked) && !QFile::rename(unpacked, executable)) {
        *error = QStringLiteral("Could not name %1's executable.").arg(name);
        QDir(staging).removeRecursively();
        return false;
    }
    if (!QFileInfo(executable).isExecutable()) {
        *error = QStringLiteral("The %1 archive did not hold an executable.").arg(name);
        QDir(staging).removeRecursively();
        return false;
    }

    if (QFileInfo::exists(target) && !QDir().rename(target, retired)) {
        *error = QStringLiteral("Could not replace the %1 that is there.").arg(name);
        QDir(staging).removeRecursively();
        return false;
    }
    if (!QDir().rename(staging, target)) {
        QDir().rename(retired, target);   // put the old one back
        *error = QStringLiteral("Could not put the new %1 in place.").arg(name);
        return false;
    }
    QDir(retired).removeRecursively();
    writeManifestValue(name, version);
    return true;
}

// The first 64-hex-digit word on the line that names `asset`, or on the only
// line there is.
QString sha256From(const QByteArray &text, const QString &asset)
{
    static const QRegularExpression hash(QStringLiteral("\\b([0-9a-fA-F]{64})\\b"));
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (lines.size() == 1 || line.contains(asset)) {
            const QRegularExpressionMatch match = hash.match(line);
            if (match.hasMatch())
                return match.captured(1).toLower();
        }
    }
    return {};
}

QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    return request;
}

QUrl releaseUrl(const Tool &spec, const QString &version, const QString &file)
{
    // Deno tags are "v2.9.7"; yt-dlp's are the version itself.
    const QString tag = spec.name == QLatin1String("deno") ? QLatin1Char('v') + version : version;
    return QUrl(QStringLiteral("https://github.com/%1/releases/download/%2/%3").arg(spec.repo, tag, file));
}

} // namespace

ToolStore::ToolStore(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
}

QString ToolStore::directory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("tools"));
}

QStringList ToolStore::executableDirectories()
{
    QStringList directories;
    for (const QString &name : kTools)
        directories << QDir(directory()).filePath(name);
    return directories;
}

void ToolStore::installBundled()
{
    const QDir bundled(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("../Resources/tools")));
    const QJsonObject carried = readManifest(bundled.path());
    for (const QString &name : kTools) {
        const QString version = carried.value(name).toString();
        const QString archive = bundled.filePath(name + QStringLiteral(".zip"));
        if (version.isEmpty() || !QFileInfo::exists(archive))
            continue;   // a build without them (Xcode, --no-deploy): PATH it is
        const QString installed = installedVersion(name);
        if (!installed.isEmpty() && AppInfo::compareVersions(installed, version) >= 0)
            continue;
        QString error;
        if (unpack(name, archive, version, &error))
            qWarning("Monolist: unpacked %s %s", qPrintable(name), qPrintable(version));
        else
            qWarning("Monolist: %s", qPrintable(error));
    }
}

void ToolStore::update(const QStringList &names)
{
    if (m_busy)
        return;
    m_busy = true;
    m_queue = names.isEmpty() ? kTools : names;
    m_updated.clear();
    next();
}

void ToolStore::checkDaily()
{
    if (m_busy)
        return;
    QString last;
    {
        QMutexLocker lock(&g_manifestLock);
        last = readManifest(directory()).value(kCheckedKey).toString();
    }
    const QDateTime checked = QDateTime::fromString(last, Qt::ISODate);
    if (checked.isValid() && checked.secsTo(QDateTime::currentDateTimeUtc()) < kCheckIntervalSecs)
        return;
    QDir().mkpath(directory());
    writeManifestValue(kCheckedKey, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    // Deno is only announced: it rarely matters, and it is large.
    latestVersion(QStringLiteral("deno"), [this](const QString &version, const QString &) {
        const QString installed = installedVersion(QStringLiteral("deno"));
        if (!version.isEmpty() && !installed.isEmpty()
            && AppInfo::compareVersions(installed, version) < 0) {
            Q_EMIT notice(QStringLiteral("Deno %1 is out: Settings › Update components fetches it.")
                              .arg(version));
        }
    });
    m_announce = true;
    update({ QStringLiteral("yt-dlp") });
}

void ToolStore::next()
{
    if (m_queue.isEmpty()) {
        m_busy = false;
        const QString message = m_updated.isEmpty()
            ? QStringLiteral("The tools are up to date.")
            : QStringLiteral("Updated %1.").arg(m_updated.join(QStringLiteral(" and ")));
        if (m_announce && !m_updated.isEmpty())
            Q_EMIT notice(message);
        m_announce = false;
        Q_EMIT finished(true, message);
        return;
    }

    m_current = m_queue.takeFirst();
    Q_EMIT progress(QStringLiteral("Looking for a newer %1…").arg(m_current));
    latestVersion(m_current, [this](const QString &version, const QString &error) {
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        const QString installed = installedVersion(m_current);
        if (!installed.isEmpty() && AppInfo::compareVersions(installed, version) >= 0) {
            Q_EMIT progress(QStringLiteral("%1 %2 is the newest.").arg(m_current, installed));
            next();
            return;
        }
        fetchChecksum(version);
    });
}

void ToolStore::fail(const QString &message)
{
    if (m_download) {
        m_download->remove();
        delete m_download;
        m_download = nullptr;
    }
    m_queue.clear();
    m_busy = false;
    m_announce = false;
    Q_EMIT finished(false, message);
}

// GitHub answers the "latest" download link with a redirect to the same file
// under the release's own tag, which is the version.
void ToolStore::latestVersion(const QString &name,
                              const std::function<void(const QString &, const QString &)> &done)
{
    const Tool spec = tool(name);
    QNetworkRequest head = request(QUrl(QStringLiteral("https://github.com/%1/releases/latest/download/%2")
                                            .arg(spec.repo, spec.checksums)));
    head.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_network->head(head);
    connect(reply, &QNetworkReply::finished, this, [reply, name, done]() {
        reply->deleteLater();
        const QString target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl().toString();
        static const QRegularExpression tag(QStringLiteral("/releases/download/v?([^/]+)/"));
        const QRegularExpressionMatch match = tag.match(target);
        if (!match.hasMatch()) {
            done({}, QStringLiteral("Could not find the latest %1: %2")
                         .arg(name, reply->error() != QNetworkReply::NoError
                                        ? reply->errorString()
                                        : QStringLiteral("GitHub did not say.")));
            return;
        }
        done(match.captured(1), {});
    });
}

void ToolStore::fetchChecksum(const QString &version)
{
    const Tool spec = tool(m_current);
    QNetworkReply *reply = m_network->get(request(releaseUrl(spec, version, spec.checksums)));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, spec]() {
        reply->deleteLater();
        const QString sha256 = reply->error() == QNetworkReply::NoError
            ? sha256From(reply->readAll(), spec.asset) : QString();
        if (sha256.isEmpty()) {
            fail(QStringLiteral("Could not read the checksum of %1 %2.").arg(m_current, version));
            return;
        }
        fetchArchive(version, sha256);
    });
}

void ToolStore::fetchArchive(const QString &version, const QString &sha256)
{
    const Tool spec = tool(m_current);
    QDir().mkpath(directory());
    // Written straight to disk as it arrives: yt-dlp's archive is ~40 MB.
    m_download = new QFile(QDir(directory()).filePath(QStringLiteral(".download-") + spec.asset), this);
    if (!m_download->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Could not write to %1.").arg(directory()));
        return;
    }

    Q_EMIT progress(QStringLiteral("Downloading %1 %2…").arg(m_current, version));
    QNetworkRequest get = request(releaseUrl(spec, version, spec.asset));
    get.setTransferTimeout(120000);
    QNetworkReply *reply = m_network->get(get);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_download)
            m_download->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, version, sha256]() {
        reply->deleteLater();
        if (!m_download)
            return;
        m_download->write(reply->readAll());
        m_download->close();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("Could not download %1: %2").arg(m_current, reply->errorString()));
            return;
        }
        const QString archive = m_download->fileName();
        delete m_download;
        m_download = nullptr;
        install(version, archive, sha256);
    });
}

void ToolStore::install(const QString &version, const QString &archive, const QString &sha256)
{
    QFile file(archive);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)
        || QString::fromLatin1(hash.result().toHex()) != sha256) {
        QFile::remove(archive);
        fail(QStringLiteral("%1 %2 did not match its published checksum, so it was not installed.")
                 .arg(m_current, version));
        return;
    }
    file.close();

    Q_EMIT progress(QStringLiteral("Installing %1 %2…").arg(m_current, version));
    // Unpacking yt-dlp takes a moment; not on the thread that draws.
    auto error = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    const QString name = m_current;
    QThread *worker = QThread::create([name, archive, version, error, ok]() {
        *ok = unpack(name, archive, version, error.get());
        QFile::remove(archive);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, name, version, error, ok]() {
        if (!*ok) {
            fail(*error);
            return;
        }
        m_updated << QStringLiteral("%1 %2").arg(name, version);
        next();
    });
    worker->start();
}
