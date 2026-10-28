// The Mac's part of the in-app update (AppUpdater): which disk image this
// copy wants, the app copied out of it and checked, and the helper that puts
// it in place once Monolist has quit.

#include "macos/appupdater_mac.h"

#include "appinfo.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#import <Foundation/Foundation.h>
#include <Security/Security.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <utility>
#include <vector>

extern char **environ;

namespace {

const QString kHdiutil = QStringLiteral("/usr/bin/hdiutil");
const QString kDitto = QStringLiteral("/usr/bin/ditto");
const QString kCodesign = QStringLiteral("/usr/bin/codesign");
const QString kOpen = QStringLiteral("/usr/bin/open");
const QString kLsregister = QStringLiteral(
    "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister");
const QString kBundleId = QStringLiteral("io.github.droidboy08-hub.monolist");

// Carried to the app the helper opens again, which `open` would start with
// LaunchServices' environment rather than this one's: a scratch library
// stays the library (a restart of a test run must not open the real one).
const char *const kCarried[] = { "MONOLIST_DATA_DIR", "MONOLIST_DOWNLOAD_DIR", "MONOLIST_MPV_AO",
                                 "MONOLIST_MPV_VO", "MONOLIST_MPV_LOG", "QT_FORCE_STDERR_LOGGING" };

constexpr int kAttachTimeoutMs = 3 * 60 * 1000;
constexpr int kDetachTimeoutMs = 60 * 1000;
constexpr int kCopyTimeoutMs = 10 * 60 * 1000;
constexpr int kVerifyTimeoutMs = 5 * 60 * 1000;

QString firstLine(const QByteArray &text)
{
    const QString line = QString::fromUtf8(text).trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    return line.isEmpty() ? QStringLiteral("no reason given") : line;
}

bool sameVolume(const QString &a, const QString &b)
{
    struct stat first;
    struct stat second;
    return ::stat(QFile::encodeName(a).constData(), &first) == 0
           && ::stat(QFile::encodeName(b).constData(), &second) == 0 && first.st_dev == second.st_dev;
}

// Where the new app is made ready, and the old one put aside: on the app's
// own volume, so that both moves are renames (atomic, and instant). The
// updates folder when it is there, as it is when both are on the Mac's own
// disk; else the folder macOS keeps for this on that volume. For an app the
// user is to put in place by hand, the updates folder: it is only checked.
QString stagingRootFor(const QString &bundle, const QString &work, bool inPlace)
{
    const QString root = QDir(work).filePath(QStringLiteral("staged"));
    QDir().mkpath(root);
    const QString parent = QFileInfo(bundle).absolutePath();
    if (!inPlace || sameVolume(root, parent))
        return root;
    @autoreleasepool {
        NSError *error = nil;
        NSURL *url = [[NSFileManager defaultManager] URLForDirectory:NSItemReplacementDirectory
                                                            inDomain:NSUserDomainMask
                                                   appropriateForURL:[NSURL fileURLWithPath:parent.toNSString()]
                                                              create:YES
                                                               error:&error];
        return url ? QString::fromNSString(url.path) : QString();
    }
}

id propertyList(const QByteArray &plist)
{
    const qsizetype start = plist.indexOf("<?xml");
    if (start < 0)
        return nil;
    NSData *data = plist.mid(start).toNSData();
    return [NSPropertyListSerialization propertyListWithData:data options:0 format:nil error:nil];
}

// The image's own whole disk among `entities`: the lowest /dev/diskN (an
// APFS image's container comes after it, with a higher number).
QString wholeDisk(id entities)
{
    if (![entities isKindOfClass:[NSArray class]])
        return {};
    static const QRegularExpression whole(QStringLiteral("^/dev/disk([0-9]+)$"));
    QString device;
    int lowest = -1;
    for (id entity in entities) {
        if (![entity isKindOfClass:[NSDictionary class]])
            continue;
        id entry = entity[@"dev-entry"];
        if (![entry isKindOfClass:[NSString class]])
            continue;
        const QString name = QString::fromNSString(entry);
        const QRegularExpressionMatch match = whole.match(name);
        if (match.hasMatch() && (lowest < 0 || match.captured(1).toInt() < lowest)) {
            lowest = match.captured(1).toInt();
            device = name;
        }
    }
    return device;
}

// ------------------------------------------------- the helper's own parts

// A program run to its end, its output thrown away: its exit code, or -1.
// No event loop: the helper runs before any application object exists.
int runTool(const QString &program, const QStringList &arguments)
{
    QList<QByteArray> encoded{ QFile::encodeName(program) };
    for (const QString &argument : arguments)
        encoded << argument.toUtf8();
    std::vector<char *> argv;
    for (QByteArray &item : encoded)
        argv.push_back(item.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
        return -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool alive(pid_t pid)
{
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

bool present(const QString &path)
{
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0;
}

bool isFolder(const QString &path)
{
    struct stat info;
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool moveItem(const QString &from, const QString &to)
{
    return ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
}

void removeItem(const QString &path)
{
    @autoreleasepool {
        [[NSFileManager defaultManager] removeItemAtPath:path.toNSString() error:nil];
    }
}

// The word left for the next launch, whole or not at all.
void say(const QString &result, const QString &text)
{
    const QString temporary = result + QStringLiteral(".tmp");
    QFile file(temporary);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write((text + QLatin1Char('\n')).toUtf8());
        file.close();
        ::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(result).constData());
    }
}

// Whether a process other than this one runs from inside `app`.
bool runningFrom(const QString &app)
{
    const QString canonical = QFileInfo(app).canonicalFilePath();
    if (canonical.isEmpty())
        return false;
    const QByteArray prefix = QFile::encodeName(canonical + QStringLiteral("/Contents/MacOS/"));
    const int wanted = proc_listallpids(nullptr, 0);
    if (wanted <= 0)
        return false;
    std::vector<pid_t> pids(size_t(wanted) + 64);
    const int count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < count; ++i) {
        if (pids[size_t(i)] == ::getpid())
            continue;
        if (proc_pidpath(pids[size_t(i)], path, sizeof(path)) > 0
            && qstrncmp(path, prefix.constData(), uint(prefix.size())) == 0)
            return true;
    }
    return false;
}

} // namespace

// ------------------------------------------------------------- the rules

QString MacUpdate::developerIdRequirement()
{
    // 1.2.840.113635.100.6.2.6: an intermediate certificate of Apple's
    // Developer ID chain; 100.6.1.13: a Developer ID Application leaf.
    return QStringLiteral("anchor apple generic"
                          " and certificate 1[field.1.2.840.113635.100.6.2.6] exists"
                          " and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
                          " and certificate leaf[subject.OU] = \"7YWFR8A74F\"");
}

bool MacUpdate::runningIsDeveloperId()
{
    @autoreleasepool {
        SecCodeRef code = nullptr;
        if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess || !code)
            return false;
        SecRequirementRef requirement = nullptr;
        bool satisfied = false;
        if (SecRequirementCreateWithString((__bridge CFStringRef)developerIdRequirement().toNSString(),
                                           kSecCSDefaultFlags, &requirement) == errSecSuccess) {
            satisfied = SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement) == errSecSuccess;
            CFRelease(requirement);
        }
        CFRelease(code);
        return satisfied;
    }
}

