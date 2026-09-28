# B8 (PP-05): --play <id> 30 --spoil over the 12 fixed ids, before and after, interleaved
# (the arm that goes first alternates). Both arms are INSTRUMENTED Release builds:
#   before  bench\build-b7\monolist.exe  (HEAD 64cf758's playback path: muxed after a refusal)
#   after   bench\build-b8\monolist.exe  (B8: a fresh InnerTube link first)
# Each arm keeps one data folder across its runs (a stored visitor id, as a user has),
# warmed by one unmeasured run first. Logs: bench\b8\logs\spoil_<arm>_<id>.log (they hold
# googlevideo URLs, i.e. the public IP: never print them). Summary: spoil.ps1 -Parse.
param([switch]$Parse, [int]$Seconds = 30, [string[]]$Extra = @(), [string]$Tag = 'spoil', [int]$From = 0, [int]$Count = 12, [switch]$NoWarmup)
$b8 = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $b8
$logs = Join-Path $b8 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$arms = [ordered]@{ before = "$bench\build-b7\monolist.exe"; after = "$bench\build-b8\monolist.exe" }
$ids = @((Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks | ForEach-Object { $_.videoId })

function ParseOne([string]$log, [string]$id) {
    $rows = foreach ($line in [IO.File]::ReadAllLines($log, [Text.Encoding]::UTF8)) {
        $m = [regex]::Match($line, '^\s*([\d.]+) (.*)$')
        if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; x = $m.Groups[2].Value } }
    }
    $play = $rows | Where-Object { $_.x -eq "selftest: playing $id" } | Select-Object -First 1
    if (-not $play) { return $null }
    $after = @($rows | Where-Object { $_.t -ge $play.t })
    $refused = $after | Where-Object { $_.x -match 'would not play from InnerTube' } | Select-Object -First 1
    $audio = $after | Where-Object { $_.x -match 'mpv-event first time-pos' } | Select-Object -First 1
    $rung = if ($after | Where-Object { $_.x -match 'asking InnerTube once more' }) { 'fresh InnerTube' }
            elseif ($after | Where-Object { $_.x -match 'again as its muxed stream' }) { 'muxed' } else { 'none' }
    $itag = ''
    if ($refused) {
        $ans = $after | Where-Object { $_.t -ge $refused.t -and $_.x -match "^(innertube|muxed): $id resolved as itag (\d+)" } | Select-Object -First 1
        if ($ans) { $itag = [regex]::Match($ans.x, 'itag (\d+)').Groups[1].Value }
    }
    $refusals = @($after | Where-Object { $_.x -match "playback: $id would not play from" }).Count
    $stream = $after | Where-Object { $_.x -match "^stream: $id from" } | Select-Object -First 1
    $codec = if ($stream) { [regex]::Match($stream.x, ': (\w+) \d+ Hz').Groups[1].Value } else { '' }
    [pscustomobject]@{
        id = $id; ttfaMs = if ($audio) { [Math]::Round(1000 * ($audio.t - $play.t)) } else { $null }
        refusedAtMs = if ($refused) { [Math]::Round(1000 * ($refused.t - $play.t)) } else { $null }
        rescueMs = if ($refused -and $audio) { [Math]::Round(1000 * ($audio.t - $refused.t)) } else { $null }
        rung = $rung; itagAfter = $itag; refusals = $refusals; codec = $codec
    }
}

function Stats($values) {
    $v = @($values | Where-Object { $null -ne $_ } | Sort-Object)
    if (-not $v.Count) { return 'n=0' }
    $median = if ($v.Count % 2) { $v[[int][Math]::Floor($v.Count / 2)] } else { ($v[$v.Count / 2 - 1] + $v[$v.Count / 2]) / 2 }
    $p90 = $v[[int][Math]::Min($v.Count - 1, [Math]::Ceiling(0.9 * $v.Count) - 1)]
    "median {0} / p90 {1} ms (n={2}, min {3}, max {4})" -f $median, $p90, $v.Count, $v[0], $v[-1]
}

if ($Parse) {
    $all = @()
    foreach ($arm in $arms.Keys) {
        foreach ($id in $ids) {
            $log = Join-Path $logs "${Tag}_${arm}_$id.log"
            if (-not (Test-Path $log)) { continue }
            $r = ParseOne $log $id
            if ($r) { $r | Add-Member arm $arm; $all += $r }
        }
    }
    $all | Format-Table arm, id, ttfaMs, refusedAtMs, rescueMs, rung, itagAfter, refusals, codec -AutoSize | Out-String -Width 200
    foreach ($arm in $arms.Keys) {
        $a = @($all | Where-Object { $_.arm -eq $arm })
        "{0}: Play->sound {1}; refusal->sound {2}; rungs: {3}" -f $arm, (Stats ($a | ForEach-Object { $_.ttfaMs })),
            (Stats ($a | ForEach-Object { $_.rescueMs })), (($a | Group-Object rung | ForEach-Object { "$($_.Name) x$($_.Count)" }) -join ', ')
    }
    $all | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $b8 "results_$Tag.json")
    return
}

$flip = $false
if (-not $NoWarmup) { foreach ($arm in $arms.Keys) {
    # One unmeasured run per arm: the visitor id stored, as a user's is.
    & "$bench\run.ps1" -Exe $arms[$arm] -Log (Join-Path $logs "${Tag}_${arm}_warmup.log") -AppArgs @('--play', 'LrM_Y39Gmhk', '8') -MpvLog 'warn' -DataDir "$b8\data_$arm" -TimeoutSec 90 | Out-Null
} }
foreach ($id in @($ids | Select-Object -Skip $From -First $Count)) {
    $order = @($arms.Keys)
    if ($flip) { [array]::Reverse($order) }
    $flip = -not $flip
    foreach ($arm in $order) {
        $log = Join-Path $logs "${Tag}_${arm}_$id.log"
        $o = & "$bench\run.ps1" -Exe $arms[$arm] -Log $log -AppArgs (@('--play', $id, "$Seconds", '--spoil') + $Extra) -MpvLog 'warn' -DataDir "$b8\data_$arm" -TimeoutSec 120
        $r = ParseOne $log $id
        Write-Output ("{0} {1}: {2} | Play->sound {3} ms, refusal->sound {4} ms, rung {5}, itag {6}" -f $arm, $id, ($o -join ' '), $r.ttfaMs, $r.rescueMs, $r.rung, $r.itagAfter)
    }
}
