#include "appupdater.h"

#include "appinfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

namespace {

// A checksum file is one line; anything much longer is not one.
constexpr qint64 kChecksumLimit = 4096;
constexpr int kChecksumTimeoutMs = 30000;
// Without a byte for this long, a download is given up (Qt's transfer
// timeout counts from the last byte, not from the start).
constexpr int kStallTimeoutMs = 60000;
constexpr int kMaxRedirects = 5;

const QString kPending = QStringLiteral("pending.json");
const QString kResult = QStringLiteral("result.txt");
const QString kPart = QStringLiteral(".part");
const QString kStaged = QStringLiteral("staged");

// The files a release publishes, and nothing else: the name becomes a file
// name here, so one with a folder in it, or anything unexpected, is never
// taken. Monolist-<version>-<what>.
const QRegularExpression &assetPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        "^Monolist-([0-9]+(?:\\.[0-9]+)+)-(?:setup-win64\\.exe|win64\\.zip|macos-[a-z0-9_]+(?:-unsigned)?\\.dmg)$"));
    return pattern;
}

QString megabytes(qint64 bytes)
{
    return QString::number(qMax<qint64>(1, (bytes + 512 * 1024) / (1024 * 1024)));
}

} // namespace

// ------------------------------------------------------------------ rules

UpdateRelease UpdateRules::parseRelease(const QByteArray &json)
{
    UpdateRelease release;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QString tag = root.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.startsWith(QLatin1Char('v')) || tag.startsWith(QLatin1Char('V')))
        tag.remove(0, 1);
    release.version = tag;
    release.page = QUrl(root.value(QStringLiteral("html_url")).toString());
    for (const QJsonValue &value : root.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject object = value.toObject();
        UpdateAsset asset;
        asset.name = object.value(QStringLiteral("name")).toString();
        asset.url = QUrl(object.value(QStringLiteral("browser_download_url")).toString());
        asset.size = qint64(object.value(QStringLiteral("size")).toDouble());
        if (!asset.name.isEmpty() && asset.url.isValid())
            release.assets.append(asset);
    }
    return release;
}

QString UpdateRules::versionInName(const QString &fileName)
{
    QString name = fileName;
    if (name.endsWith(QStringLiteral(".sha256")))
        name.chop(7);
    if (name.endsWith(kPart))
        name.chop(kPart.size());
    const QRegularExpressionMatch match = assetPattern().match(name);
    return match.hasMatch() ? match.captured(1) : QString();
}

UpdateRules::Choice UpdateRules::choose(const UpdateRelease &release, const UpdateTarget &target,
                                        const QString &arch)
{
    Choice choice;
    // In order of preference.
    QStringList wanted;
    switch (target.kind) {
    case UpdateTarget::MacBundle: {
        const QString image = QStringLiteral("-macos-") + arch;
        // The same kind as this copy: Developer ID for a copy signed so; ad
        // hoc for one signed ad hoc, or, from a release that has no ad hoc
        // image (one built without the signing secrets), its only image.
        if (target.developerId)
            wanted << image + QStringLiteral(".dmg");
        else
            wanted << image + QStringLiteral("-unsigned.dmg") << image + QStringLiteral(".dmg");
        break;
    }
    case UpdateTarget::WindowsSetup:
        wanted << QStringLiteral("-setup-win64.exe");
        break;
    case UpdateTarget::WindowsPortable:
        wanted << QStringLiteral("-win64.zip");
        break;
    case UpdateTarget::Unsupported:
        choice.missing = QStringLiteral("This copy of Monolist cannot update itself.");
        return choice;
    }

    for (const QString &end : std::as_const(wanted)) {
        for (const UpdateAsset &asset : release.assets) {
            if (asset.name.endsWith(end) && versionInName(asset.name) == release.version) {
                choice.file = asset;
                break;
            }
        }
        if (!choice.file.isNull())
            break;
    }
    if (choice.file.isNull()) {
        choice.missing = QStringLiteral("Monolist %1 has no download for this copy.").arg(release.version);
        return choice;
    }
    for (const UpdateAsset &asset : release.assets) {
        if (asset.name == choice.file.name + QStringLiteral(".sha256"))
            choice.checksum = asset;
    }
    if (choice.checksum.isNull()) {
        choice.missing = QStringLiteral("Monolist %1 publishes no checksum for %2, so it is not downloaded.")
                             .arg(release.version, choice.file.name);
        choice.file = UpdateAsset();
    }
    return choice;
}

