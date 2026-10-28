<#
.SYNOPSIS
    The in-app update, end to end, on this PC: a copy installed by the real
    setup, and a portable one, each updated by Monolist itself from a
    stand-in for GitHub on 127.0.0.1.

.DESCRIPTION
    For the release workflow's Windows machine (release-windows.yml), after
    package-windows.ps1 -Installer: never on the owner's PC, where a setup
    shares the real Monolist's AppId (CLAUDE.md, "Traps met before").

    1. Installed for this user. An older setup is made from the same package
       (ISCC /DAppVersion=0.1.1) and installed quietly into a folder with a
       space in its name. That copy, told it is 0.1.1 (MONOLIST_TEST_VERSION,
       scratch libraries only), runs --update-run against a feed offering
       this build's setup: it must find itself the setup's from the real
       uninstall key, download and check the setup, hand over and quit; the
       helper runs the setup quietly with /RELAUNCH; the setup starts the new
       copy, whose launch must say "Monolist is now <version>". The uninstall
       entry must then name the same folder and the new version.
    2. Installed for everyone (/ALLUSERS), the same way, when this PC lets a
       setup do that without asking (GitHub's machines do).
    3. Portable: the zip unpacked into a folder with a space in its name,
       updated from the zip: unpacked beside it, the folders swapped, the new
       copy started, the old folder deleted (it holds nothing the new one
       lacks).

    Every copy runs on a scratch library (MONOLIST_DATA_DIR) and download
    folder. Prints "update-e2e: N checks, M failed" and fails when M > 0.
#>
[CmdletBinding()]
param(
    [string] $Package = 'C:\dev\monolist-build\package',
    [string] $InstallRoot = 'C:\dev\monolist-deps',
    [string] $Work = '',
    [int] $Port = 18741
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# A long path: the temporary folder can be named the 8.3 way (RUNNER~1).
if (-not $Work) {
    $base = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { (Get-Item -LiteralPath ([IO.Path]::GetTempPath())).FullName }
    $Work = Join-Path $base 'monolist-update-e2e'
}

$script:checks = 0
$script:failed = 0
function Check([bool] $ok, [string] $what, [string] $detail = '') {
    $script:checks++
    if ($ok) {
        Write-Host "update-e2e: ok    $what"
    } else {
        $script:failed++
        Write-Host "update-e2e: FAIL  $what  -- $detail" -ForegroundColor Red
    }
}

$uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{8E0B6F4A-5C2D-4B7E-9A31-6D2F0C8E4B19}_is1'
function Get-Entry([string] $hive) {
    $path = "${hive}:\$uninstallKey"
    if (Test-Path -LiteralPath $path) { return Get-ItemProperty -LiteralPath $path }
    return $null
}

function Same-Folder([string] $a, [string] $b) {
    if (-not $a -or -not $b) { return $false }
    return ($a.TrimEnd('\').ToLowerInvariant() -eq $b.TrimEnd('\').ToLowerInvariant())
}

function Stop-Scratch {
    Get-Process -Name monolist -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null

$stage = Join-Path $Package 'Monolist'
$setup = Get-ChildItem -LiteralPath $Package -Filter 'Monolist-*-setup-win64.exe' | Select-Object -First 1
$zip = Get-ChildItem -LiteralPath $Package -Filter 'Monolist-*-win64.zip' | Select-Object -First 1
if (-not $setup -or -not $zip) { throw "No setup or zip in ${Package}: run package-windows.ps1 -Installer first." }
$version = [regex]::Match($setup.Name, '^Monolist-(.+)-setup-win64\.exe$').Groups[1].Value
Write-Host "update-e2e: updating to $version, from 0.1.1"

# The older setup: the same files, another version in its uninstall entry.
$iscc = @((Join-Path $InstallRoot 'innosetup\ISCC.exe'),
          (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
          (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')) |
    Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup is not installed.' }
$packaging = Join-Path (Split-Path -Parent $PSScriptRoot) 'packaging'
$oldDir = Join-Path $Work 'old-setup'
& $iscc /Q '/DAppVersion=0.1.1' "/DSourceDir=$stage" "/DOutputDir=$oldDir" '/DOutputName=old-setup' `
    "/DIconFile=$(Join-Path $packaging 'monolist.ico')" (Join-Path $packaging 'monolist.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup could not build the older setup.' }
$oldSetup = Join-Path $oldDir 'old-setup.exe'

# The stand-in for GitHub: this build's files and their checksums, and a feed
# naming them, on 127.0.0.1.
$feedDir = Join-Path $Work 'feed'
New-Item -ItemType Directory -Force $feedDir | Out-Null
$assets = @()
foreach ($file in $setup, $zip) {
    Copy-Item -LiteralPath $file.FullName -Destination $feedDir
    Copy-Item -LiteralPath ($file.FullName + '.sha256') -Destination $feedDir
    $assets += @{ name = $file.Name; browser_download_url = "http://127.0.0.1:$Port/$($file.Name)"; size = $file.Length }
    $sum = Get-Item -LiteralPath ($file.FullName + '.sha256')
    $assets += @{ name = $sum.Name; browser_download_url = "http://127.0.0.1:$Port/$($sum.Name)"; size = $sum.Length }
}
$feed = @{ tag_name = "v$version"; html_url = "https://github.com/droidboy08-hub/Monolist/releases/tag/v$version"; assets = $assets }
[IO.File]::WriteAllText((Join-Path $feedDir 'latest.json'), ($feed | ConvertTo-Json -Depth 5))
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'Python is needed for the stand-in feed.' }
$server = Start-Process -FilePath $python -ArgumentList '-m', 'http.server', "$Port", '--bind', '127.0.0.1', '--directory', "`"$feedDir`"" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

# Monolist told it is 0.1.1, on a scratch library, through Settings' own
# check, UPDATE and RESTART (--update-run); then what its successor's launch
# said of it (last.txt in the scratch library's updates folder).
function Update-Copy([string] $exe, [string] $name) {
    $data = Join-Path $Work "$name-data"
    $downloads = Join-Path $Work "$name-downloads"
    New-Item -ItemType Directory -Force $data, $downloads | Out-Null
    $env:MONOLIST_DATA_DIR = $data
    $env:MONOLIST_DOWNLOAD_DIR = $downloads
    $env:QT_FORCE_STDERR_LOGGING = '1'
    $env:MONOLIST_MPV_AO = 'null'
    $env:MONOLIST_TEST_VERSION = '0.1.1'
    $log = Join-Path $Work "$name-run.txt"
    $process = Start-Process -FilePath $exe -ArgumentList '--set', 'update.feed', "http://127.0.0.1:$Port/latest.json", '--update-run' `
        -PassThru -NoNewWindow -RedirectStandardError $log -RedirectStandardOutput (Join-Path $Work "$name-out.txt")
    $null = $process.Handle
    $finished = $process.WaitForExit(10 * 60 * 1000)
    Remove-Item Env:MONOLIST_TEST_VERSION -ErrorAction SilentlyContinue
    $text = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { '' }
    Check ($finished -and $process.ExitCode -eq 0) "$name : the copy downloaded the update, handed over and quit" ($text -split "`n" | Select-Object -Last 8 | Out-String)
    $last = Join-Path $data 'updates\last.txt'
    $deadline = (Get-Date).AddMinutes(5)
    while (-not (Test-Path -LiteralPath $last) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    $said = if (Test-Path -LiteralPath $last) { (Get-Content -LiteralPath $last -Raw).Trim() } else { '(nothing)' }
    Check ($said -eq "Monolist is now $version.") "$name : the new copy started, and said it is $version" $said
    return $text
}

try {
    # 1. Installed for this user.
    $inst = Join-Path $Work 'Programs x\Monolist'
    $p = Start-Process -FilePath $oldSetup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', "/DIR=`"$inst`"" -PassThru
    $null = $p.Handle
    if (-not $p.WaitForExit(5 * 60 * 1000)) { Stop-Process -Id $p.Id -Force; throw 'The older setup did not finish.' }
    $entry = Get-Entry 'HKCU'
    Check ($p.ExitCode -eq 0 -and $entry -and (Same-Folder $entry.InstallLocation $inst) -and $entry.DisplayVersion -eq '0.1.1') `
        'the older setup installed this user''s copy' "exit $($p.ExitCode), $($entry.InstallLocation), $($entry.DisplayVersion)"
    $text = Update-Copy (Join-Path $inst 'monolist.exe') 'installed'
    Check ($text -match "this copy is the setup's") 'installed : it took itself for the setup''s, from the real uninstall key' ''
    $entry = Get-Entry 'HKCU'
    Check ($entry -and (Same-Folder $entry.InstallLocation $inst) -and $entry.DisplayVersion -eq $version) `
        'installed : the uninstall entry names the same folder, and the new version' "$($entry.InstallLocation), $($entry.DisplayVersion)"
    $running = @(Get-Process -Name monolist -ErrorAction SilentlyContinue | Where-Object { Same-Folder (Split-Path -Parent $_.Path) $inst })
    Check ($running.Count -ge 1) 'installed : the setup started Monolist again (/RELAUNCH), from its folder' ''
    Stop-Scratch

    # 2. Installed for everyone, where a setup may do that unasked.
    $everyone = Join-Path $Work 'Everyone x\Monolist'
    $p = Start-Process -FilePath $oldSetup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/ALLUSERS', "/DIR=`"$everyone`"" -PassThru
    $null = $p.Handle
    # Waiting on a permission prompt nobody will answer: given up.
    if (-not $p.WaitForExit(5 * 60 * 1000)) {
        Get-Process -Name 'old-setup*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    $entry = Get-Entry 'HKLM'
    if ($p.ExitCode -ne 0 -or -not $entry -or -not (Same-Folder $entry.InstallLocation $everyone)) {
        Write-Host "update-e2e: (an install for everyone needs to ask on this PC: exit $($p.ExitCode); not run)"
    } else {
        $text = Update-Copy (Join-Path $everyone 'monolist.exe') 'everyone'
        Check ($text -match "this copy is the setup's") 'everyone : it took itself for the setup''s, for everyone' ''
        $entry = Get-Entry 'HKLM'
        Check ($entry -and (Same-Folder $entry.InstallLocation $everyone) -and $entry.DisplayVersion -eq $version) `
            'everyone : the uninstall entry names the same folder, and the new version' "$($entry.InstallLocation), $($entry.DisplayVersion)"
        Stop-Scratch
    }

    # 3. Portable.
    $portableRoot = Join-Path $Work 'Portable x'
    Expand-Archive -LiteralPath $zip.FullName -DestinationPath $portableRoot
    $portable = Join-Path $portableRoot 'Monolist'
    $text = Update-Copy (Join-Path $portable 'monolist.exe') 'portable'
    Check ($text -match 'this copy is portable') 'portable : it took itself for a portable copy' ''
    $left = @(Get-ChildItem -LiteralPath $portableRoot -Force | ForEach-Object { $_.Name })
    Check (($left -join ',') -eq 'Monolist') 'portable : the old folder (nothing in it the new one lacks) and the unpacked zip are gone' ($left -join ', ')
    $running = @(Get-Process -Name monolist -ErrorAction SilentlyContinue | Where-Object { Same-Folder (Split-Path -Parent $_.Path) $portable })
    Check ($running.Count -ge 1) 'portable : the new copy runs from the same folder' ''
} finally {
    Stop-Scratch
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host "update-e2e: $script:checks checks, $script:failed failed"
if ($script:failed -gt 0) {
    Get-ChildItem -LiteralPath $Work -Filter '*-run.txt' | ForEach-Object {
        Write-Host "--- $($_.Name)"
        Get-Content -LiteralPath $_.FullName | Select-Object -Last 40 | ForEach-Object { Write-Host "    $_" }
    }
    exit 1
}
exit 0
