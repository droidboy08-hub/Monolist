# Checklist row C9 (signature / n solver with a persistent cache).
# Runs the app's own yt-dlp resolve (same arguments as YtDlp::resolveAudio plus
# YtDlp::commonArguments) twice against an EMPTY scratch cache dir, then once
# more, and records: wall time, what yt-dlp says it downloads/solves (-v), and
# what it leaves in the cache dir. Nothing is downloaded (--dump-single-json).
# The user's own ~/.cache/yt-dlp is not touched (--cache-dir points here).
param(
    [string]$Tools = "C:\dev\monolist-build\release\tools",
    [string]$VideoId = "LrM_Y39Gmhk",
    [int]$Runs = 3
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cache = Join-Path $here "ytdlp-cache"
if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
New-Item -ItemType Directory -Force $cache | Out-Null
$ytdlp = Join-Path $Tools "yt-dlp\yt-dlp.exe"
$deno = Join-Path $Tools "deno.exe"
$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $here ("ytdlp_run{0}.stderr.txt" -f $i)
    $args = @('--ignore-config', '--encoding', 'utf-8', '--socket-timeout', '15', '--retries', '2',
              '--extractor-retries', '1', '--js-runtimes', "deno:$deno", '--ffmpeg-location', $Tools,
              '--cache-dir', $cache, '-v',
              "https://www.youtube.com/watch?v=$VideoId", '--dump-single-json', '--no-playlist', '-f', 'bestaudio/best')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $ytdlp -ArgumentList $args -NoNewWindow -Wait -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $here ("ytdlp_run{0}.json" -f $i))
    $sw.Stop()
    $lines = Get-Content $log
    $results += [pscustomobject]@{
        run = $i
        exit = $p.ExitCode
        ms = $sw.ElapsedMilliseconds
        playerDownloads = @($lines | Select-String -Pattern 'Downloading player|player .*\.js|Downloading .*player').Count
        jscLines = @($lines | Select-String -Pattern '\[jsc|challenge|n challenge|nsig|sig function|Solving' | ForEach-Object { $_.Line }) -join ' || '
        cacheFiles = @(Get-ChildItem $cache -Recurse -File | ForEach-Object { $_.FullName.Substring($cache.Length) }) -join ', '
    }
}
$results | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $here "results_C9_ytdlp_cache.json")
$results | Format-List