QVariantMap MacUpdate::bundleInfo(const QString &bundle)
{
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:QDir(bundle).filePath(QStringLiteral("Contents/Info.plist")).toNSString()];
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfURL:url error:nil];
        QVariantMap map;
        const auto text = [info](NSString *key) {
            id value = info[key];
            return [value isKindOfClass:[NSString class]] ? QString::fromNSString(value) : QString();
        };
        if (info) {
            map.insert(QStringLiteral("id"), text(@"CFBundleIdentifier"));
            map.insert(QStringLiteral("build"), text(@"CFBundleVersion"));
            map.insert(QStringLiteral("version"), text(@"CFBundleShortVersionString"));
        }
        return map;
    }
}

QString MacUpdate::mountPointFrom(const QByteArray &plist)
{
    @autoreleasepool {
        id root = propertyList(plist);
        if (![root isKindOfClass:[NSDictionary class]])
            return {};
        id entities = root[@"system-entities"];
        if (![entities isKindOfClass:[NSArray class]])
            return {};
        for (id entity in entities) {
            if (![entity isKindOfClass:[NSDictionary class]])
                continue;
            id point = entity[@"mount-point"];
            if ([point isKindOfClass:[NSString class]] && [point length] > 0)
                return QString::fromNSString(point);
        }
        return {};
    }
}

