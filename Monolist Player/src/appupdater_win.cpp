#include "appupdater.h"

#include <QtGlobal>

// Windows' part of the in-app update (AppUpdater): which copy this is (the
// setup's, or a portable folder), the zip unpacked beside a portable one,
// and the helper that finishes once Monolist has quit. The whole file is
// Windows-only; elsewhere the Mac's (macos/appupdater_mac.mm) or the
// fallback at the end of appupdater.cpp answers.
//
// The Windows builds are not code-signed. What vouches for a download here
// is that it came over HTTPS from GitHub and matched the SHA-256 published
// beside it (AppUpdater); there is no signature to check.
#ifdef Q_OS_WIN

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>

#include <windows.h>

namespace {

// Where the setup records itself (packaging/monolist.iss, AppId); never
// changes.
const QString kUninstallKey = QStringLiteral(
    "\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8E0B6F4A-5C2D-4B7E-9A31-6D2F0C8E4B19}_is1");
constexpr int kUnpackTimeoutMs = 10 * 60 * 1000;

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QString installLocation(const QString &hive)
{
    const QSettings key(hive + kUninstallKey, QSettings::NativeFormat);
    return key.value(QStringLiteral("InstallLocation")).toString();
}

QString powerShell()
{
    const QString full = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
                             .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    return QFileInfo::exists(full) ? native(full) : QStringLiteral("powershell.exe");
}

// The helper, run by Windows PowerShell. Every path comes as a parameter;
// nothing is written into this text. ASCII only: Windows PowerShell reads a
// script without a byte order mark in the system's code page.
const char kHelper[] = R"HELPER(# Monolist's update (AppUpdater): unpacks a portable copy's zip; or, once
# Monolist has quit, puts the new copy in place and starts Monolist again,
# leaving a word in -Result on how it went.
param(
    [Parameter(Mandatory = $true)] [ValidateSet('unzip', 'setup', 'portable')] [string] $Mode,
    [string] $App = '',
    [string] $Result = '',
    [string] $Zip = '',
    [string] $Setup = '',
    [switch] $AllUsers,
    [string] $Staged = '',
    [string] $Aside = '',
    [int] $WaitPid = 0,
    [int] $TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'

if ($Mode -eq 'unzip') {
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Staged)
        exit 0
    } catch {
        [Console]::Error.WriteLine($_.Exception.Message)
        exit 1
    }
}

function Say([string] $text) {
    [IO.File]::WriteAllText($Result, $text + "`n")
}

function Start-Monolist([string] $folder) {
    Start-Process -FilePath (Join-Path $folder 'monolist.exe') -WorkingDirectory $folder | Out-Null
}

