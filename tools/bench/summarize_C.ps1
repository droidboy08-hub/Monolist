# Summarises results_C_ua_<Tag>.jsonl, results_C_survey_<Tag>.jsonl and results_C_rangesize_<Tag>.jsonl.
param([string]$Tag = 'before')
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$ua = @(Get-Content "$bench\results_C_ua_$Tag.jsonl" | ForEach-Object { $_ | ConvertFrom-Json })
$dl = @($ua | Where-Object { $_.kind -eq 'download' })
"== Long track (Tu7oq3VNgpY, 14.6 MB Opus), 5 fresh visionOS-tier URLs; bytes/s from first byte (open) or over the request time (range1m)"
$rows = foreach ($g in ($dl | Group-Object mode, ua)) {
    $ok = @($g.Group | Where-Object { ($_.code -eq 206 -or $_.code -eq 200 -or $_.mode -eq 'range1m') -and $_.bytes -gt 0 -and -not ($_.codes -match '403') })
    $rate = if ($g.Group[0].mode -eq 'range1m') { $ok | ForEach-Object { $_.rate } } else { $ok | ForEach-Object { $_.rateFromFirstByte } }
    $s = Get-Stats $rate
    $refused = @($g.Group | Where-Object { $_.code -eq 403 -or $_.codes -match '403' }).Count
    [pscustomobject]@{ variant = $g.Name; runs = $g.Count; refused403 = $refused; okRuns = $s.n; medianBps = $s.median; p90Bps = $s.p90; minBps = $s.min; maxBps = $s.max
                       medianBytesIn20s = (Get-Stats ($ok | ForEach-Object { $_.bytes })).median }
}
$rows | Sort-Object variant | Format-Table -AutoSize | Out-String -Width 250
$res = @($ua | Where-Object { $_.kind -eq 'resolve' })
$sv = @(Get-Content "$bench\results_C_survey_$Tag.jsonl" | ForEach-Object { $_ | ConvertFrom-Json })
$res += @($sv | Where-Object { $_.kind -eq 'resolve' })
"== Resolve as the app does, with curl (fresh connections each time): n=$($res.Count)"
[pscustomobject]@{
    homeTotal = Get-Stats ($res | ForEach-Object { 1000 * $_.home.total }); homeTtfbAfterTls = Get-Stats ($res | ForEach-Object { 1000 * ($_.home.ttfb - $_.home.tls) })
    homeDownloadAfterTtfb = Get-Stats ($res | ForEach-Object { 1000 * ($_.home.total - $_.home.ttfb) }); homeBytes = Get-Stats ($res | ForEach-Object { $_.home.bytes })
    playerTotal = Get-Stats ($res | ForEach-Object { 1000 * $_.player.total }); playerTtfbAfterTls = Get-Stats ($res | ForEach-Object { 1000 * ($_.player.ttfb - $_.player.tls) })
    playerAfterTls = Get-Stats ($res | ForEach-Object { 1000 * ($_.player.total - $_.player.tls) }); playerBytes = Get-Stats ($res | ForEach-Object { $_.player.bytes })
    tlsSetup = Get-Stats ($res | ForEach-Object { 1000 * $_.player.tls })
} | ConvertTo-Json -Depth 3
"== Survey: every track, one fresh URL, open-ended vs 1 MiB ranges (Chrome UA)"
$sv | Where-Object { $_.kind -eq 'download' } | Select-Object n, videoId, itag, mode, clen, bytes, @{n='Bps';e={ if ($_.mode -eq 'range1m') { $_.rate } else { $_.rateFromFirstByte } }}, complete, codes | Format-Table -AutoSize | Out-String -Width 250
if (Test-Path "$bench\results_C_rangesize_$Tag.jsonl") {
    "== Range size vs pacing (long track), 3 fresh URLs"
    Get-Content "$bench\results_C_rangesize_$Tag.jsonl" | ForEach-Object { $_ | ConvertFrom-Json } | Format-Table run, range, code, bytes, total, rateFromFirstByte -AutoSize | Out-String -Width 200
}