bool UpdateRules::isLoopback(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (host == QLatin1String("localhost"))
        return true;
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

bool UpdateRules::allowedSource(const QUrl &url, const QUrl &feed)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    // A test's stand-in on this computer, and only for a feed that is one.
    if (isLoopback(feed) && isLoopback(url))
        return scheme == QLatin1String("http") || scheme == QLatin1String("https");
    if (scheme != QLatin1String("https") || (url.port() != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com") || host.endsWith(QLatin1String(".githubusercontent.com"));
}

QString UpdateRules::sha256From(const QByteArray &text, const QString &name)
{
    static const QRegularExpression hash(QStringLiteral("(?:^|[^0-9a-fA-F])([0-9a-fA-F]{64})(?![0-9a-fA-F])"));
    const QStringList lines = QString::fromUtf8(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (lines.size() != 1 && !line.contains(name))
            continue;
        const QRegularExpressionMatch match = hash.match(line);
        if (match.hasMatch())
            return match.captured(1).toLower();
    }
    return {};
}

// ----------------------------------------------------- Windows' rules

bool WinUpdate::sameFolder(const QString &a, const QString &b)
{
    const auto normal = [](const QString &path) {
        QString clean = QDir::cleanPath(path.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/')));
        while (clean.size() > 3 && clean.endsWith(QLatin1Char('/')))
            clean.chop(1);
        return clean.toLower();
    };
    return !a.trimmed().isEmpty() && !b.trimmed().isEmpty() && normal(a) == normal(b);
}

UpdateTarget WinUpdate::describeFolder(const QString &folder, const QString &currentUserLocation,
                                       const QString &allUsersLocation)
{
    UpdateTarget target;
    target.path = QDir::cleanPath(QString(folder).replace(QLatin1Char('\\'), QLatin1Char('/')));
    if (sameFolder(currentUserLocation, folder) || sameFolder(allUsersLocation, folder)) {
        target.kind = UpdateTarget::WindowsSetup;
        target.allUsers = !sameFolder(currentUserLocation, folder);
        return target;
    }
    const QDir dir(target.path);
    // A development build runs from its build folder: never swapped for a
    // package, which would take the build tree with it.
    if (dir.exists(QStringLiteral("CMakeCache.txt"))
        || !dir.exists(QStringLiteral("LICENSES/Monolist-MIT.txt")))
        return target;
    target.kind = UpdateTarget::WindowsPortable;
    const auto canWriteIn = [](const QString &path) {
        QFile probe(QDir(path).filePath(QStringLiteral(".monolist-update-probe")));
        const bool ok = probe.open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (ok) {
            probe.close();
            probe.remove();
        }
        return ok;
    };
    if (!canWriteIn(target.path) || !canWriteIn(QFileInfo(target.path).absolutePath()))
        target.blocker = QStringLiteral("Monolist may not change the folder it is in");
    return target;
}

// ---------------------------------------------------------------- updater

AppUpdater::AppUpdater(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
}

AppUpdater::~AppUpdater()
{
    // A quit mid-download leaves no half file behind; a quit after RESTART
    // leaves the staged copy for the helper (stopPlatform knows).
    ++m_round;
    stopNetwork();
    stopPlatform();
    if (m_worker)
        m_worker->wait();
}

QString AppUpdater::directory()
{
    // A scratch library keeps its updates beside it, so that a test never
    // touches the real cache.
    const QString scratch = qEnvironmentVariable("MONOLIST_DATA_DIR");
    const QString base = scratch.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : scratch;
    return QDir(base).filePath(QStringLiteral("updates"));
}

QString AppUpdater::runningBuild()
{
    return AppInfo::currentBuild();
}

QString AppUpdater::runningVersion()
{
    return AppInfo::currentRelease();
}

void AppUpdater::setPhase(Phase phase, const QString &message)
{
    m_phase = phase;
    m_message = message;
    Q_EMIT changed();
}

void AppUpdater::fail(const QString &message)
{
    qWarning("update: %s", qPrintable(message));
    // Whatever is still out answers to nobody now.
    ++m_round;
    stopNetwork();
    stopPlatform();
    setPhase(Failed, message);
}

UpdateTarget AppUpdater::target()
{
    if (!m_targetKnown) {
        m_target = describeThisCopy();
        m_targetKnown = true;
    }
    return m_target;
}

bool AppUpdater::canUpdate(const UpdateRelease &release, QString *why)
{
    const UpdateRules::Choice choice = UpdateRules::choose(release, target(), QSysInfo::currentCpuArchitecture());
    if (why)
        *why = choice.missing;
    return !choice.file.isNull();
}

void AppUpdater::start(const UpdateRelease &release, const QUrl &feed)
{
    cancel();
    m_handedOver = false;
    m_release = release;
    m_feed = feed;
    m_percent = -1;
    m_file.clear();
    m_staged.clear();
    m_choice = UpdateRules::choose(release, target(), QSysInfo::currentCpuArchitecture());
    if (m_choice.file.isNull()) {
        fail(m_choice.missing);
        return;
    }
    for (const UpdateAsset &asset : { m_choice.file, m_choice.checksum }) {
        if (!UpdateRules::allowedSource(asset.url, m_feed)) {
            fail(QStringLiteral("The link to %1 points somewhere other than GitHub (%2), so it was not followed.")
                     .arg(asset.name, asset.url.host().isEmpty() ? asset.url.toString() : asset.url.host()));
            return;
        }
    }
    if (!QDir().mkpath(workDirectory())) {
        fail(QStringLiteral("Monolist could not make a folder for the update (%1).")
                 .arg(QDir::toNativeSeparators(workDirectory())));
        return;
    }

    // Room for the download and what is made of it: the app copied out of a
    // disk image, or the zip unpacked beside it.
    const qint64 size = m_choice.file.size;
    if (size > 0) {
        const double factor = m_target.kind == UpdateTarget::WindowsSetup ? 1.1 : 2.6;
        const qint64 needed = qint64(double(size) * factor) + 64 * 1024 * 1024;
        const QStorageInfo storage(workDirectory());
        if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < needed) {
            fail(QStringLiteral("There is not enough free space for the update: it needs about %1 MB, and %2 "
                                "has %3 MB free.")
                     .arg(megabytes(needed), storage.displayName(), megabytes(storage.bytesAvailable())));
            return;
        }
    }

    setPhase(Downloading, QStringLiteral("Downloading Monolist %1…").arg(m_release.version));
    fetchChecksum();
}

void AppUpdater::cancel()
{
    ++m_round;
    const bool busy = m_phase == Downloading || m_phase == Preparing || m_phase == Ready;
    stopNetwork();
    stopPlatform();
    if (busy)
        setPhase(Idle, QString());
}

void AppUpdater::stopNetwork()
{
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        reply->abort();
    }
    if (m_part) {
        m_part->close();
        m_part->remove();
        delete m_part;
        m_part = nullptr;
    }
    m_hash.reset();
}

QNetworkReply *AppUpdater::get(const QUrl &url, qint64 timeoutMs)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist/%1").arg(runningVersion()));
    // Every hop checked against the same rule as the first (allowedSource):
    // GitHub sends a release's files on to its download hosts.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(kMaxRedirects);
    request.setTransferTimeout(int(timeoutMs));
    // GitHub's downloads need no cookie, so none goes, and none is kept.
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    QNetworkReply *reply = m_network->get(request);
    const int round = m_round;
    connect(reply, &QNetworkReply::redirected, this, [this, reply, round](const QUrl &to) {
        if (round == m_round && UpdateRules::allowedSource(to, m_feed)) {
            Q_EMIT reply->redirectAllowed();
            return;
        }
        reply->setProperty("refusedHost", to.host().isEmpty() ? to.toString() : to.host());
        reply->abort();
    });
    return reply;
}

