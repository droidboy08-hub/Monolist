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
// beside it (AppUpdater), checked again by the helper just before it runs a
// setup; there is no signature to check.
#ifdef Q_OS_WIN

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <windows.h>

namespace {

// Where the setup records itself (packaging/monolist.iss, AppId); never
// changes.
const wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8E0B6F4A-5C2D-4B7E-9A31-6D2F0C8E4B19}_is1";
constexpr int kUnpackTimeoutMs = 10 * 60 * 1000;

QString native(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

// The setup's InstallLocation in one hive and view, read only: the key is
// opened, never made (QSettings would make an empty one, and Inno Setup
// takes any key there for an installation of this user's).
QString installLocation(HKEY hive, REGSAM view)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive, kUninstallKey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    const LSTATUS status = RegGetValueW(key, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buffer, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || size < sizeof(wchar_t))
        return {};
    return QString::fromWCharArray(buffer, int(size / sizeof(wchar_t)) - 1);
}

QString allUsersLocation()
{
    // The setup runs in 64-bit mode (monolist.iss), so its key is in the
    // 64-bit view; an older 32-bit one would have written the other.
    const QString wide = installLocation(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
    return wide.isEmpty() ? installLocation(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY) : wide;
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
    [string] $Sha256 = '',
    [switch] $AllUsers,
    [string] $Staged = '',
    [string] $Aside = '',
    [string] $Started = '',
    [int] $WaitPid = 0,
    [int] $TimeoutSeconds = 60,
    [int] $StartSeconds = 60,
    [int] $SetupMinutes = 30
)

$ErrorActionPreference = 'Stop'

# Never in Monolist's folder: a process's current folder cannot be renamed.
if ($Result) {
    try { [IO.Directory]::SetCurrentDirectory((Split-Path -Parent $Result)) } catch { }
}

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
    $temp = $Result + '.tmp'
    [IO.File]::WriteAllText($temp, $text + "`n")
    Move-Item -LiteralPath $temp -Destination $Result -Force
}

# A program started as Explorer would start it, its handle kept from the
# start: Start-Process -PassThru looks the process up afresh, and one that has
# already ended (a setup that fails at once) then has no exit code, or throws.
function Start-Program([string] $file, [string] $arguments, [string] $folder) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $file
    $info.Arguments = $arguments
    $info.WorkingDirectory = $folder
    $info.UseShellExecute = $true
    return [Diagnostics.Process]::Start($info)
}

function Start-Monolist([string] $folder) {
    return Start-Program (Join-Path $folder 'monolist.exe') '' $folder
}

