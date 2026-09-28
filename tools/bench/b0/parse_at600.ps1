# Summarises bench\b0\logs\at600_r*.log (trace-level mpv log through the app's stderr).
# Prints no URLs or addresses. Output: bench\b0\results_at600.json and one line per run.
param([string]$Pattern = 'at600_r*.log')
$b0 = Split-Path -Parent $MyInvocation.MyCommand.Path
$logs = Join-Path $b0 'logs'
$records = @()
foreach ($f in (Get-ChildItem $logs -Filter $Pattern | Where-Object { $_.Name -match '^at600_r\d+\.log$' } | Sort-Object { [int]([regex]::Match($_.Name, 'r(\d+)\.log').Groups[1].Value) })) {
    $lines = [IO.File]::ReadAllLines($f.FullName)
    $reqs = New-Object System.Collections.Generic.List[object]
    $cur = $null
    $itags = @(); $httpErrors = @(); $rescue = @(); $statuses = @(); $eofCodes = @()
    $seekT = $null; $restartAfterSeek = $null; $finalPos = ''; $finalTitle = ''; $streamSeeks = 0
    foreach ($line in $lines) {
        $m = [regex]::Match($line, '^\s*([\d.]+) (.*)$'); if (-not $m.Success) { continue }
        $t = [double]$m.Groups[1].Value; $x = $m.Groups[2].Value
        if ($x -match '^innertube: Tu7oq3VNgpY resolved as itag (\d+)') { $itags += [int]$Matches[1] }
        elseif ($x -match 'curl: > GET ') { $cur = [ordered]@{ t = $t; range = ''; status = $null }; $reqs.Add($cur) }
        elseif ($x -match 'curl: Range: bytes=(\S+)' -and $cur -and -not $cur.range) { $cur.range = $Matches[1] }
        elseif ($x -match 'curl: < HTTP/[\d.]+ (\d{3})' -and $cur -and -not $cur.status) { $cur.status = [int]$Matches[1]; $cur.ms = [Math]::Round(1000 * ($t - $cur.t)) }
        elseif ($x -match 'curl: HTTP error (\d+)') { $httpErrors += [int]$Matches[1] }
        elseif ($x -match 'curl: stream level seek from') { $streamSeeks++ }
        elseif ($x -match 'would not play from|Trying another source|Refreshing the source|muxed stream \(itag 18\)') { $rescue += ($x -replace 'https://\S+', '<url>') }
        elseif ($x -match '^selftest: \+\d+ ms status=(.*)$') { $statuses += $Matches[1] }
        elseif ($x -match 'cplayer: EOF code: (\d+)') { $eofCodes += [int]$Matches[1] }
        elseif ($x -match 'cplayer: Set property: time-pos=600') { if (-not $seekT) { $seekT = $t } }
        elseif ($x -match 'cplayer: playback restart complete @ ([\d.]+)' -and $seekT -and -not $restartAfterSeek -and [double]$Matches[1] -ge 590) { $restartAfterSeek = $t }
        elseif ($x -match '^selftest: position (.*)$') { $finalPos = $Matches[1] }
        elseif ($x -match '^selftest: playing "(.*)" by') { $finalTitle = $Matches[1] }
    }
    $env_ = $null; if (Test-Path "$($f.FullName).env.json") { $env_ = Get-Content -Raw "$($f.FullName).env.json" | ConvertFrom-Json }
    # Outcome of the seek: a clean seek plays on from ~600 s in the same track; a rescue
    # re-resolves; an end of file moves to the next (autoplay) track.
    $sameTrack = ($finalTitle -eq 'Selftest')
    $posSec = $null
    if ($finalPos -match '^(\d+):(\d+) of') { $posSec = [int]$Matches[1] * 60 + [int]$Matches[2] }
    $afterSeek = @($reqs | Where-Object { $seekT -and $_.t -ge $seekT })
    $outcome = if ($rescue.Count -gt 0) { 'rescue' } elseif (-not $sameTrack) { 'end-of-file skip' } elseif ($posSec -ge 600 -and ($httpErrors.Count -eq 0)) { 'clean seek' } else { 'other' }
    $r = [ordered]@{
        run = [int]([regex]::Match($f.Name, 'r(\d+)\.log').Groups[1].Value); itags = ($itags -join ','); requests = $reqs.Count
        firstStatus = if ($reqs.Count) { $reqs[0].status } else { $null }
        requestsAfterSeek = $afterSeek.Count
        afterSeekStatuses = (($afterSeek | ForEach-Object { "$($_.status)@$($_.range)" }) -join ' ')
        allStatuses = (($reqs | ForEach-Object { $_.status }) -join ',')
        httpErrors = ($httpErrors -join ','); streamLevelSeeks = $streamSeeks
        seekToAudioMs = if ($seekT -and $restartAfterSeek) { [Math]::Round(1000 * ($restartAfterSeek - $seekT)) } else { $null }
        rescueLines = $rescue; statuses = ($statuses -join ' | '); eofCodes = ($eofCodes -join ',')
        finalPosition = $finalPos; stillSameTrack = $sameTrack; outcome = $outcome
        foreignProcesses = if ($env_) { ($env_.foreignSeen -join ',') } else { '' }
    }
    $records += [pscustomobject]$r
    Write-Output ("r{0} itag={1} reqs={2} first={3} afterSeek=[{4}] errors=[{5}] seek->audio={6} ms final='{7}' outcome={8}" -f $r.run, $r.itags, $r.requests, $r.firstStatus, $r.afterSeekStatuses, $r.httpErrors, $r.seekToAudioMs, $r.finalPosition, $r.outcome)
}
$json = $records | ConvertTo-Json -Depth 4
[IO.File]::WriteAllText((Join-Path $b0 'results_at600.json'), $json, (New-Object Text.UTF8Encoding($false)))
