# B7 (PP-04 + QT5) re-measurement: instrumented bench_B.ps1 seq (--again) and skip over the
# 12 benchmark ids, on two instrumented builds of the same tree (bench\build-b7):
#   b7before  monolist_before.exe  HEAD fd0dc9e + instrumentation (bookkeeping before loadfile)
#   b7after   monolist.exe         the B7 change + the same instrumentation
# Two history sizes: an empty scratch database (Step 2's conditions, tags ...e) and one seeded
# with 480 songs in Recently played and 480 play events (--bench-seed 480, the size of the
# owner's Mac library, tags ...s). Arms interleaved, alternating which goes first.
param([switch]$SkipOnly, [switch]$SeqOnly, [int[]]$Rotations = @(0, 6))
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exeBefore = "$bench\build-b7\monolist_before.exe"
$exeAfter = "$bench\build-b7\monolist.exe"
$seed = @('--bench-seed', '480')
$flip = $false
foreach ($rot in $Rotations) {
    foreach ($suite in @('seq', 'skip')) {
        if ($SkipOnly -and $suite -ne 'skip') { continue }
        if ($SeqOnly -and $suite -ne 'seq') { continue }
        foreach ($seeded in @($false, $true)) {
            $arms = @(@('b7before', $exeBefore), @('b7after', $exeAfter))
            if ($flip) { [array]::Reverse($arms) }
            $flip = -not $flip
            foreach ($arm in $arms) {
                $tag = $arm[0] + $(if ($seeded) { 's' } else { 'e' })
                Write-Output ("{0} {1} rot{2} start {3}" -f $tag, $suite, $rot, (Get-Date -Format 'HH:mm:ss'))
                if ($seeded) { & "$bench\bench_B.ps1" -Suite $suite -Exe $arm[1] -Tag $tag -Rotate $rot -Extra $seed | Select-Object -Last 1 }
                else { & "$bench\bench_B.ps1" -Suite $suite -Exe $arm[1] -Tag $tag -Rotate $rot | Select-Object -Last 1 }
            }
        }
    }
}
Write-Output ("done {0}" -f (Get-Date -Format 'HH:mm:ss'))
