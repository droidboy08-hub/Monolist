<#
.SYNOPSIS
    Installs everything Monolist needs to build and run on Windows.

.DESCRIPTION
    Everything lands under -InstallRoot (default C:\dev\monolist-deps). Nothing is
    installed system-wide, no installer runs, and PATH is left alone:
    build-windows.ps1 puts these tools on PATH for its own process only.

        Qt\<version>\mingw_64   Qt for MinGW 64-bit, plus qtimageformats (WebP artwork)
        Qt\Tools\               MinGW 13.1, CMake, Ninja
        libmpv\                 libmpv SDK, x86_64 (matches the app, not the host)
        bin\                    yt-dlp, ffmpeg, ffprobe, deno
        git\                    PortableGit (skip with -SkipGit)
        downloads\              the archives, kept so a re-run need not fetch them again

    Qt and its tools come straight from Qt's official online repository
    (download.qt.io): the script reads the repository's package index and checks
    every archive against the SHA-1 published next to it. That is what the Qt
    installer does, without needing a Qt account or a third-party tool that has
    to keep up with changes to the repository layout.

    The app is built as x64 with MinGW, the kit the first working build used. On
    ARM64 Windows that runs under the built-in x64 emulation, because Qt ships
    native ARM64 packages only for MSVC. yt-dlp, FFmpeg and Deno run as separate
    processes, so they use native ARM64 builds when the host is ARM64.

    Deno is not optional: yt-dlp needs a JavaScript runtime to solve YouTube's
    player challenges, and without one the high-quality audio formats disappear.

    Qt is pinned. yt-dlp, FFmpeg and Deno follow their latest releases, because
    YouTube changes constantly and an old yt-dlp simply stops working; pass
    -Update to refresh them. Re-running is safe: finished steps are skipped.

.EXAMPLE
    .\setup-windows.ps1
    .\setup-windows.ps1 -Update               # refresh yt-dlp, FFmpeg and Deno
    .\setup-windows.ps1 -InstallRoot D:\deps
#>
[CmdletBinding()]
param(
    [string] $InstallRoot = 'C:\dev\monolist-deps',
    [string] $QtVersion   = '6.11.2',
    [switch] $Update,
    [switch] $SkipGit
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # the progress bar makes Invoke-WebRequest many times slower
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$isArm64  = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq [Runtime.InteropServices.Architecture]::Arm64
$qtRepo   = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop'
$qtTag    = $QtVersion -replace '\.', ''          # 6.11.2 -> 6112

$qtRoot   = Join-Path $InstallRoot 'Qt'
$qtPrefix = Join-Path $qtRoot "$QtVersion\mingw_64"
$mpvRoot  = Join-Path $InstallRoot 'libmpv'
$binDir   = Join-Path $InstallRoot 'bin'
$gitRoot  = Join-Path $InstallRoot 'git'
$cacheDir = Join-Path $InstallRoot 'downloads'
New-Item -ItemType Directory -Force -Path $InstallRoot, $qtRoot, $binDir, $cacheDir | Out-Null

# ------------------------------------------------------------------- helpers

function Write-Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "    $text" -ForegroundColor Green }
function Write-Skip($text) { Write-Host "    $text (already present, skipping)" -ForegroundColor DarkGray }

$webHeaders = @{ 'User-Agent' = 'monolist-setup' }

function Get-Text([string] $url) {
    $content = (Invoke-WebRequest -Uri $url -UseBasicParsing -Headers $webHeaders).Content
    if ($content -is [byte[]]) { return [Text.Encoding]::UTF8.GetString($content) }
    [string] $content
}

function Invoke-Download([string] $url, [string] $path) {
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing -Headers $webHeaders
            return
        } catch {
            if ($attempt -ge 3) { throw }
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
}

# tar.exe is bsdtar (built into Windows 10 1803+); it reads zip and 7z alike.
function Expand-To([string] $archive, [string] $destination) {
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    & tar.exe -xf $archive -C $destination
    if ($LASTEXITCODE -ne 0) { throw "Extracting $archive failed (tar exit code $LASTEXITCODE)." }
}

# ------------------------------------------------------------ Qt repository