void AppUpdater::fetchChecksum()
{
    QNetworkReply *reply = get(m_choice.checksum.url, kChecksumTimeoutMs);
    m_reply = reply;
    const int round = m_round;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64) {
        if (received > kChecksumLimit)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, round]() {
        reply->deleteLater();
        if (round != m_round)
            return;
        m_reply = nullptr;
        const QString refused = reply->property("refusedHost").toString();
        if (!refused.isEmpty()) {
            fail(QStringLiteral("The checksum of Monolist %1 was sent somewhere other than GitHub (%2), so it was "
                                "not fetched.").arg(m_release.version, refused));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("Could not fetch the checksum of Monolist %1: %2")
                     .arg(m_release.version, reply->errorString()));
            return;
        }
        const QString sha256 = UpdateRules::sha256From(reply->read(kChecksumLimit), m_choice.file.name);
        if (sha256.isEmpty()) {
            fail(QStringLiteral("The checksum published for %1 could not be read, so it was not downloaded.")
                     .arg(m_choice.file.name));
            return;
        }
        checkExisting(sha256);
    });
}

// A file of this release downloaded before (a cancel after it arrived, an
// update that did not go in) is used again when it still matches.
void AppUpdater::checkExisting(const QString &sha256)
{
    const QString path = QDir(workDirectory()).filePath(m_choice.file.name);
    if (!QFileInfo::exists(path)) {
        download(sha256);
        return;
    }
    setPhase(Downloading, QStringLiteral("Checking the Monolist %1 downloaded before…").arg(m_release.version));
    auto digest = std::make_shared<QString>();
    QThread *worker = QThread::create([path, digest]() {
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (file.open(QIODevice::ReadOnly) && hash.addData(&file))
            *digest = QString::fromLatin1(hash.result().toHex());
    });
    m_worker = worker;
    const int round = m_round;
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, round, path, digest, sha256]() {
        if (round != m_round)
            return;
        if (*digest == sha256) {
            qInfo("update: %s was downloaded before and still matches", qPrintable(m_choice.file.name));
            m_file = path;
            downloaded();
            return;
        }
        QFile::remove(path);
        download(sha256);
    });
    worker->start();
}

