# Suite B: the INSTRUMENTED copy (bench\build\monolist.exe, built by build-bench.ps1
# from bench\src-copy; patch = bench\instrumentation.diff). mpv writes its own
# timestamped log-file, and every bench mark is in both logs (parse_bench.ps1).
#  -Suite cold     one fresh process per track: --bench seq --ids <id> (TTFA-cold right after
#                  launch, with the visitorData wait and the /player POST split out)
#  -Suite seq      one process: every track in turn with playSource (new, uncached link in a
#                  warm session: TTFA-cold "b"), then all again (link-cache hit: TTFA-warm).
#                  -Rotate k starts the list at track k+1.
#  -Suite skip     one process: playTracks(all) and Next after -Listen s (prefetched link)
#  -Suite play     one process: playTracks(-Ids or all) for -Total s, natural track ends
#  -Suite resolve  one process: every track through InnerTube, yt-dlp and muxed alone
param(
    [ValidateSet('cold', 'seq', 'skip', 'play', 'resolve')][string]$Suite = 'seq',
    [string]$Exe = '',
    [string]$Tag = 'before',
    [int]$Listen = 8,
    [int]$Total = 660,
    [int]$Rotate = 0,
    [int]$Settle = 500,
    [string]$Ids = '',
    # B3: extra app arguments for every run (e.g. '--set','youtube.format','bitrate'
    # for the switch-back arm on the same binary). Empty by default: runs as before.
    [string[]]$Extra = @()
)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Exe) { $Exe = "$bench\build\monolist.exe" }
$tracks = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks
$list = @($tracks | ForEach-Object { $_.videoId })
if ($Rotate -gt 0) { $list = @($list[$Rotate..($list.Count - 1)]) + @($list[0..($Rotate - 1)]) }
if ($Ids) { $list = $Ids.Split(',') }

function RunOne([string]$name, [string[]]$appArgs, [int]$timeout = 900) {
    $data = "$bench\data\$name"
    if (Test-Path $data) { Remove-Item -Recurse -Force $data }
    $qtLog = "$bench\logs\$name.qt.log"
    $mpvLog = "$bench\logs\$name.mpv.log"
    $r = & "$bench\run.ps1" -Exe $Exe -Log $qtLog -MpvLogFile $mpvLog -MpvLog 'v' -DataDir $data -TimeoutSec $timeout -AppArgs (@($appArgs) + @($Extra))
    Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
    & "$bench\parse_bench.ps1" -QtLog $qtLog -MpvLog $mpvLog -Out "$bench\logs\$name.parsed.json" | Out-Null
    Write-Output "$name : $r"
}

$stamp = Get-Date -Format 'HHmmss'
# B2 (NC-1): the canary first, so a VISIONOS client YouTube stopped answering shows up
# before the numbers are read (canary.ps1; curl only, whatever -Exe is).
& "$bench\canary.ps1" | Write-Host
switch ($Suite) {
    'cold' {
        foreach ($id in $list) { RunOne "B_${Tag}_cold_${id}_$stamp" @('--bench', 'seq', '--ids', $id, '--listen', '6', '--settle', "$Settle") 120 }
    }
    'seq' {
        RunOne "B_${Tag}_seq_rot${Rotate}_$stamp" @('--bench', 'seq', '--ids', ($list -join ','), '--listen', "$Listen", '--settle', "$Settle", '--again') 1200
    }
    'skip' {
        RunOne "B_${Tag}_skip_rot${Rotate}_$stamp" @('--bench', 'skip', '--ids', ($list -join ','), '--listen', "$Listen", '--settle', "$Settle") 1200
    }
    'play' {
        RunOne "B_${Tag}_play_$stamp" @('--bench', 'play', '--ids', ($list -join ','), '--total', "$Total", '--settle', "$Settle", '--sample-ms', '500') ($Total + 300)
    }
    'resolve' {
        RunOne "B_${Tag}_resolve_$stamp" @('--bench', 'resolve', '--ids', ($list -join ','), '--tiers', '0,1,2', '--settle', '4000') 1200
    }
}
