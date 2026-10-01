<#
.SYNOPSIS
    Builds Monolist in Release and packs it, with everything it needs, into a
    folder and a zip that run on a Windows PC with nothing else installed.

.DESCRIPTION
    The development build (build-windows.ps1) links its tools in from the
    toolchain folder; this one carries real copies of all of them:

        Monolist\
            monolist.exe
            Qt's and MinGW's runtime DLLs, the Qt plugins and QML modules
                (windeployqt), with QtQuick.Controls as build-windows.ps1 adds it
            libmpv-2.dll            playback
            vulkan-1.dll            the Vulkan loader libmpv links against,
                                    which a PC without a Vulkan driver lacks
            WebView2Loader.dll      opens the Google sign-in window on the
                                    WebView2 engine Windows carries
            tools\yt-dlp\           yt-dlp, unpacked
            tools\ffmpeg.exe, ffprobe.exe, deno.exe
            LICENSES\, THIRD-PARTY-NOTICES.txt, README.txt

    It then checks every program and library in the package for a DLL it
    needs that is neither in the package nor part of Windows, and fails if it
    finds one. The zip goes beside the folder, with its SHA-256; with
    -Installer, a setup program too (packaging\monolist.iss, Inno Setup), with
    its own.

    Needs the toolchain setup-windows.ps1 installs, with -Installer for Inno
    Setup, and for vulkan-1.dll either its -Vulkan or a Vulkan runtime on this
    PC (any current GPU driver has one).

.EXAMPLE
    .\package-windows.ps1                 # build Release, stage, check, zip
    .\package-windows.ps1 -Installer      # and the installer
    .\package-windows.ps1 -SkipBuild      # re-stage the last Release build
#>
[CmdletBinding()]
param(
    [string] $InstallRoot = 'C:\dev\monolist-deps',
    [string] $BuildRoot   = 'C:\dev\monolist-build',
    [string] $QtVersion   = '6.11.2',
    [string] $OutDir      = '',
    [switch] $SkipBuild,
    [switch] $NoZip,
    [switch] $Installer
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

function Write-Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }

$source    = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$packaging = Join-Path $source 'packaging'
$release   = Join-Path $BuildRoot 'release'
if (-not $OutDir) { $OutDir = Join-Path $BuildRoot 'package' }

$qtRoot   = Join-Path $InstallRoot 'Qt'
$qtPrefix = Join-Path $qtRoot "$QtVersion\mingw_64"
$mingwBin = Join-Path $qtRoot 'Tools\mingw1310_64\bin'
$objdump  = Join-Path $mingwBin 'objdump.exe'
$env:PATH = @($mingwBin, (Join-Path $qtPrefix 'bin'), $env:PATH) -join ';'

# ---------------------------------------------------------------- build

if (-not $SkipBuild) {
    Write-Step 'Release build'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-windows.ps1') `
        -Config Release -InstallRoot $InstallRoot -BuildRoot $BuildRoot -QtVersion $QtVersion
    if ($LASTEXITCODE -ne 0) { throw 'The Release build failed.' }
}
$exe = Join-Path $release 'monolist.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "No Release build at $exe. Run without -SkipBuild." }

# What the build says it is: version, build number, and whether the tree had
# changes that no commit names.
$buildInfo = Get-Content -LiteralPath (Join-Path $release 'generated\buildinfo.h') -Raw
function Read-Define([string] $name) {
    if ($buildInfo -match ('#define\s+' + $name + '\s+"?([^"\r\n]*)"?')) { return $Matches[1].Trim() }
    return ''
}
$version = Read-Define 'MONOLIST_VERSION'
$build   = Read-Define 'MONOLIST_BUILD_NUMBER'
$commit  = Read-Define 'MONOLIST_COMMIT'
$dirty   = (Read-Define 'MONOLIST_DIRTY') -eq 'true'
# The version as releases are tagged: v0.1.130 is version 0.1, build 130.
$fullVersion = "$version.$build"
$suffix  = if ($dirty) { '-dirty' } else { '' }
$name    = "Monolist-$fullVersion-win64$suffix"
if ($dirty) { Write-Warning 'The source tree has uncommitted changes: the package is marked -dirty.' }

# ---------------------------------------------------------------- stage

Write-Step "Staging $name"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path $OutDir 'Monolist'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage

# Qt, its plugins and QML modules, and MinGW's runtime, into a folder that
# holds nothing else, so nothing of the build tree comes along.
& windeployqt --qmldir $source --no-translations --compiler-runtime (Join-Path $stage 'monolist.exe') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }

# SQLite is the one database: the other drivers windeployqt adds (Oracle,
# PostgreSQL, Firebird, Mimer, ODBC) would each want a client library the
# package does not have, and nothing loads them.
Get-ChildItem -LiteralPath (Join-Path $stage 'sqldrivers') -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'qsqlite.dll' } | Remove-Item -Force

