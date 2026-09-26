<#
.SYNOPSIS
    Configures, builds and deploys Monolist with the toolchain from setup-windows.ps1.

.DESCRIPTION
    The sources stay where they are; the build tree goes to -BuildRoot on a local
    disk. Building inside a network share (a Parallels or SMB folder) is slow,
    and cmd.exe cannot use a UNC path as its working directory, which breaks
    Qt's code-generation steps. So a UNC source path is swapped for the mapped
    drive letter that points at the same share.

    After the build, windeployqt copies the Qt runtime next to monolist.exe and
    libmpv is copied beside it, so the build folder runs on its own. The runtime
    tools (yt-dlp, FFmpeg, Deno) stay in <InstallRoot>\bin, which this script puts
    on PATH for itself and for the app it starts with -Run.

.EXAMPLE
    .\build-windows.ps1                        # Debug build; its console shows the app's log
    .\build-windows.ps1 -Config Release
    .\build-windows.ps1 -Run                   # build, then start the app
    .\build-windows.ps1 -Run -AppArgs '--play','<videoId>','20'   # playback self-test, quits by itself
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]   $Config      = 'Debug',
    [string]   $InstallRoot = 'C:\dev\monolist-deps',
    [string]   $BuildRoot   = 'C:\dev\monolist-build',
    [string]   $QtVersion   = '6.11.2',
    [switch]   $NoMpv,
    [switch]   $Run,
    [string[]] $AppArgs     = @()
)

$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------- source path

$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
if ($source.StartsWith('\\')) {
    # \\server\share\rest: look for a drive letter mapped to a share of the same
    # name that holds the same project (Parallels exposes \\psf\Home as \\Mac\Home).
    $parts = $source.TrimStart('\').Split('\')
    $share = $parts[1]
    $rest  = ($parts | Select-Object -Skip 2) -join '\'
    $drive = Get-PSDrive -PSProvider FileSystem | Where-Object {
        $_.DisplayRoot -and $_.DisplayRoot.TrimEnd('\').Split('\')[-1] -eq $share -and
        (Test-Path -LiteralPath (Join-Path $_.Root (Join-Path $rest 'CMakeLists.txt')))
    } | Select-Object -First 1
    if ($drive) {
        $source = Join-Path $drive.Root $rest
    } else {
        Write-Warning "Building from a UNC path ($source). If code generation fails, map the share to a drive letter."
    }
}

# ------------------------------------------------------------------ toolchain

$qtRoot   = Join-Path $InstallRoot 'Qt'
$qtPrefix = Join-Path $qtRoot "$QtVersion\mingw_64"
$mingwBin = Join-Path $qtRoot 'Tools\mingw1310_64\bin'
$cmakeBin = Join-Path $qtRoot 'Tools\CMake_64\bin'
$ninjaDir = Join-Path $qtRoot 'Tools\Ninja'
$mpvRoot  = Join-Path $InstallRoot 'libmpv'
$binDir   = Join-Path $InstallRoot 'bin'
foreach ($path in $qtPrefix, $mingwBin, $cmakeBin, $ninjaDir, $binDir) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing $path. Run setup-windows.ps1 first." }
}
# Only this process and its children see the change. PortableGit goes on too,
# so the build can stamp itself with the commit it was made from; it is not
# required, and a build without it simply says so.
$gitBin = Join-Path $InstallRoot 'git\cmd'
$env:PATH = @($mingwBin, $cmakeBin, $ninjaDir, (Join-Path $qtPrefix 'bin'), $binDir, $gitBin, $env:PATH) -join ';'

$suffix   = if ($NoMpv) { '-nompv' } else { '' }
$buildDir = Join-Path $BuildRoot ($Config.ToLowerInvariant() + $suffix)

# ---------------------------------------------------------- configure, build

$configure = @('-S', $source, '-B', $buildDir, '-G', 'Ninja',
               "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_PREFIX_PATH=$qtPrefix")
if ($NoMpv) {
    $configure += '-DMONOLIST_NO_MPV=ON'
} else {
    $configure += @('-DMONOLIST_NO_MPV=OFF', "-DMPV_ROOT=$mpvRoot")
}

Write-Host "Source: $source`nBuild:  $buildDir" -ForegroundColor Cyan

# The app may be open while it is rebuilt, being tried out. Windows will not
# let the linker overwrite a running exe, but it will rename one: the running
# copy is moved aside and keeps playing, and the next start gets the new build.
# Copies moved aside earlier are removed once nothing is running them.
$exe = Join-Path $buildDir 'monolist.exe'
if (Test-Path -LiteralPath $buildDir) {
    Get-ChildItem -LiteralPath $buildDir -Filter 'monolist.*.running.exe' | ForEach-Object {
        try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { }
    }
}

function Move-RunningExeAside {
    if (-not (Test-Path -LiteralPath $exe)) { return $false }
    try {
        [IO.File]::Open($exe, 'Open', 'ReadWrite', 'None').Dispose()
        return $false
    } catch {
        $aside = 'monolist.{0}.running.exe' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
        Rename-Item -LiteralPath $exe -NewName $aside
        Write-Host "monolist.exe is running; moved it aside as $aside so it can keep playing." -ForegroundColor Yellow
        return $true
    }
}
$null = Move-RunningExeAside

& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) {
    # Started while this build compiled: move it aside now and link again.
    if (Move-RunningExeAside) { & cmake --build $buildDir --parallel }
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

# A QML mistake — a property that does not exist, a missing type, a required
# property left unset — only surfaces when its view loads, as a blank window.
# The linter finds them now. Its "unqualified" and backend "import" notes come
# from the C++ singletons being registered at run time, and are not errors.
$lint = & cmake --build $buildDir --target monolist_qmllint 2>&1 | ForEach-Object { "$_" }
# Only the message lines: the linter also echoes the offending source line,
# which can end in something bracketed too ("artworks[index]").
$problems = @($lint | Where-Object {
    $_ -match '^(Warning|Error): .*\[([a-z-]+)\]\s*$' -and $Matches[2] -notin @('unqualified', 'import', 'unused-imports')
})
if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    throw "QML lint found $($problems.Count) problem(s)."
}

