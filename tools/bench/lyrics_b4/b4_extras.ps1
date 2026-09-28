# B4 extras, as batch.ps1 -Extras runs them, in this folder: 3 proxyfail passes
# (LRCLIB stub answers 503 at once), dead_provider.ps1 on #9, #18, #30, then
# the plan's LY-1 check: the same data dirs again with the real LRCLIB (#9
# must come back synced from LRCLIB), and LRCLIB's search-vs-get audit,
# extra_stats.ts and report.ts.
param([string]$Exe = 'C:\dev\monolist-build\release\monolist.exe')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $here
$run = Join-Path $here 'run_lyrics.ps1'
$deno = 'C:\dev\monolist-deps\bin\deno.exe'
Write-Output ("extras start " + (Get-Date -Format o))
for ($i = 1; $i -le 3; $i++) { & $run -Pass ('proxyfail{0:D2}' -f $i) -ProxyPort 8766 -ProxyFail -Exe $Exe }
& $deno run --allow-read --allow-write (Join-Path $here 'analyze.ts') proxyfail01 proxyfail02 proxyfail03 | Out-Null
Copy-Item (Join-Path $here 'results\summary.json') (Join-Path $here 'results\summary_proxyfail.json') -Force

& (Join-Path $here 'dead_provider.ps1') -Tracks 9,18,30 -Exe $Exe

# The same data dirs, LRCLIB reachable again.
$list = (Get-Content -Raw -Encoding UTF8 (Join-Path $bench 'lyrics_tracks.json') | ConvertFrom-Json).tracks
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time yyyy-MM-ddTHH:mm:ss.zzz} %{message}'
foreach ($mode in @('blackhole', 'refused')) {
    $logDir = Join-Path $here "logs\dead_$($mode)_rerun"
    New-Item -ItemType Directory -Force $logDir | Out-Null
    foreach ($t in $list) {
        if (@(9, 18, 30) -notcontains [int]$t.n) { continue }
        $env:MONOLIST_DATA_DIR = Join-Path $here ("data\dead_{0}\{1:D2}" -f $mode, [int]$t.n)
        $log = Join-Path $logDir ("{0:D2}.err" -f [int]$t.n)
        # The setting back to LRCLIB itself, as the user would put it back.
        $p = Start-Process -FilePath $Exe -ArgumentList @('--set', 'lrclib_url', 'https://lrclib.net', '--lyrics', ('"' + $t.query + '"')) `
            -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
        if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {} }
        $lines = Get-Content $log -Encoding UTF8 | Where-Object { $_ -match 'selftest:\s{3}(kept \S+ shown|\S+ in \d+ ms)' } | Select-Object -First 2
        Write-Output ("rerun {0,-9} #{1,2} {2}" -f $mode, $t.n, (($lines | ForEach-Object { $_ -replace '^\S+ selftest:\s+', '' }) -join ' | '))
        & $deno run --allow-read (Join-Path $here 'lyrics_table.ts') $env:MONOLIST_DATA_DIR | Out-File -Encoding utf8 (Join-Path $logDir ("{0:D2}.table.txt" -f [int]$t.n))
    }
}

& $deno run --allow-net --allow-read --allow-write (Join-Path $here 'lrclib_audit.ts')
& $deno run --allow-read (Join-Path $here 'extra_stats.ts') | Tee-Object -FilePath (Join-Path $here 'results\extra_stats.txt')
& $deno run --allow-read --allow-write (Join-Path $here 'report.ts') | Out-Null
Write-Output ("extras done " + (Get-Date -Format o))
