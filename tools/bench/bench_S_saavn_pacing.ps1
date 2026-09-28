# Suite S (JioSaavn follow-ups, step JS): does JioSaavn's CDN pace one request
# the way googlevideo paces one that asks for more than 8-12 MiB (Step 2, suite
# C)? The same measures as bench_C_throughput.ps1 / bench_C_rangesize.ps1, with
# curl.exe, on the 10 benchmark songs JioSaavn has (the ids its matcher took in
# saavn\live12-final-on.log), a fresh link per song:
#   open     one GET with "Range: bytes=0-" (what mpv sends), file size sampled
#            every 200 ms: rate from the first byte, and bytes in the first 5 s
#            against the rate after them (burst-then-pace check)
#   norange  one GET with no Range header
#   range1m  sequential 1 MiB ranges on one kept-alive connection (--next)
#   sizes    single bounded GETs 0-1MiB, 0-4MiB, 0-8MiB, 0-12MiB, and the last
#            ~4 MB open-ended, on the three largest files
# Bytes land in bench\tmp and are deleted as soon as each transfer is counted.
param([int]$OpenRuns = 3, [int]$RangeRuns = 2, [int]$MaxSec = 30, [string]$Tag = 'js')
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$outFile = "$bench\results_S_pacing_$Tag.jsonl"
$urlFile = Join-Path $tmp 'url_s.txt'
$ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
$fmt = '%{http_code} %{time_connect} %{time_appconnect} %{time_starttransfer} %{time_total} %{size_download} %{speed_download} %{num_connects}\n'
$songs = @(
    @{ n = 1;  id = 'ZdcslMe3'; title = 'The Fate of Ophelia' },
    @{ n = 2;  id = 'UklZuIOK'; title = 'Manchild' },
    @{ n = 3;  id = 'n7JHo42T'; title = 'Bohemian Rhapsody' },
    @{ n = 5;  id = 'qrJ2aEQy'; title = 'Not Like Us' },
    @{ n = 6;  id = '0NIzs17f'; title = 'Supernova' },
    @{ n = 7;  id = '83M1xxgd'; title = 'DtMF' },
    @{ n = 8;  id = 'aRZbUYD7'; title = 'Tum Hi Ho' },
    @{ n = 10; id = 'auQ6LDS0'; title = 'Baibaba Bimba' },
    @{ n = 11; id = 'PGVgj5hF'; title = 'Last Last' },
    @{ n = 12; id = '4qz8qApJ'; title = 'Get Lucky' }
)

function Emit($o) { ($o | ConvertTo-Json -Compress -Depth 4) | Add-Content -Encoding utf8 $outFile; $o }

function One-Request($url, $clen, $mode, $range) {
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $out = Join-Path $tmp "s_$tag.bin"; $wfile = Join-Path $tmp "sw_$tag.txt"
    $a = @('-s', '--http1.1', '--max-time', "$MaxSec", '-o', "`"$out`"", '-w', "`"$fmt`"", '-A', "`"$ua`"")
    if ($range) { $a += @('-r', $range) }
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
    $q = (Get-Content $wfile -Raw).Trim().Split(' ')
    $r = [ordered]@{ mode = $mode; range = $range; clen = $clen; code = [int]$q[0]; connect = [double]$q[1]; tls = [double]$q[2]
                     ttfb = [double]$q[3]; total = [double]$q[4]; bytes = [long]$q[5] }
    $r.rateFromFirstByte = [Math]::Round($r.bytes / [Math]::Max(0.001, $r.total - $r.ttfb))
    $t0 = $null; $b5 = $null; $t5 = $null
    foreach ($s in $samples) { if ($null -eq $t0 -and $s[1] -gt 0) { $t0 = $s[0] } }
    if ($null -ne $t0) {
        foreach ($s in $samples) { if ($s[0] -le $t0 + 5) { $b5 = $s[1]; $t5 = $s[0] } }
        $r.bytesFirst5s = $b5
        $r.rateAfter5s = if ($final -gt $b5 -and $wall -gt $t5) { [Math]::Round(($final - $b5) / ($wall - $t5)) } else { $null }
    }
    [IO.File]::Delete($out); [IO.File]::Delete($wfile)
    $r
}

