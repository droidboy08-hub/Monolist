# B2 re-measurement driver: bench_A_play.ps1, the B1 release (rel-b1, same-day control)
# and the B2 release interleaved round by round, n=12 per invocation, 24 per arm; then
# the B2 release with the second client forced (--set youtube.player_client second), n=12.
# One benchmark at a time; nothing else is built while this runs.
param([string[]]$Rounds = @('A', 'B'), [switch]$NoSecond)
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$old = Join-Path $bench 'rel-b1\monolist.exe'
$new = 'C:\dev\monolist-build\release\monolist.exe'
$log = Join-Path $bench 'b2\run_A.log'
function Say($text) { $line = (Get-Date -Format 'HH:mm:ss') + ' ' + $text; Add-Content -Encoding utf8 $log $line }
Say 'start'
foreach ($round in $Rounds) {
    Say "b2base$round (B1 release)"
    & "$bench\bench_A_play.ps1" -Exe $old -Tag "b2base$round" -Rounds 1 *>> $log
    Say "b2new$round (B2 release)"
    & "$bench\bench_A_play.ps1" -Exe $new -Tag "b2new$round" -Rounds 1 *>> $log
}
if (-not $NoSecond) {
Say 'b2second (B2 release, youtube.player_client=second)'
& "$bench\bench_A_play.ps1" -Exe $new -Tag 'b2second' -Rounds 1 -Extra @('--set', 'youtube.player_client', 'second') *>> $log
}
Say 'done'
