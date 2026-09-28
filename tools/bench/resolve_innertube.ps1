# Resolves a video id exactly the way Monolist's InnerTube tier does, with curl.exe:
#  1. GET https://www.youtube.com/ as the visionOS Safari UA, scrape "visitorData"
#     (innertube.cpp fetchVisitorData), keeping cookies like Qt's cookie jar does;
#  2. POST /youtubei/v1/player?prettyPrint=false as client VISIONOS 1.02, with
#     visitorData in the context and as X-Goog-Visitor-Id (innertube.cpp post/player);
#  3. pick the audio-only format with a plain url and the highest bitrate.
# Prints one JSON object: timings of both requests (curl -w) and the chosen URL.
# The URL is written to -UrlFile only (it carries the machine's public IP).
# B2: -ClientVersion 0.1 asks as the app's second client (VISIONOS 0.1, innertube.cpp
# PlayerFallback); everything else is identical. The result names the version.
param(
    [Parameter(Mandatory = $true)][string]$VideoId,
    [string]$UrlFile = '',
    [string]$VisitorData = '',   # reuse a visitorData (skip step 1), like a warm session
    [string]$ClientVersion = '1.02'
)
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$visionUA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$cookies = Join-Path $tmp "cookies_$tag.txt"
$page = Join-Path $tmp "home_$tag.html"
$fmt = '%{http_code} %{time_namelookup} %{time_connect} %{time_appconnect} %{time_starttransfer} %{time_total} %{size_download}'

function Split-W([string]$w) {
    $p = $w.Trim().Split(' ')
    [ordered]@{ code = [int]$p[0]; dns = [double]$p[1]; connect = [double]$p[2]; tls = [double]$p[3];
                ttfb = [double]$p[4]; total = [double]$p[5]; bytes = [long]$p[6] }
}

$result = [ordered]@{ videoId = $VideoId; client = "VISIONOS $ClientVersion"; at = (Get-Date).ToString('o') }
if (-not $VisitorData) {
    $w = & curl.exe -s -o $page -c $cookies -A $visionUA -w $fmt 'https://www.youtube.com/'
    $result.home = Split-W $w
    $html = [IO.File]::ReadAllText($page)
    $m = [regex]::Match($html, '"visitorData":"(.*?)"')
    if ($m.Success) { $VisitorData = (ConvertFrom-Json ('["' + $m.Groups[1].Value + '"]'))[0] }
    $result.visitorFound = [bool]$m.Success
    Remove-Item $page -ErrorAction SilentlyContinue
}
$body = [ordered]@{
    videoId = $VideoId; contentCheckOk = $true; racyCheckOk = $true
    context = [ordered]@{ client = [ordered]@{
        hl = 'en'; gl = 'US'; clientName = 'VISIONOS'; clientVersion = $ClientVersion; deviceMake = 'Apple'
        deviceModel = 'RealityDevice17,1'; osName = 'visionOS'; osVersion = '26.5.23O471'; userAgent = $visionUA
        visitorData = $VisitorData } }
}
$bodyFile = Join-Path $tmp "body_$tag.json"
$respFile = Join-Path $tmp "player_$tag.json"
[IO.File]::WriteAllText($bodyFile, ($body | ConvertTo-Json -Depth 5 -Compress), (New-Object Text.UTF8Encoding($false)))
$args2 = @('-s', '-o', $respFile, '-w', $fmt, '-A', $visionUA, '-H', 'Content-Type: application/json',
           '-H', 'Origin: https://www.youtube.com', '-H', 'Referer: https://www.youtube.com/',
           '-H', "X-Goog-Visitor-Id: $VisitorData", '--data-binary', "@$bodyFile")
if (Test-Path $cookies) { $args2 += @('-b', $cookies) }
$args2 += 'https://www.youtube.com/youtubei/v1/player?prettyPrint=false'
$w = & curl.exe @args2
$result.player = Split-W $w
$json = [IO.File]::ReadAllText($respFile) | ConvertFrom-Json
$result.status = $json.playabilityStatus.status
$best = $null
foreach ($f in $json.streamingData.adaptiveFormats) {
    if (-not $f.url) { continue }
    if (-not ($f.mimeType -like 'audio*')) { continue }
    if (-not $best -or [int]$f.bitrate -gt [int]$best.bitrate) { $best = $f }
}
if ($best) {
    $result.itag = $best.itag; $result.bitrate = $best.bitrate; $result.mime = $best.mimeType
    $result.clen = [long]$best.contentLength; $result.audioQuality = $best.audioQuality
    $result.host = ([uri]$best.url).Host
    $q = ([uri]$best.url).Query
    $result.hasN = [bool]($q -match '[?&]n='); $result.hasPot = [bool]($q -match '[?&]pot=')
    if ($UrlFile) { [IO.File]::WriteAllText($UrlFile, $best.url) }
}
$result.visitorData = if ($VisitorData) { 'present' } else { 'none' }
Remove-Item $bodyFile, $respFile, $cookies -ErrorAction SilentlyContinue
$result | ConvertTo-Json -Depth 4 -Compress
