# C9 follow-up: force a client that needs the signature / n solver
# (default: tv, which yt-dlp serves with ciphered URLs), run it N times against
# one scratch --cache-dir, and see what is re-done on the warm runs:
# player JS download, Deno solve, and what the cache dir holds afterwards.
# Metadata only (--dump-single-json); the user's ~/.cache/yt-dlp is untouched.
param(
    [string]$Tools = "C:\dev\monolist-build\release\tools",
    [string]$VideoId = "LrM_Y39Gmhk",
    [string]$Client = "tv",
    [int]$Runs = 3
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cache = Join-Path $here "ytdlp-cache-forced"
if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
New-Item -ItemType Directory -Force $cache | Out-Null
$ytdlp = Join-Path $Tools "yt-dlp\yt-dlp.exe"
$deno = Join-Path $Tools "deno.exe"
$results = @()
for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $here ("ytdlp_forced_run{0}.stderr.txt" -f $i)
    $args = @('--ignore-config', '--encoding', 'utf-8', '--socket-timeout', '15', '--retries', '2',
              '--extractor-retries', '1', '--js-runtimes', "deno:$deno", '--ffmpeg-location', $Tools,
              '--cache-dir', $cache, '-v', '--extractor-args', "youtube:player_client=$Client",
              "https://www.youtube.com/watch?v=$VideoId", '--dump-single-json', '--no-playlist', '-f', 'bestaudio/best')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $ytdlp -ArgumentList $args -NoNewWindow -Wait -PassThru `
        -RedirectStandardError $log -RedirectStandardOutput (Join-Path $here ("ytdlp_forced_run{0}.json" -f $i))
    $sw.Stop()
    $lines = Get-Content $log
    $results += [pscustomobject]@{
        run = $i
        client = $Client
        exit = $p.ExitCode
        ms = $sw.ElapsedMilliseconds
        playerJsDownloaded = @($lines | Select-String -Pattern 'Downloading player ').Count
        denoRuns = @($lines | Select-String -Pattern 'Running deno').Count
        cacheLines = @($lines | Select-String -Pattern '(?i)(Saving|Loading|Loaded) .* (to|from) cache|cache' | ForEach-Object { $_.Line }) -join ' || '
        cacheFiles = @(Get-ChildItem $cache -Recurse -File | ForEach-Object { $_.FullName.Substring($cache.Length) }) -join ', '
    }
}
$results | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $here "results_C9_ytdlp_cache_forced.json")
$results | Format-List