QString MacUpdate::deviceFrom(const QByteArray &plist)
{
    @autoreleasepool {
        id root = propertyList(plist);
        if (![root isKindOfClass:[NSDictionary class]])
            return {};
        return wholeDisk(root[@"system-entities"]);
    }
}

QStringList MacUpdate::devicesAttachedFrom(const QByteArray &plist, const QString &folder)
{
    QStringList devices;
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    if (canonical.isEmpty())
        return devices;
    @autoreleasepool {
        id root = propertyList(plist);
        if (![root isKindOfClass:[NSDictionary class]])
            return devices;
        id images = root[@"images"];
        if (![images isKindOfClass:[NSArray class]])
            return devices;
        for (id image in images) {
            if (![image isKindOfClass:[NSDictionary class]])
                continue;
            id path = image[@"image-path"];
            if (![path isKindOfClass:[NSString class]])
                continue;
            const QString file = QString::fromNSString(path);
            const QString where = QFileInfo(file).canonicalFilePath().isEmpty() ? file
                                                                                 : QFileInfo(file).canonicalFilePath();
            if (!where.startsWith(canonical + QLatin1Char('/')))
                continue;
            const QString device = wholeDisk(image[@"system-entities"]);
            if (!device.isEmpty())
                devices << device;
        }
    }
    return devices;
}

bool MacUpdate::isQuarantined(const QString &path)
{
    return ::getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", nullptr, 0, 0, 0) >= 0;
}

QString MacUpdate::blockerFor(const QString &bundle)
{
    const QFileInfo info(bundle);
    const QString parent = info.absolutePath();
    // Opened from where it was downloaded, macOS runs a quarantined app from
    // a read-only copy of its own, until it is moved.
    if (bundle.contains(QLatin1String("/AppTranslocation/")))
        return QStringLiteral("macOS runs it from a temporary place, as it was opened where it was downloaded");
    // statfs, not QStorageInfo: that matches mount points by the path's
    // text, and a path under one of macOS's firmlinks (/Applications,
    // /Users, /private) then reads as on the sealed, read-only system
    // volume rather than the data volume it is on.
    struct statfs volume;
    if (::statfs(QFile::encodeName(bundle).constData(), &volume) == 0 && (volume.f_flags & MNT_RDONLY))
        return QStringLiteral("it runs from a disk image, or a disk that cannot be written to");
    if (QFileInfo::exists(QDir(parent).filePath(QStringLiteral("CMakeCache.txt"))))
        return QStringLiteral("it is a build of the source, in its build folder");
    if (!QFileInfo(parent).isWritable() || !info.isWritable())
        return QStringLiteral("Monolist may not change the folder it is in");
    return {};
}

UpdateTarget MacUpdate::describeBundle(const QString &bundle, bool developerId)
{
    UpdateTarget target;
    target.kind = UpdateTarget::MacBundle;
    target.path = QDir::cleanPath(bundle);
    target.developerId = developerId;
    target.blocker = blockerFor(target.path);
    return target;
}

// ------------------------------------------------------------- the helper