void AppUpdater::download(const QString &sha256)
{
    const QString path = QDir(workDirectory()).filePath(m_choice.file.name);
    QFile::remove(path + kPart);
    m_part = new QFile(path + kPart, this);
    if (!m_part->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString error = m_part->errorString();
        delete m_part;
        m_part = nullptr;
        fail(QStringLiteral("Could not save the update in %1: %2")
                 .arg(QDir::toNativeSeparators(workDirectory()), error));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_expectedSha = sha256;
    m_received = 0;
    m_percent = 0;
    setPhase(Downloading, QStringLiteral("Downloading Monolist %1… 0 %").arg(m_release.version));

    QNetworkReply *reply = get(m_choice.file.url, kStallTimeoutMs);
    m_reply = reply;
    const int round = m_round;
    // Straight to disk as it arrives, hashed on the way: these files are
    // 150 to 280 MB, and none of it is held in memory.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, round]() {
        if (round != m_round || !m_part)
            return;
        const QByteArray chunk = reply->readAll();
        if (m_part->write(chunk) != chunk.size()) {
            reply->setProperty("writeError", m_part->errorString());
            reply->abort();
            return;
        }
        m_hash->addData(chunk);
        m_received += chunk.size();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, round](qint64 received, qint64 total) {
        if (round != m_round)
            return;
        if (total <= 0)
            total = m_choice.file.size;
        const int percent = total > 0 ? int(qBound(qint64(0), received * 100 / total, qint64(100))) : -1;
        if (percent == m_percent)
            return;
        m_percent = percent;
        setPhase(Downloading, percent >= 0
                                  ? QStringLiteral("Downloading Monolist %1… %2 %").arg(m_release.version).arg(percent)
                                  : QStringLiteral("Downloading Monolist %1… %2 MB")
                                        .arg(m_release.version, megabytes(received)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, round]() {
        reply->deleteLater();
        if (round != m_round || !m_part)
            return;
        m_reply = nullptr;

        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty() && m_part->write(rest) == rest.size())
            m_hash->addData(rest);
        m_part->close();
        const QString part = m_part->fileName();
        delete m_part;
        m_part = nullptr;
        const QString got = QString::fromLatin1(m_hash->result().toHex());
        m_hash.reset();

        const QString refused = reply->property("refusedHost").toString();
        const QString writeError = reply->property("writeError").toString();
        QString problem;
        if (!refused.isEmpty()) {
            problem = QStringLiteral("The download was sent somewhere other than GitHub (%1), so it was stopped.")
                          .arg(refused);
        } else if (!writeError.isEmpty()) {
            problem = QStringLiteral("Could not save the update: %1").arg(writeError);
        } else if (reply->error() != QNetworkReply::NoError) {
            problem = QStringLiteral("Could not download Monolist %1: %2").arg(m_release.version, reply->errorString());
        } else if (got != m_expectedSha) {
            problem = QStringLiteral("The download did not match its published checksum, so it was deleted.");
        }
        const QString path = part.chopped(kPart.size());
        if (problem.isEmpty()) {
            QFile::remove(path);
            if (!QFile::rename(part, path))
                problem = QStringLiteral("Could not save the update in %1.").arg(QDir::toNativeSeparators(path));
        }
        if (!problem.isEmpty()) {
            QFile::remove(part);
            fail(problem);
            return;
        }
        qInfo("update: %s downloaded, and matches its published SHA-256", qPrintable(m_choice.file.name));
        m_file = path;
        downloaded();
    });
}