# Whether the Monolist just started got going: its word ($Started), or still
# running, or ended of itself (exit code 0), when the time is up. Only one
# that ended with an error and left no word did not.
function Test-Started($process) {
    $deadline = (Get-Date).AddSeconds($StartSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($Started -and (Test-Path -LiteralPath $Started)) { return $true }
        if ($process.HasExited) {
            if ($Started -and (Test-Path -LiteralPath $Started)) { return $true }
            return ($process.ExitCode -eq 0)
        }
        Start-Sleep -Milliseconds 200
    }
    return $true
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

# Whether the old folder holds anything the new one does not: a file of the
# user's, or one the old package had and the new one drops; or a link to
# somewhere else (a junction), never followed. Such a folder is kept, never
# deleted.
function Test-Leftovers([string] $old, [string] $new) {
    try {
        $links = @(Get-ChildItem -LiteralPath $old -Recurse -Force -Attributes ReparsePoint -ErrorAction Stop)
        if ($links.Count -gt 0) { return $true }
        $prefix = $old.TrimEnd('\') + '\'
        foreach ($file in (Get-ChildItem -LiteralPath $old -Recurse -Force -File -ErrorAction Stop)) {
            $relative = $file.FullName.Substring($prefix.Length)
            if (-not (Test-Path -LiteralPath (Join-Path $new $relative) -PathType Leaf)) { return $true }
        }
        return $false
    } catch {
        return $true
    }
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
    $hold = $null
    try {
        # Read once more just before it runs, and held, so that nothing can
        # change it in between: the file run is the one that matched.
        $hold = [IO.File]::Open($Setup, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $hasher = [Security.Cryptography.SHA256]::Create()
        $got = ([BitConverter]::ToString($hasher.ComputeHash($hold))).Replace('-', '').ToLowerInvariant()
        if ($Sha256 -and $got -ne $Sha256.ToLowerInvariant()) {
            $hold.Dispose()
            Say 'failed: the installer had changed since it was checked, so it was not run.'
            Start-Monolist $App | Out-Null
            exit 1
        }
        $process = Start-Program $Setup ($arguments -join ' ') (Split-Path -Parent $Setup)
        if (-not $process.WaitForExit($SetupMinutes * 60 * 1000)) {
            $hold.Dispose()
            Say 'failed: the installer did not finish in time (was Windows asking for permission?), so Monolist was started again as it was.'
            Start-Monolist $App | Out-Null
            exit 1
        }
        $code = $process.ExitCode
        $hold.Dispose()
    } catch {
        if ($hold) { $hold.Dispose() }
        Say ('failed: the installer could not be started (' + $_.Exception.Message + '), so nothing was changed.')
        Start-Monolist $App | Out-Null
        exit 1
    }
    if ($code -ne 0) {
        Say ('failed: the installer stopped (code ' + $code + ') and left this copy as it was.')
        Start-Monolist $App | Out-Null
        exit 1
    }
    Say 'ok'
    exit 0
}

# A portable copy: its folder swapped for the new one, with a way back.
if (-not (Test-Path -LiteralPath (Join-Path $Staged 'monolist.exe'))) {
    Say 'failed: the new Monolist was not where it was left, so nothing was changed.'
    Start-Monolist $App | Out-Null
    exit 1
}
if (-not (Move-Folder $App $Aside)) {
    Say 'failed: the folder of this copy was in use, so nothing was changed.'
    Start-Monolist $App | Out-Null
    exit 1
}
if (-not (Move-Folder $Staged $App)) {
    if (Move-Folder $Aside $App) {
        Say 'failed: the new Monolist could not be put in place, so the one you had is back.'
        Start-Monolist $App | Out-Null
    } else {
        Say ('failed: the new Monolist could not be put in place, and the one you had is at ' + $Aside + '.')
    }
    exit 1
}
# (PowerShell's names ignore case: $Started is the word's file.)
$wentOn = $false
try {
    $wentOn = Test-Started (Start-Monolist $App)
} catch {
    $wentOn = $false
}
if (-not $wentOn) {
    if ((Move-Folder $App $Staged) -and (Move-Folder $Aside $App)) {
        Say 'failed: the new Monolist would not start, so the one you had is back.'
        Start-Monolist $App | Out-Null
    } else {
        Say ('failed: the new Monolist would not start, and the one you had is at ' + $Aside + '.')
    }
    exit 1
}
if (Test-Leftovers $Aside $App) {
    Say ("ok`nkept: " + $Aside)
} else {
    Say 'ok'
    Remove-Item -LiteralPath $Aside -Recurse -Force -ErrorAction SilentlyContinue
}
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
        *error = QStringLiteral("Monolist could not write on this computer's disk");
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

// The zip unpacked beside this copy, left by a crash or a failed swap:
// nothing in it but the zip's own folder.
bool isUnpackedZip(const QString &folder)
{
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        if (!entry.isDir() || entry.isSymLink() || !QFileInfo::exists(QDir(entry.filePath()).filePath(QStringLiteral("monolist.exe"))))
            return false;
    }
    return true;
}

} // namespace

UpdateTarget AppUpdater::describeThisCopy()
{
    // Compared as the folders they are, whichever way written: a short
    // (8.3) name, a junction or a subst drive in one and not the other.
    const auto real = [](const QString &path) {
        const QString canonical = path.isEmpty() ? QString() : QFileInfo(path).canonicalFilePath();
        return canonical.isEmpty() ? path : canonical;
    };
    const QString mine = installLocation(HKEY_CURRENT_USER, KEY_WOW64_64KEY);
    const QString everyone = allUsersLocation();
    UpdateTarget target = WinUpdate::describeFolder(real(QCoreApplication::applicationDirPath()), real(mine),
                                                    real(everyone), userFolders());
    // The setup is given the folder as it recorded it.
    if (target.kind == UpdateTarget::WindowsSetup) {
        const QString recorded = target.allUsers ? everyone : mine;
        target.path = QDir::cleanPath(QString(recorded).replace(QLatin1Char('\\'), QLatin1Char('/')));
    }
    return target;
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
        fail(QStringLiteral("Monolist could not make a folder beside this copy for the update."), m_stagingRoot);
        return;
    }
    QString error;
    const QString script = writeHelper(workDirectory(), &error);
    if (script.isEmpty()) {
        fail(error + QLatin1Char('.'), workDirectory());
        return;
    }
    run(powerShell(),
        helperStart(script) << QStringLiteral("-Mode") << QStringLiteral("unzip") << QStringLiteral("-Zip")
                            << native(m_file) << QStringLiteral("-Staged") << native(m_stagingRoot),
        kUnpackTimeoutMs, [this, version](int code, const QByteArray &, const QByteArray &err) {
            if (code != 0) {
                fail(QStringLiteral("The zip of Monolist %1 could not be unpacked beside this copy.").arg(version),
                     QString::fromLocal8Bit(err).trimmed().section(QLatin1Char('\n'), 0, 0));
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
                reject(QStringLiteral("The zip of Monolist %1 has no monolist.exe in it, so nothing was changed.")
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

void AppUpdater::markDownloaded()
{
}

void AppUpdater::clearPlatformLeftovers()
{
    // The zip a crash, or a swap that did not go in, left unpacked beside a
    // portable copy. Never a copy put aside (".old-…"): one kept is kept
    // because it holds the user's files.
    const QString app = m_targetKnown ? m_target.path : QCoreApplication::applicationDirPath();
    const QString unpacked = app + QStringLiteral(".update");
    if (QFileInfo(unpacked).isDir() && isUnpackedZip(unpacked))
        QDir(unpacked).removeRecursively();
}

bool AppUpdater::launchHelper(QString *error)
{
    const QString script = writeHelper(workDirectory(), error);
    if (script.isEmpty())
        return false;
    const qint64 pid = m_helper.pid > 0 ? m_helper.pid : QCoreApplication::applicationPid();
    const QDir work(workDirectory());
    QStringList arguments = helperStart(script);
    arguments << QStringLiteral("-WaitPid") << QString::number(pid) << QStringLiteral("-App") << native(m_target.path)
              << QStringLiteral("-Result") << native(work.filePath(QStringLiteral("result.txt")))
              << QStringLiteral("-Started") << native(work.filePath(QStringLiteral("started")))
              << QStringLiteral("-TimeoutSeconds") << QString::number(qMax(1, m_helper.timeoutSeconds))
              << QStringLiteral("-StartSeconds") << QString::number(qMax(1, m_helper.startSeconds));
    if (m_target.kind == UpdateTarget::WindowsSetup) {
        arguments << QStringLiteral("-Mode") << QStringLiteral("setup") << QStringLiteral("-Setup") << native(m_staged)
                  << QStringLiteral("-Sha256") << m_expectedSha;
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
    // Started in the updates folder, never in Monolist's own, which it
    // would hold: Windows cannot rename a folder a process is in.
    helper.setWorkingDirectory(workDirectory());
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
    if (m_file.isEmpty() || m_phase != Manual)
        return;
    QProcess explorer;
    explorer.setProgram(QStringLiteral("explorer.exe"));
    explorer.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(native(m_file)));
    explorer.startDetached();
}

#endif // Q_OS_WIN
