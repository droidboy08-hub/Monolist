# B0 (4): candidate no-cipher InnerTube clients for NC-1, with curl.exe, on the 12 benchmark ids.
# Client names and versions are the paper's §3.2.1 catalogue (facts only; no reference code
# opened). Device fields for ANDROID_VR / TVHTML5_SIMPLY are the publicly known identities.
# Per (client, id):
#   /player POST as that client (visitorData from one home-page scrape, as the app does);
#   status, audio formats with a plain url vs signatureCipher, whether the best URL carries
#   n= or pot=; then two media requests on the best audio URL with the app's mpv UA:
#     r1 = bytes=0-1048575 (first request), r2 = bytes=1048576- (open-ended rest, 20 s cap),
#   which shows a first-request refusal, "one request, then 403", and n-throttling.
# URLs are never printed or saved. Output: bench\b0\results_nc1.jsonl + a summary.
param([int]$MaxSec = 20)
$ErrorActionPreference = 'Stop'
Set-Location $env:TEMP
$b0 = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $b0
$tmp = Join-Path $b0 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$outFile = Join-Path $b0 'results_nc1.jsonl'
Remove-Item $outFile -ErrorAction SilentlyContinue
$visionUA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
$mpvUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
function VrUA($v) { "com.google.android.apps.youtube.vr.oculus/$v (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip" }
function VrClient($v) {
    @{ name = "ANDROID_VR $v"; ua = (VrUA $v); web = $false; hdr = @{ 'X-YouTube-Client-Name' = '28'; 'X-YouTube-Client-Version' = $v }
       client = [ordered]@{ clientName = 'ANDROID_VR'; clientVersion = $v; deviceMake = 'Oculus'; deviceModel = 'Quest 3'
                            androidSdkVersion = 32; osName = 'Android'; osVersion = '12L'; userAgent = (VrUA $v) } }
}
function VisionClient($v) {
    @{ name = "VISIONOS $v"; ua = $visionUA; web = $true; hdr = @{}
       client = [ordered]@{ clientName = 'VISIONOS'; clientVersion = $v; deviceMake = 'Apple'; deviceModel = 'RealityDevice17,1'
                            osName = 'visionOS'; osVersion = '26.5.23O471'; userAgent = $visionUA } }
}
$clients = @(
    (VisionClient '1.02'),   # control: exactly what the app sends today
    (VisionClient '0.1'),
    (VrClient '1.65.10'), (VrClient '1.61.48'), (VrClient '1.43.32'),
    @{ name = 'TVHTML5_SIMPLY 1.0'; ua = $mpvUA; web = $true; hdr = @{ 'X-YouTube-Client-Name' = '75'; 'X-YouTube-Client-Version' = '1.0' }
       client = [ordered]@{ clientName = 'TVHTML5_SIMPLY'; clientVersion = '1.0' } }
)
$tracks = (Get-Content (Join-Path $bench 'playback_tracks.json') -Raw | ConvertFrom-Json).tracks

$cookies = Join-Path $tmp 'nc_cookies.txt'
$page = Join-Path $tmp 'nc_home.html'
& curl.exe -s -o $page -c $cookies -A $visionUA 'https://www.youtube.com/' | Out-Null
$m = [regex]::Match([IO.File]::ReadAllText($page), '"visitorData":"(.*?)"')
$vd = if ($m.Success) { (ConvertFrom-Json ('["' + $m.Groups[1].Value + '"]'))[0] } else { '' }
Remove-Item $page -ErrorAction SilentlyContinue
Write-Output ("visitorData " + $(if ($vd) { 'present' } else { 'MISSING' }))