int MacUpdate::runSwapHelper(int argc, char **argv)
{
    // argv[1] is --update-swap.
    QStringList arguments;
    for (int i = 2; i < argc; ++i)
        arguments << QString::fromUtf8(argv[i]);
    if (arguments.size() < 12 || arguments.at(0) != QLatin1String(kSwapProtocol)) {
        std::fprintf(stderr, "update-swap: not the arguments this build's helper takes\n");
        return 2;
    }
    const pid_t pid = pid_t(arguments.at(1).toLongLong());
    const QString app = arguments.at(2);
    const QString fresh = arguments.at(3);
    const QString aside = arguments.at(4);
    const QString lsregister = arguments.at(5);
    const QString opener = arguments.at(6);
    const QString result = arguments.at(7);
    const QString started = arguments.at(8);
    const int quitSeconds = qMax(1, arguments.at(9).toInt());
    const int startSeconds = qMax(1, arguments.at(10).toInt());
    const QString requirement = arguments.at(11);
    const QStringList environment = arguments.mid(12);

    // Nothing sent to the Monolist that started it (a logout's hang-up)
    // ends it half way.
    ::signal(SIGHUP, SIG_IGN);

    const auto reopen = [&](const QString &bundle) {
        QStringList open{ QStringLiteral("-n") };
        for (const QString &pair : environment)
            open << QStringLiteral("--env") << pair;
        open << bundle;
        return runTool(opener, open) == 0;
    };

    // Until Monolist has quit, a tenth of a second at a time.
    for (int tries = quitSeconds * 10; alive(pid); --tries) {
        if (tries <= 0) {
            say(result, QStringLiteral("failed: Monolist did not quit, so nothing was changed."));
            return 1;
        }
        ::usleep(100000);
    }

    if (!isFolder(fresh)) {
        say(result, QStringLiteral("failed: the new Monolist was not where it was left, so nothing was changed."));
        reopen(app);
        return 1;
    }
    // Checked once more just before it goes in: it has waited in a folder of
    // the user's since it was checked.
    if (runTool(kCodesign, { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), fresh })
            != 0
        || (!requirement.isEmpty()
            && runTool(kCodesign, { QStringLiteral("--verify"), QStringLiteral("-R=") + requirement, fresh }) != 0)) {
        say(result, QStringLiteral("failed: the new Monolist no longer passed its signature check, so nothing was "
                                   "changed."));
        reopen(app);
        return 1;
    }

    if (present(aside))
        removeItem(aside);
    if (!moveItem(app, aside)) {
        say(result, QStringLiteral("failed: the Monolist in place could not be moved aside, so nothing was changed."));
        reopen(app);
        return 1;
    }
    if (present(app) || !moveItem(fresh, app)) {
        // The way back: the one there was, where it was.
        if (!present(app) && moveItem(aside, app)) {
            say(result, QStringLiteral("failed: the new Monolist could not be put in place, so the one you had is "
                                       "back."));
            reopen(app);
        } else {
            say(result, QStringLiteral("failed: the new Monolist could not be put in place, and the one you had is "
                                       "at %1.").arg(aside));
        }
        return 1;
    }

    // Registered afresh, so that its file types and its Spotlight and Quick
    // Look plug-ins are the new copy's.
    runTool(lsregister, { QStringLiteral("-f"), app });

    const auto putBack = [&](const QString &why) {
        if (moveItem(app, fresh) && moveItem(aside, app)) {
            runTool(lsregister, { QStringLiteral("-f"), app });
            say(result, QStringLiteral("failed: %1, so the one you had is back.").arg(why));
            reopen(app);
        } else {
            say(result, QStringLiteral("failed: %1, and the one you had is at %2.").arg(why, aside));
        }
        return 1;
    };

    ::unlink(QFile::encodeName(started).constData());
    if (!reopen(app))
        return putBack(QStringLiteral("the new Monolist would not open"));
    // Until it has started: the word it leaves once its window is up; or,
    // when that is slow in coming, it still running. One that opened and
    // ended at once (a part of it missing) has done neither.
    bool up = false;
    for (int tries = startSeconds * 5; tries > 0 && !up; --tries) {
        up = present(started);
        if (!up)
            ::usleep(200000);
    }
    if (!up)
        up = runningFrom(app);
    if (!up)
        return putBack(QStringLiteral("the new Monolist did not start"));

    // Only now is the old one let go of.
    removeItem(aside);
    say(result, QStringLiteral("ok"));
    return 0;
}

// ------------------------------------------------------------ the updater

UpdateTarget AppUpdater::describeThisCopy()
{
    const QString bundle = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../.."));
    if (!bundle.endsWith(QLatin1String(".app"))
        || !QFileInfo::exists(QDir(bundle).filePath(QStringLiteral("Contents/Info.plist"))))
        return UpdateTarget();
    return MacUpdate::describeBundle(bundle, MacUpdate::runningIsDeveloperId());
}

QString AppUpdater::helperFileName()
{
    // No file: the helper is this program (--update-swap).
    return {};
}

QByteArray AppUpdater::helperScript()
{
    return {};
}

