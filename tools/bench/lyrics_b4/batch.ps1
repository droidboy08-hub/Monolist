# The whole lyrics measurement, as run for Step 2 (re-run as-is in Step 4):
#   10 cold passes (fresh database per track), 5 proxy passes (cold, LRCLIB
#   through lrclib_proxy.ts for the per-provider split), interleaved;
#   3 cached passes over cold01..cold03's databases; then analyze.ts.
#   -Extras adds: 3 proxyfail passes (LRCLIB stub answers 503 at once, so the
#   YouTube Music leg is timed alone and its own hit rate shows), the dead-
#   LRCLIB experiment (dead_provider.ps1), the LRCLIB search-vs-get audit
#   (lrclib_audit.ts), extra_stats.ts and report.ts.
# Step 2 ran: batch.ps1 -StartCold 2 (cold01 had been run by hand first), then
# the extras one by one, plus two ytmonly passes (-LrclibUrl http://127.0.0.1:9).
# Usage: batch.ps1 [-StartCold 1] [-Exe <path>] [-Extras]
param([int]$StartCold = 1, [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe', [switch]$Extras)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'run_lyrics.ps1'
$deno = 'C:\dev\monolist-deps\bin\deno.exe'
for ($i = $StartCold; $i -le 10; $i++) {
    & $run -Pass ('cold{0:D2}' -f $i) -Exe $Exe
    if ($i -ge 2 -and $i -le 6) { & $run -Pass ('proxy{0:D2}' -f ($i - 1)) -ProxyPort 8765 -Exe $Exe }
}
for ($i = 1; $i -le 3; $i++) {
    & $run -Pass ('cached{0:D2}' -f $i) -ReuseFrom ('cold{0:D2}' -f $i) -Exe $Exe
}
$passes = @(1..10 | ForEach-Object { 'cold{0:D2}' -f $_ }) + @(1..5 | ForEach-Object { 'proxy{0:D2}' -f $_ }) + @(1..3 | ForEach-Object { 'cached{0:D2}' -f $_ })
& $deno run --allow-read --allow-write (Join-Path $here 'analyze.ts') @passes | Out-Null
Copy-Item (Join-Path $here 'results\summary.json') (Join-Path $here 'results\summary_main.json') -Force
if ($Extras) {
    for ($i = 1; $i -le 3; $i++) { & $run -Pass ('proxyfail{0:D2}' -f $i) -ProxyPort 8766 -ProxyFail -Exe $Exe }
    & $deno run --allow-read --allow-write (Join-Path $here 'analyze.ts') proxyfail01 proxyfail02 proxyfail03 | Out-Null
    Copy-Item (Join-Path $here 'results\summary.json') (Join-Path $here 'results\summary_proxyfail.json') -Force
    & (Join-Path $here 'dead_provider.ps1') -Tracks 9,18,30 -Exe $Exe
    & $deno run --allow-net --allow-read --allow-write (Join-Path $here 'lrclib_audit.ts')
    & $deno run --allow-read (Join-Path $here 'extra_stats.ts') | Tee-Object -FilePath (Join-Path $here 'results\extra_stats.txt')
    & $deno run --allow-read --allow-write (Join-Path $here 'report.ts') | Out-Null
}
Write-Output ("batch done " + (Get-Date -Format o))
