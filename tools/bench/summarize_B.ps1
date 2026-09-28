# Summarises the parsed instrumented runs (logs\B_<Tag>_*.parsed.json) into the
# playback metrics, median / p90 / n per group, and prints the per-play table.
param([string]$Tag = 'before', [switch]$Table)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$rows = @()
foreach ($f in Get-ChildItem "$bench\logs" -Filter "B_${Tag}_*.parsed.json") {
    $suite = ($f.Name -split '_')[2]
    $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
    foreach ($r in $j.records) {
        $r | Add-Member -NotePropertyName suite -NotePropertyValue $suite -Force
        $r | Add-Member -NotePropertyName file -NotePropertyValue $f.Name -Force
        $rows += $r
    }
}
function Group-Metric($name, $sel, $field) {
    $v = @($sel | ForEach-Object { $_.$field })
    $s = Get-Stats $v
    [pscustomobject]@{ metric = $name; field = $field; n = $s.n; median = $s.median; p90 = $s.p90; min = $s.min; max = $s.max }
}
$coldA = @($rows | Where-Object { $_.suite -eq 'cold' -and $_.kind -eq 'play-first' })
$coldB = @($rows | Where-Object { $_.suite -eq 'seq' -and $_.kind -eq 'play-first' -and $_.linkSource -eq 'resolve' -and $_.press -gt 0 } | Where-Object { $_.file -and $true })
# In a seq run, the first play of the process is a launch play (cold A), not a warm-session one.
$firstOfFile = @{}
foreach ($r in ($rows | Where-Object { $_.suite -eq 'seq' } | Sort-Object file, press)) { if (-not $firstOfFile.ContainsKey($r.file)) { $firstOfFile[$r.file] = $r.press } }
$coldB = @($coldB | Where-Object { $_.press -ne $firstOfFile[$_.file] })
# The first play of every seq / skip / play process is a launch play too (same 500 ms settle).
$coldA += @($rows | Where-Object { $_.suite -eq 'seq' -and $_.kind -eq 'play-first' -and $_.press -eq $firstOfFile[$_.file] })
$coldA += @($rows | Where-Object { $_.suite -in @('skip', 'play') -and $_.kind -eq 'queue-first' })
$warm = @($rows | Where-Object { $_.suite -eq 'seq' -and $_.kind -eq 'play-again' })
$skip = @($rows | Where-Object { $_.suite -eq 'skip' -and $_.kind -eq 'next' })
$eofAdv = @($rows | Where-Object { $_.kind -eq 'eof-advance' })
$out = @()
foreach ($set in @(@('TTFA-cold (a) launch', $coldA), @('TTFA-cold (b) new track, warm session', $coldB), @('TTFA-warm (link cache hit)', $warm), @('Skip latency (Next, prefetched)', $skip), @('Track change at natural end (EOF->audio)', $eofAdv))) {
    $name = $set[0]; $sel = $set[1]
    foreach ($field in 'ttfaMs', 'ttfaQtLoggedMs', 'ttfaFirstPosEventMs', 'resolveMs', 'visitorWaitMs', 'playerPostMs', 'resolvedToLoadfileMs', 'loadfileToHttpOpenMs', 'httpOpenToResponseMs', 'responseToAudioMs', 'downloadMs', 'downloadRate') {
        $g = Group-Metric $name $sel $field
        if ($g.n -gt 0) { $out += $g }
    }
    $out += [pscustomobject]@{ metric = $name; field = 'linkSource'; n = $sel.Count; median = (($sel | Group-Object linkSource | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' '); p90 = (($sel | Group-Object tier | ForEach-Object { "tier$($_.Name)=$($_.Count)" }) -join ' ') }
}
# Rebuffer ratio over the continuous-play runs: stall seconds after first audio / audible seconds.
$playRows = @($rows | Where-Object { $_.suite -eq 'play' -and $_.audibleWindowSec })
if ($playRows.Count -gt 0) {
    $stall = ($playRows | Measure-Object stallSec -Sum).Sum
    $window = ($playRows | Measure-Object audibleWindowSec -Sum).Sum
    $out += [pscustomobject]@{ metric = 'Rebuffer (continuous play runs)'; field = 'stalls / stallSec / playSec'; n = $playRows.Count
                               median = "$(($playRows | Measure-Object stalls -Sum).Sum) stalls, $([Math]::Round($stall, 2)) s stalled"
                               p90 = "ratio $([Math]::Round($stall / [Math]::Max(0.001, $window - $stall), 5)) over $([Math]::Round($window - $stall, 1)) s played" }
}
$refused = @($rows | Where-Object { $_.httpErrors })
$out += [pscustomobject]@{ metric = 'All plays: link refused by googlevideo (HTTP error)'; field = 'httpErrors'; n = $rows.Count; median = $refused.Count; p90 = (($refused | ForEach-Object { $_.id + ':' + $_.httpErrors + '->tier' + $_.finalTier }) -join ' ') }
$out | Format-Table metric, field, n, median, p90, min, max -AutoSize | Out-String -Width 250
if ($Table) { $rows | Sort-Object file, press | Format-Table suite, id, kind, linkSource, tier, finalTier, httpErrors, recoveryMs, resolveMs, visitorWaitMs, playerPostMs, resolvedToLoadfileMs, loadfileToHttpOpenMs, httpOpenToResponseMs, responseToAudioMs, ttfaMs, ttfaQtLoggedMs, bytes, downloadMs, stalls, stallSec -AutoSize | Out-String -Width 400 }
$out | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$bench\results_B_$Tag.json"
