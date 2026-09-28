# B1 (PP-03) re-measurement: bench_B.ps1 -Suite cold -Settle 0 (the results_B_launch0.json
# conditions: one fresh process per track, the play issued the moment the window is ready),
# with one difference that the stored visitor id needs:
#   -Primed   every launch starts from a copy of one "primed" database, made by a first
#             launch that fetched and stored a visitor id: the user's second and later
#             launches. Without it each launch gets an empty data folder: a first-ever launch.
# Same run.ps1 / parse_bench.ps1 / summarize_B.ps1 as bench_B, same 12 tracks.
# Also prints, per launch, how many visitorData fetches ran and how many bytes they took.
param(
    [string]$Exe = '',
    [string]$Tag = 'b1stored',
    [switch]$Primed,
    [int]$Settle = 0,
    [int]$Rounds = 1,
    [string]$Ids = ''
)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = "$bench\build-b1\monolist.exe" }
$tracks = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks
$list = @($tracks | ForEach-Object { $_.videoId })
if ($Ids) { $list = $Ids.Split(',') }
$stamp = Get-Date -Format 'HHmmss'

$primeDb = $null
if ($Primed) {
    # One launch that fetches and stores the id (its play is not counted).
    $primeData = "$bench\data\B1prime_$stamp"
    $qtLog = "$bench\logs\B1prime_$stamp.qt.log"
    $r = & "$bench\run.ps1" -Exe $Exe -Log $qtLog -MpvLog 'v' -DataDir $primeData -TimeoutSec 120 `
        -AppArgs @('--bench', 'seq', '--ids', 'LrM_Y39Gmhk', '--listen', '3', '--settle', '0')
    $stored = Select-String -Path $qtLog -Pattern 'a new visitor id from .*; stored' -Quiet
    Write-Output "prime: $r stored=$stored"
    if (-not $stored) { throw 'the priming launch stored no visitor id' }
    $primeDb = "$bench\data\B1prime_$stamp.db"
    New-Item -ItemType Directory -Force $primeDb | Out-Null
    Get-ChildItem $primeData -Filter 'monolist.db*' | ForEach-Object { Copy-Item $_.FullName $primeDb }
    Remove-Item -Recurse -Force $primeData -ErrorAction SilentlyContinue
}

for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($id in $list) {
        $name = "B_${Tag}_cold_${id}_r${round}_$stamp"
        $data = "$bench\data\$name"
        if (Test-Path $data) { Remove-Item -Recurse -Force $data }
        New-Item -ItemType Directory -Force $data | Out-Null
        if ($primeDb) { Get-ChildItem $primeDb | ForEach-Object { Copy-Item $_.FullName $data } }
        $qtLog = "$bench\logs\$name.qt.log"
        $mpvLog = "$bench\logs\$name.mpv.log"
        $r = & "$bench\run.ps1" -Exe $Exe -Log $qtLog -MpvLogFile $mpvLog -MpvLog 'v' -DataDir $data -TimeoutSec 120 `
            -AppArgs @('--bench', 'seq', '--ids', $id, '--listen', '6', '--settle', "$Settle")
        Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
        & "$bench\parse_bench.ps1" -QtLog $qtLog -MpvLog $mpvLog -Out "$bench\logs\$name.parsed.json" | Out-Null
        $starts = @(Select-String -Path $qtLog -Pattern 'bench: \[\d+\] visitorData fetch start').Count
        $bytes = 0
        foreach ($m in (Select-String -Path $qtLog -Pattern 'bench: \[\d+\] visitorData fetch done .*bytes=(\d+)')) { $bytes += [long]$m.Matches[0].Groups[1].Value }
        $retry = @(Select-String -Path $qtLog -Pattern 'refused with the stored visitor id').Count
        Write-Output "$name : $r fetches=$starts fetchBytes=$bytes loginRetries=$retry"
    }
}
if ($primeDb) { Remove-Item -Recurse -Force $primeDb -ErrorAction SilentlyContinue }
