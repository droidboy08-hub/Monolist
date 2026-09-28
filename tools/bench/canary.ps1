# B2 (NC-1): the canary. Is YouTube still answering the app's /player clients?
# For each canary id (playback_tracks.json "canary", or -Ids), asks /player as
# VISIONOS 1.02 (the app's first client) and VISIONOS 0.1 (its second) exactly as
# the app does, with curl (resolve_innertube.ps1, so it works whatever build is
# benchmarked), then fetches the link's first and second MiB with the mpv UA.
# A client passes when it answers OK with a plain link (no n=, no pot=) that
# serves both MiBs (a link that serves only the first is how a client needing a PO
# token shows itself; ANDROID_VR did exactly that in B0).
# Prints one line per (id, client) and a verdict: "CANARY OK", or which client broke.
# Exits 1 when VISIONOS 1.02 is broken: every play would then cost the second
# client's round trip, or yt-dlp's ~3 s. Appends each run to results_canary.jsonl.
# URLs are never printed or kept (they carry the machine's public IP).
param([string[]]$Ids = @())
$ErrorActionPreference = 'Stop'
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$tmp = Join-Path $bench 'tmp'
New-Item -ItemType Directory -Force $tmp | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
if (-not $Ids) { $Ids = @((Get-Content (Join-Path $bench 'playback_tracks.json') -Raw | ConvertFrom-Json).canary) }
$mpvUA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36'

function MediaGet($urlFile, $range) {
    $cfg = Join-Path $tmp 'canary_media.cfg'
    $url = [IO.File]::ReadAllText($urlFile)
    [IO.File]::WriteAllText($cfg, "url = `"$url`"`nrange = `"$range`"`nuser-agent = `"$mpvUA`"`n", $utf8)
    $w = & curl.exe -s -o NUL -K $cfg --max-time 20 -w '%{http_code} %{size_download}'
    Remove-Item $cfg -ErrorAction SilentlyContinue
    $p = "$w".Trim().Split(' ')
    [ordered]@{ code = [int]$p[0]; bytes = [long]$p[1] }
}

$rows = @()
foreach ($id in $Ids) {
    foreach ($v in '1.02', '0.1') {
        $urlFile = Join-Path $tmp ("canary_url_" + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.txt')
        $r = $null
        try { $r = & (Join-Path $bench 'resolve_innertube.ps1') -VideoId $id -ClientVersion $v -UrlFile $urlFile | ConvertFrom-Json } catch { }
        $row = [ordered]@{ at = (Get-Date).ToString('o'); id = $id; client = "VISIONOS $v"; status = $(if ($r) { $r.status } else { 'no answer' })
                           playerMs = $(if ($r) { [Math]::Round(1000 * $r.player.total) } else { $null }); itag = $null; plain = $false; first = ''; second = ''; ok = $false }
        if ($r -and $r.itag -and (Test-Path $urlFile)) {
            $row.itag = $r.itag
            $row.plain = -not $r.hasN -and -not $r.hasPot
            $a = MediaGet $urlFile '0-1048575'
            $row.first = "$($a.code) $($a.bytes)"
            $firstOk = $a.code -eq 206 -and $a.bytes -eq [Math]::Min(1048576, $r.clen)
            $secondOk = $true
            if ($r.clen -gt 1048576) {
                $b = MediaGet $urlFile '1048576-2097151'
                $row.second = "$($b.code) $($b.bytes)"
                $secondOk = $b.code -eq 206 -and $b.bytes -eq [Math]::Min(1048576, $r.clen - 1048576)
            }
            $row.ok = $row.status -eq 'OK' -and $row.plain -and $firstOk -and $secondOk
        }
        Remove-Item $urlFile -ErrorAction SilentlyContinue
        $rows += [pscustomobject]$row
        ($row | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 (Join-Path $bench 'results_canary.jsonl')
        Write-Output ("canary {0} {1,-14} {2,-14} {3,5} ms itag={4} plain={5} 1st=[{6}] 2nd=[{7}] {8}" -f $id, $row.client, $row.status, $row.playerMs, $row.itag, $row.plain, $row.first, $row.second, $(if ($row.ok) { 'ok' } else { 'BROKEN' }))
    }
}
$primaryBroken = @($rows | Where-Object { $_.client -eq 'VISIONOS 1.02' -and -not $_.ok }).Count
$secondBroken = @($rows | Where-Object { $_.client -eq 'VISIONOS 0.1' -and -not $_.ok }).Count
if ($primaryBroken -eq 0 -and $secondBroken -eq 0) { Write-Output 'CANARY OK' }
if ($primaryBroken -gt 0) { Write-Output "CANARY: VISIONOS 1.02 BROKEN on $primaryBroken of $($Ids.Count) canary ids (the app's first client)" }
if ($secondBroken -gt 0) { Write-Output "CANARY: VISIONOS 0.1 BROKEN on $secondBroken of $($Ids.Count) canary ids (the app's second client)" }
if ($primaryBroken -gt 0) { exit 1 }
exit 0
