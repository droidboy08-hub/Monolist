# Builds the INSTRUMENTED copy of Monolist (bench\src-copy) into bench\build,
# with the same toolchain and steps as "Monolist Player\scripts\build-windows.ps1"
# (configure, build, windeployqt, QtQuick.Controls copy, libmpv, tools\).
# Never touches the repo or C:\dev\monolist-build.
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Config = 'Release',
    [string]$InstallRoot = 'C:\dev\monolist-deps',
    [string]$QtVersion = '6.11.2'
)
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $bench 'src-copy'
$buildDir = Join-Path $bench 'build'

$qtRoot   = Join-Path $InstallRoot 'Qt'
$qtPrefix = Join-Path $qtRoot "$QtVersion\mingw_64"
$mingwBin = Join-Path $qtRoot 'Tools\mingw1310_64\bin'
$cmakeBin = Join-Path $qtRoot 'Tools\CMake_64\bin'
$ninjaDir = Join-Path $qtRoot 'Tools\Ninja'
$mpvRoot  = Join-Path $InstallRoot 'libmpv'
$binDir   = Join-Path $InstallRoot 'bin'
$env:PATH = @($mingwBin, $cmakeBin, $ninjaDir, (Join-Path $qtPrefix 'bin'), $binDir, $env:PATH) -join ';'

& cmake -S $source -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_PREFIX_PATH=$qtPrefix" '-DMONOLIST_NO_MPV=OFF' "-DMPV_ROOT=$mpvRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$exe = Join-Path $buildDir 'monolist.exe'
& windeployqt --qmldir $source --no-translations $exe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }

$controlsFrom = Join-Path $qtPrefix 'qml\QtQuick\Controls'
$controlsTo = Join-Path $buildDir 'qml\QtQuick\Controls'
New-Item -ItemType Directory -Force -Path $controlsTo | Out-Null
foreach ($file in 'qmldir', 'plugins.qmltypes', 'qtquickcontrols2plugin.dll') {
    Copy-Item -Force (Join-Path $controlsFrom $file) (Join-Path $controlsTo $file)
}
$mpvDll = Get-ChildItem -LiteralPath $mpvRoot -Recurse -Filter 'libmpv*.dll' | Select-Object -First 1
Copy-Item -Force $mpvDll.FullName (Join-Path $buildDir $mpvDll.Name)

# Runtime tools beside the exe, as build-windows.ps1 does: yt-dlp as a junction,
# the others copied (not hard-linked, so nothing here shares a file with C:\dev).
$toolsOut = Join-Path $buildDir 'tools'
New-Item -ItemType Directory -Force -Path $toolsOut | Out-Null
$ytdlpDir = Join-Path $InstallRoot 'yt-dlp'
$ytdlpLink = Join-Path $toolsOut 'yt-dlp'
if (-not (Test-Path -LiteralPath $ytdlpLink)) { New-Item -ItemType Junction -Path $ytdlpLink -Target $ytdlpDir | Out-Null }
foreach ($tool in 'ffmpeg.exe', 'ffprobe.exe', 'deno.exe') {
    $from = Join-Path $binDir $tool
    $to = Join-Path $toolsOut $tool
    if ((Test-Path -LiteralPath $from) -and -not (Test-Path -LiteralPath $to)) { Copy-Item $from $to }
}
Write-Host "Built $exe"
