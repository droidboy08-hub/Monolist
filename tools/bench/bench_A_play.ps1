# Suite A: the UNMODIFIED app (any build: pass -Exe), one fresh process per track:
#   monolist.exe --play <id> <Seconds> --again
# with MONOLIST_MPV_LOG=v. Per track: TTFA-cold (first play right after launch,
# including any visitorData wait), resolve time, and TTFA-warm (the same id again
# at Seconds/2, a link-cache hit; there is no byte cache).
# Repeat after Step 4 with:  .\bench_A_play.ps1 -Exe <new monolist.exe> -Tag after
param(
    [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe',
    [string]$Tag = 'before',
    [int]$Seconds = 30,
    [int]$Rounds = 1,
    [switch]$ReparseOnly,  # re-read logs\A_<Tag>_*.log instead of running
    # B2: extra app arguments after --play (e.g. '--set','youtube.player_client','second').
    [string[]]$Extra = @()
)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$tracks = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks
$all = @()
# B2 (NC-1): the canary first, so a VISIONOS client YouTube stopped answering shows up
# before the numbers are read (canary.ps1; curl only, whatever -Exe is).
if (-not $ReparseOnly) { & "$bench\canary.ps1" | Write-Host }
if ($ReparseOnly) {
    foreach ($log in Get-ChildItem "$bench\logs" -Filter "A_${Tag}_*.log" | Where-Object { $_.Name -match '\.log$' }) {
        $rows = & "$bench\parse_play.ps1" -Log $log.FullName | ConvertFrom-Json
        foreach ($row in $rows) { $row | Add-Member -NotePropertyName run -NotePropertyValue $log.BaseName; $all += $row }
    }
    $Rounds = 0
}
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($t in $tracks) {
        $name = "A_${Tag}_r${r}_$($t.n)_$($t.videoId)"
        $log = "$bench\logs\$name.log"
        $data = "$bench\data\$name"
        if (Test-Path $data) { Remove-Item -Recurse -Force $data }
        & "$bench\run.ps1" -Exe $Exe -Log $log -MpvLog 'v' -DataDir $data -AppArgs (@('--play', $t.videoId, "$Seconds", '--again') + $Extra) | Out-Null
        $rows = & "$bench\parse_play.ps1" -Log $log | ConvertFrom-Json
        foreach ($row in $rows) { $row | Add-Member -NotePropertyName run -NotePropertyValue $name; $all += $row }
        Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
    }
}
$all | ConvertTo-Json -Depth 3 | Set-Content -Encoding utf8 "$bench\results_A_$Tag.json"
$cold = $all | Where-Object { -not $_.again }
$warm = $all | Where-Object { $_.again }
[ordered]@{
    ttfaColdLaunch = Get-Stats ($cold | ForEach-Object { $_.ttfaMs })
    resolveColdLaunch = Get-Stats ($cold | ForEach-Object { $_.resolveMs })
    ttfaWarm = Get-Stats ($warm | ForEach-Object { $_.ttfaMs })
    resolveWarm = Get-Stats ($warm | ForEach-Object { $_.resolveMs })
    openToEofCold = Get-Stats ($cold | ForEach-Object { $_.openToEofMs })
    tiers = ($all | Group-Object tier | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' '
    # Real googlevideo refusals, and what rescued them (refusals_A.ps1): so a rung that
    # rescues nothing, or a replay that pays for it again, shows up in every summary.
    refusals = (& "$bench\refusals_A.ps1" -Tags $Tag | ConvertFrom-Json)
} | ConvertTo-Json -Depth 4
