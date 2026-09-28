# Runs monolist.exe once with scratch data/download dirs and timestamped logs.
# Usage: run.ps1 -Log <file> -AppArgs @('--play','<id>','30') [-Exe <path>] [-MpvLog v|debug|trace|warn|''] [-DataDir <dir>]
# The log gets one line per message: "<seconds since process start> <message>".
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [Parameter(Mandatory = $true)][string[]]$AppArgs,
    [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$MpvLog = 'v',
    [string]$DataDir = '',
    [int]$TimeoutSec = 900,
    # Instrumented build only: mpv's own timestamped log-file, and its msg-level.
    [string]$MpvLogFile = '',
    [string]$MsgLevel = 'all=v,curl=debug'
)
if ($MpvLogFile) { $env:MONOLIST_MPV_LOGFILE = $MpvLogFile; $env:MONOLIST_MPV_MSGLEVEL = $MsgLevel }
else { Remove-Item Env:MONOLIST_MPV_LOGFILE -ErrorAction SilentlyContinue; Remove-Item Env:MONOLIST_MPV_MSGLEVEL -ErrorAction SilentlyContinue }
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $DataDir) { $DataDir = Join-Path $bench 'data\run' }
New-Item -ItemType Directory -Force $DataDir | Out-Null
$env:MONOLIST_DATA_DIR = $DataDir
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $bench 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time process} %{message}'
if ($MpvLog) { $env:MONOLIST_MPV_LOG = $MpvLog } else { Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue }
$quoted = $AppArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
$out = "$Log.stdout"
# Other agents share this VM: wait (up to 90 s) until no other monolist.exe runs,
# then record every foreign monolist / yt-dlp / curl / deno process seen during the run.
$foreignNames = 'monolist', 'yt-dlp', 'curl', 'deno', 'ffmpeg', 'cc1plus', 'ninja'
$waitClock = [Diagnostics.Stopwatch]::StartNew()
while ((Get-Process -Name monolist -ErrorAction SilentlyContinue) -and $waitClock.Elapsed.TotalSeconds -lt 90) { Start-Sleep -Milliseconds 500 }
$waitedSec = [Math]::Round($waitClock.Elapsed.TotalSeconds, 1)
$p = Start-Process -FilePath $Exe -ArgumentList $quoted -RedirectStandardError $Log -RedirectStandardOutput $out -PassThru -WindowStyle Minimized
$foreign = @{}
$runClock = [Diagnostics.Stopwatch]::StartNew()
$timedOut = $false
while (-not $p.WaitForExit(1000)) {
    foreach ($q in (Get-Process -Name $foreignNames -ErrorAction SilentlyContinue)) {
        if ($q.Id -ne $p.Id) { $foreign["$($q.ProcessName):$($q.Id)"] = $true }
    }
    if ($runClock.Elapsed.TotalSeconds -gt $TimeoutSec) { try { $p.Kill() } catch {}; $timedOut = $true; break }
}
# yt-dlp/deno started by this run are its own children, not foreign; they are listed but marked.
$env_ = [ordered]@{ waitedForOthersSec = $waitedSec; foreignSeen = @($foreign.Keys); timedOut = $timedOut; start = (Get-Date).ToString('o') }
$env_ | ConvertTo-Json -Compress | Set-Content -Encoding utf8 "$Log.env.json"
if ($timedOut) { Write-Output "TIMEOUT" }
Write-Output ("exit " + $p.ExitCode + " foreign=" + ($foreign.Keys -join ','))