function MediaGet($url, $range, $ua, $maxSec) {
    $cfg = Join-Path $tmp 'nc_media.cfg'
    [IO.File]::WriteAllText($cfg, "url = `"$url`"`nrange = `"$range`"`nuser-agent = `"$ua`"`n", $utf8)
    $w = & curl.exe -s -o NUL -K $cfg --max-time $maxSec -w '%{http_code} %{size_download} %{time_starttransfer} %{time_total} %{content_type}'
    Remove-Item $cfg -ErrorAction SilentlyContinue
    $p = "$w".Trim().Split(' ')
    [ordered]@{ code = [int]$p[0]; bytes = [long]$p[1]; ttfbMs = [Math]::Round(1000 * [double]$p[2]); ms = [Math]::Round(1000 * [double]$p[3]); type = $(if ($p.Count -gt 4) { $p[4] } else { '' }) }
}

foreach ($c in $clients) {
    foreach ($tr in $tracks) {
        $id = $tr.videoId
        $ctx = [ordered]@{ hl = 'en'; gl = 'US' }
        foreach ($k in $c.client.Keys) { $ctx[$k] = $c.client[$k] }
        $ctx.visitorData = $vd
        $body = [ordered]@{ videoId = $id; contentCheckOk = $true; racyCheckOk = $true; context = [ordered]@{ client = $ctx } }
        $bodyFile = Join-Path $tmp 'nc_body.json'; $respFile = Join-Path $tmp 'nc_player.json'
        [IO.File]::WriteAllText($bodyFile, ($body | ConvertTo-Json -Depth 5 -Compress), $utf8)
        $a = @('-s', '-o', $respFile, '-w', '%{http_code} %{time_total}', '-A', $c.ua, '-H', 'Content-Type: application/json', '-H', "X-Goog-Visitor-Id: $vd")
        if ($c.web) { $a += @('-H', 'Origin: https://www.youtube.com', '-H', 'Referer: https://www.youtube.com/') }
        foreach ($h in $c.hdr.Keys) { $a += @('-H', "${h}: $($c.hdr[$h])") }
        $a += @('-b', $cookies, '--data-binary', "@$bodyFile", 'https://www.youtube.com/youtubei/v1/player?prettyPrint=false')
        $w = (& curl.exe @a).Trim().Split(' ')
        $rec = [ordered]@{ client = $c.name; n = $tr.n; id = $id; playerHttp = [int]$w[0]; playerMs = [Math]::Round(1000 * [double]$w[1]) }
        $j = $null
        try { $j = [IO.File]::ReadAllText($respFile) | ConvertFrom-Json } catch { }
        Remove-Item $bodyFile, $respFile -ErrorAction SilentlyContinue
        $rec.status = if ($j) { $j.playabilityStatus.status } else { 'no-json' }
        $reason = if ($j) { "$($j.playabilityStatus.reason)" } else { '' }
        $rec.reason = if ($reason.Length -gt 80) { $reason.Substring(0, 80) } else { $reason }
        $audio = @(); if ($j -and $j.streamingData) { $audio = @($j.streamingData.adaptiveFormats | Where-Object { $_.mimeType -like 'audio*' }) }
        $rec.audioFormats = $audio.Count
        $rec.plain = @($audio | Where-Object { $_.url }).Count
        $rec.cipher = @($audio | Where-Object { $_.signatureCipher -or $_.cipher }).Count
        $rec.sabrOnly = [bool]($j -and $j.streamingData -and $j.streamingData.serverAbrStreamingUrl -and $rec.plain -eq 0)
        $best = $null
        foreach ($f in $audio) { if ($f.url -and (-not $best -or [int]$f.bitrate -gt [int]$best.bitrate)) { $best = $f } }
        if ($best) {
            $q = ([uri]$best.url).Query
            $rec.itag = [int]$best.itag; $rec.clen = [long]$best.contentLength
            $rec.hasN = [bool]($q -match '[?&]n='); $rec.hasPot = [bool]($q -match '[?&]pot=')
            $rec.cParam = ([regex]::Match($q, '[?&]c=([^&]+)')).Groups[1].Value
            $rec.loudnessDb = $best.loudnessDb
            $r1 = MediaGet $best.url '0-1048575' $mpvUA 10
            $rec.r1 = "$($r1.code) $($r1.bytes)B $($r1.ms)ms"
            $rec.r1ok = ($r1.code -eq 206 -and $r1.bytes -eq [Math]::Min(1048576, $rec.clen) -and $r1.type -like 'audio*')
            if (-not $rec.r1ok) {
                $r1b = MediaGet $best.url '0-1048575' $c.ua 10
                $rec.r1ClientUA = "$($r1b.code) $($r1b.bytes)B"
            }
            if ($rec.clen -gt 1048576) {
                $r2 = MediaGet $best.url '1048576-' $mpvUA $MaxSec
                $rest = $rec.clen - 1048576
                $rec.r2 = "$($r2.code) $($r2.bytes)/$rest B $($r2.ms)ms"
                $rec.r2ok = ($r2.code -eq 206 -and $r2.bytes -eq $rest)
                $rec.r2RateKBs = if ($r2.ms -gt 0) { [Math]::Round($r2.bytes / $r2.ms) } else { $null }   # bytes/ms = kB/s
            }
        }
        ($rec | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 $outFile
        Write-Output ("{0,-18} {1,2} {2} http={3} {4,5}ms {5,-14} audio={6} plain={7} cipher={8} itag={9} n={10} pot={11} r1=[{12}] r2=[{13}] {14}" -f $c.name, $tr.n, $id, $rec.playerHttp, $rec.playerMs, $rec.status, $rec.audioFormats, $rec.plain, $rec.cipher, $rec.itag, $rec.hasN, $rec.hasPot, $rec.r1, $rec.r2, $rec.reason)
        Start-Sleep -Milliseconds 400
    }
}
Remove-Item $cookies -ErrorAction SilentlyContinue
