# B7 summary: parsed runs logs\B_<tag>_*.parsed.json for the four tags of run_b7.ps1.
# Groups, as Step 2 named them:
#   new    seq, play-first, not the first play of the process (link not cached: resolve)
#   replay seq, play-again (link-cache hit)
#   skip   skip, next (prefetched link)
#   launch first play of each seq/skip process
# Metrics: resolved->loadfile, resolved->"listening recorded" (the bookkeeping), TTFA (press ->
# mpv 'starting audio playback'), and the network stages, median / p90 / n. Refused first
# opens (HTTP errors) are counted and left out of the TTFA rows ("clean").
param([string[]]$Tags = @('b7beforee', 'b7aftere', 'b7befores', 'b7afters'), [switch]$Rows)
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. "$bench\stats.ps1"
$out = @()
$all = @()
foreach ($tag in $Tags) {
    $recs = @()
    foreach ($f in Get-ChildItem "$bench\logs" -Filter "B_${tag}_*.parsed.json") {
        $suite = ($f.Name -split '_')[2]
        $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
        $first = ($j.records | Sort-Object press | Select-Object -First 1).press
        foreach ($r in $j.records) {
            $group = if ($r.press -eq $first) { 'launch' }
                     elseif ($suite -eq 'seq' -and $r.kind -eq 'play-first') { 'new' }
                     elseif ($suite -eq 'seq' -and $r.kind -eq 'play-again') { 'replay' }
                     elseif ($suite -eq 'skip' -and $r.kind -eq 'next') { 'skip' }
                     else { 'other' }
            $r | Add-Member -NotePropertyName group -NotePropertyValue $group -Force
            $r | Add-Member -NotePropertyName tag -NotePropertyValue $tag -Force
            $r | Add-Member -NotePropertyName file -NotePropertyValue $f.Name -Force
            $recs += $r
        }
    }
    $all += $recs
    foreach ($group in 'new', 'replay', 'skip', 'launch') {
        $sel = @($recs | Where-Object { $_.group -eq $group })
        if ($sel.Count -eq 0) { continue }
        $clean = @($sel | Where-Object { -not $_.httpErrors })
        foreach ($m in @(@('resolved->loadfile', 'resolvedToLoadfileMs', $sel), @('resolved->recorded', 'listenRecordMs', $sel),
                         @('TTFA (clean)', 'ttfaMs', $clean), @('resolve (press->resolved)', 'resolveMs', $clean),
                         @('loadfile->http open', 'loadfileToHttpOpenMs', $clean), @('http open->response', 'httpOpenToResponseMs', $clean),
                         @('response->audio', 'responseToAudioMs', $clean))) {
            $s = Get-Stats @($m[2] | ForEach-Object { $_.($m[1]) })
            $out += [pscustomobject]@{ tag = $tag; group = $group; metric = $m[0]; n = $s.n; median = $s.median; p90 = $s.p90; min = $s.min; max = $s.max }
        }
        $out += [pscustomobject]@{ tag = $tag; group = $group; metric = 'refused first opens'; n = $sel.Count; median = ($sel.Count - $clean.Count); p90 = $null; min = $null; max = $null }
    }
}
$out | Format-Table tag, group, metric, n, median, p90, min, max -AutoSize | Out-String -Width 200
if ($Rows) { $all | Sort-Object tag, file, press | Format-Table tag, file, group, id, linkSource, httpErrors, resolveMs, resolvedToLoadfileMs, listenRecordMs, ttfaMs -AutoSize | Out-String -Width 250 }
$out | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$bench\b7\results_b7.json"