# A folder renamed, tried for a while: an antivirus reading the files just
# unpacked, or Windows letting go of the ones Monolist had open, can hold it.
function Move-Folder([string] $from, [string] $to) {
    for ($i = 0; $i -lt 20; $i++) {
        try {
            [IO.Directory]::Move($from, $to)
            return $true
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    return $false
}

# Until Monolist has quit.
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ($WaitPid -gt 0 -and (Get-Process -Id $WaitPid -ErrorAction SilentlyContinue)) {
    if ((Get-Date) -gt $deadline) {
        Say 'failed: Monolist did not quit, so nothing was changed.'
        exit 1
    }
    Start-Sleep -Milliseconds 100
}

if ($Mode -eq 'setup') {
    # The installer upgrades this copy where it is, quietly, and starts it
    # again when it is done (/RELAUNCH, monolist.iss).
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CLOSEAPPLICATIONS',
                   ('/DIR="' + $App + '"'), '/RELAUNCH')
    if ($AllUsers) { $arguments += '/ALLUSERS' } else { $arguments += '/CURRENTUSER' }
    try {
        $process = Start-Process -FilePath $Setup -ArgumentList $arguments -PassThru
        $null = $process.Handle
        $process.WaitForExit()
        $code = $process.ExitCode
    } catch {
        Say ('failed: the installer could not be started (' + $_.Exception.Message + '), so nothing was changed.')
        Start-Monolist $App
        exit 1
    }
    if ($code -ne 0) {
        Say ('failed: the installer stopped (code ' + $code + ') and left this copy as it was.')
        Start-Monolist $App
        exit 1
    }
    Say 'ok'
    exit 0
}

# A portable copy: its folder swapped for the new one, with a way back.
if (-not (Test-Path -LiteralPath (Join-Path $Staged 'monolist.exe'))) {
    Say 'failed: the new Monolist was not where it was left, so nothing was changed.'
    Start-Monolist $App
    exit 1
}
if (-not (Move-Folder $App $Aside)) {
    Say 'failed: the folder of this copy was in use, so nothing was changed.'
    Start-Monolist $App
    exit 1
}
if (-not (Move-Folder $Staged $App)) {
    if (Move-Folder $Aside $App) {
        Say 'failed: the new Monolist could not be put in place, so the one you had is back.'
        Start-Monolist $App
    } else {
        Say ('failed: the new Monolist could not be put in place, and the one you had is at ' + $Aside + '.')
    }
    exit 1
}
try {
    Start-Monolist $App
} catch {
    if ((Move-Folder $App $Staged) -and (Move-Folder $Aside $App)) {
        Say 'failed: the new Monolist would not start, so the one you had is back.'
        Start-Monolist $App
    } else {
        Say ('failed: the new Monolist would not start, and the one you had is at ' + $Aside + '.')
    }
    exit 1
}
Say 'ok'
Remove-Item -LiteralPath $Aside -Recurse -Force -ErrorAction SilentlyContinue
$root = Split-Path -Parent $Staged
if ((Test-Path -LiteralPath $root) -and -not (Get-ChildItem -LiteralPath $root -Force)) {
    Remove-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
}
exit 0
)HELPER";

// The helper written into the updates folder, or empty (and why).
QString writeHelper(const QString &folder, QString *error)
{
    const QString script = QDir(folder).filePath(AppUpdater::helperFileName());
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(AppUpdater::helperScript()) < 0) {
        *error = QStringLiteral("Monolist could not write in %1").arg(native(folder));
        return {};
    }
    return script;
}

QStringList helperStart(const QString &script)
{
    // -ExecutionPolicy Bypass for this one script, as the tool update runs
    // the setup script; the policy of the machine is not changed.
    return { QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
             QStringLiteral("Bypass"), QStringLiteral("-File"), native(script) };
}

void withoutWindow(QProcess &process)
{
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags &= ~DWORD(CREATE_NEW_CONSOLE);
        arguments->flags |= CREATE_NO_WINDOW;
    });
}

} // namespace

UpdateTarget AppUpdater::describeThisCopy()
{
    return WinUpdate::describeFolder(QCoreApplication::applicationDirPath(),
                                     installLocation(QStringLiteral("HKEY_CURRENT_USER")),
                                     installLocation(QStringLiteral("HKEY_LOCAL_MACHINE")));
}

QString AppUpdater::helperFileName()
{
    return QStringLiteral("update-helper.ps1");
}

QByteArray AppUpdater::helperScript()
{
    return QByteArray(kHelper);
}

