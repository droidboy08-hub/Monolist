# Network conditions at measurement time: curl.exe timing (new connection each
# time) to the two lyrics providers' hosts. Prints one line per request and
# saves logs\net_baseline_<stamp>.txt.
# Usage: net_baseline.ps1 [-Runs 10]
param([int]$Runs = 10)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path $here "logs\net_baseline_$stamp.txt"
New-Item -ItemType Directory -Force (Join-Path $here 'logs') | Out-Null
$fmt = 'dns=%{time_namelookup} tcp=%{time_connect} tls=%{time_appconnect} ttfb=%{time_starttransfer} total=%{time_total} code=%{http_code} size=%{size_download} ip=%{remote_ip}\n'
$targets = @(
    @{ name = 'lrclib-search'; url = 'https://lrclib.net/api/search?track_name=Blinding%20Lights&artist_name=The%20Weeknd' },
    @{ name = 'ytmusic-home';  url = 'https://music.youtube.com/generate_204' }
)
$lines = @()
foreach ($t in $targets) {
    for ($i = 1; $i -le $Runs; $i++) {
        $r = & curl.exe -s -o NUL -A 'Monolist/0.1 (desktop music player)' -w $fmt $t.url
        $lines += ("{0} {1,2} {2}" -f $t.name, $i, $r.Trim())
    }
}
$lines | Tee-Object -FilePath $out
