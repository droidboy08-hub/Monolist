# B0 (3): EBU R128 loudness of the stream the app would play, for the 13 loudness-probe
# tracks (the 12 benchmark ids + IPeJ7iM55hc, as in qt_probe\loudness.json).
# Per track: resolve as the app does (VISIONOS 1.02 /player, the highest-bitrate audio
# format with a plain URL), keep that format's loudnessDb and perceptualLoudnessDb, then
# stream the bytes with curl into ffmpeg's ebur128 filter (-f null: nothing is written).
# Requests are at most 8,000,000 bytes each, because googlevideo paces requests that ask
# for more than ~8-12 MiB (Step 2). URLs never leave this script (they carry the public IP).
# Output: bench\b0\results_ebur128.json
param([string[]]$Ids = @('7nVctvQVz0U','DntZ3-yCaFs','BSTsnWoslP4','7TDeBi34OtE','phLb_SoPBlA','LCIs3JXb5Aw',
                         'TiebZllW8As','fsiPzT50ZiM','Tu7oq3VNgpY','munhCAshfH0','CmThpha4Hoo','4D7u5KF7SP8','IPeJ7iM55hc'))
$ErrorActionPreference = 'Stop'
Set-Location $env:TEMP   # cmd.exe cannot start in a UNC directory
$b0 = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $b0 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$ffmpeg = 'C:\dev\monolist-deps\bin\ffmpeg.exe'
$visionUA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
$mpvUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
$utf8 = New-Object Text.UTF8Encoding($false)
$cookies = Join-Path $tmp 'eb_cookies.txt'
$page = Join-Path $tmp 'eb_home.html'
& curl.exe -s -o $page -c $cookies -A $visionUA 'https://www.youtube.com/' | Out-Null
$m = [regex]::Match([IO.File]::ReadAllText($page), '"visitorData":"(.*?)"')
$vd = if ($m.Success) { (ConvertFrom-Json ('["' + $m.Groups[1].Value + '"]'))[0] } else { '' }
Remove-Item $page -ErrorAction SilentlyContinue
$out = @()
foreach ($id in $Ids) {
  $failed = @()
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    $body = [ordered]@{ videoId = $id; contentCheckOk = $true; racyCheckOk = $true
        context = [ordered]@{ client = [ordered]@{ hl = 'en'; gl = 'US'; clientName = 'VISIONOS'; clientVersion = '1.02'; deviceMake = 'Apple'
            deviceModel = 'RealityDevice17,1'; osName = 'visionOS'; osVersion = '26.5.23O471'; userAgent = $visionUA; visitorData = $vd } } }
    $bodyFile = Join-Path $tmp 'eb_body.json'; $respFile = Join-Path $tmp 'eb_player.json'
    [IO.File]::WriteAllText($bodyFile, ($body | ConvertTo-Json -Depth 5 -Compress), $utf8)
    & curl.exe -s -o $respFile -A $visionUA -H 'Content-Type: application/json' -H 'Origin: https://www.youtube.com' `
        -H 'Referer: https://www.youtube.com/' -H "X-Goog-Visitor-Id: $vd" -b $cookies --data-binary "@$bodyFile" `
        'https://www.youtube.com/youtubei/v1/player?prettyPrint=false' | Out-Null
    $j = [IO.File]::ReadAllText($respFile) | ConvertFrom-Json
    Remove-Item $bodyFile, $respFile -ErrorAction SilentlyContinue
    $best = $null
    foreach ($f in $j.streamingData.adaptiveFormats) {
        if (-not $f.url -or -not ($f.mimeType -like 'audio*')) { continue }
        if (-not $best -or [int]$f.bitrate -gt [int]$best.bitrate) { $best = $f }
    }
    $rec = [ordered]@{ id = $id; status = $j.playabilityStatus.status }
    if (-not $best) { $failed += "no plain audio ($($rec.status))"; continue }
    $clen = [long]$best.contentLength
    $rec.itag = [int]$best.itag; $rec.codec = ($best.mimeType -replace '.*codecs="', '') -replace '"', ''; $rec.clen = $clen
    $rec.formatLoudnessDb = $best.loudnessDb; $rec.perceptualLoudnessDb = $j.playerConfig.audioConfig.perceptualLoudnessDb
    $rec.topLevelLoudnessDb = $j.playerConfig.audioConfig.loudnessDb
    # One curl config per range, so the URL never appears on a command line.
    $chunk = 8000000L; $cfgs = @(); $start = 0L; $k = 0
    while ($start -lt $clen) {
        $end = [Math]::Min($clen - 1, $start + $chunk - 1)
        $cfg = Join-Path $tmp "eb_range$k.cfg"
        [IO.File]::WriteAllText($cfg, "url = `"$($best.url)`"`nrange = `"$start-$end`"`nuser-agent = `"$mpvUA`"`nsilent`nshow-error`nwrite-out = `"%{stderr}%{http_code} %{size_download}\n`"`n", $utf8)
        $cfgs += $cfg; $start = $end + 1; $k++
    }
    $curlErr = Join-Path $tmp 'eb_curl.txt'; $ffErr = Join-Path $tmp 'eb_ff.txt'
    Remove-Item $curlErr, $ffErr -ErrorAction SilentlyContinue
    $chain = ($cfgs | ForEach-Object { "curl.exe -K `"$_`" 2>>`"$curlErr`"" }) -join ' & '
    $clock = [Diagnostics.Stopwatch]::StartNew()
    & cmd.exe /c "($chain) | `"$ffmpeg`" -hide_banner -nostats -i pipe:0 -vn -af ebur128=peak=true -f null - 2>`"$ffErr`""
    $rec.seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 1)
    $cl = @(Get-Content $curlErr -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\d{3} \d+$' })
    $rec.requests = ($cl -join '; ')
    $got = 0L; foreach ($c in $cl) { $got += [long]($c.Split(' ')[1]) }
    $rec.bytesRead = $got; $rec.complete = ($got -eq $clen)
    $ff = [IO.File]::ReadAllText($ffErr)
    $sum = $ff.Substring([Math]::Max(0, $ff.LastIndexOf('Summary:')))
    if ($sum -match 'I:\s+(-?[\d.]+) LUFS') { $rec.integratedLufs = [double]$Matches[1] }
    if ($sum -match 'LRA:\s+(-?[\d.]+) LU') { $rec.lraLu = [double]$Matches[1] }
    if ($sum -match 'Peak:\s+(-?[\d.]+|-inf) dBFS') { $rec.truePeakDbfs = $Matches[1] }
    Remove-Item (@($cfgs) + @($curlErr, $ffErr)) -ErrorAction SilentlyContinue
    if (-not $rec.complete -or $null -eq $rec.integratedLufs) { $failed += "attempt ${attempt}: [$($rec.requests)]"; Start-Sleep -Seconds 1; continue }
    $rec.attempts = $attempt; $rec.failedAttempts = ($failed -join ' | ')
    $out += [pscustomobject]$rec
    Write-Output ("{0} itag={1} ld={2} I={3} LUFS LRA={4} peak={5} bytes {6}/{7} [{8}] {9}s" -f $id, $rec.itag, $rec.formatLoudnessDb, $rec.integratedLufs, $rec.lraLu, $rec.truePeakDbfs, $got, $clen, $rec.requests, $rec.seconds)
    break
  }
  if ($failed.Count -ge 3) { $out += [pscustomobject]@{ id = $id; failedAttempts = ($failed -join ' | ') }; Write-Output "$id FAILED: $($failed -join ' | ')" }
  Start-Sleep -Milliseconds 500
}
Remove-Item $cookies -ErrorAction SilentlyContinue
[IO.File]::WriteAllText((Join-Path $b0 'results_ebur128.json'), ($out | ConvertTo-Json -Depth 4), $utf8)
