#include "appupdater.h"

#include "appdatabase.h"
#include "appinfo.h"
#include "downloadmanager.h"

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
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <sys/statvfs.h>
#endif

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
const QString kStarted = QStringLiteral("started");
const QString kLast = QStringLiteral("last.txt");
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

// The nearest folder of `path` that exists.
QString existingFolder(const QString &path)
{
    QFileInfo info(path);
    QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    while (!QFileInfo(folder).exists() && QFileInfo(folder).absolutePath() != folder)
        folder = QFileInfo(folder).absolutePath();
    return folder;
}

// Free bytes for this user on the disk `path` is on; -1 when not known.
// statvfs rather than QStorageInfo on a Mac: that matches mount points by the
// path's text, and a path under one of macOS's firmlinks then reads as on
// the sealed system volume.
qint64 freeBytes(const QString &path)
{
    const QString folder = existingFolder(path);
#ifdef Q_OS_WIN
    const QStorageInfo storage(folder);
    return storage.isValid() ? storage.bytesAvailable() : -1;
#else
    struct statvfs volume;
    if (::statvfs(QFile::encodeName(folder).constData(), &volume) != 0)
        return -1;
    return qint64(volume.f_bavail) * qint64(volume.f_frsize);
#endif
}

bool onSameDisk(const QString &a, const QString &b)
{
    const QString first = existingFolder(a);
    const QString second = existingFolder(b);
#ifdef Q_OS_WIN
    return QStorageInfo(first).rootPath().compare(QStorageInfo(second).rootPath(), Qt::CaseInsensitive) == 0;
#else
    struct stat one;
    struct stat two;
    return ::stat(QFile::encodeName(first).constData(), &one) == 0
           && ::stat(QFile::encodeName(second).constData(), &two) == 0 && one.st_dev == two.st_dev;
#endif
}