# Installs packages from Qt's online repository: read the folder's Updates.xml,
# fetch every archive a package lists, check it against the .sha1 published
# next to it, and unpack it where the package's Extract operations say
# (@TargetDir@/6.x.y/mingw_64, .../bin, Tools/CMake_64, ...). An archive with
# no operation carries its own install-relative paths and unpacks at the root.
function Install-QtPackages([string] $repoDir, [string[]] $packageNames, [string] $skipArchives = '^$') {
    $index = [xml] (Get-Text "$qtRepo/$repoDir/Updates.xml")
    foreach ($name in $packageNames) {
        $package = $index.Updates.PackageUpdate | Where-Object { $_.Name -eq $name } | Select-Object -First 1
        if (-not $package) { throw "Package $name is not listed in $qtRepo/$repoDir/Updates.xml" }
        $targets = @{}
        foreach ($operation in @($package.Operations.Operation)) {
            if (-not $operation -or $operation.name -ne 'Extract') { continue }
            $arguments = @($operation.Argument)
            $targets[[string] $arguments[1]] = ([string] $arguments[0]).Replace('@TargetDir@', $qtRoot).Replace('/', '\')
        }
        $archives = "$($package.DownloadableArchives)" -split ',\s*' | Where-Object { $_ -and $_ -notmatch $skipArchives }
        foreach ($archive in $archives) {
            $url  = "$qtRepo/$repoDir/$name/$($package.Version)$archive"
            $sha1 = (Get-Text "$url.sha1").Trim().Split()[0].ToUpperInvariant()
            $path = Join-Path $cacheDir $archive
            $cached = (Test-Path -LiteralPath $path) -and ((Get-FileHash -Algorithm SHA1 -LiteralPath $path).Hash -eq $sha1)
            if (-not $cached) {
                Write-Host "    downloading $archive"
                Invoke-Download $url $path
                if ((Get-FileHash -Algorithm SHA1 -LiteralPath $path).Hash -ne $sha1) {
                    Remove-Item -LiteralPath $path -Force
                    throw "SHA-1 mismatch for $archive"
                }
            }
            $destination = if ($targets.ContainsKey($archive)) { $targets[$archive] } else { $qtRoot }
            Expand-To $path $destination
            Write-Ok ("{0} ({1:N1} MB, SHA-1 verified)" -f $archive, ((Get-Item -LiteralPath $path).Length / 1MB))
        }
    }
}

# ---------------------------------------------------------- GitHub releases

function Get-Release([string] $repo) {
    Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers $webHeaders
}

function Get-Asset($release, [string] $pattern) {
    $asset = $release.assets | Where-Object { $_.name -match $pattern } | Select-Object -First 1
    if (-not $asset) { throw "No asset matching '$pattern' in $($release.html_url)" }
    $asset
}

$sha256Pattern = '(?<![0-9a-fA-F])[0-9a-fA-F]{64}(?![0-9a-fA-F])'

# Finds the SHA-256 for $name in checksum text: "<hash>  <name>" lines, a
# Markdown table row, or (with -SingleAsset) a file that covers only $name.
function Find-Sha256([string] $text, [string] $name, [switch] $SingleAsset) {
    $token = '(?<![\w.-])' + [regex]::Escape($name) + '(?![\w.-])'
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -notmatch $token) { continue }
        $m = [regex]::Match($line, $sha256Pattern)
        if ($m.Success) { return $m.Value }
    }
    if ($SingleAsset) {
        $m = [regex]::Match($text, $sha256Pattern)
        if ($m.Success) { return $m.Value }
    }
    $null
}

# The publisher's own checksum for an asset, from wherever the project keeps it:
# a <name>.sha256sum file, a shared SUMS list, or the release notes. GitHub's
# upload digest is the fallback.
function Get-ReleaseHash($release, [string] $name) {
    $dedicated = $release.assets | Where-Object { $_.name -in @("$name.sha256sum", "$name.sha256") } | Select-Object -First 1
    if ($dedicated) {
        $hash = Find-Sha256 (Get-Text $dedicated.browser_download_url) $name -SingleAsset
        if ($hash) { return $hash }
    }
    $lists = $release.assets | Where-Object { $_.name -match '(?i)(sha2?-?256|checksum)' -and $_.name -notmatch '\.(sig|asc|sha256sum)$' }
    foreach ($list in $lists) {
        $hash = Find-Sha256 (Get-Text $list.browser_download_url) $name
        if ($hash) { return $hash }
    }
    if ($release.body) {
        $hash = Find-Sha256 $release.body $name
        if ($hash) { return $hash }
    }
    $asset = $release.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if ($asset -and $asset.PSObject.Properties['digest'] -and "$($asset.digest)" -match '^sha256:([0-9a-fA-F]{64})$') {
        return $Matches[1]
    }
    $null
}

