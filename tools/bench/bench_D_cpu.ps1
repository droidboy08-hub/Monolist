# Suite D: CPU and memory of the UNMODIFIED app (paper §15.1 "CPU / battery" and "Memory").
# One fresh process per track from playback_tracks.json:  monolist.exe --play <id> <Seconds>
# (audio only, window minimized, mpv log off), plus -IdleRuns launches with no flags
# (app open, nothing playing) as a baseline. The process is sampled every 500 ms:
# TotalProcessorTime, WorkingSet64, PrivateMemorySize64.
#   cpuPct   = CPU time used between WindowFrom and WindowTo seconds after launch,
#              as % of ONE logical core (100 = one core busy).
#   peakWsMB / peakPrivMB = the highest sample over the whole run.
#   wsAtEndMB = working set at WindowTo.
# Repeat after Step 4 with:  .\bench_D_cpu.ps1 -Exe <new monolist.exe> -Tag after
param(
    [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$Tag = 'before',
    [int]$Seconds = 50,
    [int]$WindowFrom = 15,
    [int]$WindowTo = 45,
    [int]$IdleRuns = 4
)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$tracks = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $bench 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue
Remove-Item Env:MONOLIST_MPV_LOGFILE -ErrorAction SilentlyContinue

function Measure-Run([string]$name, [string[]]$appArgs, [bool]$kill) {
    $data = "$bench\data\$name"
    New-Item -ItemType Directory -Force $data | Out-Null
    $env:MONOLIST_DATA_DIR = $data
    $log = "$bench\logs\$name.log"
    while (Get-Process -Name monolist -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }
    $foreign = @{}
    if ($appArgs.Count -gt 0) {
        $p = Start-Process -FilePath $Exe -ArgumentList $appArgs -RedirectStandardError $log -RedirectStandardOutput "$log.stdout" -PassThru -WindowStyle Minimized
    } else {
        $p = Start-Process -FilePath $Exe -RedirectStandardError $log -RedirectStandardOutput "$log.stdout" -PassThru -WindowStyle Minimized
    }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $samples = @()
    while (-not $p.HasExited) {
        try {
            $p.Refresh()
            $samples += [pscustomobject]@{ t = $clock.Elapsed.TotalSeconds; cpuMs = $p.TotalProcessorTime.TotalMilliseconds; ws = $p.WorkingSet64; priv = $p.PrivateMemorySize64 }
        } catch {}
        foreach ($q in (Get-Process -Name 'cc1plus', 'ninja', 'curl', 'deno', 'yt-dlp' -ErrorAction SilentlyContinue)) { $foreign["$($q.ProcessName):$($q.Id)"] = $true }
        if ($kill -and $clock.Elapsed.TotalSeconds -ge $Seconds) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; break }
        if ($clock.Elapsed.TotalSeconds -gt ($Seconds + 60)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; break }
        Start-Sleep -Milliseconds 500
    }
    $a = $samples | Where-Object { $_.t -ge $WindowFrom } | Select-Object -First 1
    $b = $samples | Where-Object { $_.t -le $WindowTo } | Select-Object -Last 1
    $cpuPct = $null
    if ($a -and $b -and $b.t -gt $a.t) { $cpuPct = [Math]::Round(100 * ($b.cpuMs - $a.cpuMs) / (1000 * ($b.t - $a.t)), 1) }
    $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    [pscustomobject]@{
        run = $name
        cpuPct = $cpuPct
        windowSec = if ($a -and $b) { [Math]::Round($b.t - $a.t, 1) } else { $null }
        peakWsMB = [Math]::Round((($samples | Measure-Object ws -Maximum).Maximum) / 1MB, 1)
        peakPrivMB = [Math]::Round((($samples | Measure-Object priv -Maximum).Maximum) / 1MB, 1)
        wsAtEndMB = if ($b) { [Math]::Round($b.ws / 1MB, 1) } else { $null }
        source = if ($text -match 'status=Streaming source=([^\r\n]+)') { $Matches[1].Trim() } else { '' }
        position = if ($text -match 'selftest: position ([^\r\n]+)') { $Matches[1].Trim() } else { '' }
        foreign = @($foreign.Keys) -join ','
    }
}

$rows = @()
foreach ($t in $tracks) {
    $rows += Measure-Run "D_${Tag}_play_$($t.n)_$($t.videoId)" @('--play', $t.videoId, "$Seconds") $false
}
for ($i = 1; $i -le $IdleRuns; $i++) {
    $rows += Measure-Run "D_${Tag}_idle_$i" @() $true
}
$rows | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$bench\results_D_$Tag.json"
$play = $rows | Where-Object { $_.run -like '*_play_*' }
$idle = $rows | Where-Object { $_.run -like '*_idle_*' }
[ordered]@{
    playCpuPctOneCore = Get-Stats ($play | ForEach-Object { $_.cpuPct })
    idleCpuPctOneCore = Get-Stats ($idle | ForEach-Object { $_.cpuPct })
    playPeakWorkingSetMB = Get-Stats ($play | ForEach-Object { $_.peakWsMB })
    playPeakPrivateMB = Get-Stats ($play | ForEach-Object { $_.peakPrivMB })
    idlePeakWorkingSetMB = Get-Stats ($idle | ForEach-Object { $_.peakWsMB })
    idlePeakPrivateMB = Get-Stats ($idle | ForEach-Object { $_.peakPrivMB })
} | ConvertTo-Json -Depth 3
