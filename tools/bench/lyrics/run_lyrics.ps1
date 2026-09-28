# Runs the app's real lyrics lookup (monolist.exe --lyrics "<query>") for every
# track in bench\lyrics_tracks.json, one process per track, and keeps the log
# and a dump of the lyrics table for each.
#
# --lyrics searches (songs filter), then looks up the first three songs in
# order, then the first once more (from the database). The first lookup is the
# measured one: it is the list's track (checked afterwards by analyze.ts) and
# the first request of the process to lrclib.net (a fresh TLS connection).
#
# Usage:
#   run_lyrics.ps1 -Pass cold01                     # fresh data dir per track: empty lyrics table
#   run_lyrics.ps1 -Pass cached01 -ReuseFrom cold01 # same data dirs again: every lookup is a database hit
#   run_lyrics.ps1 -Pass proxy01 -ProxyPort 8765    # LRCLIB through lrclib_proxy.ts (per-provider split)
#   run_lyrics.ps1 -Pass ytmonly01 -LrclibUrl http://127.0.0.1:9   # LRCLIB port closed (Windows retries ~4 s), then YouTube Music
#   run_lyrics.ps1 -Pass proxyfail01 -ProxyPort 8766 -ProxyFail     # LRCLIB stub answers 503 at once: YouTube Music leg alone
#   -Only 3,17      runs just those track numbers
param(
    [Parameter(Mandatory = $true)][string]$Pass,
    [string]$ReuseFrom = '',
    [int]$ProxyPort = 0,
    [string]$LrclibUrl = '',
    [switch]$ProxyFail,
    [int[]]$Only = @(),
    [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$Deno = 'C:\dev\monolist-deps\bin\deno.exe'
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $here
$list = (Get-Content -Raw -Encoding UTF8 (Join-Path $bench 'lyrics_tracks.json') | ConvertFrom-Json).tracks
$logDir = Join-Path $here "logs\$Pass"
New-Item -ItemType Directory -Force $logDir, (Join-Path $here 'dl') | Out-Null

$env:MONOLIST_DOWNLOAD_DIR = Join-Path $here 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
# Wall-clock stamps, so proxy timestamps (epoch ms) line up with the app's.
$env:QT_MESSAGE_PATTERN = '%{time yyyy-MM-ddTHH:mm:ss.zzz} %{message}'
Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue

$proxy = $null
if ($ProxyPort -gt 0) {
    $proxyLog = Join-Path $logDir 'lrclib_proxy.jsonl'
    $proxyArgs = @('run', '--allow-net', '--allow-write',
        ('"' + (Join-Path $here 'lrclib_proxy.ts') + '"'), $ProxyPort, ('"' + $proxyLog + '"'))
    if ($ProxyFail) { $proxyArgs += '--fail' }
    $proxy = Start-Process -FilePath $Deno -ArgumentList $proxyArgs `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'proxy.out') `
        -RedirectStandardError (Join-Path $logDir 'proxy.err')
    Start-Sleep -Seconds 3
}

$meta = @()
foreach ($t in $list) {
    if ($Only.Count -gt 0 -and $Only -notcontains [int]$t.n) { continue }
    $dataName = if ($ReuseFrom) { $ReuseFrom } else { $Pass }
    $data = Join-Path $here ("data\{0}\{1:D2}" -f $dataName, [int]$t.n)
    if (-not $ReuseFrom -and (Test-Path $data)) { Remove-Item -Recurse -Force $data }
    New-Item -ItemType Directory -Force $data | Out-Null
    $env:MONOLIST_DATA_DIR = $data
    $log = Join-Path $logDir ("{0:D2}.err" -f [int]$t.n)
    $appArgs = @('--lyrics', ('"' + $t.query + '"'))
    if ($ProxyPort -gt 0) { $appArgs += @('--set', 'lrclib_url', "http://127.0.0.1:$ProxyPort") }
    elseif ($LrclibUrl) { $appArgs += @('--set', 'lrclib_url', $LrclibUrl) }
    $started = Get-Date
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -ArgumentList $appArgs -RedirectStandardError $log `
        -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
    $timedOut = $false
    if (-not $p.WaitForExit(90000)) { try { $p.Kill() } catch {}; $timedOut = $true }
    $wall = $sw.ElapsedMilliseconds
    & $Deno run --allow-read --allow-write (Join-Path $here 'dumpdb.ts') $data (Join-Path $logDir ("{0:D2}.db.json" -f [int]$t.n)) | Out-Null
    $meta += [pscustomobject]@{ n = [int]$t.n; started = $started.ToString('o'); wallMs = $wall;
                                exit = $p.ExitCode; timedOut = $timedOut; data = $data }
    $res = (Get-Content $log -Encoding UTF8 | Where-Object { $_ -match 'selftest:\s{3}\S+ in \d+ ms' } | Select-Object -First 1)
    Write-Output ("{0} #{1,2} {2,-45} {3}" -f $Pass, $t.n, $t.query, ($res -replace '^\S+ selftest:\s+', ''))
}
if ($proxy) { try { Stop-Process -Id $proxy.Id -Force } catch {} }
$json = $meta | ConvertTo-Json -Depth 3
$runsName = if ($Only.Count -gt 0) { 'runs_only_' + ($Only -join '_') + '.json' } else { 'runs.json' }
[IO.File]::WriteAllText((Join-Path $logDir $runsName), $json, (New-Object Text.UTF8Encoding($false)))