# Downloads a release asset into the cache, reusing an earlier copy only when it
# still matches, and verifies the SHA-256 when one is known.
function Save-Asset($asset, [string] $sha256) {
    $path = Join-Path $cacheDir $asset.name
    $usable = (Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Length -eq $asset.size)
    if ($usable -and $sha256) {
        $usable = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash -eq $sha256.ToUpperInvariant()
    }
    if (-not $usable) {
        Write-Host ("    downloading {0} ({1:N1} MB)" -f $asset.name, ($asset.size / 1MB))
        Invoke-Download $asset.browser_download_url $path
    }
    if ($sha256) {
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if ($actual -ne $sha256.ToUpperInvariant()) {
            Remove-Item -LiteralPath $path -Force
            throw "SHA-256 mismatch for $($asset.name): expected $sha256, got $actual"
        }
        Write-Ok "$($asset.name): SHA-256 verified"
    } else {
        Write-Warning "$($asset.name): no published checksum, relying on HTTPS only"
    }
    $path
}

# ------------------------------------------------------------------------ Qt

Write-Step "Qt $QtVersion for MinGW 64-bit (download.qt.io)"
if (Test-Path -LiteralPath (Join-Path $qtPrefix 'bin\Qt6Core.dll')) {
    Write-Skip $qtPrefix
} else {
    # The base package bundles qtbase, qtsvg, qtdeclarative, qttools and the
    # MinGW runtime; qtimageformats adds the WebP decoder, since YouTube serves
    # many thumbnails as WebP. The offline documentation is skipped.
    Install-QtPackages "qt6_$qtTag/qt6_$($qtTag)_mingw" @(
        "qt.qt6.$qtTag.win64_mingw",
        "qt.qt6.$qtTag.addons.qtimageformats.win64_mingw"
    ) '^qtdoc-'
    if (-not (Test-Path -LiteralPath (Join-Path $qtPrefix 'bin\Qt6Core.dll'))) { throw "Qt unpacked, but $qtPrefix is incomplete." }
    Write-Ok $qtPrefix
}
# Tells qmake, qtpaths and windeployqt where the installation lives now.
$qtConf = Join-Path $qtPrefix 'bin\qt.conf'
if (-not (Test-Path -LiteralPath $qtConf)) { Set-Content -LiteralPath $qtConf -Value "[Paths]`r`nPrefix=.." -Encoding ASCII }

# ------------------------------------------------------- MinGW, CMake, Ninja

$tools = @(
    @{ Repo = 'tools_mingw1310'; Package = 'qt.tools.win64_mingw1310'; Probe = 'Tools\mingw1310_64\bin\g++.exe' },
    @{ Repo = 'tools_cmake';     Package = 'qt.tools.cmake';           Probe = 'Tools\CMake_64\bin\cmake.exe' },
    @{ Repo = 'tools_ninja';     Package = 'qt.tools.ninja';           Probe = 'Tools\Ninja\ninja.exe' }
)
foreach ($tool in $tools) {
    Write-Step "$($tool.Package) (download.qt.io)"
    $probe = Join-Path $qtRoot $tool.Probe
    if (Test-Path -LiteralPath $probe) { Write-Skip $probe; continue }
    Install-QtPackages $tool.Repo @($tool.Package)
    if (-not (Test-Path -LiteralPath $probe)) { throw "$($tool.Package) unpacked, but $probe is missing." }
    Write-Ok $probe
}

# -------------------------------------------------------------------- libmpv

Write-Step 'libmpv SDK (x86_64, to match the x64 app)'
if (Test-Path -LiteralPath (Join-Path $mpvRoot 'include\mpv\client.h')) {
    Write-Skip $mpvRoot
} else {
    $release = Get-Release 'shinchiro/mpv-winbuild-cmake'
    # Plain x86_64, not x86_64-v3: v3 needs AVX2, which x64 emulation may not offer.
    $asset = Get-Asset $release '^mpv-dev-x86_64-[0-9]{8}-git-[0-9a-f]+\.7z$'
    Expand-To (Save-Asset $asset (Get-ReleaseHash $release $asset.name)) $mpvRoot
    Write-Ok "$mpvRoot ($($release.tag_name))"
}

# ------------------------------------------------------------- runtime tools

Write-Step 'yt-dlp (search, stream resolution, downloads)'
$ytdlpExe = Join-Path $binDir 'yt-dlp.exe'
if ((Test-Path -LiteralPath $ytdlpExe) -and -not $Update) {
    Write-Skip $ytdlpExe
} else {
    $release = Get-Release 'yt-dlp/yt-dlp'
    $name = if ($isArm64) { 'yt-dlp_arm64.exe' } else { 'yt-dlp.exe' }
    $asset = Get-Asset $release ('^' + [regex]::Escape($name) + '$')
    Copy-Item -LiteralPath (Save-Asset $asset (Get-ReleaseHash $release $name)) -Destination $ytdlpExe -Force
    Write-Ok "$ytdlpExe ($($release.tag_name))"
}