function Ranged($url, $clen) {
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $wfile = Join-Path $tmp "sr_$tag.txt"
    $chunk = 1048576
    $n = [Math]::Ceiling($clen / $chunk)
    $a = @()
    for ($i = 0; $i -lt $n; $i++) {
        $from = $i * $chunk; $to = [Math]::Min($from + $chunk - 1, $clen - 1)
        if ($i -gt 0) { $a += '--next' }
        $a += @('-s', '--http1.1', '-r', "$from-$to", '-o', 'NUL', '-w', "`"$fmt`"", '-A', "`"$ua`"", "`"$url`"")
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath curl.exe -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput $wfile
    $killed = $false
    if (-not $p.WaitForExit($MaxSec * 1000)) { try { $p.Kill() } catch {}; $killed = $true }
    $wall = $sw.Elapsed.TotalSeconds
    $lines = @(Get-Content $wfile | Where-Object { $_.Trim() })
    $bytes = 0; $sum = 0.0; $codes = @(); $ttfbs = @(); $connects = 0
    foreach ($l in $lines) { $q = $l.Trim().Split(' '); $codes += [int]$q[0]; $sum += [double]$q[4]; $bytes += [long]$q[5]; $ttfbs += [double]$q[3]; $connects += [int]$q[7] }
    [IO.File]::Delete($wfile)
    [ordered]@{ mode = 'range1m'; clen = $clen; requests = $lines.Count; codes = (($codes | Group-Object | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ',')
                bytes = $bytes; wall = [Math]::Round($wall, 3); rate = if ($sum -gt 0) { [Math]::Round($bytes / $sum) } else { 0 }
                rateWall = [Math]::Round($bytes / $wall); newConnections = $connects; firstTtfb = $ttfbs[0]
                medianLaterTtfb = if ($ttfbs.Count -gt 1) { ($ttfbs[1..($ttfbs.Count - 1)] | Sort-Object)[[int](($ttfbs.Count - 1) / 2)] } else { $null }
                complete = ($bytes -ge $clen); killedAtMaxSec = $killed }
}

$sizes = @()
foreach ($s in $songs) {
    $res = & "$bench\resolve_saavn.ps1" -SaavnId $s.id -UrlFile $urlFile | ConvertFrom-Json
    $url = [IO.File]::ReadAllText($urlFile).Trim()
    $clen = [long]$res.bytes
    $sizes += @{ n = $s.n; clen = $clen; url = $url }
    Emit ([ordered]@{ kind = 'resolve'; n = $s.n; saavnId = $s.id; kbps = $res.kbps; bytes = $clen; apiMs = $res.apiMs; server = $res.server }) | Out-Null
    $modes = @()
    for ($i = 0; $i -lt $OpenRuns; $i++) { $modes += 'open' }
    $modes += 'norange'
    for ($i = 0; $i -lt $RangeRuns; $i++) { $modes += 'range1m' }
    foreach ($m in $modes) {
        $o = switch ($m) {
            'open'    { One-Request $url $clen 'open' '0-' }
            'norange' { One-Request $url $clen 'norange' $null }
            'range1m' { Ranged $url $clen }
        }
        $o.kind = 'download'; $o.n = $s.n
        Emit $o | Out-Null
        $rate = if ($o.rateFromFirstByte) { $o.rateFromFirstByte } else { $o.rate }
        Write-Output ("#{0,-2} {1,-8} code={2} bytes={3,9}/{4} rate={5,9} B/s first5s={6} after5s={7}" -f $s.n, $m, $(if ($o.code) { $o.code } else { $o.codes }), $o.bytes, $clen, $rate, $o.bytesFirst5s, $o.rateAfter5s)
    }
}
# Single bounded requests of growing size on the three largest files: where
# googlevideo starts pacing (8-12 MiB), does this CDN?
foreach ($f in ($sizes | Sort-Object { $_.clen } -Descending | Select-Object -First 3)) {
    $ranges = @('0-1048575', '0-4194303', '0-8388607', '0-12582911', "$([Math]::Max(0, $f.clen - 4194304))-")
    foreach ($range in $ranges) {
        $o = One-Request $f.url $f.clen 'size' $range
        $o.kind = 'rangesize'; $o.n = $f.n
        Emit $o | Out-Null
        Write-Output ("#{0,-2} size {1,-18} code={2} bytes={3,9} rate={4,9} B/s" -f $f.n, $range, $o.code, $o.bytes, $o.rateFromFirstByte)
    }
}
