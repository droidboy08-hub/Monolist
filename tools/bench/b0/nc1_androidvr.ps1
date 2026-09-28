# B0 (4b): follow-up on ANDROID_VR 1.65.10, whose /player answered with plain URLs that
# googlevideo then refused (nc1_clients.ps1). The paper (§3.2.5, §5.2) records that its URLs
# are fetched with NO client headers (OkHttp's default UA), in ranges of at most 512 KiB, and
# with a cpn= appended. Here: a FRESH /player per (id, variant), then three sequential
# 512 KiB ranges (0, 512K, 1M), per variant:
#   none    : curl with no User-Agent header, no cpn
#   okhttp  : User-Agent okhttp/4.12.0, cpn appended
#   client  : the ANDROID_VR client UA, cpn appended
#   chrome  : the app's mpv (Chrome) UA, cpn appended
# URLs are never printed or saved. Output: bench\b0\results_nc1_androidvr.jsonl
param([string[]]$Ids = @('7nVctvQVz0U','BSTsnWoslP4','phLb_SoPBlA','LCIs3JXb5Aw','Tu7oq3VNgpY','CmThpha4Hoo'))
$ErrorActionPreference = 'Stop'
Set-Location $env:TEMP
$b0 = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $b0 'tmp'
$utf8 = New-Object Text.UTF8Encoding($false)
$outFile = Join-Path $b0 'results_nc1_androidvr.jsonl'
Remove-Item $outFile -ErrorAction SilentlyContinue
$visionUA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
$mpvUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
$v = '1.65.10'
$vrUA = "com.google.android.apps.youtube.vr.oculus/$v (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip"
$variants = @(@{ name = 'none'; ua = ''; cpn = $false }, @{ name = 'okhttp'; ua = 'okhttp/4.12.0'; cpn = $true },
              @{ name = 'client'; ua = $vrUA; cpn = $true }, @{ name = 'chrome'; ua = $mpvUA; cpn = $true })
$cookies = Join-Path $tmp 'vr_cookies.txt'
$page = Join-Path $tmp 'vr_home.html'
& curl.exe -s -o $page -c $cookies -A $visionUA 'https://www.youtube.com/' | Out-Null
$m = [regex]::Match([IO.File]::ReadAllText($page), '"visitorData":"(.*?)"')
$vd = if ($m.Success) { (ConvertFrom-Json ('["' + $m.Groups[1].Value + '"]'))[0] } else { '' }
Remove-Item $page -ErrorAction SilentlyContinue
$alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
foreach ($id in $Ids) {
    foreach ($va in $variants) {
        $body = [ordered]@{ videoId = $id; contentCheckOk = $true; racyCheckOk = $true; context = [ordered]@{ client = [ordered]@{
            hl = 'en'; gl = 'US'; clientName = 'ANDROID_VR'; clientVersion = $v; deviceMake = 'Oculus'; deviceModel = 'Quest 3'
            androidSdkVersion = 32; osName = 'Android'; osVersion = '12L'; userAgent = $vrUA; visitorData = $vd } } }
        $bodyFile = Join-Path $tmp 'vr_body.json'; $respFile = Join-Path $tmp 'vr_player.json'
        [IO.File]::WriteAllText($bodyFile, ($body | ConvertTo-Json -Depth 5 -Compress), $utf8)
        & curl.exe -s -o $respFile -A $vrUA -H 'Content-Type: application/json' -H "X-Goog-Visitor-Id: $vd" `
            -H 'X-YouTube-Client-Name: 28' -H "X-YouTube-Client-Version: $v" -b $cookies --data-binary "@$bodyFile" `
            'https://www.youtube.com/youtubei/v1/player?prettyPrint=false' | Out-Null
        $j = [IO.File]::ReadAllText($respFile) | ConvertFrom-Json
        Remove-Item $bodyFile, $respFile -ErrorAction SilentlyContinue
        $best = $null
        foreach ($f in $j.streamingData.adaptiveFormats) { if ($f.url -and $f.mimeType -like 'audio*' -and (-not $best -or [int]$f.bitrate -gt [int]$best.bitrate)) { $best = $f } }
        $rec = [ordered]@{ id = $id; variant = $va.name; status = $j.playabilityStatus.status }
        if ($best) {
            $url = $best.url
            if ($va.cpn) { $cpn = -join (1..16 | ForEach-Object { $alphabet[(Get-Random -Maximum 64)] }); $url += "&cpn=$cpn" }
            $codes = @()
            foreach ($rg in '0-524287', '524288-1048575', '1048576-1572863') {
                $cfg = Join-Path $tmp 'vr_media.cfg'
                $uaLine = if ($va.ua) { "user-agent = `"$($va.ua)`"" } else { 'header = "User-Agent:"' }
                [IO.File]::WriteAllText($cfg, "url = `"$url`"`nrange = `"$rg`"`n$uaLine`n", $utf8)
                $w = & curl.exe -s -o NUL -K $cfg --max-time 10 -w '%{http_code} %{size_download}'
                Remove-Item $cfg -ErrorAction SilentlyContinue
                $codes += "$w".Trim()
            }
            $rec.ranges = ($codes -join '; ')
            $rec.allOk = (@($codes | Where-Object { $_ -match '^206 524288$' }).Count -eq 3)
        }
        ($rec | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 $outFile
        Write-Output ("{0} {1,-7} {2} [{3}] ok={4}" -f $id, $va.name, $rec.status, $rec.ranges, $rec.allOk)
        Start-Sleep -Milliseconds 400
    }
}
Remove-Item $cookies -ErrorAction SilentlyContinue
