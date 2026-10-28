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
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

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
                                 "MONOLIST_MPV_VO", "MONOLIST_MPV_LOG" };

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
// disk; else the folder macOS keeps for this on that volume.
QString stagingRootFor(const QString &bundle, const QString &work)
{
    const QString root = QDir(work).filePath(QStringLiteral("staged"));
    QDir().mkpath(root);
    const QString parent = QFileInfo(bundle).absolutePath();
    if (sameVolume(root, parent))
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

// The helper, run by /bin/sh once Monolist has quit. Every path comes as an
// argument; nothing is written into this text. ASCII only.
const char kHelper[] = R"HELPER(#!/bin/sh
# Monolist's update, put in place once Monolist has quit (AppUpdater). Its
# arguments, in order:
#   1 the process to wait for        2 the app to replace
#   3 the new app, made ready on the same volume
#   4 where the old app is put aside while the new one goes in
#   5 lsregister                     6 what opens an app (open)
#   7 the file to leave a word in    8 how long to wait, in seconds
#   9... NAME=value for the environment of the app opened again
pid=$1
app=$2
new=$3
aside=$4
lsregister=$5
opener=$6
result=$7
timeout=$8
shift 8

say() {
    printf '%s\n' "$1" > "$result.tmp" && mv -f "$result.tmp" "$result"
}

# reopen <app> NAME=value...: opened as a new instance, with that environment.
reopen() {
    target=$1
    shift
    count=$#
    while [ "$count" -gt 0 ]; do
        set -- "$@" --env "$1"
        shift
        count=$((count - 1))
    done
    "$opener" -n "$@" "$target"
}

# Until Monolist has quit, a tenth of a second at a time.
tries=$((timeout * 10))
while kill -0 "$pid" 2>/dev/null; do
    tries=$((tries - 1))
    if [ "$tries" -le 0 ]; then
        say "failed: Monolist did not quit, so nothing was changed."
        exit 1
    fi
    sleep 0.1
done

if [ ! -d "$new" ]; then
    say "failed: the new Monolist was not where it was left, so nothing was changed."
    reopen "$app" "$@"
    exit 1
fi

rm -rf "$aside" 2>/dev/null
if ! mv "$app" "$aside"; then
    say "failed: the Monolist in place could not be moved aside, so nothing was changed."
    reopen "$app" "$@"
    exit 1
fi

if [ -e "$app" ] || ! mv "$new" "$app"; then
    # The way back: the one there was, where it was.
    if [ ! -e "$app" ] && mv "$aside" "$app"; then
        say "failed: the new Monolist could not be put in place, so the one you had is back."
        reopen "$app" "$@"
    else
        say "failed: the new Monolist could not be put in place, and the one you had is at $aside."
    fi
    exit 1
fi

# Registered afresh, so that its file types and its Spotlight and Quick Look
# plug-ins are the new copy's.
"$lsregister" -f "$app" >/dev/null 2>&1

if ! reopen "$app" "$@"; then
    # It would not open: the old one back, and opened instead.
    if mv "$app" "$new" && mv "$aside" "$app"; then
        "$lsregister" -f "$app" >/dev/null 2>&1
        say "failed: the new Monolist would not open, so the one you had is back."
        reopen "$app" "$@"
    else
        say "failed: the new Monolist would not open, and the one you had is at $aside."
    fi
    exit 1
fi

say "ok"
rm -rf "$aside"
exit 0
)HELPER";

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
    const qsizetype start = plist.indexOf("<?xml");
    if (start < 0)
        return {};
    @autoreleasepool {
        NSData *data = plist.mid(start).toNSData();
        id root = [NSPropertyListSerialization propertyListWithData:data options:0 format:nil error:nil];
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
    const qsizetype start = plist.indexOf("<?xml");
    if (start < 0)
        return {};
    @autoreleasepool {
        NSData *data = plist.mid(start).toNSData();
        id root = [NSPropertyListSerialization propertyListWithData:data options:0 format:nil error:nil];
        if (![root isKindOfClass:[NSDictionary class]])
            return {};
        id entities = root[@"system-entities"];
        if (![entities isKindOfClass:[NSArray class]])
            return {};
        // The whole disks: the image's own, and (APFS) the container made
        // from it, which comes after it, with a higher number.
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
    return QStringLiteral("update-helper.sh");
}

QByteArray AppUpdater::helperScript()
{
    return QByteArray(kHelper);
}

void AppUpdater::stagePlatform()
{
    const QString version = m_release.version;
    setPhase(Preparing, QStringLiteral("Checking Monolist %1…").arg(version));

    m_stagingRoot = stagingRootFor(m_target.path, workDirectory());
    if (m_stagingRoot.isEmpty()) {
        fail(QStringLiteral("Monolist found nowhere to get the update ready on the disk it is on."));
        return;
    }
    QDir(m_stagingRoot).removeRecursively();
    const QString mountRoot = QDir(workDirectory()).filePath(QStringLiteral("mount"));
    if (!QDir().mkpath(m_stagingRoot + QStringLiteral("/new")) || !QDir().mkpath(mountRoot)) {
        fail(QStringLiteral("Monolist could not make a folder to get the update ready in (%1).")
                 .arg(m_stagingRoot));
        return;
    }

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
                fail(QStringLiteral("The disk image of Monolist %1 could not be opened: %2").arg(version, firstLine(err)));
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
                fail(QStringLiteral("The disk image of Monolist %1 opened, but macOS did not mount it.").arg(version));
                return;
            }
            const QString source = QDir(point).filePath(QStringLiteral("Monolist.app"));
            if (!QFileInfo(source).isDir()) {
                fail(QStringLiteral("The disk image of Monolist %1 has no Monolist in it.").arg(version));
                return;
            }
            m_staged = m_stagingRoot + QStringLiteral("/new/Monolist.app");
            // The quarantine is not carried: nothing downloaded here has one,
            // and the checks below stand where Gatekeeper's would.
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
                                fail(QStringLiteral("Monolist %1 could not be copied out of its disk image: %2")
                                         .arg(version, copyProblem));
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

// What the new app is, before anything of the old is touched: Monolist, a
// newer build, sealed as signed, and for a Developer ID copy, signed by the
// project's Developer ID as well.
void AppUpdater::verifyStaged()
{
    const QString version = m_release.version;
    const QVariantMap fresh = MacUpdate::bundleInfo(m_staged);
    const QVariantMap mine = MacUpdate::bundleInfo(m_target.path);
    const QString wantedId = mine.value(QStringLiteral("id")).toString().isEmpty()
                                 ? kBundleId : mine.value(QStringLiteral("id")).toString();
    const QString id = fresh.value(QStringLiteral("id")).toString();
    if (id != wantedId) {
        fail(QStringLiteral("The update is another app (%1), not Monolist, so it was not put in place.")
                 .arg(id.isEmpty() ? QStringLiteral("unnamed") : id));
        return;
    }
    const QString build = fresh.value(QStringLiteral("build")).toString();
    const QString current = mine.value(QStringLiteral("build")).toString().isEmpty()
                                ? runningBuild() : mine.value(QStringLiteral("build")).toString();
    if (build.isEmpty() || AppInfo::compareVersions(build, current) <= 0) {
        fail(QStringLiteral("The update is not newer than this copy (its build is %1, this one's %2), so it was "
                            "not put in place.").arg(build.isEmpty() ? QStringLiteral("unknown") : build, current));
        return;
    }

    run(kCodesign, { QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), m_staged },
        kVerifyTimeoutMs, [this, version](int code, const QByteArray &, const QByteArray &err) {
            if (code != 0) {
                fail(QStringLiteral("The new Monolist did not pass macOS's signature check, so it was not put in "
                                    "place: %1").arg(firstLine(err)));
                return;
            }
            if (!m_target.developerId) {
                ready();
                return;
            }
            run(kCodesign,
                { QStringLiteral("--verify"), QStringLiteral("-R=") + MacUpdate::developerIdRequirement(), m_staged },
                kVerifyTimeoutMs, [this](int code, const QByteArray &, const QByteArray &) {
                    if (code != 0) {
                        fail(QStringLiteral("The new Monolist is not signed by Monolist's developer, as this copy "
                                            "is, so it was not put in place."));
                        return;
                    }
                    ready();
                });
        });
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

bool AppUpdater::launchHelper(QString *error)
{
    const QDir work(workDirectory());
    const QString script = work.filePath(helperFileName());
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(helperScript()) < 0) {
        *error = QStringLiteral("Monolist could not write in %1").arg(work.path());
        return false;
    }
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QString asideFolder = m_stagingRoot + QStringLiteral("/old");
    QDir(asideFolder).removeRecursively();
    if (!QDir().mkpath(asideFolder)) {
        *error = QStringLiteral("Monolist could not make a folder in %1").arg(m_stagingRoot);
        return false;
    }

    const qint64 pid = m_helper.pid > 0 ? m_helper.pid : QCoreApplication::applicationPid();
    QStringList arguments{
        script,
        QString::number(pid),
        m_target.path,
        m_staged,
        asideFolder + QStringLiteral("/Monolist.app"),
        m_helper.lsregister.isEmpty() ? kLsregister : m_helper.lsregister,
        m_helper.opener.isEmpty() ? kOpen : m_helper.opener,
        work.filePath(QStringLiteral("result.txt")),
        QString::number(qMax(1, m_helper.timeoutSeconds)),
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
        helper.start(QStringLiteral("/bin/sh"), arguments);
        if (!helper.waitForStarted(10000)) {
            *error = QStringLiteral("the helper that puts it in place could not be started");
            return false;
        }
        helper.waitForFinished(m_helper.waitMs);
        return true;
    }
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), arguments)) {
        *error = QStringLiteral("the helper that puts it in place could not be started");
        return false;
    }
    return true;
}

void AppUpdater::showDownloaded()
{
    // The disk image opened in the Finder, as if double-clicked: its window
    // has Monolist beside a link to Applications.
    if (!m_file.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_file));
}
