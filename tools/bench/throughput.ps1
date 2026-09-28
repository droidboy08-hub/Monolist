# Downloads a googlevideo URL with curl.exe and reports bytes/s.
#  -Mode open     one GET with "Range: bytes=0-" (what mpv's curl stream sends), up to -MaxSec
#  -Mode norange  one GET with no Range header, up to -MaxSec
#  -Mode range1m  sequential 1 MiB Range requests in ONE curl process (connection reused,
#                 like OkHttp's pool in BitChord), until the file ends or -MaxSec passes
#  -UA chrome     the Chrome/149 UA mpv uses for InnerTube links (mpvengine.cpp kBrowserUserAgent)
#  -UA vision     the visionOS Safari UA the link was minted with (innertube.cpp kVisionUserAgent)
#  -UA none       no User-Agent header at all
#  -UA mpv        chrome UA plus mpv's other headers (Accept-Encoding, Icy-MetaData: 1)
# Output: one JSON line. For open/norange the file size is sampled every 200 ms, so the
# rate over the first 5 s and over 5 s..end are reported separately (burst-then-pace check).
param(
    [Parameter(Mandatory = $true)][string]$UrlFile,
    [ValidateSet('open', 'norange', 'range1m')][string]$Mode = 'open',
    [ValidateSet('chrome', 'vision', 'none', 'mpv')][string]$UA = 'chrome',
    [int]$MaxSec = 20,
    [long]$ChunkBytes = 1048576
)
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$url = [IO.File]::ReadAllText($UrlFile).Trim()
$clen = [long]([regex]::Match($url, '[?&]clen=(\d+)').Groups[1].Value)
$chromeUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
$visionUA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
$uaArgs = switch ($UA) {
    'chrome' { @('-A', $chromeUA) }
    'vision' { @('-A', $visionUA) }
    'none'   { @('-H', 'User-Agent:') }
    'mpv'    { @('-A', $chromeUA, '-H', 'Accept-Encoding: deflate, gzip, br, zstd', '-H', 'Icy-MetaData: 1') }
}
$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$out = Join-Path $tmp "dl_$tag.bin"
$wfile = Join-Path $tmp "w_$tag.txt"
$fmt = '%{http_code} %{time_connect} %{time_appconnect} %{time_starttransfer} %{time_total} %{size_download} %{speed_download} %{num_connects}\n'
$res = [ordered]@{ mode = $Mode; ua = $UA; clen = $clen; at = (Get-Date).ToString('o') }

if ($Mode -ne 'range1m') {
    $a = @('-s', '--http1.1', '--max-time', "$MaxSec", '-o', "`"$out`"", '-w', "`"$fmt`"") + ($uaArgs | ForEach-Object { if ($_ -match '\s|;|:') { "`"$_`"" } else { $_ } })
    if ($Mode -eq 'open') { $a += @('-r', '0-') }
    $a += "`"$url`""
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath curl.exe -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput $wfile
    $samples = New-Object System.Collections.Generic.List[object]
    while (-not $p.HasExited) {
        Start-Sleep -Milliseconds 200
        $len = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
        $samples.Add(@($sw.Elapsed.TotalSeconds, $len))
    }
    $p.WaitForExit()
    $wall = $sw.Elapsed.TotalSeconds
    $final = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
    $samples.Add(@($wall, $final))
    $p2 = (Get-Content $wfile -Raw).Trim().Split(' ')
    $res.code = [int]$p2[0]; $res.connect = [double]$p2[1]; $res.tls = [double]$p2[2]; $res.ttfb = [double]$p2[3]
    $res.total = [double]$p2[4]; $res.bytes = [long]$p2[5]; $res.curlSpeed = [double]$p2[6]
    # rate from first byte to the end of the transfer
    $body = [Math]::Max(0.001, $res.total - $res.ttfb)
    $res.rateFromFirstByte = [Math]::Round($res.bytes / $body)
    $res.complete = ($clen -gt 0 -and $res.bytes -ge $clen)
    # bytes in the first 5 s after the first byte, and the rate after that
    $t0 = $null; $b5 = $null
    foreach ($s in $samples) { if ($null -eq $t0 -and $s[1] -gt 0) { $t0 = $s[0] } }
    if ($null -ne $t0) {
        foreach ($s in $samples) { if ($s[0] -le $t0 + 5) { $b5 = $s[1]; $t5 = $s[0] } }
        $res.bytesFirst5s = $b5
        if ($final -gt $b5 -and $wall -gt $t5) { $res.rateAfter5s = [Math]::Round(($final - $b5) / ($wall - $t5)) } else { $res.rateAfter5s = $null }
    }
} else {
    $a = @('-s', '--http1.1')
    $n = if ($clen -gt 0) { [Math]::Ceiling($clen / $ChunkBytes) } else { 64 }
    for ($i = 0; $i -lt $n; $i++) {
        $from = $i * $ChunkBytes
        $to = [Math]::Min($from + $ChunkBytes - 1, $clen - 1)
        if ($i -gt 0) { $a += '--next' }
        $a += @('-s', '--http1.1', '-r', "$from-$to", '-o', "`"$out.$i`"", '-w', "`"$fmt`"") + ($uaArgs | ForEach-Object { if ($_ -match '\s|;|:') { "`"$_`"" } else { $_ } }) + "`"$url`""
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath curl.exe -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput $wfile
    if (-not $p.WaitForExit($MaxSec * 1000)) { try { $p.Kill() } catch {}; $res.killedAtMaxSec = $true }
    $wall = $sw.Elapsed.TotalSeconds
    $lines = @(Get-Content $wfile | Where-Object { $_.Trim() })
    $bytes = 0; $sumTotal = 0.0; $codes = @(); $ttfbs = @(); $connects = 0
    foreach ($l in $lines) {
        $q = $l.Trim().Split(' ')
        $codes += [int]$q[0]; $sumTotal += [double]$q[4]; $bytes += [long]$q[5]; $ttfbs += [double]$q[3]; $connects += [int]$q[7]
    }
    $res.requests = $lines.Count; $res.codes = ($codes | Group-Object | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ','
    $res.bytes = $bytes; $res.sumRequestTime = [Math]::Round($sumTotal, 3); $res.wall = [Math]::Round($wall, 3)
    $res.newConnections = $connects
    $res.rate = if ($sumTotal -gt 0) { [Math]::Round($bytes / $sumTotal) } else { 0 }
    $res.rateWall = [Math]::Round($bytes / $wall)
    $res.firstTtfb = $ttfbs[0]; $res.medianLaterTtfb = if ($ttfbs.Count -gt 1) { ($ttfbs[1..($ttfbs.Count - 1)] | Sort-Object)[[int](($ttfbs.Count - 1) / 2)] } else { $null }
    $res.complete = ($clen -gt 0 -and $bytes -ge $clen)
    Get-ChildItem $tmp -Filter "dl_$tag.bin.*" | Remove-Item -ErrorAction SilentlyContinue
}
Remove-Item $out, $wfile -ErrorAction SilentlyContinue
$res | ConvertTo-Json -Compress