# The one tool tip every ToolTip.text shows is built from QtQuick.Controls,
# which windeployqt leaves out (see build-windows.ps1).
$controlsFrom = Join-Path $qtPrefix 'qml\QtQuick\Controls'
$controlsTo = Join-Path $stage 'qml\QtQuick\Controls'
New-Item -ItemType Directory -Force -Path $controlsTo | Out-Null
foreach ($file in 'qmldir', 'plugins.qmltypes', 'qtquickcontrols2plugin.dll') {
    Copy-Item -LiteralPath (Join-Path $controlsFrom $file) -Destination $controlsTo -Force
}

# Playback, and the loader it links against.
$mpv = Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'libmpv') -Recurse -Filter 'libmpv*.dll' | Select-Object -First 1
if (-not $mpv) { throw 'No libmpv DLL in the toolchain.' }
Copy-Item -LiteralPath $mpv.FullName -Destination $stage
# LunarG's (setup-windows.ps1 -Vulkan) where there is one, else this PC's.
$vulkan = Join-Path $InstallRoot 'bin\vulkan-1.dll'
if (-not (Test-Path -LiteralPath $vulkan)) { $vulkan = Join-Path $env:WINDIR 'System32\vulkan-1.dll' }
if (Test-Path -LiteralPath $vulkan) {
    Copy-Item -LiteralPath $vulkan -Destination $stage
} else {
    Write-Warning 'No Vulkan runtime on this PC to bundle: the package will need one on the PC it runs on.'
}
# The Google sign-in window's loader (WebView2 SDK, unpacked by hand from
# NuGet's Microsoft.Web.WebView2 into webview2\sdk); the engine itself is
# Windows' own. Without it the app signs in by cookie import only.
$webView2Sdk = Join-Path $InstallRoot 'webview2\sdk'
$webView2Loader = Join-Path $webView2Sdk 'build\native\x64\WebView2Loader.dll'
if (Test-Path -LiteralPath $webView2Loader) {
    Copy-Item -LiteralPath $webView2Loader -Destination $stage
} else {
    Write-Warning 'No WebView2 SDK: the package will sign in by cookie import only.'
}

# The tools, as real copies (the development build links them in).
$tools = Join-Path $stage 'tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item -LiteralPath (Join-Path $InstallRoot 'yt-dlp') -Destination (Join-Path $tools 'yt-dlp') -Recurse
foreach ($tool in 'ffmpeg.exe', 'ffprobe.exe', 'deno.exe') {
    $from = Join-Path $InstallRoot "bin\$tool"
    if (-not (Test-Path -LiteralPath $from)) { throw "$tool is not in the toolchain: run setup-windows.ps1." }
    Copy-Item -LiteralPath $from -Destination $tools
}

# ---------------------------------------------------------------- notices

