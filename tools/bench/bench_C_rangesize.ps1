# Suite C, part 3: which requests does googlevideo pace? On the long track (a fresh
# visionOS-tier URL per run), single GETs with different Range headers, Chrome UA, 20 s cap:
#   0-1MiB, 0-4MiB, 0-8MiB, 0-12MiB (bounded), 10000000- (open, last ~4.6 MB),
#   0-<clen-1> (explicit full range), 0- (open-ended, what mpv sends)
param([int]$Runs = 3, [string]$VideoId = 'Tu7oq3VNgpY', [string]$Tag = 'before')
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
$outFile = "$bench\results_C_rangesize_$Tag.jsonl"
$urlFile = Join-Path $tmp 'url_rs.txt'
$chromeUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'
$fmt = '%{http_code} %{time_appconnect} %{time_starttransfer} %{time_total} %{size_download}'
for ($r = 1; $r -le $Runs; $r++) {
    $res = & "$bench\resolve_innertube.ps1" -VideoId $VideoId -UrlFile $urlFile | ConvertFrom-Json
    $url = [IO.File]::ReadAllText($urlFile).Trim()
    $clen = [long]$res.clen
    $ranges = @('0-1048575', '0-4194303', '0-8388607', '0-12582911', '10000000-', "0-$($clen - 1)", '0-')
    foreach ($range in $ranges) {
        $w = & curl.exe -s --http1.1 --max-time 20 -o NUL -A $chromeUA -r $range -w $fmt $url
        $p = $w.Trim().Split(' ')
        $bytes = [long]$p[4]; $ttfb = [double]$p[2]; $total = [double]$p[3]
        $o = [ordered]@{ run = $r; range = $range; code = [int]$p[0]; bytes = $bytes; ttfb = $ttfb; total = $total
                         rateFromFirstByte = [Math]::Round($bytes / [Math]::Max(0.001, $total - $ttfb)) }
        ($o | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 $outFile
        Write-Output ("run {0} range {1,-16} code={2} bytes={3,9} time={4,6:N2}s rate={5}" -f $r, $range, $o.code, $bytes, $total, $o.rateFromFirstByte)
    }
}