void AppUpdater::stagePlatform()
{
    const QString version = m_release.version;
    setPhase(Preparing, QStringLiteral("Checking Monolist %1…").arg(version));

    const bool inPlace = m_target.blocker.isEmpty();
    m_stagingRoot = stagingRootFor(m_target.path, workDirectory(), inPlace);
    if (m_stagingRoot.isEmpty()) {
        fail(QStringLiteral("Monolist found nowhere to get the update ready on the disk it is on."));
        return;
    }
    QDir(m_stagingRoot).removeRecursively();
    const QString mountRoot = QDir(workDirectory()).filePath(QStringLiteral("mount"));
    if (!QDir().mkpath(m_stagingRoot + QStringLiteral("/new")) || !QDir().mkpath(mountRoot)) {
        fail(QStringLiteral("Monolist could not make a folder to get the update ready in."), m_stagingRoot);
        return;
    }
    // A disk image shown before (for a copy put in place by hand) carries the
    // quarantine; what is copied out of it here is checked here instead.
    ::removexattr(QFile::encodeName(m_file).constData(), "com.apple.quarantine", 0);

    attachImage(1);
}

// Attached where nothing sees it: not in the Finder, not opened, read only;
// under a folder of the update's own rather than /Volumes, so that another
// "Monolist" disk the user has open is never mistaken for it. Its contents
// are already vouched for by the SHA-256, hence -noverify.
void AppUpdater::attachImage(int attempt)
{
    const QString version = m_release.version;
    const QString mountRoot = QDir(workDirectory()).filePath(QStringLiteral("mount"));
    run(kHdiutil,
        { QStringLiteral("attach"), QStringLiteral("-nobrowse"), QStringLiteral("-readonly"),
          QStringLiteral("-noautoopen"), QStringLiteral("-noverify"), QStringLiteral("-plist"),
          QStringLiteral("-mountrandom"), mountRoot, m_file },
        kAttachTimeoutMs, [this, version, attempt](int code, const QByteArray &out, const QByteArray &err) {
            if (code != 0) {
                reject(QStringLiteral("The disk image of Monolist %1 could not be opened, so it was deleted.")
                           .arg(version));
                qWarning("update: hdiutil attach: %s", qPrintable(firstLine(err)));
                return;
            }
            const QString point = MacUpdate::mountPointFrom(out);
            // What lets it go: its device, which there is even when nothing
            // of it was mounted.
            m_mountPoint = MacUpdate::deviceFrom(out);
            if (m_mountPoint.isEmpty())
                m_mountPoint = point;
            if (point.isEmpty()) {
                // Attached, and nothing mounted: macOS does this now and
                // then. Let go of, and tried once more.
                detachImage();
                if (attempt < 2) {
                    const int round = m_round;
                    QTimer::singleShot(1000, this, [this, round, attempt]() {
                        if (round == m_round)
                            attachImage(attempt + 1);
                    });
                    return;
                }
                fail(QStringLiteral("The disk image of Monolist %1 opened, but macOS did not mount it. Try again in "
                                    "a moment.").arg(version));
                return;
            }
            const QString source = QDir(point).filePath(QStringLiteral("Monolist.app"));
            if (!QFileInfo(source).isDir()) {
                reject(QStringLiteral("The disk image of Monolist %1 has no Monolist in it, so it was deleted.")
                           .arg(version));
                return;
            }
            m_staged = m_stagingRoot + QStringLiteral("/new/Monolist.app");
            // The quarantine is not carried: the checks below stand where
            // Gatekeeper's would.
            run(kDitto, { QStringLiteral("--noqtn"), source, m_staged }, kCopyTimeoutMs,
                [this, version](int code, const QByteArray &, const QByteArray &err) {
                    const QString copyProblem = code == 0 ? QString() : firstLine(err);
                    // The image let go of first, whatever came of the copy.
                    const QString image = std::exchange(m_mountPoint, QString());
                    run(kHdiutil, { QStringLiteral("detach"), image }, kDetachTimeoutMs,
                        [this, version, image, copyProblem](int code, const QByteArray &, const QByteArray &) {
                            if (code != 0) {
                                m_mountPoint = image;
                                detachImage();
                            }
                            if (!copyProblem.isEmpty()) {
                                const bool full = copyProblem.contains(QLatin1String("No space left"));
                                fail(full ? QStringLiteral("There is not enough free space on the disk Monolist is "
                                                           "on to get Monolist %1 ready.").arg(version)
                                          : QStringLiteral("Monolist %1 could not be copied out of its disk image.")
                                                .arg(version),
                                     copyProblem);
                                return;
                            }
                            verifyStaged();
                        });
                });
        });
}

