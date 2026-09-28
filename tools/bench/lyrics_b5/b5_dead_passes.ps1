# B5: the dead-LRCLIB penalty over the whole 30-track list (Step 2's refused
# figure, 4,564 ms, was n=58 from two such passes). New build: two passes with
# LRCLIB's port refused, one with its host blackholed; the B4 build (rel-b4,
# the lyrics code before the race) once each, the same day, as the control.
param(
    [string]$New = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$Old = (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'rel-b4\monolist.exe')
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$run = Join-Path $here 'run_lyrics.ps1'
Write-Output ("dead passes start " + (Get-Date -Format o))
& $run -Pass 'ytmonly01' -LrclibUrl 'http://127.0.0.1:9' -Exe $New
& $run -Pass 'baseytmonly01' -LrclibUrl 'http://127.0.0.1:9' -Exe $Old
& $run -Pass 'ytmonly02' -LrclibUrl 'http://127.0.0.1:9' -Exe $New
& $run -Pass 'blackhole01' -LrclibUrl 'http://10.255.255.1' -Exe $New
& $run -Pass 'baseblackhole01' -LrclibUrl 'http://10.255.255.1' -Exe $Old
Write-Output ("dead passes done " + (Get-Date -Format o))
