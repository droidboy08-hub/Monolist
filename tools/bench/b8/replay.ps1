# B8 (QT3): a replay after a rescue. --play <id> 40 --again with the song's InnerTube link
# refused until the muxed stream (itag 18) rescues it, then the song played again at 20 s:
#   before  build-b7: --spoil      (one refusal is enough: muxed comes next)
#   after   build-b8: --spoil 2    (the fresh InnerTube link refused as well)
# What the replay played: the itag InnerTube resolved, or the remembered link (and its tier),
# and the codec mpv reports. Logs in bench\b8\logs\replay_<arm>_<id>.log (never printed).
param([string[]]$Ids = @('7nVctvQVz0U', 'BSTsnWoslP4', 'CmThpha4Hoo', '4D7u5KF7SP8'), [switch]$Parse)
$b8 = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $b8
$logs = Join-Path $b8 'logs'
$arms = [ordered]@{ before = @("$bench\build-b7\monolist.exe", @('--spoil')); after = @("$bench\build-b8\monolist.exe", @('--spoil', '2')) }

function ParseOne([string]$log, [string]$id) {
    $rows = foreach ($line in [IO.File]::ReadAllLines($log, [Text.Encoding]::UTF8)) {
        $m = [regex]::Match($line, '^\s*([\d.]+) (.*)$')
        if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; x = $m.Groups[2].Value } }
    }
    $again = $rows | Where-Object { $_.x -eq "selftest: playing $id again" } | Select-Object -First 1
    $first = @($rows | Where-Object { -not $again -or $_.t -lt $again.t })
    $second = @($rows | Where-Object { $again -and $_.t -ge $again.t })
    $rescue = $first | Where-Object { $_.x -match "^muxed: $id resolved as itag (\S+)" } | Select-Object -First 1
    $firstStream = $first | Where-Object { $_.x -match "^stream: $id from" } | Select-Object -Last 1
    $hit = $second | Where-Object { $_.x -match "resolve $id cache-hit tier=(\S+)" } | Select-Object -First 1
    $fresh = $second | Where-Object { $_.x -match "^innertube: $id resolved as itag (\d+)" } | Select-Object -First 1
    $secondStream = $second | Where-Object { $_.x -match "^stream: $id from" } | Select-Object -First 1
    [pscustomobject]@{
        id = $id
        rescued = if ($rescue) { 'itag ' + [regex]::Match($rescue.x, 'itag (\S+)').Groups[1].Value } else { 'no muxed rescue' }
        firstPlays = if ($firstStream) { (@([regex]::Match($firstStream.x, 'from (.*?): (\w+) (\d+) Hz').Groups | Select-Object -Skip 1 | ForEach-Object { $_.Value }) -join ' ') } else { '' }
        replay = if ($hit) { 'remembered link, tier ' + [regex]::Match($hit.x, 'tier=(\S+)').Groups[1].Value } elseif ($fresh) { 'InnerTube itag ' + [regex]::Match($fresh.x, 'itag (\d+)').Groups[1].Value } else { 'not seen' }
        replayPlays = if ($secondStream) { (@([regex]::Match($secondStream.x, 'from (.*?): (\w+) (\d+) Hz').Groups | Select-Object -Skip 1 | ForEach-Object { $_.Value }) -join ' ') } else { '' }
    }
}

if (-not $Parse) {
    foreach ($id in $Ids) {
        foreach ($arm in $arms.Keys) {
            $log = Join-Path $logs "replay_${arm}_$id.log"
            $o = & "$bench\run.ps1" -Exe $arms[$arm][0] -Log $log -AppArgs (@('--play', $id, '40', '--again') + $arms[$arm][1]) -MpvLog 'warn' -DataDir "$b8\data_$arm" -TimeoutSec 150
            Write-Output ("{0} {1}: {2}" -f $arm, $id, ($o -join ' '))
        }
    }
}
$all = foreach ($arm in $arms.Keys) { foreach ($id in $Ids) { $log = Join-Path $logs "replay_${arm}_$id.log"; if (Test-Path $log) { $r = ParseOne $log $id; $r | Add-Member arm $arm; $r } } }
$all | Format-Table arm, id, rescued, firstPlays, replay, replayPlays -AutoSize | Out-String -Width 220
$all | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $b8 'results_replay.json')
