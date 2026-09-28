# Suite C: googlevideo transport outside the app, with curl.exe.
#  -Part ua      5 runs on the long track (fresh visionOS-tier URL per run, resolved exactly
#                as the app does by resolve_innertube.ps1); per run, every variant:
#                open (Range: bytes=0-) x {chrome, vision, none, mpv}, norange x chrome,
#                range1m x {chrome, vision, none}; each capped at 20 s.
#  -Part survey  every track in playback_tracks.json once: fresh URL, open/chrome (20 s cap)
#                and range1m/chrome; plus the curl-measured split of the resolve itself
#                (home-page GET for visitorData, then the /player POST).
param(
    [ValidateSet('ua', 'survey')][string]$Part = 'ua',
    [int]$Runs = 5,
    [string]$VideoId = 'Tu7oq3VNgpY',
    [string]$Tag = 'before'
)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$outFile = "$bench\results_C_${Part}_$Tag.jsonl"
$urlFile = Join-Path $tmp 'url_c.txt'
function Emit($obj, $extra) {
    $o = $obj | ConvertFrom-Json
    foreach ($k in $extra.Keys) { $o | Add-Member -NotePropertyName $k -NotePropertyValue $extra[$k] -Force }
    ($o | ConvertTo-Json -Compress -Depth 4) | Add-Content -Encoding utf8 $outFile
    $o
}
if ($Part -eq 'ua') {
    $variants = @(@('open', 'chrome'), @('range1m', 'chrome'), @('open', 'vision'), @('range1m', 'vision'),
                  @('open', 'none'), @('range1m', 'none'), @('open', 'mpv'), @('norange', 'chrome'))
    for ($r = 1; $r -le $Runs; $r++) {
        $res = & "$bench\resolve_innertube.ps1" -VideoId $VideoId -UrlFile $urlFile
        Emit $res @{ run = $r; kind = 'resolve' } | Out-Null
        # rotate the order each run so no variant always goes first on a fresh URL
        $k = ($r - 1) % $variants.Count
        $order = @($variants[$k..($variants.Count - 1)]) + @(if ($k -gt 0) { $variants[0..($k - 1)] })
        foreach ($v in $order) {
            $t = & "$bench\throughput.ps1" -UrlFile $urlFile -Mode $v[0] -UA $v[1] -MaxSec 20
            $o = Emit $t @{ run = $r; kind = 'download'; videoId = $VideoId }
            Write-Output ("run {0} {1,-8} {2,-6} bytes={3} rate={4}" -f $r, $v[0], $v[1], $o.bytes, $(if ($o.rateFromFirstByte) { $o.rateFromFirstByte } else { $o.rate }))
        }
    }
} else {
    $tracks = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks
    foreach ($tr in $tracks) {
        $res = & "$bench\resolve_innertube.ps1" -VideoId $tr.videoId -UrlFile $urlFile
        $ro = Emit $res @{ kind = 'resolve'; n = $tr.n }
        foreach ($v in @(@('open', 'chrome'), @('range1m', 'chrome'))) {
            $t = & "$bench\throughput.ps1" -UrlFile $urlFile -Mode $v[0] -UA $v[1] -MaxSec 20
            $o = Emit $t @{ kind = 'download'; videoId = $tr.videoId; n = $tr.n; itag = $ro.itag }
            Write-Output ("{0,2} {1} itag={2} {3,-8} bytes={4}/{5} rate={6}" -f $tr.n, $tr.videoId, $ro.itag, $v[0], $o.bytes, $o.clen, $(if ($o.rateFromFirstByte) { $o.rateFromFirstByte } else { $o.rate }))
        }
    }
}
