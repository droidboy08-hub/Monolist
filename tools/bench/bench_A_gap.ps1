# Suite A-gap: the UNMODIFIED app, a natural track end followed by the next queued track
# whose link was prefetched (autoplay's first song):
#   monolist.exe --play <id> 25 --at <duration-8>
# MONOLIST_MPV_LOG=debug. Per run: the silent gap from the end of the old track's audio
# (mpv "draining left over audio" ... "Uninit wasapi done") to the next "starting audio
# playback", and the phases between (as the Qt thread logged them).
param([string]$Exe = 'C:\dev\monolist-build\release\monolist.exe', [string]$Tag = 'before')
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$runs = @(@('LCIs3JXb5Aw', 170), @('CmThpha4Hoo', 164), @('DntZ3-yCaFs', 206), @('7nVctvQVz0U', 218), @('TiebZllW8As', 230),
          @('BSTsnWoslP4', 347), @('7TDeBi34OtE', 294), @('phLb_SoPBlA', 267), @('fsiPzT50ZiM', 254), @('4D7u5KF7SP8', 361))
$rows = @()
foreach ($r in $runs) {
    $name = "Agap_${Tag}_$($r[0])"
    $log = "$bench\logs\$name.log"
    $data = "$bench\data\$name"
    if (Test-Path $data) { Remove-Item -Recurse -Force $data }
    & "$bench\run.ps1" -Exe $Exe -Log $log -MpvLog 'debug' -DataDir $data -AppArgs @('--play', $r[0], '25', '--at', "$($r[1])") | Out-Null
    Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
    $lines = foreach ($l in [IO.File]::ReadAllLines($log)) { $m = [regex]::Match($l, '^\s*([\d.]+) (.*)$'); if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; text = $m.Groups[2].Value } } }
    $drain = $lines | Where-Object { $_.text -match 'cplayer: draining left over audio' } | Select-Object -First 1
    if (-not $drain) { $rows += [pscustomobject]@{ id = $r[0]; note = 'no natural end seen' }; continue }
    $after = $lines | Where-Object { $_.t -ge $drain.t }
    $un = $after | Where-Object { $_.text -match 'ao/wasapi: Uninit wasapi done' } | Select-Object -First 1
    $st = $after | Where-Object { $_.text -match 'status=Streaming source=(.*)$' } | Select-Object -First 1
    $open = $after | Where-Object { $_.text -match 'curl: Opening https' } | Select-Object -First 1
    $resp = $after | Where-Object { $_.text -match 'curl: proto=https ok=1 code=2' } | Select-Object -First 1
    $aud = $after | Where-Object { $_.text -match 'cplayer: starting audio playback' } | Select-Object -First 1
    $ms = { param($a, $b) if ($a -and $b) { [int](1000 * ($b.t - $a.t)) } else { $null } }
    $rows += [pscustomobject]@{ id = $r[0]; drainMs = & $ms $drain $un; drainedToStatusMs = & $ms $un $st; drainedToRequestMs = & $ms $un $open
                                requestToResponseMs = & $ms $open $resp; responseToAudioMs = & $ms $resp $aud; silentGapMs = & $ms $un $aud
                                source = if ($st) { ([regex]::Match($st.text, 'source=(.*)$')).Groups[1].Value } else { $null } }
}
$rows | Format-Table -AutoSize | Out-String -Width 200
"silent gap: " + (Get-Stats ($rows | ForEach-Object { $_.silentGapMs }) | ConvertTo-Json -Compress)
$rows | ConvertTo-Json | Set-Content -Encoding utf8 "$bench\results_Agap_$Tag.json"
