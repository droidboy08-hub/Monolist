# B3 (QT4) re-measurement: bench_B.ps1 seq (MONOLIST_MPV_LOG=v, mpv log-file) over the
# 12 benchmark ids, on the instrumented B3 build (bench\build-b3). Two arms on the same
# binary, interleaved and alternating which goes first:
#   b3opus  the new order (Opus 774, 251, AAC 141, 140, the rest), the default
#   b3rate  --set youtube.format bitrate: the old highest-bitrate rule (the switch back)
# Three runs per arm at rotations 0, 6, 3 (Step 2 had two, rot0 and rot6).
# Then IPeJ7iM55hc alone (the probed track whose AAC 140 peaked above its Opus 251),
# twice per arm, tags b3opusIP / b3rateIP.
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = "$bench\build-b3\monolist.exe"
$rate = @('--set', 'youtube.format', 'bitrate')
$plan = @(
    @('b3opus', 0, $false), @('b3rate', 0, $true),
    @('b3rate', 6, $true), @('b3opus', 6, $false),
    @('b3opus', 3, $false), @('b3rate', 3, $true)
)
foreach ($p in $plan) {
    Write-Output ("{0} seq rot{1} start {2}" -f $p[0], $p[1], (Get-Date -Format 'HH:mm:ss'))
    if ($p[2]) { & "$bench\bench_B.ps1" -Suite seq -Exe $exe -Tag $p[0] -Rotate $p[1] -Extra $rate }
    else { & "$bench\bench_B.ps1" -Suite seq -Exe $exe -Tag $p[0] -Rotate $p[1] }
}
foreach ($k in 1..2) {
    foreach ($arm in @(@('b3opusIP', $false), @('b3rateIP', $true))) {
        Write-Output ("{0} seq IPeJ7iM55hc start {1}" -f $arm[0], (Get-Date -Format 'HH:mm:ss'))
        if ($arm[1]) { & "$bench\bench_B.ps1" -Suite seq -Exe $exe -Tag $arm[0] -Ids 'IPeJ7iM55hc' -Extra $rate }
        else { & "$bench\bench_B.ps1" -Suite seq -Exe $exe -Tag $arm[0] -Ids 'IPeJ7iM55hc' }
    }
}
Write-Output ("done {0}" -f (Get-Date -Format 'HH:mm:ss'))
