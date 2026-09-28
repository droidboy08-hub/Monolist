# B5's whole re-measurement on the final build, one benchmark after another:
# dead_provider.ps1 (#9, #18, #30), full passes with LRCLIB dead (two with its
# port refused, one blackholed, and the blackholed control on rel-b4; the
# refused control pass on rel-b4 was run earlier the same day), then
# b5_batch.ps1, then b5_pane.ps1 (5 passes per arm).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'run_lyrics.ps1'
$new = 'C:\dev\monolist-build\release\monolist.exe'
$old = Join-Path (Split-Path -Parent $here) 'rel-b4\monolist.exe'
Write-Output ("all start " + (Get-Date -Format o))
& (Join-Path $here 'dead_provider.ps1') -Tracks 9,18,30
& $run -Pass 'ytmonly01' -LrclibUrl 'http://127.0.0.1:9' -Exe $new
& $run -Pass 'ytmonly02' -LrclibUrl 'http://127.0.0.1:9' -Exe $new
& $run -Pass 'blackhole01' -LrclibUrl 'http://10.255.255.1' -Exe $new
& $run -Pass 'baseblackhole01' -LrclibUrl 'http://10.255.255.1' -Exe $old
Write-Output ("dead done " + (Get-Date -Format o))
& (Join-Path $here 'b5_batch.ps1')
& (Join-Path $here 'b5_pane.ps1') -Passes 5
Write-Output ("all done " + (Get-Date -Format o))