// The image let go of now, forced if it will not go quietly.
void AppUpdater::detachImage()
{
    const QString image = std::exchange(m_mountPoint, QString());
    if (image.isEmpty())
        return;
    QProcess detach;
    detach.start(kHdiutil, { QStringLiteral("detach"), image });
    if (detach.waitForFinished(kDetachTimeoutMs) && detach.exitStatus() == QProcess::NormalExit
        && detach.exitCode() == 0)
        return;
    QProcess force;
    force.start(kHdiutil, { QStringLiteral("detach"), QStringLiteral("-force"), image });
    force.waitForFinished(kDetachTimeoutMs);
}

// What the new app is, before anything of the old is touched, and before
// any of it is shown to be put in place by hand: Monolist, a newer build,
// sealed as signed, and for a Developer ID copy, signed by the project's
// Developer ID as well. One that is not is deleted, download and all.
void AppUpdater::verifyStaged()
{
    const QString version = m_release.version;
    const QVariantMap fresh = MacUpdate::bundleInfo(m_staged);
    const QVariantMap mine = MacUpdate::bundleInfo(m_target.path);
    const QString wantedId = mine.value(QStringLiteral("id")).toString().isEmpty()
                                 ? kBundleId : mine.value(QStringLiteral("id")).toString();
    const QString id = fresh.value(QStringLiteral("id")).toString();
    if (id != wantedId) {
        reject(QStringLiteral("The update is another app (%1), not Monolist, so it was not put in place.")
                   .arg(id.isEmpty() ? QStringLiteral("unnamed") : id));
        return;
    }
    const QString build = fresh.value(QStringLiteral("build")).toString();
    // The copy's own build; a test's stand-in version (AppInfo) when given.
    const QString current = AppInfo::testVersionGiven() || mine.value(QStringLiteral("build")).toString().isEmpty()
                                ? runningBuild() : mine.value(QStringLiteral("build")).toString();
    if (build.isEmpty() || AppInfo::compareVersions(build, current) <= 0) {
        reject(QStringLiteral("The update is not newer than this copy (its build is %1, this one's %2), so it was "
                              "not put in place.").arg(build.isEmpty() ? QStringLiteral("unknown") : build, current));
        return;
    }

    run(kCodesign, { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), m_staged },
        kVerifyTimeoutMs, [this](int code, const QByteArray &, const QByteArray &err) {
            if (code != 0) {
                qWarning("update: codesign: %s", qPrintable(firstLine(err)));
                reject(QStringLiteral("The new Monolist did not pass macOS's signature check, so it was not put in "
                                      "place."));
                return;
            }
            const auto passed = [this]() {
                if (m_target.blocker.isEmpty()) {
                    ready();
                    return;
                }
                // Checked, for the user to put in place: the copy made for
                // the checks is not needed.
                if (!m_stagingRoot.isEmpty())
                    QDir(std::exchange(m_stagingRoot, QString())).removeRecursively();
                m_staged.clear();
                manual();
            };
            if (!m_target.developerId) {
                passed();
                return;
            }
            run(kCodesign,
                { QStringLiteral("--verify"), QStringLiteral("-R=") + MacUpdate::developerIdRequirement(), m_staged },
                kVerifyTimeoutMs, [this, passed](int code, const QByteArray &, const QByteArray &) {
                    if (code != 0) {
                        reject(QStringLiteral("The new Monolist is not signed by Monolist's developer, as this copy "
                                              "is, so it was not put in place."));
                        return;
                    }
                    passed();
                });
        });
}

// The disk image marked as downloaded from the internet, as a browser
// would mark it, before it is shown: Gatekeeper then checks the Monolist
// dragged out of it on its first opening, as it did one downloaded by hand.
void AppUpdater::markDownloaded()
{
    if (m_file.isEmpty())
        return;
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:m_file.toNSString()];
        NSMutableDictionary *properties = [NSMutableDictionary dictionary];
        properties[@"LSQuarantineAgentName"] = @"Monolist";
        properties[@"LSQuarantineType"] = @"LSQuarantineTypeWebDownload";
        NSURL *origin = m_choice.file.url.toNSURL();
        if (origin)
            properties[@"LSQuarantineDataURL"] = origin;
        NSError *error = nil;
        if (![url setResourceValue:properties forKey:NSURLQuarantinePropertiesKey error:&error])
            qWarning("update: could not mark the disk image as downloaded: %s",
                     qPrintable(QString::fromNSString(error.localizedDescription)));
    }
}