void AppUpdater::downloaded()
{
    m_percent = 100;
    stage();
}

void AppUpdater::stage()
{
    switch (m_target.kind) {
    case UpdateTarget::WindowsSetup:
        // The setup program is what puts it in place; there is nothing to
        // unpack. (Not code-signed: HTTPS from GitHub and the SHA-256 it
        // just matched are what vouch for it.)
        m_staged = m_file;
        ready();
        return;
    case UpdateTarget::MacBundle:
    case UpdateTarget::WindowsPortable:
        if (!m_target.blocker.isEmpty()) {
            setPhase(Manual, QStringLiteral("Monolist %1 is downloaded, but this copy cannot be replaced where it "
                                            "is: %2. %3")
                                 .arg(m_release.version, m_target.blocker,
                                      m_target.kind == UpdateTarget::MacBundle
                                          ? QStringLiteral("Drag Monolist from the window that opened to "
                                                           "Applications, then open it from there.")
                                          : QStringLiteral("Unzip it over this copy once Monolist has quit.")));
            return;
        }
        stagePlatform();
        return;
    case UpdateTarget::Unsupported:
        break;
    }
    fail(QStringLiteral("This copy of Monolist cannot update itself."));
}

void AppUpdater::ready()
{
    qInfo("update: Monolist %s is ready", qPrintable(m_release.version));
    setPhase(Ready, QStringLiteral("Monolist %1 is ready: Monolist restarts to finish.").arg(m_release.version));
}

