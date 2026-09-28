# B9: two more interleaved rounds of bench_A_play.ps1, the B9 release FIRST this time, then
# the B8 release (b9\rel-base), so an order effect cannot favour either arm.
param([string[]]$Rounds = @('C', 'D'))
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$old = Join-Path $bench 'b9\rel-base\monolist.exe'
$new = 'C:\dev\monolist-build\release\monolist.exe'
$log = Join-Path $bench 'b9\run_A_rev.log'
function Say($text) { $line = (Get-Date -Format 'HH:mm:ss') + ' ' + $text; Add-Content -Encoding utf8 $log $line }
Say 'start'
foreach ($round in $Rounds) {
    Say "b9new$round (B9 release)"
    & "$bench\bench_A_play.ps1" -Exe $new -Tag "b9new$round" -Rounds 1 *>> $log
    Say "b9base$round (B8 release)"
    & "$bench\bench_A_play.ps1" -Exe $old -Tag "b9base$round" -Rounds 1 *>> $log
}
Say 'done'
