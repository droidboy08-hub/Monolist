# Step JS (QT7): are YouTube's and JioSaavn's copies of a song on the same
# timeline? The mid-song move lines the two players up by their clocks, which is
# right only if the same second of music sits at the same time in both files.
# For each of the 10 benchmark songs JioSaavn has: 6 s decoded from each (fresh
# links, mono 8 kHz, bench\tmp, deleted after) around 30 s and 120 s, and the
# lag of the best normalised cross-correlation within +-1 s. Positive: the
# music comes later in JioSaavn's file than in YouTube's.
param([double[]]$Points = @(30, 120), [string]$Tag = 'js')
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
$out = "$bench\results_S_align_$Tag.jsonl"
$ffmpeg = 'C:\dev\monolist-deps\bin\ffmpeg.exe'
Add-Type -TypeDefinition @'
public static class Xcorr {
    // a: the reference window; b: a longer window centred on it (b starts `pad` samples earlier).
    public static double[] Best(short[] a, short[] b, int pad, int n) {
        double ea = 0; for (int i = 0; i < n; i++) ea += (double)a[i] * a[i];
        double best = double.MinValue; int lag = 0;
        for (int l = -pad; l <= pad; l++) {
            double s = 0, eb = 0; int o = pad + l;
            for (int i = 0; i < n; i++) { double v = b[o + i]; s += a[i] * v; eb += v * v; }
            double c = (ea > 0 && eb > 0) ? s / System.Math.Sqrt(ea * eb) : 0;
            if (c > best) { best = c; lag = l; }
        }
        return new double[] { lag, best };
    }
}
'@
function Decode($url, $from, $seconds) {
    $ErrorActionPreference = 'Continue'
    $f = Join-Path $tmp ("al_" + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.raw')
    & $ffmpeg -hide_banner -loglevel error -y -ss $from -t $seconds -i $url -ac 1 -ar 8000 -f s16le $f 2>$null
    if (-not (Test-Path $f)) { return , (New-Object Int16[] 0) }
    $bytes = [IO.File]::ReadAllBytes($f); [IO.File]::Delete($f)
    $s = New-Object Int16[] ([int]($bytes.Length / 2)); [Buffer]::BlockCopy($bytes, 0, $s, 0, $bytes.Length); , $s
}
$songs = @(
    @{ n = 1; v = '7nVctvQVz0U'; s = 'ZdcslMe3' }, @{ n = 2; v = 'DntZ3-yCaFs'; s = 'UklZuIOK' },
    @{ n = 3; v = 'BSTsnWoslP4'; s = 'n7JHo42T' }, @{ n = 5; v = 'phLb_SoPBlA'; s = 'qrJ2aEQy' },
    @{ n = 6; v = 'LCIs3JXb5Aw'; s = '0NIzs17f' }, @{ n = 7; v = 'TiebZllW8As'; s = '83M1xxgd' },
    @{ n = 8; v = 'fsiPzT50ZiM'; s = 'aRZbUYD7' }, @{ n = 10; v = 'munhCAshfH0'; s = 'auQ6LDS0' },
    @{ n = 11; v = 'CmThpha4Hoo'; s = 'PGVgj5hF' }, @{ n = 12; v = '4D7u5KF7SP8'; s = '4qz8qApJ' }
)
foreach ($song in $songs) {
    $yt = & "$bench\resolve_innertube.ps1" -VideoId $song.v -UrlFile "$tmp\al_yt.txt" | ConvertFrom-Json
    $ytUrl = [IO.File]::ReadAllText("$tmp\al_yt.txt").Trim()
    $sv = & "$bench\resolve_saavn.ps1" -SaavnId $song.s -UrlFile "$tmp\al_sv.txt" | ConvertFrom-Json
    $svUrl = [IO.File]::ReadAllText("$tmp\al_sv.txt").Trim()
    foreach ($p in $Points) {
        # YouTube 4 s at p-2, JioSaavn 6 s at p-3: lags of +-1 s.
        $a = Decode $ytUrl ($p - 2) 4
        if ($a.Length -lt 32000) {   # refused: a fresh link, once
            & "$bench\resolve_innertube.ps1" -VideoId $song.v -UrlFile "$tmp\al_yt.txt" | Out-Null
            $ytUrl = [IO.File]::ReadAllText("$tmp\al_yt.txt").Trim()
            $a = Decode $ytUrl ($p - 2) 4
        }
        $b = Decode $svUrl ($p - 3 + 0.0125) 6
        $o = [ordered]@{ n = $song.n; videoId = $song.v; at = $p; itag = $yt.itag; ytSamples = $a.Length; svSamples = $b.Length }
        if ($a.Length -ge 32000 -and $b.Length -ge 47800) {
            $r = [Xcorr]::Best($a, $b, 7900, 32000)
            $o.offsetMs = [Math]::Round($r[0] / 8.0, 1); $o.peak = [Math]::Round($r[1], 3)
        }
        ($o | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 $out
        Write-Output ("#{0,-2} at {1,4} s itag {2}: JioSaavn {3} ms from YouTube (peak {4})" -f $song.n, $p, $yt.itag, $o.offsetMs, $o.peak)
    }
}
