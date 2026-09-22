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
# Only this process and its children see the change.
$env:PATH = @($mingwBin, $cmakeBin, $ninjaDir, (Join-Path $qtPrefix 'bin'), $binDir, $env:PATH) -join ';'

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
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

# --------------------------------------------------------------------- deploy

$exe = Join-Path $buildDir 'monolist.exe'
# windeployqt reads the build type from the binary; MinGW Qt has one set of DLLs.
& windeployqt --qmldir $source --no-translations $exe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }
if (-not $NoMpv) {
    $mpvDll = Get-ChildItem -LiteralPath $mpvRoot -Recurse -Filter 'libmpv*.dll' | Select-Object -First 1
    if (-not $mpvDll) { throw "No libmpv DLL under $mpvRoot." }
    Copy-Item -LiteralPath $mpvDll.FullName -Destination $buildDir -Force
}

# The runtime tools go in tools\ beside the exe, where the app looks first, so
# the build also runs when started directly rather than through -Run. Hard links
# cost no space and are refreshed on every build; a copy is the fallback when
# the build is on another volume.
$toolsOut = Join-Path $buildDir 'tools'
New-Item -ItemType Directory -Force -Path $toolsOut | Out-Null
foreach ($tool in 'yt-dlp.exe', 'ffmpeg.exe', 'ffprobe.exe', 'deno.exe') {
    $from = Join-Path $binDir $tool
    if (-not (Test-Path -LiteralPath $from)) { continue }
    $to = Join-Path $toolsOut $tool
    if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Force }
    try {
        New-Item -ItemType HardLink -Path $to -Target $from -ErrorAction Stop | Out-Null
    } catch {
        Copy-Item -LiteralPath $from -Destination $to
    }
}
Write-Host "`nBuilt $exe" -ForegroundColor Green

# ------------------------------------------------------------------------ run

if ($Run) {
    & $exe @AppArgs
    exit $LASTEXITCODE
}
