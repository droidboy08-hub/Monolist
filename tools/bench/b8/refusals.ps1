# B8 (PP-05): every in-app refusal over the bench_B runs, with its rung and outcome.
# Reads bench\logs\B_<Tag>_*.qt.log (instrumented build, QT_MESSAGE_PATTERN '%{time process} ...').
#  opens     = mpv loadfile marks (each a googlevideo open; the replays' too)
#  refusals  = "playback: <id> would not play from <tier>: <reason>", with the HTTP code mpv logged
#  rung      = what followed: a fresh InnerTube link, the muxed stream, another tier
#  outcome   = the song's sound started on the next link (rescued), or it was refused again
# Also counts early ends ("stopped short") and resolves that failed. Prints no URLs.
param([string]$Tag = 'b8', [string]$Out = '')
$b8 = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $b8
$logs = Get-ChildItem "$bench\logs" -Filter "B_${Tag}_*.qt.log" | Sort-Object LastWriteTime
$opens = 0; $plays = 0; $records = @(); $earlyEnds = @(); $failedResolves = 0
foreach ($f in $logs) {
    $rows = foreach ($line in [IO.File]::ReadAllLines($f.FullName, [Text.Encoding]::UTF8)) {
        $m = [regex]::Match($line, '^\s*([\d.]+) (.*)$')
        if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; x = $m.Groups[2].Value } }
    }
    $opens += @($rows | Where-Object { $_.x -match '^bench: \[\d+\] mpv loadfile' }).Count
    $plays += @($rows | Where-Object { $_.x -match '^bench: \[\d+\] beginTrack \S+ autoPlay=1' }).Count
    $failedResolves += @($rows | Where-Object { $_.x -match '^resolve failed for ' }).Count
    $earlyEnds += @($rows | Where-Object { $_.x -match 'stopped short' } | ForEach-Object { "$($f.Name): $($_.x)" })
    for ($i = 0; $i -lt $rows.Count; $i++) {
        $m = [regex]::Match($rows[$i].x, '^playback: (\S+) would not play from (.+?): (.*)$')
        if (-not $m.Success) { continue }
        $id = $m.Groups[1].Value
        $code = ''
        for ($j = [Math]::Max(0, $i - 6); $j -lt $i; $j++) {
            $c = [regex]::Match($rows[$j].x, 'HTTP error (\d+)')
            if ($c.Success) { $code = $c.Groups[1].Value }
        }
        $rung = 'none (stopped)'; $outcome = ''
        for ($j = $i + 1; $j -lt $rows.Count; $j++) {
            $x = $rows[$j].x
            if ($x -match "^playback: asking InnerTube once more for $id") { $rung = 'fresh InnerTube'; continue }
            if ($x -match "^playback: trying $id again as its muxed stream") { $rung = 'muxed'; continue }
            if ($x -match "^bench: \[\d+\] tier start $id tier=(\S+) prefetch=0" -and $rung -eq 'none (stopped)') { $rung = $Matches[1]; continue }
            if ($x -match "^playback: $id would not play from") { $outcome = 'refused again'; break }
            if ($x -match '^bench: \[\d+\] mpv-event first time-pos') { $outcome = 'rescued: sound started'; break }
            if ($x -match '^bench: \[\d+\] beginTrack') { $outcome = 'left before any sound'; break }
            if ($x -match "^resolve failed for $id") { $outcome = 'resolve failed'; break }
        }
        $records += [pscustomobject]@{ log = $f.Name; atS = $rows[$i].t; id = $id; from = $m.Groups[2].Value; reason = $m.Groups[3].Value; http = $code; rung = $rung; outcome = $outcome }
    }
}
"{0} runs, {1} song starts, {2} opens (mpv loadfile), {3} refusals, {4} early ends, {5} failed resolves" -f $logs.Count, $plays, $opens, $records.Count, $earlyEnds.Count, $failedResolves
$records | Format-Table log, atS, id, from, http, reason, rung, outcome -AutoSize | Out-String -Width 220
$earlyEnds
if ($records.Count) {
    $fresh = @($records | Where-Object { $_.rung -eq 'fresh InnerTube' })
    $rescued = @($fresh | Where-Object { $_.outcome -like 'rescued*' })
    "fresh InnerTube rung: {0} tried, {1} rescued ({2:P0})" -f $fresh.Count, $rescued.Count, $(if ($fresh.Count) { $rescued.Count / $fresh.Count } else { 0 })
}
if ($Out) { $records | ConvertTo-Json | Set-Content -Encoding utf8 $Out }
