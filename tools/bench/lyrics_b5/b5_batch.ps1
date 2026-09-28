# B5 re-measurement: batch.ps1's passes for the B5 build (cold01-10, proxy01-05,
# cached01-03), interleaved with a same-day control on the B4 build
# (bench\rel-b4: the lyrics code before the race) as base01-10 and
# basecached01-03, exactly as b4_batch.ps1 ran B4 against rel-b1. Runs in
# this folder (lyrics_b5), so Step 2's and B4's logs stay as they were.
param(
    [string]$New = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$Old = (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'rel-b4\monolist.exe'),
    [int]$StartCold = 1
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'run_lyrics.ps1'
$deno = 'C:\dev\monolist-deps\bin\deno.exe'
Write-Output ("b5 batch start " + (Get-Date -Format o) + " new=$New old=$Old")
for ($i = $StartCold; $i -le 10; $i++) {
    & $run -Pass ('cold{0:D2}' -f $i) -Exe $New
    & $run -Pass ('base{0:D2}' -f $i) -Exe $Old
    if ($i -ge 2 -and $i -le 6) { & $run -Pass ('proxy{0:D2}' -f ($i - 1)) -ProxyPort 8765 -Exe $New }
}
for ($i = 1; $i -le 3; $i++) {
    & $run -Pass ('cached{0:D2}' -f $i) -ReuseFrom ('cold{0:D2}' -f $i) -Exe $New
    & $run -Pass ('basecached{0:D2}' -f $i) -ReuseFrom ('base{0:D2}' -f $i) -Exe $Old
}
$passes = @(1..10 | ForEach-Object { 'cold{0:D2}' -f $_ }) + @(1..5 | ForEach-Object { 'proxy{0:D2}' -f $_ }) +
          @(1..3 | ForEach-Object { 'cached{0:D2}' -f $_ }) + @(1..10 | ForEach-Object { 'base{0:D2}' -f $_ }) +
          @(1..3 | ForEach-Object { 'basecached{0:D2}' -f $_ })
& $deno run --allow-read --allow-write (Join-Path $here 'analyze.ts') @passes | Out-Null
Copy-Item (Join-Path $here 'results\summary.json') (Join-Path $here 'results\summary_main.json') -Force
Write-Output ("b5 batch done " + (Get-Date -Format o))