void AppUpdater::stopPlatform()
{
    if (QProcess *process = m_process) {
        m_process = nullptr;
        process->kill();
        process->waitForFinished(5000);
    }
    detachImage();
    // After RESTART the staged copy is the helper's.
    if (m_handedOver)
        return;
    if (!m_stagingRoot.isEmpty())
        QDir(std::exchange(m_stagingRoot, QString())).removeRecursively();
    m_staged.clear();
}

void AppUpdater::clearPlatformLeftovers()
{
    // A disk image of the update's that a crash (or a kill) left attached,
    // hidden from the Finder.
    QProcess info;
    info.start(kHdiutil, { QStringLiteral("info"), QStringLiteral("-plist") });
    if (!info.waitForFinished(20000) || info.exitStatus() != QProcess::NormalExit || info.exitCode() != 0)
        return;
    const QStringList devices = MacUpdate::devicesAttachedFrom(info.readAllStandardOutput(), workDirectory());
    for (const QString &device : devices) {
        qInfo("update: letting go of a disk image left attached (%s)", qPrintable(device));
        QProcess detach;
        detach.start(kHdiutil, { QStringLiteral("detach"), QStringLiteral("-force"), device });
        detach.waitForFinished(kDetachTimeoutMs);
    }
}

bool AppUpdater::launchHelper(QString *error)
{
    const QDir work(workDirectory());
    const QString asideFolder = m_stagingRoot + QStringLiteral("/old");
    QDir(asideFolder).removeRecursively();
    if (!QDir().mkpath(asideFolder)) {
        *error = QStringLiteral("Monolist could not make a folder beside it for the update");
        return false;
    }

    // The helper is this program: signed as this copy is, by the same
    // developer as the copy it replaces, which macOS's App Management asks
    // of anything that changes a notarised app (a shell, say, would not be).
    const QString program = m_helper.program.isEmpty() ? QCoreApplication::applicationFilePath() : m_helper.program;
    const qint64 pid = m_helper.pid > 0 ? m_helper.pid : QCoreApplication::applicationPid();
    QStringList arguments{
        QStringLiteral("--update-swap"),
        QString::fromLatin1(MacUpdate::kSwapProtocol),
        QString::number(pid),
        m_target.path,
        m_staged,
        asideFolder + QStringLiteral("/Monolist.app"),
        m_helper.lsregister.isEmpty() ? kLsregister : m_helper.lsregister,
        m_helper.opener.isEmpty() ? kOpen : m_helper.opener,
        work.filePath(QStringLiteral("result.txt")),
        work.filePath(QStringLiteral("started")),
        QString::number(qMax(1, m_helper.timeoutSeconds)),
        QString::number(qMax(1, m_helper.startSeconds)),
        m_target.developerId ? MacUpdate::developerIdRequirement() : QString(),
    };
    for (const char *name : kCarried) {
        if (qEnvironmentVariableIsSet(name))
            arguments << QString::fromLatin1(name) + QLatin1Char('=') + qEnvironmentVariable(name);
    }

    // The end-to-end test's way to see the way back taken: the new app made
    // immutable, so that moving it into place really fails. A scratch
    // library's alone.
    if (!qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")
        && qEnvironmentVariable("MONOLIST_UPDATE_TEST_FAULT") == QLatin1String("move-new")) {
        qWarning("update: test fault: the new app is made immutable, so that putting it in place fails");
        ::chflags(QFile::encodeName(m_staged).constData(), UF_IMMUTABLE);
    }

    if (m_helper.wait) {
        QProcess helper;
        helper.setWorkingDirectory(workDirectory());
        helper.start(program, arguments);
        if (!helper.waitForStarted(10000)) {
            *error = QStringLiteral("the helper that puts it in place could not be started");
            return false;
        }
        helper.waitForFinished(m_helper.waitMs);
        return true;
    }
    if (!QProcess::startDetached(program, arguments, workDirectory())) {
        *error = QStringLiteral("the helper that puts it in place could not be started");
        return false;
    }
    return true;
}

void AppUpdater::showDownloaded()
{
    // The disk image opened in the Finder, as if double-clicked: its window
    // has Monolist beside a link to Applications. Only one that passed the
    // checks (Manual is reached through them alone).
    if (!m_file.isEmpty() && m_phase == Manual)
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_file));
}
