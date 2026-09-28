# B3 (QT4): per open (every "mpv loadfile" in an instrumented run's mpv log-file),
# the itag loaded (from the link's itag= parameter), the decoded codec and rate
# mpv reports ("Audio --aid=1 ... (opus 2ch 48000 Hz)"), and whether swresample
# changed the sample rate ("[swresample] 44100Hz ... -> 48000Hz ..."). Every open
# shows a swresample line (floatp -> float at the same rate); only a rate change
# counts as resampled. The track is the last "BENCH beginTrack <id>" before it.
# The Qt log's "innertube: <id> chose itag ..." lines (B3 build only) are collected too.
# Links are never printed.
param(
    [Parameter(Mandatory = $true)][string]$Pattern,   # e.g. 'B_b3opus_seq_*'
    [string]$Out = ''
)
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$opens = @()
$chose = @()
foreach ($f in Get-ChildItem "$bench\logs" -Filter "$Pattern.mpv.log") {
    $track = ''
    $cur = $null
    foreach ($line in [IO.File]::ReadAllLines($f.FullName)) {
        if ($line -match 'BENCH beginTrack (\S+)') { $track = $Matches[1]; continue }
        if ($line -match 'BENCH mpv loadfile ') {
            if ($cur) { $opens += [pscustomobject]$cur }
            # The mark's copy of the link is cut short; the itag is read from the
            # first full link after it (mpv's "[curl] Opening https://...").
            $cur = [ordered]@{ file = $f.Name; id = $track; itag = $null; codec = $null; rate = $null; resampleFrom = $null; resampleTo = $null }
            continue
        }
        if (-not $cur) { continue }
        if ($null -eq $cur.itag -and $line -match 'https://\S*[?&]itag=(\d+)') { $cur.itag = [int]$Matches[1] }
        if (-not $cur.codec -and $line -match '--aid=\d+.*\((\S+) \d+ch (\d+) Hz') { $cur.codec = $Matches[1]; $cur.rate = [int]$Matches[2]; continue }
        if (-not $cur.resampleFrom -and $line -match '\[swresample\] (\d+)Hz \S+ \S+ -> (\d+)Hz') { $cur.resampleFrom = [int]$Matches[1]; $cur.resampleTo = [int]$Matches[2] }
    }
    if ($cur) { $opens += [pscustomobject]$cur }
    $qt = $f.FullName -replace '\.mpv\.log$', '.qt.log'
    if (Test-Path $qt) {
        foreach ($line in [IO.File]::ReadAllLines($qt)) {
            if ($line -match 'innertube: (\S+) chose itag (\d+): (.*)$') { $chose += [pscustomobject]@{ file = $f.Name; id = $Matches[1]; itag = [int]$Matches[2]; text = $Matches[3] } }
        }
    }
}
$played = @($opens | Where-Object { $_.codec })
$resampled = @($played | Where-Object { $_.resampleFrom -and $_.resampleFrom -ne $_.resampleTo })
$summary = [ordered]@{
    pattern = $Pattern
    files = @($opens | Select-Object -ExpandProperty file -Unique).Count
    opens = $opens.Count
    openedWithAudio = $played.Count
    byItag = (($opens | Group-Object itag | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')
    byCodecRate = (($played | Group-Object { "$($_.codec)@$($_.rate)" } | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')
    resampled = $resampled.Count
    resampledIds = (($resampled | Group-Object id | ForEach-Object { "$($_.Name)x$($_.Count)(itag " + (($_.Group | Select-Object -ExpandProperty itag -Unique) -join '/') + ")" }) -join ' ')
    opusOfferedButAac = @($chose | Where-Object { $_.itag -ne 251 -and $_.itag -ne 774 -and $_.text -match '\b251\b' }).Count
    choseLines = $chose.Count
    choseByItag = (($chose | Group-Object itag | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')
    choseNon251 = (($chose | Where-Object { $_.itag -ne 251 } | ForEach-Object { "$($_.id): itag $($_.itag): $($_.text)" } | Select-Object -Unique) -join ' | ')
}
$json = [pscustomobject]$summary | ConvertTo-Json -Depth 3
if ($Out) { [IO.File]::WriteAllText($Out, $json, (New-Object Text.UTF8Encoding($false))) }
$json