# --------------------------------------------------------------------- deploy

# Puts `from` at `to`, leaving an identical file alone — the usual case, and the
# only safe one while the app is open with it loaded. A changed file that is in
# use is moved aside first, as the exe is above.
function Install-File([string] $from, [string] $to, [switch] $HardLink) {
    if (Test-Path -LiteralPath $to) {
        $source = Get-Item -LiteralPath $from
        $target = Get-Item -LiteralPath $to
        if ($source.Length -eq $target.Length -and $source.LastWriteTimeUtc -eq $target.LastWriteTimeUtc) { return }
        try {
            Remove-Item -LiteralPath $to -Force -ErrorAction Stop
        } catch {
            $aside = '{0}.{1}.running{2}' -f [IO.Path]::GetFileNameWithoutExtension($to),
                     (Get-Date -Format 'yyyyMMdd-HHmmss'), [IO.Path]::GetExtension($to)
            Rename-Item -LiteralPath $to -NewName $aside
        }
    }
    if ($HardLink) {
        try {
            New-Item -ItemType HardLink -Path $to -Target $from -ErrorAction Stop | Out-Null
            return
        } catch { }   # another volume: copy instead
    }
    Copy-Item -LiteralPath $from -Destination $to
}

# windeployqt reads the build type from the binary; MinGW Qt has one set of DLLs.
& windeployqt --qmldir $source --no-translations $exe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }

# Qt builds the one tool tip that every `ToolTip.text` shows at run time, from
# "import QtQuick.Controls; ToolTip {}". The QML here imports only
# QtQuick.Controls.Basic, so windeployqt leaves QtQuick.Controls itself out, and
# every tooltip failed with "QQmlComponent: Component is not ready". Its files
# are copied by hand: importing the module in QML instead would have windeployqt
# ship its six other styles as well, some 17 MB that are never used.
$controlsFrom = Join-Path $qtPrefix 'qml\QtQuick\Controls'
$controlsTo = Join-Path $buildDir 'qml\QtQuick\Controls'
New-Item -ItemType Directory -Force -Path $controlsTo | Out-Null
foreach ($file in 'qmldir', 'plugins.qmltypes', 'qtquickcontrols2plugin.dll') {
    Install-File (Join-Path $controlsFrom $file) (Join-Path $controlsTo $file)
}
if (-not $NoMpv) {
    $mpvDll = Get-ChildItem -LiteralPath $mpvRoot -Recurse -Filter 'libmpv*.dll' | Select-Object -First 1
    if (-not $mpvDll) { throw "No libmpv DLL under $mpvRoot." }
    Install-File $mpvDll.FullName (Join-Path $buildDir $mpvDll.Name)
}

# The runtime tools go in tools\ beside the exe, where the app looks first, so
# the build also runs when started directly rather than through -Run. Hard links
# cost no space; a copy is the fallback when the build is on another volume.
$toolsOut = Join-Path $buildDir 'tools'
New-Item -ItemType Directory -Force -Path $toolsOut | Out-Null
Get-ChildItem -LiteralPath $toolsOut -Filter '*.running.*' | ForEach-Object {
    try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch { }
}

# yt-dlp is a folder (the exe and its _internal runtime), so it is joined in as
# a directory junction. Removing a junction removes only the link.
$ytdlpDir = Join-Path $InstallRoot 'yt-dlp'
$ytdlpLink = Join-Path $toolsOut 'yt-dlp'
$staleSingleFile = Join-Path $toolsOut 'yt-dlp.exe'   # from builds before the unpacked yt-dlp
if (Test-Path -LiteralPath $staleSingleFile) { Remove-Item -LiteralPath $staleSingleFile -Force }
$ytdlpExists = Test-Path -LiteralPath (Join-Path $ytdlpDir 'yt-dlp.exe')
$linkIsCurrent = (Test-Path -LiteralPath $ytdlpLink) -and ((Get-Item -LiteralPath $ytdlpLink).Target -contains $ytdlpDir)
if ($ytdlpExists -and -not $linkIsCurrent) {
    if (Test-Path -LiteralPath $ytdlpLink) { [IO.Directory]::Delete($ytdlpLink, $false) }
    New-Item -ItemType Junction -Path $ytdlpLink -Target $ytdlpDir | Out-Null
}

foreach ($tool in 'ffmpeg.exe', 'ffprobe.exe', 'deno.exe') {
    $from = Join-Path $binDir $tool
    if (Test-Path -LiteralPath $from) { Install-File $from (Join-Path $toolsOut $tool) -HardLink }
}
Write-Host "`nBuilt $exe" -ForegroundColor Green

# ------------------------------------------------------------------------ run

if ($Run) {
    & $exe @AppArgs
    exit $LASTEXITCODE
}