Write-Step 'FFmpeg (yt-dlp''s builds: audio extraction, tags, cover art)'
$ffmpegExe = Join-Path $binDir 'ffmpeg.exe'
if ((Test-Path -LiteralPath $ffmpegExe) -and -not $Update) {
    Write-Skip $ffmpegExe
} else {
    $release = Get-Release 'yt-dlp/FFmpeg-Builds'
    $name = if ($isArm64) { 'ffmpeg-master-latest-winarm64-gpl.zip' } else { 'ffmpeg-master-latest-win64-gpl.zip' }
    $archive = Save-Asset (Get-Asset $release ('^' + [regex]::Escape($name) + '$')) (Get-ReleaseHash $release $name)
    $staging = Join-Path $cacheDir 'ffmpeg-staging'
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    Expand-To $archive $staging
    foreach ($exe in 'ffmpeg.exe', 'ffprobe.exe') {
        $found = Get-ChildItem -LiteralPath $staging -Recurse -Filter $exe | Select-Object -First 1
        if (-not $found) { throw "$exe not found in $name" }
        Copy-Item -LiteralPath $found.FullName -Destination $binDir -Force
    }
    Remove-Item -LiteralPath $staging -Recurse -Force
    Write-Ok "$ffmpegExe, ffprobe.exe"
}

Write-Step 'Deno (the JavaScript runtime yt-dlp needs for YouTube)'
$denoExe = Join-Path $binDir 'deno.exe'
if ((Test-Path -LiteralPath $denoExe) -and -not $Update) {
    Write-Skip $denoExe
} else {
    $release = Get-Release 'denoland/deno'
    $name = if ($isArm64) { 'deno-aarch64-pc-windows-msvc.zip' } else { 'deno-x86_64-pc-windows-msvc.zip' }
    $archive = Save-Asset (Get-Asset $release ('^' + [regex]::Escape($name) + '$')) (Get-ReleaseHash $release $name)
    Expand-To $archive $binDir   # the archive holds deno.exe alone
    Write-Ok "$denoExe ($($release.tag_name))"
}

# ----------------------------------------------------------------------- git

$gitExe = Join-Path $gitRoot 'cmd\git.exe'
if (-not $SkipGit) {
    Write-Step 'PortableGit (no installer, nothing registered)'
    if (Test-Path -LiteralPath $gitExe) {
        Write-Skip $gitExe
    } else {
        $release = Get-Release 'git-for-windows/git'
        $pattern = if ($isArm64) { '^PortableGit-[0-9.]+-arm64\.7z\.exe$' } else { '^PortableGit-[0-9.]+-64-bit\.7z\.exe$' }
        $asset = Get-Asset $release $pattern
        $archive = Save-Asset $asset (Get-ReleaseHash $release $asset.name)
        # PortableGit is a 7z self-extractor, run here only after its SHA-256
        # checked out. tar cannot stand in: the ARM64 build uses an LZMA filter
        # libarchive does not support.
        New-Item -ItemType Directory -Force -Path $gitRoot | Out-Null
        & $archive -y "-o$gitRoot" | Out-Null
        if (-not (Test-Path -LiteralPath $gitExe)) { throw "PortableGit did not unpack to $gitRoot" }
        Write-Ok "$gitExe ($($release.tag_name))"
    }
}

# ------------------------------------------------------------------- summary

Write-Step 'Installed'
$report = [ordered]@{
    'Qt'     = $qtPrefix
    'g++'    = (& (Join-Path $qtRoot 'Tools\mingw1310_64\bin\g++.exe') --version | Select-Object -First 1)
    'CMake'  = (& (Join-Path $qtRoot 'Tools\CMake_64\bin\cmake.exe') --version | Select-Object -First 1)
    'Ninja'  = (& (Join-Path $qtRoot 'Tools\Ninja\ninja.exe') --version)
    'libmpv' = (Get-ChildItem -LiteralPath $mpvRoot -Recurse -Filter 'libmpv*.dll' | Select-Object -First 1).Name
    'yt-dlp' = (& $ytdlpExe --version)
    'FFmpeg' = ((& $ffmpegExe -version | Select-Object -First 1) -replace '\s+Copyright.*$', '')
    'Deno'   = (& $denoExe --version | Select-Object -First 1)
}
if (-not $SkipGit) { $report['Git'] = (& $gitExe --version) }
foreach ($entry in $report.GetEnumerator()) { Write-Host ("    {0,-7} {1}" -f $entry.Key, $entry.Value) }

Write-Host "`nNext:  .\build-windows.ps1        (add -Run to start the app)" -ForegroundColor Yellow