Write-Step 'Licences'
$licenses = Join-Path $stage 'LICENSES'
Copy-Item -LiteralPath (Join-Path $packaging 'LICENSES') -Destination $licenses -Recurse
Copy-Item -LiteralPath (Join-Path $packaging 'README.txt') -Destination $stage
# FFmpeg's own, from the archive it came in; yt-dlp's bundled Python and
# libraries, from its own folder.
$ffmpegZip = Join-Path $InstallRoot 'downloads\ffmpeg-master-latest-win64-gpl.zip'
if (Test-Path -LiteralPath $ffmpegZip) {
    $scratch = Join-Path $OutDir 'ffmpeg-license'
    New-Item -ItemType Directory -Force -Path $scratch | Out-Null
    & tar.exe -xf $ffmpegZip -C $scratch 'ffmpeg-master-latest-win64-gpl/LICENSE.txt'
    Copy-Item -LiteralPath (Join-Path $scratch 'ffmpeg-master-latest-win64-gpl\LICENSE.txt') -Destination (Join-Path $licenses 'FFmpeg-LICENSE.txt')
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
$ytdlpThirdParty = Join-Path $InstallRoot 'yt-dlp\_internal\THIRD_PARTY_LICENSES.txt'
if (Test-Path -LiteralPath $ytdlpThirdParty) {
    Copy-Item -LiteralPath $ytdlpThirdParty -Destination (Join-Path $licenses 'yt-dlp-THIRD_PARTY_LICENSES.txt')
}

function First-Line([string] $program, [string[]] $arguments) {
    try { return ((& $program @arguments 2>$null) | Select-Object -First 1) -replace '\s+Copyright.*$', '' }
    catch { return 'unknown' }
}
$ytdlpVersion  = First-Line (Join-Path $tools 'yt-dlp\yt-dlp.exe') @('--version')
$ffmpegVersion = First-Line (Join-Path $tools 'ffmpeg.exe') @('-version')
$denoVersion   = First-Line (Join-Path $tools 'deno.exe') @('--version')
$mpvVersion    = (Get-Item -LiteralPath $mpv.FullName).VersionInfo.ProductVersion
$vulkanVersion = if (Test-Path -LiteralPath $vulkan) { (Get-Item -LiteralPath $vulkan).VersionInfo.FileVersion } else { 'not bundled' }
$webView2Version = if (Test-Path -LiteralPath $webView2Loader) { (Get-Item -LiteralPath $webView2Loader).VersionInfo.FileVersion } else { 'not bundled' }
if (Test-Path -LiteralPath (Join-Path $webView2Sdk 'LICENSE.txt')) {
    Copy-Item -LiteralPath (Join-Path $webView2Sdk 'LICENSE.txt') -Destination (Join-Path $licenses 'WebView2-BSD-3-Clause.txt')
}

$notices = @"
Monolist $version (build $build, commit $commit)
https://github.com/droidboy08-hub/Monolist

Monolist is MIT licensed (LICENSES\Monolist-MIT.txt). This package also
contains the following, each under its own licence. The source code of each
is available where it says; for the GPL and LGPL components, the corresponding
source is at those addresses, and on request from the Monolist project.

Qt $QtVersion (MinGW 64-bit): the Qt Company and contributors
  LGPL-3.0 (LICENSES\LGPL-3.0.txt, which adds to LICENSES\GPL-3.0.txt).
  Linked dynamically; the Qt DLLs in this folder may be replaced by your own.
  Source: https://download.qt.io/official_releases/qt/

MinGW-w64 runtime (libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll)
  GCC runtime libraries: GPL-3.0 with the GCC Runtime Library Exception 3.1
  (LICENSES\GPL-3.0.txt, LICENSES\GCC-Runtime-Library-Exception-3.1.txt).
  winpthreads: MIT-style (LICENSES\mingw-w64-winpthreads.txt).
  Source: https://gcc.gnu.org/ and https://www.mingw-w64.org/

libmpv ($mpvVersion, libmpv-2.dll): the mpv developers, built by shinchiro
  GPL-3.0 as built, with the libraries it includes (FFmpeg among them)
  (LICENSES\GPL-3.0.txt).
  Source: https://github.com/mpv-player/mpv and
  https://github.com/shinchiro/mpv-winbuild-cmake

Vulkan loader ($vulkanVersion, vulkan-1.dll): The Khronos Group and LunarG
  Apache-2.0 (LICENSES\Apache-2.0.txt).
  Source: https://github.com/KhronosGroup/Vulkan-Loader

WebView2 loader ($webView2Version, WebView2Loader.dll): Microsoft Corporation
  BSD 3-Clause (LICENSES\WebView2-BSD-3-Clause.txt). It opens the Google
  sign-in window on the Microsoft Edge WebView2 Runtime that Windows carries,
  which is not part of this package.
  Source: https://www.nuget.org/packages/Microsoft.Web.WebView2

FFmpeg ($ffmpegVersion; tools\ffmpeg.exe, tools\ffprobe.exe): the FFmpeg developers,
  built by yt-dlp's FFmpeg-Builds
  GPL-3.0 as built (LICENSES\FFmpeg-LICENSE.txt).
  Source: https://ffmpeg.org/ and https://github.com/yt-dlp/FFmpeg-Builds

yt-dlp ($ytdlpVersion; tools\yt-dlp\): the yt-dlp authors
  The Unlicense (LICENSES\yt-dlp-Unlicense.txt); the Python runtime and
  libraries bundled with it under their own licences
  (LICENSES\yt-dlp-THIRD_PARTY_LICENSES.txt).
  Source: https://github.com/yt-dlp/yt-dlp

Deno ($denoVersion; tools\deno.exe): the Deno authors
  MIT (LICENSES\Deno-MIT.txt).
  Source: https://github.com/denoland/deno

Archivo (the interface's typeface, built into monolist.exe): the Archivo
  Project Authors
  SIL Open Font License 1.1 (LICENSES\Archivo-OFL-1.1.txt).

The recommendation data Monolist can download from Settings is not part of
this package, and has its own licence (CC BY-NC), shown where it is offered.
"@
Set-Content -LiteralPath (Join-Path $stage 'THIRD-PARTY-NOTICES.txt') -Value $notices -Encoding UTF8

# ---------------------------------------------------------------- check

Write-Step 'Checking that nothing is missing'
# Every DLL a program or library in the package loads when it starts, with
# where it is found: in the package, or in Windows. The Windows loader looks
# in the program's own folder first, then in System32; a DLL that is in
# neither on a clean PC is one this package forgot.
$windows = Join-Path $env:WINDIR 'System32'
# Present on this PC, but installed with Visual Studio's runtime or a driver
# rather than with Windows, so not on every PC.
$notWindows = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                'concrt140.dll', 'vccorlib140.dll', 'vulkan-1.dll', 'opencl.dll', 'nvcuda.dll')
$inPackage = @{}
Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.dll', '.pyd', '.exe' } |
    ForEach-Object { $inPackage[$_.Name.ToLowerInvariant()] = $true }
$missing = @{}
$scanned = 0
Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.dll', '.pyd', '.exe' } | ForEach-Object {
    $file = $_
    $scanned++
    $imports = & $objdump -p $file.FullName 2>$null | Select-String -Pattern '^\s*DLL Name:\s*(.+)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value.Trim().ToLowerInvariant() }
    foreach ($dll in $imports) {
        if ($inPackage.ContainsKey($dll)) { continue }
        if ($dll.StartsWith('api-ms-win-') -or $dll.StartsWith('ext-ms-')) { continue }
        if (($notWindows -notcontains $dll) -and (Test-Path -LiteralPath (Join-Path $windows $dll))) { continue }
        if (-not $missing.ContainsKey($dll)) { $missing[$dll] = @() }
        $missing[$dll] += $file.FullName.Substring($stage.Length + 1)
    }
}
if ($missing.Count -gt 0) {
    foreach ($entry in $missing.GetEnumerator()) {
        Write-Host ("  missing {0}, needed by {1}" -f $entry.Key, (($entry.Value | Select-Object -First 3) -join ', ')) -ForegroundColor Red
    }
    throw "$($missing.Count) DLL(s) needed by the package are neither in it nor part of Windows."
}
Write-Host "  $scanned programs and libraries checked: every DLL they load is in the package or in Windows." -ForegroundColor Green