void AppUpdater::run(const QString &program, const QStringList &arguments, int timeoutMs,
                     const std::function<void(int, const QByteArray &, const QByteArray &)> &done)
{
    auto *process = new QProcess(this);
    m_process = process;
    const int round = m_round;
    auto settled = std::make_shared<bool>(false);
    const auto settle = [this, process, round, done, settled](int code) {
        if (*settled)
            return;
        *settled = true;
        process->deleteLater();
        if (m_process == process)
            m_process = nullptr;
        if (round != m_round)
            return;
        done(code, process->readAllStandardOutput(), process->readAllStandardError());
    };
    connect(process, &QProcess::finished, this, [settle](int code, QProcess::ExitStatus status) {
        settle(status == QProcess::NormalExit ? code : -1);
    });
    connect(process, &QProcess::errorOccurred, this, [settle](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            settle(-1);
    });
    QTimer::singleShot(timeoutMs, process, [process]() {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    process->start(program, arguments);
}

bool AppUpdater::writePending(QString *error) const
{
    QFile file(QDir(workDirectory()).filePath(kPending));
    const QJsonObject pending{
        { QStringLiteral("from"), runningVersion() },
        { QStringLiteral("to"), m_release.version },
        { QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
    };
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(QJsonDocument(pending).toJson()) < 0) {
        *error = QStringLiteral("Monolist could not write in %1.").arg(QDir::toNativeSeparators(workDirectory()));
        return false;
    }
    return true;
}

bool AppUpdater::handOver(QString *error)
{
    if (m_phase != Ready || m_staged.isEmpty() || !QFileInfo::exists(m_staged)) {
        *error = QStringLiteral("there is no update ready to put in place");
        return false;
    }
    QFile::remove(QDir(workDirectory()).filePath(kResult));
    if (!writePending(error))
        return false;
    if (!launchHelper(error)) {
        QFile::remove(QDir(workDirectory()).filePath(kPending));
        return false;
    }
    qInfo("update: handed over to the helper, for Monolist %s", qPrintable(m_release.version));
    // From here the staged copy is the helper's: no cancel or quit may
    // remove it.
    m_handedOver = true;
    return true;
}

QString AppUpdater::housekeeping(bool *ok)
{
    if (ok)
        *ok = true;
    const QDir dir(workDirectory());
    if (!dir.exists())
        return {};

    QString said;
    QFile pendingFile(dir.filePath(kPending));
    if (pendingFile.open(QIODevice::ReadOnly)) {
        const QJsonObject pending = QJsonDocument::fromJson(pendingFile.read(64 * 1024)).object();
        pendingFile.close();
        QString result;
        QFile resultFile(dir.filePath(kResult));
        if (resultFile.open(QIODevice::ReadOnly))
            result = QString::fromUtf8(resultFile.read(64 * 1024)).section(QLatin1Char('\n'), 0, 0).trimmed();
        const QString to = pending.value(QStringLiteral("to")).toString();
        const QString running = runningVersion();
        if (!to.isEmpty() && AppInfo::compareVersions(running, to) >= 0) {
            said = QStringLiteral("Monolist is now %1.").arg(running);
            qInfo("update: now running %s (was %s)", qPrintable(running),
                  qPrintable(pending.value(QStringLiteral("from")).toString()));
        } else {
            if (ok)
                *ok = false;
            QString reason = result.startsWith(QLatin1String("failed:"))
                                 ? result.mid(7).trimmed()
                                 : QStringLiteral("it did not finish");
            if (!reason.isEmpty())
                reason[0] = reason.at(0).toUpper();
            if (!reason.endsWith(QLatin1Char('.')))
                reason += QLatin1Char('.');
            said = QStringLiteral("The update to %1 was not put in place. %2 This is still %3.")
                       .arg(to, reason, running);
            qWarning("update: %s", qPrintable(said));
        }
    }
    QFile::remove(dir.filePath(kPending));
    QFile::remove(dir.filePath(kResult));
    QFile::remove(dir.filePath(kResult + QStringLiteral(".tmp")));
    if (!helperFileName().isEmpty())
        QFile::remove(dir.filePath(helperFileName()));

    // Half downloads, and the files of releases no newer than this one.
    const QString running = runningVersion();
    for (const QFileInfo &entry : dir.entryInfoList(QDir::Files | QDir::Hidden)) {
        const QString name = entry.fileName();
        const QString version = UpdateRules::versionInName(name);
        if (name.endsWith(kPart) || (!version.isEmpty() && AppInfo::compareVersions(version, running) <= 0))
            QFile::remove(entry.filePath());
    }
    // A staged copy no RESTART took: made again from its file when wanted.
    QDir(dir.filePath(kStaged)).removeRecursively();
    QDir(dir.filePath(QStringLiteral("mount"))).removeRecursively();
    return said;
}

// ------------------------------------------------- elsewhere (Linux, …)

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)

UpdateTarget AppUpdater::describeThisCopy()
{
    // Nothing is published for it: the release page, as before.
    return UpdateTarget();
}

QString AppUpdater::helperFileName()
{
    return {};
}

QByteArray AppUpdater::helperScript()
{
    return {};
}

void AppUpdater::stagePlatform()
{
    fail(QStringLiteral("This copy of Monolist cannot update itself."));
}

void AppUpdater::stopPlatform()
{
}

bool AppUpdater::launchHelper(QString *error)
{
    *error = QStringLiteral("this copy of Monolist cannot update itself");
    return false;
}

void AppUpdater::showDownloaded()
{
}

#endif
