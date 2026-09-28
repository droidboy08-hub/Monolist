# Parses one run of the INSTRUMENTED build (bench\build\monolist.exe --bench ...):
#  -QtLog   stderr of the app, QT_MESSAGE_PATTERN='%{time process} %{message}'
#           (lines "  12.345 bench: [ms] <text>", "bench-sample: ...", "mpv[v] ...")
#  -MpvLog  mpv's own log-file (MONOLIST_MPV_LOGFILE), lines "[  1.234][v][cplayer] text"
# Every benchMark() line exists in both (print-text "BENCH <text>"), which aligns mpv's
# clock to the Qt clock: offset = min(mpvTime - qtTime) over all marks.
# Output: JSON with one record per track start, in Qt-clock seconds.
param(
    [Parameter(Mandatory = $true)][string]$QtLog,
    [Parameter(Mandatory = $true)][string]$MpvLog,
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$qt = New-Object System.Collections.Generic.List[object]
foreach ($line in [IO.File]::ReadAllLines($QtLog)) {
    $m = [regex]::Match($line, '^\s*([\d.]+) (.*)$')
    if ($m.Success) { $qt.Add([pscustomobject]@{ t = [double]$m.Groups[1].Value; text = $m.Groups[2].Value }) }
}
$mpv = New-Object System.Collections.Generic.List[object]
foreach ($line in [IO.File]::ReadAllLines($MpvLog)) {
    $m = [regex]::Match($line, '^\[\s*([\d.]+)\]\[(\w)\]\[([^\]]*)\]\s?(.*)$')
    if ($m.Success) { $mpv.Add([pscustomobject]@{ t = [double]$m.Groups[1].Value; lvl = $m.Groups[2].Value; mod = $m.Groups[3].Value; text = $m.Groups[4].Value }) }
}

# --- clock alignment -------------------------------------------------------
$qtMarks = @{}
foreach ($e in $qt) {
    $m = [regex]::Match($e.text, '^bench: \[\d+\] (.*)$')
    if ($m.Success) { $k = $m.Groups[1].Value; if (-not $qtMarks.ContainsKey($k)) { $qtMarks[$k] = New-Object System.Collections.Generic.List[double] }; $qtMarks[$k].Add($e.t) }
}
$diffs = @()
$seen = @{}
foreach ($e in $mpv) {
    if ($e.text -notmatch '^BENCH (.*)$') { continue }
    $k = $Matches[1]
    if (-not $qtMarks.ContainsKey($k)) { continue }
    $i = if ($seen.ContainsKey($k)) { $seen[$k] } else { 0 }
    if ($i -lt $qtMarks[$k].Count) { $diffs += ($e.t - $qtMarks[$k][$i]) }
    $seen[$k] = $i + 1
}
$offset = ($diffs | Measure-Object -Minimum).Minimum
$sortedDiffs = $diffs | Sort-Object
$markDelayMedianMs = [Math]::Round(1000 * (($sortedDiffs[[int]($sortedDiffs.Count / 2)]) - $offset), 1)
foreach ($e in $mpv) { $e | Add-Member -NotePropertyName q -NotePropertyValue ($e.t - $offset) }

function Bench([string]$pattern) { $qt | Where-Object { $_.text -match "^bench: \[\d+\] $pattern" } }
function FirstQt([double]$from, [double]$to, [string]$pattern) { $qt | Where-Object { $_.t -ge $from -and $_.t -lt $to -and $_.text -match $pattern } | Select-Object -First 1 }
function FirstMpv([double]$from, [double]$to, [string]$pattern, [string]$mod = '') { $mpv | Where-Object { $_.q -ge $from -and $_.q -lt $to -and ($mod -eq '' -or $_.mod -eq $mod) -and $_.text -match $pattern } | Select-Object -First 1 }
function AllMpv([double]$from, [double]$to, [string]$pattern) { $mpv | Where-Object { $_.q -ge $from -and $_.q -lt $to -and $_.text -match $pattern } }
function Ms($a, $b) { if ($null -eq $a -or $null -eq $b) { return $null }; [Math]::Round(1000 * ($b - $a)) }

# --- one record per track start (beginTrack) ------------------------------
$begins = @(Bench 'beginTrack (\S+) autoPlay=1')
# The run ends at "bench done" / "played for the total": mpv's teardown after that
# ("EOF code: 5", "EOF reached") is not the stream ending.
$runEnd = $qt | Where-Object { $_.text -match '^bench: \[\d+\] (bench done|played for the total|bench timed out)' } | Select-Object -First 1
$runEndT = if ($runEnd) { $runEnd.t } else { [double]::MaxValue }
$records = @()
for ($i = 0; $i -lt $begins.Count; $i++) {
    $b = $begins[$i]
    $id = ([regex]::Match($b.text, 'beginTrack (\S+)')).Groups[1].Value
    $end = if ($i + 1 -lt $begins.Count) { $begins[$i + 1].t } else { $runEndT }
    # what triggered it: a PLAY/NEXT/PLAYQUEUE mark just before, else an end of file
    $trigger = $qt | Where-Object { $_.t -le $b.t -and $_.t -ge $b.t - 0.25 -and $_.text -match '^bench: \[\d+\] (PLAY |NEXT |PLAYQUEUE |handleEndOfFile)' } | Select-Object -Last 1
    $press = if ($trigger) { $trigger.t } else { $b.t }
    $kind = if (-not $trigger) { 'unknown' } elseif ($trigger.text -match 'PLAY (\S+) n=(\d+) pass=(\w+)') { 'play-' + $Matches[3] } elseif ($trigger.text -match 'NEXT') { 'next' } elseif ($trigger.text -match 'PLAYQUEUE') { 'queue-first' } else { 'eof-advance' }
    $cacheHit = FirstQt $b.t $end "resolve $id cache-hit"
    $takeover = FirstQt $b.t $end "resolve $id takes-over-prefetch"
    $pcall = FirstQt ($b.t - 0.001) $end "player call $id "
    $pvis = FirstQt ($b.t - 0.001) $end "player visitor-ready $id "
    $pans = FirstQt ($b.t - 0.001) $end "player answered $id "
    $tierOk = FirstQt ($b.t - 30) $end "tier ok $id "
    $resolved = FirstQt $b.t $end "handleResolved $id tier"
    $loading = FirstQt $b.t $end "handleResolved $id listening recorded"
    $loadfile = FirstQt $b.t $end '^bench: \[\d+\] mpv loadfile'
    $lf = if ($loadfile) { $loadfile.t } else { $b.t }
    $aoStart = FirstMpv $lf $end '^starting audio playback'
    # A refused link (HTTP 403 ...) is re-resolved and loaded again: the transport
    # phases are then taken from the load that played, and the refusal is recorded.
    $loadfiles = @($qt | Where-Object { $_.t -ge $b.t -and $_.t -lt $end -and $_.text -match '^bench: \[\d+\] mpv loadfile' })
    $played = if ($aoStart) { $loadfiles | Where-Object { $_.t -le $aoStart.q + 0.002 } | Select-Object -Last 1 } else { $loadfiles | Select-Object -Last 1 }
    $plf = if ($played) { $played.t } else { $lf }
    $httpErrors = @(AllMpv $lf $end 'HTTP error (\d+)' | ForEach-Object { ([regex]::Match($_.text, 'HTTP error (\d+)')).Groups[1].Value })
    $resolvedAll = @($qt | Where-Object { $_.t -ge $b.t -and $_.t -lt $end -and $_.text -match "handleResolved $id tier=(\d+)" })
    $firstErr = AllMpv $lf $end 'HTTP error (\d+)' | Select-Object -First 1
    $open = FirstMpv $plf $end '^Opening https'
    $resp = FirstMpv $plf $end 'code=2\d\d size='
    $opened = FirstMpv $plf $end '^Opening done'
    $restart = FirstMpv $lf $end '^playback restart complete'
    $eof = FirstMpv $(if ($resp) { $resp.q } else { $plf }) $end '^EOF reached'
    $qtAudio = FirstQt $lf $end 'mpv\[v\] cplayer: starting audio playback'
    $qtRestartEvt = FirstQt $lf $end 'mpv-event PLAYBACK_RESTART'
    $qtFirstPos = FirstQt $lf $end 'mpv-event first time-pos'
    $size = $null
    if ($resp -and $resp.text -match 'size=(\d+)') { $size = [long]$Matches[1] }
    $r = [ordered]@{
        id = $id; kind = $kind; press = [Math]::Round($press, 3)
        linkSource = if ($cacheHit) { 'cache' } elseif ($takeover) { 'prefetch-takeover' } else { 'resolve' }
        resolveMs = Ms $press ($(if ($resolved) { $resolved.t } else { $null }))
        visitorWaitMs = if ($pvis -and $pvis.text -match 'waited=(\d+)') { [int]$Matches[1] } else { $null }
        playerPostMs = if ($pans -and $pans.text -match 'post=(\d+)') { [int]$Matches[1] } else { $null }
        tier = if ($resolved -and $resolved.text -match 'tier=(\d+)') { [int]$Matches[1] } else { $null }
        finalTier = if ($resolvedAll.Count -gt 0 -and $resolvedAll[-1].text -match 'tier=(\d+)') { [int]$Matches[1] } else { $null }
        httpErrors = ($httpErrors -join ',')
        recoveryMs = Ms ($(if ($firstErr) { $firstErr.q } else { $null })) ($(if ($firstErr -and $aoStart) { $aoStart.q } else { $null }))
        tierMs = if ($tierOk -and $tierOk.text -match 'tierMs=(\d+)') { [int]$Matches[1] } else { $null }
        resolvedToLoadfileMs = Ms ($(if ($resolved) { $resolved.t } else { $null })) ($(if ($loadfile) { $loadfile.t } else { $null }))
        listenRecordMs = Ms ($(if ($resolved) { $resolved.t } else { $null })) ($(if ($loading) { $loading.t } else { $null }))
        loadfileToHttpOpenMs = Ms $plf ($(if ($open) { $open.q } else { $null }))
        httpOpenToResponseMs = Ms ($(if ($open) { $open.q } else { $null })) ($(if ($resp) { $resp.q } else { $null }))
        responseToAudioMs = Ms ($(if ($resp) { $resp.q } else { $null })) ($(if ($aoStart) { $aoStart.q } else { $null }))
        ttfaMs = Ms $press ($(if ($aoStart) { $aoStart.q } else { $null }))
        ttfaRestartMs = Ms $press ($(if ($restart) { $restart.q } else { $null }))
        ttfaQtLoggedMs = Ms $press ($(if ($qtAudio) { $qtAudio.t } else { $null }))
        ttfaFirstPosEventMs = Ms $press ($(if ($qtFirstPos) { $qtFirstPos.t } else { $null }))
        bytes = $size
        downloadMs = Ms ($(if ($open) { $open.q } else { $null })) ($(if ($eof) { $eof.q } else { $null }))   # request start -> whole file in the demuxer cache
    }
    if ($r.bytes -and $r.downloadMs) { $r.downloadRate = [Math]::Round($r.bytes / [Math]::Max(0.001, $r.downloadMs / 1000.0)) }
    # stalls after audio started, until the next track
    if ($aoStart) {
        $enters = @(AllMpv $aoStart.q $end '^Enter buffering')
        $ends = @(AllMpv $aoStart.q $end '^End buffering \(waited ([\d.]+) secs\)')
        $stall = 0.0
        foreach ($e in $ends) { if ($e.text -match 'waited ([\d.]+) secs') { $stall += [double]$Matches[1] } }
        $r.stalls = $enters.Count
        $r.stallSec = [Math]::Round($stall, 3)
        $lastT = if ($end -lt [double]::MaxValue) { $end } else { ($qt | Select-Object -Last 1).t }
        $r.audibleWindowSec = [Math]::Round($lastT - $aoStart.q, 3)
    }
    $records += [pscustomobject]$r
}
$result = [ordered]@{ qtLog = $QtLog; offsetSec = $offset; marks = $diffs.Count; markDelayMedianMs = $markDelayMedianMs; records = $records }
$json = $result | ConvertTo-Json -Depth 5
if ($Out) { [IO.File]::WriteAllText($Out, $json, (New-Object Text.UTF8Encoding($false))) }
$json
