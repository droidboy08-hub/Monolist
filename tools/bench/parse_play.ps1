# Parses a run of the UNMODIFIED app: monolist.exe --play <id> <s> [--again], with
# MONOLIST_MPV_LOG=v and QT_MESSAGE_PATTERN='%{time process} %{message}' (run.ps1).
# Works on any build, so it can be repeated unchanged after the Step 4 changes.
# For each "selftest: playing <id>[ again]" line (the clock start of --play):
#   resolveMs  -> the "status=Streaming source=<tier>" line
#   ttfaMs     -> the next "mpv[v] cplayer: starting audio playback" line (as the Qt thread
#                 logged it, so it includes the main thread's delivery lag; see the report)
#   downloadMs -> "curl: proto=... size=N" (debug) or "curl: Opening" (v) -> "mkv|lavf: EOF reached"
param([Parameter(Mandatory = $true)][string]$Log)
$lines = foreach ($l in [IO.File]::ReadAllLines($Log)) {
    $m = [regex]::Match($l, '^\s*([\d.]+) (.*)$')
    if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; text = $m.Groups[2].Value } }
}
$starts = @($lines | Where-Object { $_.text -match '^selftest: playing (\S+)( again)?$' })
# mpv's teardown after the self-test's own report is not the stream ending.
$done = $lines | Where-Object { $_.text -match '^selftest: position ' } | Select-Object -First 1
$runEnd = if ($done) { $done.t } else { [double]::MaxValue }
$out = @()
for ($i = 0; $i -lt $starts.Count; $i++) {
    $s = $starts[$i]
    $end = if ($i + 1 -lt $starts.Count) { $starts[$i + 1].t } else { $runEnd }
    $seg = @($lines | Where-Object { $_.t -ge $s.t -and $_.t -lt $end })
    $id = ([regex]::Match($s.text, 'playing (\S+)')).Groups[1].Value
    $streaming = $seg | Where-Object { $_.text -match 'status=Streaming source=(.*)$' } | Select-Object -First 1
    $tier = if ($streaming) { ([regex]::Match($streaming.text, 'source=(.*)$')).Groups[1].Value } else { $null }
    $loadfile = $seg | Where-Object { $_.text -match 'Run command: loadfile|Set property: pause=false' } | Select-Object -First 1
    $open = $seg | Where-Object { $_.text -match 'curl: Opening https' } | Select-Object -First 1
    $audio = $seg | Where-Object { $_.text -match 'cplayer: starting audio playback' } | Select-Object -First 1
    $eof = $seg | Where-Object { $open -and $_.t -ge $open.t -and $_.text -match '(mkv|lavf): EOF reached' } | Select-Object -First 1
    $stalls = @($seg | Where-Object { $_.text -match 'cplayer: Enter buffering' })
    $stallSec = 0.0
    foreach ($e in ($seg | Where-Object { $_.text -match 'End buffering \(waited ([\d.]+) secs\)' })) { $null = $e.text -match 'waited ([\d.]+) secs'; $stallSec += [double]$Matches[1] }
    $out += [pscustomobject][ordered]@{
        id = $id; again = [bool]($s.text -match 'again$'); tier = $tier
        resolveMs = if ($streaming) { [Math]::Round(1000 * ($streaming.t - $s.t)) } else { $null }
        resolvedToOpenMs = if ($streaming -and $open) { [Math]::Round(1000 * ($open.t - $streaming.t)) } else { $null }
        ttfaMs = if ($audio) { [Math]::Round(1000 * ($audio.t - $s.t)) } else { $null }
        openToEofMs = if ($open -and $eof) { [Math]::Round(1000 * ($eof.t - $open.t)) } else { $null }
        stalls = $stalls.Count; stallSec = [Math]::Round($stallSec, 3)
    }
}
$out | ConvertTo-Json -Depth 3
