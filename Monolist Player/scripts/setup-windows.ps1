<#
.SYNOPSIS
    Installs everything Monolist needs to build on Windows.

.DESCRIPTION
    This machine has MSVC (Visual Studio 2026) but no Qt, CMake, Ninja, libmpv,
    yt-dlp or ffmpeg. There is no winget on this Windows edition, so the script
    uses pip — Python is already present — plus a direct download for libmpv.

    Everything lands under -InstallRoot (default C:\dev\monolist-deps) except the
    pip packages, which go into the current Python environment. Nothing is
    written outside those two places, and nothing touches the project folder.

    Re-running is safe: each step is skipped when already satisfied.

.PARAMETER InstallRoot
    Where Qt and libmpv are unpacked.

.PARAMETER QtVersion
    Qt version to fetch via aqtinstall.

.EXAMPLE
    .\setup-windows.ps1
    .\setup-windows.ps1 -InstallRoot D:\deps -QtVersion 6.8.1
#>

[CmdletBinding()]
param(
    [string] $InstallRoot = 'C:\dev\monolist-deps',
    [string] $QtVersion   = '6.8.1',
    [string] $QtArch      = 'win64_msvc2022_64'
)

$ErrorActionPreference = 'Stop'

function Write-Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Skip($text) { Write-Host "    $text (already present, skipping)" -ForegroundColor DarkGray }

# ---------------------------------------------------------------- prerequisites

Write-Step 'Checking Python'
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'Python was not found on PATH. Install Python 3.10+ and re-run.' }
Write-Ok "python: $python"

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null

# ------------------------------------------------------------ pip-based tooling

Write-Step 'Installing build tools and yt-dlp via pip'
foreach ($pkg in @('cmake', 'ninja', 'yt-dlp', 'aqtinstall', 'py7zr')) {
    & $python -m pip install --upgrade --quiet $pkg
    if ($LASTEXITCODE -ne 0) { throw "pip install $pkg failed." }
    Write-Ok "$pkg"
}

# ------------------------------------------------------------------------- Qt 6

$qtRoot = Join-Path $InstallRoot 'Qt'
$qtPrefix = Join-Path $qtRoot "$QtVersion\msvc2022_64"

Write-Step "Installing Qt $QtVersion ($QtArch)"
if (Test-Path (Join-Path $qtPrefix 'bin\qmake.exe')) {
    Write-Skip $qtPrefix
} else {
    Write-Host '    Downloading Qt (~1.5 GB, this takes a while)...'
    & $python -m aqt install-qt windows desktop $QtVersion $QtArch `
        --outputdir $qtRoot `
        --modules qtimageformats qtsvg
    if ($LASTEXITCODE -ne 0) { throw 'aqt install-qt failed.' }
    Write-Ok $qtPrefix
}

# ----------------------------------------------------------------------- libmpv

$mpvRoot = Join-Path $InstallRoot 'libmpv'

Write-Step 'Installing libmpv SDK'
if (Test-Path (Join-Path $mpvRoot 'include\mpv\client.h')) {
    Write-Skip $mpvRoot
} else {
    # Asset names carry a build date, so resolve the current one from the API
    # rather than pinning a URL that will 404 in a few months.
    Write-Host '    Resolving latest mpv-dev release...'
    $api = 'https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest'
    $release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'monolist-setup' }
    $asset = $release.assets | Where-Object { $_.name -like 'mpv-dev-x86_64-*.7z' } | Select-Object -First 1
    if (-not $asset) { throw 'Could not find an mpv-dev x86_64 asset in the latest release.' }

    $archive = Join-Path $env:TEMP $asset.name
    Write-Host "    Downloading $($asset.name) ..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive -UseBasicParsing

    New-Item -ItemType Directory -Force -Path $mpvRoot | Out-Null
    & $python -c "import py7zr,sys; py7zr.SevenZipFile(sys.argv[1]).extractall(sys.argv[2])" $archive $mpvRoot
    if ($LASTEXITCODE -ne 0) { throw 'Extracting the libmpv archive failed.' }
    Remove-Item $archive -Force
    Write-Ok $mpvRoot
}

# MSVC cannot link the MinGW-style .dll.a, so generate an import library from
# the DLL's export table when one is not already provided.
$mpvDll = Get-ChildItem -Recurse -Path $mpvRoot -Filter 'libmpv*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
$mpvLib = Get-ChildItem -Recurse -Path $mpvRoot -Filter 'mpv.lib' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($mpvDll -and -not $mpvLib) {
    Write-Step 'Generating mpv.lib import library for MSVC'
    $defFile = Get-ChildItem -Recurse -Path $mpvRoot -Filter '*.def' -ErrorAction SilentlyContinue | Select-Object -First 1
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if ((Test-Path $vcvars) -and $defFile) {
        $outLib = Join-Path $mpvDll.DirectoryName 'mpv.lib'
        cmd /c "`"$vcvars`" && lib /def:`"$($defFile.FullName)`" /out:`"$outLib`" /machine:x64" | Out-Null
        if (Test-Path $outLib) { Write-Ok $outLib } else { Write-Warning 'lib.exe did not produce mpv.lib.' }
    } else {
        Write-Warning 'No .def file or vcvars64.bat found; link against the provided library manually.'
    }
}

# ------------------------------------------------------------------------ mpv / ffmpeg runtime

Write-Step 'Runtime notes'
Write-Host @"
    yt-dlp     installed via pip (on PATH as yt-dlp, or python -m yt_dlp)
    ffmpeg     needed only for download remuxing. The libmpv build above bundles
               the codecs for playback; for downloads, put ffmpeg.exe on PATH.
               Get it from https://www.gyan.dev/ffmpeg/builds/ (release-essentials).
"@

# -------------------------------------------------------------------- next steps

$projectRoot = Split-Path -Parent $PSScriptRoot

Write-Step 'Done — configure and build with'
Write-Host @"

    cmake -S "$projectRoot" -B "$projectRoot\build" ``
          -G Ninja ``
          -DCMAKE_BUILD_TYPE=Release ``
          -DCMAKE_PREFIX_PATH="$qtPrefix" ``
          -DMPV_ROOT="$mpvRoot"

    cmake --build "$projectRoot\build"

    # Copy the Qt and mpv runtime next to the binary before running:
    & "$qtPrefix\bin\windeployqt.exe" --qmldir "$projectRoot" "$projectRoot\build\monolist.exe"

"@ -ForegroundColor Yellow