void AppUpdater::stagePlatform()
{
    if (m_target.kind != UpdateTarget::WindowsPortable) {
        fail(QStringLiteral("This copy of Monolist cannot update itself."));
        return;
    }
    const QString version = m_release.version;
    setPhase(Preparing, QStringLiteral("Unpacking Monolist %1…").arg(version));

    // Beside this copy, on its own disk, so that the swap is two renames.
    const QFileInfo app(m_target.path);
    m_stagingRoot = QDir(app.absolutePath()).filePath(app.fileName() + QStringLiteral(".update"));
    QDir(m_stagingRoot).removeRecursively();
    if (!QDir().mkpath(m_stagingRoot)) {
        fail(QStringLiteral("Monolist could not make a folder beside this copy (%1).").arg(native(m_stagingRoot)));
        return;
    }
    QString error;
    const QString script = writeHelper(workDirectory(), &error);
    if (script.isEmpty()) {
        fail(error + QLatin1Char('.'));
        return;
    }
    run(powerShell(),
        helperStart(script) << QStringLiteral("-Mode") << QStringLiteral("unzip") << QStringLiteral("-Zip")
                            << native(m_file) << QStringLiteral("-Staged") << native(m_stagingRoot),
        kUnpackTimeoutMs, [this, version](int code, const QByteArray &, const QByteArray &err) {
            if (code != 0) {
                fail(QStringLiteral("The zip of Monolist %1 could not be unpacked: %2")
                         .arg(version, QString::fromLocal8Bit(err).trimmed().section(QLatin1Char('\n'), 0, 0)));
                return;
            }
            // The zip's one folder ("Monolist"), whatever it is called.
            QString staged;
            for (const QFileInfo &entry : QDir(m_stagingRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (QFileInfo::exists(QDir(entry.filePath()).filePath(QStringLiteral("monolist.exe")))) {
                    staged = entry.filePath();
                    break;
                }
            }
            if (staged.isEmpty()) {
                fail(QStringLiteral("The zip of Monolist %1 has no monolist.exe in it, so nothing was changed.")
                         .arg(version));
                return;
            }
            m_staged = staged;
            ready();
        });
}

void AppUpdater::stopPlatform()
{
    if (QProcess *process = m_process) {
        m_process = nullptr;
        process->kill();
        process->waitForFinished(5000);
    }
    // After RESTART the staged copy is the helper's.
    if (m_handedOver)
        return;
    if (m_target.kind == UpdateTarget::WindowsPortable && !m_stagingRoot.isEmpty())
        QDir(m_stagingRoot).removeRecursively();
    m_stagingRoot.clear();
    m_staged.clear();
}

bool AppUpdater::launchHelper(QString *error)
{
    const QString script = writeHelper(workDirectory(), error);
    if (script.isEmpty())
        return false;
    const qint64 pid = m_helper.pid > 0 ? m_helper.pid : QCoreApplication::applicationPid();
    QStringList arguments = helperStart(script);
    arguments << QStringLiteral("-WaitPid") << QString::number(pid) << QStringLiteral("-App") << native(m_target.path)
              << QStringLiteral("-Result") << native(QDir(workDirectory()).filePath(QStringLiteral("result.txt")))
              << QStringLiteral("-TimeoutSeconds") << QString::number(qMax(1, m_helper.timeoutSeconds));
    if (m_target.kind == UpdateTarget::WindowsSetup) {
        arguments << QStringLiteral("-Mode") << QStringLiteral("setup") << QStringLiteral("-Setup") << native(m_staged);
        if (m_target.allUsers)
            arguments << QStringLiteral("-AllUsers");
    } else {
        const QFileInfo app(m_target.path);
        const QString aside = QDir(app.absolutePath())
                                  .filePath(app.fileName() + QStringLiteral(".old-")
                                            + QString::number(QCoreApplication::applicationPid()));
        arguments << QStringLiteral("-Mode") << QStringLiteral("portable") << QStringLiteral("-Staged")
                  << native(m_staged) << QStringLiteral("-Aside") << native(aside);
    }

    QProcess helper;
    helper.setProgram(powerShell());
    helper.setArguments(arguments);
    withoutWindow(helper);
    if (m_helper.wait) {
        helper.start();
        if (!helper.waitForStarted(10000)) {
            *error = QStringLiteral("the helper that puts it in place could not be started");
            return false;
        }
        helper.waitForFinished(m_helper.waitMs);
        return true;
    }
    if (!helper.startDetached()) {
        *error = QStringLiteral("the helper that puts it in place could not be started");
        return false;
    }
    return true;
}

void AppUpdater::showDownloaded()
{
    if (m_file.isEmpty())
        return;
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(native(m_file)));
    explorer.startDetached();
}

#endif // Q_OS_WIN