# A call the optimiser decided could never happen comes out as a call to the
# image's lowest address, which crashes the moment it is reached. GCC does
# that to a call through an interface it thinks nothing implements (a COM
# interface declared in an anonymous namespace, say), and only in an
# optimised build, so the Debug build the tests run on never shows it.
$bogus = & $objdump -d --no-show-raw-insn (Join-Path $stage 'monolist.exe') |
    Select-String -Pattern '(call|jmp)\s+0x100000000$'
if ($bogus) {
    $bogus | Select-Object -First 5 | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
    throw "monolist.exe calls nowhere in $(@($bogus).Count) place(s): code the optimiser took for unreachable."
}
Write-Host '  monolist.exe has no calls the optimiser turned into jumps to nowhere.' -ForegroundColor Green

# ---------------------------------------------------------------- zip

$size = (Get-ChildItem -LiteralPath $stage -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("  {0:N0} files, {1:N0} MB" -f (Get-ChildItem -LiteralPath $stage -Recurse -File).Count, ($size / 1MB))
if (-not $NoZip) {
    Write-Step 'Zip'
    $zip = Join-Path $OutDir "$name.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    # bsdtar writes a zip that Explorer opens; the folder inside is "Monolist".
    & tar.exe -a -c -f $zip -C $OutDir 'Monolist'
    if ($LASTEXITCODE -ne 0) { throw 'Could not write the zip.' }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$zip.sha256" -Value "$hash  $name.zip" -Encoding ASCII
    Write-Host ("  {0} ({1:N0} MB)`n  SHA-256 {2}" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB), $hash) -ForegroundColor Green
}

# ---------------------------------------------------------------- installer

if ($Installer) {
    Write-Step 'Installer'
    $iscc = @((Join-Path $InstallRoot 'innosetup\ISCC.exe'),
              (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
              (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $iscc) { throw 'Inno Setup is not installed: run setup-windows.ps1 -Installer.' }
    $setupName = "Monolist-$fullVersion-setup-win64$suffix"
    $setup = Join-Path $OutDir "$setupName.exe"
    if (Test-Path -LiteralPath $setup) { Remove-Item -LiteralPath $setup -Force }
    & $iscc /Q "/DAppVersion=$fullVersion" "/DSourceDir=$stage" "/DOutputDir=$OutDir" "/DOutputName=$setupName" `
        "/DIconFile=$(Join-Path $packaging 'monolist.ico')" (Join-Path $packaging 'monolist.iss')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $setup)) { throw 'Inno Setup could not build the installer.' }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()
    Set-Content -LiteralPath "$setup.sha256" -Value "$hash  $setupName.exe" -Encoding ASCII
    Write-Host ("  {0} ({1:N0} MB)`n  SHA-256 {2}" -f $setup, ((Get-Item -LiteralPath $setup).Length / 1MB), $hash) -ForegroundColor Green
}
Write-Host "`nPackaged $stage" -ForegroundColor Green
