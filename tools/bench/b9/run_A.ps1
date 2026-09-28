# B9 re-measurement driver: bench_A_play.ps1 on the B8 release (b9\rel-base, an unmodified
# build of 477f89d, the same-day control) and the B9 release interleaved round by round,
# n=12 per invocation, 24 per arm. One benchmark at a time; nothing is built while it runs.
param([string[]]$Rounds = @('A', 'B'))
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$old = Join-Path $bench 'b9\rel-base\monolist.exe'
$new = 'C:\dev\monolist-build\release\monolist.exe'
$log = Join-Path $bench 'b9\run_A.log'
function Say($text) { $line = (Get-Date -Format 'HH:mm:ss') + ' ' + $text; Add-Content -Encoding utf8 $log $line }
Say 'start'
foreach ($round in $Rounds) {
    Say "b9base$round (B8 release)"
    & "$bench\bench_A_play.ps1" -Exe $old -Tag "b9base$round" -Rounds 1 *>> $log
    Say "b9new$round (B9 release)"
    & "$bench\bench_A_play.ps1" -Exe $new -Tag "b9new$round" -Rounds 1 *>> $log
}
Say 'done'