QString kindName(UpdateTarget::Kind kind)
{
    switch (kind) {
    case UpdateTarget::MacBundle: return QStringLiteral("a Mac app");
    case UpdateTarget::WindowsSetup: return QStringLiteral("the setup's");
    case UpdateTarget::WindowsPortable: return QStringLiteral("portable");
    case UpdateTarget::Unsupported: break;
    }
    return QStringLiteral("one that cannot update itself");
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
    release.page = safePage(QUrl(root.value(QStringLiteral("html_url")).toString()));
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

QUrl UpdateRules::safePage(const QUrl &page)
{
    const QString prefix = QStringLiteral("/%1/").arg(QLatin1String(kRepository));
    if (page.isValid() && page.scheme() == QLatin1String("https") && page.userInfo().isEmpty()
        && page.host().compare(QLatin1String("github.com"), Qt::CaseInsensitive) == 0 && page.port() == -1
        && page.path().startsWith(prefix, Qt::CaseInsensitive))
        return page;
    return QUrl(QStringLiteral("https://github.com/%1/releases").arg(QLatin1String(kRepository)));
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

bool UpdateRules::allowedSource(const QUrl &url, const QUrl &feed, bool redirect)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    // A test's stand-in on this computer: only for a feed that is one, and
    // only for a scratch library.
    if (isLoopback(feed) && isLoopback(url) && !qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR"))
        return scheme == QLatin1String("http") || scheme == QLatin1String("https");
    if (scheme != QLatin1String("https") || (url.port() != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    if (host == QLatin1String("github.com")) {
        // The project's release files, and nobody else's.
        const QString path = QDir::cleanPath(url.path());
        return path.startsWith(QStringLiteral("/%1/releases/download/").arg(QLatin1String(kRepository)),
                               Qt::CaseInsensitive);
    }
    // Where GitHub keeps release files, as it redirects to them; never the
    // first link, which the feed gives on github.com.
    return redirect
           && (host == QLatin1String("objects.githubusercontent.com")
               || host == QLatin1String("release-assets.githubusercontent.com"));
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

QString UpdateRules::withoutQuery(const QString &text)
{
    static const QRegularExpression query(QStringLiteral("(\\b[a-zA-Z][a-zA-Z0-9+.-]*://[^\\s?#]*)[?#][^\\s]*"));
    QString clean = text;
    clean.replace(query, QStringLiteral("\\1"));
    return clean;
}

qint64 UpdateRules::sizeLimit(qint64 declared)
{
    // A little over what the feed says (it counts the same bytes, but a
    // little room costs nothing); 1 GB when it says nothing, five times the
    // largest release so far.
    return declared > 0 ? declared + 1024 * 1024 : qint64(1024) * 1024 * 1024;
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

bool WinUpdate::isWithin(const QString &inner, const QString &folder)
{
    if (inner.trimmed().isEmpty() || folder.trimmed().isEmpty())
        return false;
    if (sameFolder(inner, folder))
        return true;
    const auto normal = [](const QString &path) {
        QString clean = QDir::cleanPath(QString(path).trimmed().replace(QLatin1Char('\\'), QLatin1Char('/')));
        if (!clean.endsWith(QLatin1Char('/')))
            clean += QLatin1Char('/');
        return clean;
    };
    return normal(inner).startsWith(normal(folder), Qt::CaseInsensitive);
}

UpdateTarget WinUpdate::describeFolder(const QString &folder, const QString &currentUserLocation,
                                       const QString &allUsersLocation, const QStringList &userFolders)
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
    // package, which would take the build tree with it. And a folder the
    // setup made that no uninstall key names (a junction, a subst drive, a
    // copy of one) is never taken for a portable one either: swapping it
    // would leave its uninstaller behind.
    if (dir.exists(QStringLiteral("CMakeCache.txt")) || dir.exists(QStringLiteral("unins000.exe"))
        || !dir.exists(QStringLiteral("LICENSES/Monolist-MIT.txt")))
        return target;
    target.kind = UpdateTarget::WindowsPortable;
    for (const QString &mine : userFolders) {
        if (isWithin(mine, target.path)) {
            target.blocker = QStringLiteral("its folder holds your library or your downloads");
            return target;
        }
    }
    // A file made and removed in the folder itself; a folder in the one
    // around it, where the new copy is unpacked and the old one put aside
    // (at the root of a drive, a standard user may make folders but not
    // files).
    QFile probe(dir.filePath(QStringLiteral(".monolist-update-probe")));
    bool writable = probe.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (writable) {
        probe.close();
        probe.remove();
    }
    const QString around = QFileInfo(target.path).absolutePath();
    const QString probeFolder = QDir(around).filePath(QStringLiteral(".monolist-update-probe"));
    if (writable) {
        writable = QDir().mkdir(probeFolder);
        if (writable)
            QDir().rmdir(probeFolder);
    }
    if (!writable)
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

QStringList AppUpdater::userFolders()
{
    QStringList folders;
    folders << QFileInfo(AppDatabase::databaseFilePath()).absolutePath()
            << DownloadManager::currentDirectory() << directory()
            << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            << QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString tools = qEnvironmentVariable("MONOLIST_TOOLS_DIR");
    if (!tools.isEmpty())
        folders << tools;
    folders.removeAll(QString());
    return folders;
}

void AppUpdater::noteStarted()
{
    const QDir dir(directory());
    if (!dir.exists(kPending))
        return;
    QSaveFile file(dir.filePath(kStarted));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QStringLiteral("%1 %2\n").arg(QCoreApplication::applicationPid()).arg(runningVersion()).toUtf8());
        file.commit();
    }
}

void AppUpdater::setPhase(Phase phase, const QString &message)
{
    m_phase = phase;
    m_message = message;
    Q_EMIT changed();
}

void AppUpdater::fail(const QString &message, const QString &detail)
{
    if (detail.isEmpty())
        qWarning("update: %s", qPrintable(message));
    else
        qWarning("update: %s (%s)", qPrintable(message), qPrintable(UpdateRules::withoutQuery(detail)));
    // Whatever is still out answers to nobody now.
    ++m_round;
    stopNetwork();
    stopPlatform();
    setPhase(Failed, message);
}

void AppUpdater::reject(const QString &message)
{
    // Not to be offered again by TRY AGAIN, nor kept: a newer release, or a
    // corrected one, is downloaded afresh.
    const QString file = m_file;
    fail(message);
    if (!file.isEmpty()) {
        QFile::remove(file);
        if (m_file == file)
            m_file.clear();
    }
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
    // After RESTART the update is the helper's alone.
    if (m_handedOver)
        return;
    cancel();
    m_release = release;
    m_feed = feed;
    m_percent = -1;
    m_file.clear();
    m_staged.clear();
    const UpdateTarget copy = target();
    qInfo("update: this copy is %s (%s)%s", qPrintable(kindName(copy.kind)),
          qPrintable(QDir::toNativeSeparators(copy.path)),
          copy.blocker.isEmpty() ? "" : qPrintable(QStringLiteral(", to be replaced by hand: ") + copy.blocker));
    m_choice = UpdateRules::choose(release, copy, QSysInfo::currentCpuArchitecture());
    if (m_choice.file.isNull()) {
        fail(m_choice.missing);
        return;
    }
    for (const UpdateAsset &asset : { m_choice.file, m_choice.checksum }) {
        if (!UpdateRules::allowedSource(asset.url, m_feed)) {
            fail(QStringLiteral("The link to %1 points somewhere other than Monolist's releases on GitHub (%2), "
                                "so it was not followed.")
                     .arg(asset.name, asset.url.host().isEmpty() ? QStringLiteral("no host") : asset.url.host()),
                 asset.url.toString());
            return;
        }
    }
    if (!QDir().mkpath(workDirectory())) {
        fail(QStringLiteral("Monolist could not make a folder for the update."), workDirectory());
        return;
    }
    if (!enoughSpace())
        return;

    setPhase(Downloading, QStringLiteral("Downloading Monolist %1…").arg(m_release.version));
    fetchChecksum();
}

// Room for the download and what is made of it: the app copied out of a
// disk image (beside the app on a Mac, unless it is put in place by hand),
// the zip unpacked beside a portable copy. A guess from the download's size,
// generous: what is made of it is about three times as big.
bool AppUpdater::enoughSpace()
{
    const qint64 size = m_choice.file.size;
    if (size <= 0)
        return true;
    const qint64 margin = 64 * 1024 * 1024;
    const qint64 unpacked = size * 7 / 2;
    struct Need {
        QString where;
        qint64 bytes;
        QString said;
    };
    QList<Need> needs;
    needs.append({ workDirectory(), size + margin, QStringLiteral("the disk the update is saved on") });
    const bool unpackedBeside = m_target.kind == UpdateTarget::WindowsPortable
                                || (m_target.kind == UpdateTarget::MacBundle && m_target.blocker.isEmpty());
    if (m_target.kind == UpdateTarget::MacBundle && !m_target.blocker.isEmpty())
        needs.first().bytes += unpacked;
    if (unpackedBeside) {
        const QString beside = QFileInfo(m_target.path).absolutePath();
        if (onSameDisk(beside, workDirectory()))
            needs.first().bytes += unpacked;
        else
            needs.append({ beside, unpacked + margin, QStringLiteral("the disk Monolist is on") });
    }
    for (const Need &need : std::as_const(needs)) {
        const qint64 free = freeBytes(need.where);
        if (free >= 0 && free < need.bytes) {
            fail(QStringLiteral("There is not enough free space for the update: it needs about %1 MB on %2, "
                                "which has %3 MB free.")
                     .arg(megabytes(need.bytes), need.said, megabytes(free)),
                 need.where);
            return false;
        }
    }
    return true;
}

void AppUpdater::cancel()
{
    // After RESTART there is nothing here to cancel: the helper has it.
    if (m_handedOver)
        return;
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

QString AppUpdater::networkProblem(QNetworkReply *reply, const QString &what) const
{
    qWarning("update: %s: %s", qPrintable(what), qPrintable(UpdateRules::withoutQuery(reply->errorString())));
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::TimeoutError:
        return QStringLiteral("Monolist could not reach GitHub for %1. Check the connection, then try again.")
            .arg(what);
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("The download of %1 stopped arriving, so it was given up.").arg(what);
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("The connection closed before %1 had all arrived.").arg(what);
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("The connection to GitHub for %1 could not be made secure, so nothing was "
                              "downloaded.").arg(what);
    case QNetworkReply::ContentNotFoundError:
        return QStringLiteral("GitHub does not have %1 (any more).").arg(what);
    default:
        break;
    }
    return status > 0 ? QStringLiteral("Could not download %1: GitHub answered %2.").arg(what).arg(status)
                      : QStringLiteral("Could not download %1: the connection failed.").arg(what);
}

QNetworkReply *AppUpdater::get(const QUrl &url, qint64 timeoutMs, bool *refusedBefore)
{
    // The first link checked here as well as by start(), hop by hop after
    // it: a link from anywhere else is never even asked for.
    if (!UpdateRules::allowedSource(url, m_feed)) {
        if (refusedBefore)
            *refusedBefore = true;
        return nullptr;
    }
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
        if (round == m_round && UpdateRules::allowedSource(to, m_feed, true)) {
            Q_EMIT reply->redirectAllowed();
            return;
        }
        reply->setProperty("refusedHost", to.host().isEmpty() ? QStringLiteral("no host") : to.host());
        reply->abort();
    });
    return reply;
}

void AppUpdater::fetchChecksum()
{
    bool refused = false;
    QNetworkReply *reply = get(m_choice.checksum.url, kChecksumTimeoutMs, &refused);
    if (!reply) {
        fail(QStringLiteral("The checksum's link points somewhere other than Monolist's releases on GitHub, so it "
                            "was not followed."));
        return;
    }
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
        const QString refusedHost = reply->property("refusedHost").toString();
        if (!refusedHost.isEmpty()) {
            fail(QStringLiteral("The checksum of Monolist %1 was sent somewhere other than GitHub (%2), so it was "
                                "not fetched.").arg(m_release.version, refusedHost));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(networkProblem(reply, QStringLiteral("the checksum of Monolist %1").arg(m_release.version)));
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
    m_expectedSha = sha256;
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
        fail(QStringLiteral("Monolist could not save the update on this computer's disk."), error);
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_expectedSha = sha256;
    m_received = 0;
    m_limit = UpdateRules::sizeLimit(m_choice.file.size);
    m_percent = 0;
    setPhase(Downloading, QStringLiteral("Downloading Monolist %1… 0 %").arg(m_release.version));

    QNetworkReply *reply = get(m_choice.file.url, kStallTimeoutMs);
    if (!reply) {
        stopNetwork();
        fail(QStringLiteral("The link to Monolist %1 points somewhere other than Monolist's releases on GitHub, so "
                            "it was not followed.").arg(m_release.version));
        return;
    }
    m_reply = reply;
    const int round = m_round;
    // Straight to disk as it arrives, hashed on the way: these files are
    // 150 to 280 MB, and none of it is held in memory. Never more than the
    // feed said: a source sending without end is stopped, not let fill the
    // disk before the checksum could say no.
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, round]() {
        if (round != m_round || !m_part)
            return;
        const QByteArray chunk = reply->readAll();
        if (m_received + chunk.size() > m_limit) {
            reply->setProperty("tooLong", true);
            reply->abort();
            return;
        }
        if (m_part->write(chunk) != chunk.size()) {
            reply->setProperty("writeError", m_part->errorString());
            reply->abort();
            return;
        }
        m_hash->addData(chunk);
        m_received += chunk.size();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, round](qint64 received, qint64 total) {
        if (round != m_round)
            return;
        if (total > m_limit) {
            reply->setProperty("tooLong", true);
            reply->abort();
            return;
        }
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
        bool tooLong = reply->property("tooLong").toBool();
        if (!rest.isEmpty()) {
            if (m_received + rest.size() > m_limit)
                tooLong = true;
            else if (m_part->write(rest) == rest.size())
                m_hash->addData(rest);
        }
        m_part->close();
        const QString part = m_part->fileName();
        delete m_part;
        m_part = nullptr;
        const QString got = QString::fromLatin1(m_hash->result().toHex());
        m_hash.reset();

        const QString refused = reply->property("refusedHost").toString();
        const QString writeError = reply->property("writeError").toString();
        QString problem;
        QString detail;
        if (!refused.isEmpty()) {
            problem = QStringLiteral("The download was sent somewhere other than GitHub (%1), so it was stopped.")
                          .arg(refused);
        } else if (tooLong) {
            problem = QStringLiteral("The download was bigger than Monolist %1 is, so it was stopped and deleted.")
                          .arg(m_release.version);
        } else if (!writeError.isEmpty()) {
            const qint64 free = freeBytes(workDirectory());
            problem = free >= 0 && free < 16 * 1024 * 1024
                          ? QStringLiteral("There is not enough free space to save the update.")
                          : QStringLiteral("Monolist could not save the update on this computer's disk.");
            detail = writeError;
        } else if (reply->error() != QNetworkReply::NoError) {
            problem = networkProblem(reply, QStringLiteral("Monolist %1").arg(m_release.version));
        } else if (got != m_expectedSha) {
            problem = QStringLiteral("The download did not match its published checksum, so it was deleted.");
        }
        const QString path = part.chopped(kPart.size());
        if (problem.isEmpty()) {
            QFile::remove(path);
            if (!QFile::rename(part, path)) {
                problem = QStringLiteral("Monolist could not save the update on this computer's disk.");
                detail = path;
            }
        }
        if (!problem.isEmpty()) {
            QFile::remove(part);
            fail(problem, detail);
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
        // Copied out and checked even when the user is to put it in place:
        // what is shown them has passed the same checks (stagePlatform ends
        // in manual() then).
        stagePlatform();
        return;
    case UpdateTarget::WindowsPortable:
        // Nothing more to check of a zip than its checksum.
        if (!m_target.blocker.isEmpty()) {
            manual();
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
    QString message = QStringLiteral("Monolist %1 is ready: Monolist restarts to finish.").arg(m_release.version);
    if (m_target.kind == UpdateTarget::WindowsSetup && m_target.allUsers)
        message += QStringLiteral(" Windows asks for permission first, as Monolist is installed for everyone.");
    setPhase(Ready, message);
}

void AppUpdater::manual()
{
    markDownloaded();
    qInfo("update: Monolist %s is downloaded, to be put in place by hand", qPrintable(m_release.version));
    setPhase(Manual, QStringLiteral("Monolist %1 is downloaded, but this copy cannot be replaced where it is: %2. %3")
                         .arg(m_release.version, m_target.blocker,
                              m_target.kind == UpdateTarget::MacBundle
                                  ? QStringLiteral("Drag Monolist from the window that opens to Applications, then "
                                                   "open it from there.")
                                  : QStringLiteral("Once Monolist has quit, copy everything in the zip's Monolist "
                                                   "folder into this copy's folder, replacing the files there.")));
}

void AppUpdater::run(const QString &program, const QStringList &arguments, int timeoutMs,
                     const std::function<void(int, const QByteArray &, const QByteArray &)> &done)
{
    auto *process = new QProcess(this);
    // Never the app's own folder (Windows: a process's current folder cannot
    // be renamed while it is in it).
    process->setWorkingDirectory(workDirectory());
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
    QSaveFile file(QDir(workDirectory()).filePath(kPending));
    const QJsonObject pending{
        { QStringLiteral("from"), runningVersion() },
        { QStringLiteral("to"), m_release.version },
        { QStringLiteral("page"), m_release.page.toString() },
        { QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
    };
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(pending).toJson()) < 0 || !file.commit()) {
        *error = QStringLiteral("Monolist could not write on this computer's disk");
        return false;
    }
    return true;
}

bool AppUpdater::handOver(QString *error)
{
    // A second RESTART (a double press, the toast's and Settings' both):
    // the helper already has it.
    if (m_handedOver)
        return true;
    if (m_phase != Ready || m_staged.isEmpty() || !QFileInfo::exists(m_staged)) {
        *error = QStringLiteral("there is no update ready to put in place");
        return false;
    }
    const QDir work(workDirectory());
    QFile::remove(work.filePath(kResult));
    QFile::remove(work.filePath(kStarted));
    if (!writePending(error))
        return false;
    if (!launchHelper(error)) {
        QFile::remove(work.filePath(kPending));
        return false;
    }
    qInfo("update: handed over to the helper, for Monolist %s", qPrintable(m_release.version));
    // From here the staged copy is the helper's: no cancel or quit may
    // remove it.
    m_handedOver = true;
    return true;
}

bool AppUpdater::helperDone() const
{
    const QDir dir(workDirectory());
    return !dir.exists(kPending) || dir.exists(kResult);
}

QString AppUpdater::housekeeping(bool *ok, QString *to)
{
    if (ok)
        *ok = true;
    // Never while an update is on its way: what is cleared here would be
    // its download, or its staged copy.
    if (busy())
        return {};
    const QDir dir(workDirectory());
    if (!dir.exists())
        return {};

    QString said;
    QFile pendingFile(dir.filePath(kPending));
    if (pendingFile.open(QIODevice::ReadOnly)) {
        const QJsonObject pending = QJsonDocument::fromJson(pendingFile.read(64 * 1024)).object();
        pendingFile.close();
        QStringList lines;
        QFile resultFile(dir.filePath(kResult));
        if (resultFile.open(QIODevice::ReadOnly))
            lines = QString::fromUtf8(resultFile.read(64 * 1024)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString result = lines.value(0).trimmed();
        QString kept;
        for (const QString &line : std::as_const(lines)) {
            if (line.startsWith(QLatin1String("kept:")))
                kept = line.mid(5).trimmed();
        }
        const QString target = pending.value(QStringLiteral("to")).toString();
        if (to)
            *to = target;
        const QString running = runningVersion();
        if (!target.isEmpty() && AppInfo::compareVersions(running, target) >= 0) {
            said = QStringLiteral("Monolist is now %1.").arg(running);
            if (!kept.isEmpty())
                said += QStringLiteral(" The folder of the copy before it is kept at %1, as it held files the new one "
                                       "does not have.").arg(QDir::toNativeSeparators(kept));
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
                       .arg(target, reason, running);
            qWarning("update: %s", qPrintable(said));
        }
        // What the last RESTART came to, kept until the next one: for a
        // look afterwards, and for the end-to-end tests.
        QSaveFile last(dir.filePath(kLast));
        if (last.open(QIODevice::WriteOnly)) {
            last.write((said + QLatin1Char('\n')).toUtf8());
            last.commit();
        }
    }
    QFile::remove(dir.filePath(kPending));
    QFile::remove(dir.filePath(kResult));
    QFile::remove(dir.filePath(kResult + QStringLiteral(".tmp")));
    QFile::remove(dir.filePath(kStarted));
    if (!helperFileName().isEmpty())
        QFile::remove(dir.filePath(helperFileName()));

    // A disk image a crash left attached, or a zip a crash left unpacked.
    clearPlatformLeftovers();
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

void AppUpdater::markDownloaded()
{
}

void AppUpdater::clearPlatformLeftovers()
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
