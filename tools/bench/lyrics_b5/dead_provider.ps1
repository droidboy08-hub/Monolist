# Serial-fallback penalty: the app's real lookup with LRCLIB made unreachable
# through the lrclib_url setting (no code change), so the time to YouTube
# Music's answer shows what a dead or slow first provider costs.
#   blackhole: http://10.255.255.1  (SYN never answered: Qt's 10 s transfer timeout)
#   refused:   http://127.0.0.1:9   (connection refused at once)
# Fresh data dir per run. Usage: dead_provider.ps1 [-Tracks 9,18,30] [-Exe <path>] [-Tag <suffix>]
# B5: -Tag keeps a second binary's logs apart (dead_<mode><tag>), and each
# line also shows the race's interim answer ("interim <state> shown after N
# ms"): the first lines on screen, before the answer is final.
param([int[]]$Tracks = @(9, 18, 30), [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe', [string]$Tag = '')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $here
$list = (Get-Content -Raw -Encoding UTF8 (Join-Path $bench 'lyrics_tracks.json') | ConvertFrom-Json).tracks
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $here 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time yyyy-MM-ddTHH:mm:ss.zzz} %{message}'
Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue
$modes = [ordered]@{ blackhole = 'http://10.255.255.1'; refused = 'http://127.0.0.1:9' }
foreach ($mode in $modes.Keys) {
    $logDir = Join-Path $here "logs\dead_$mode$Tag"
    New-Item -ItemType Directory -Force $logDir | Out-Null
    foreach ($t in $list) {
        if ($Tracks -notcontains [int]$t.n) { continue }
        $data = Join-Path $here ("data\dead_{0}{1}\{2:D2}" -f $mode, $Tag, [int]$t.n)
        if (Test-Path $data) { Remove-Item -Recurse -Force $data }
        New-Item -ItemType Directory -Force $data | Out-Null
        $env:MONOLIST_DATA_DIR = $data
        $log = Join-Path $logDir ("{0:D2}.err" -f [int]$t.n)
        $p = Start-Process -FilePath $Exe -ArgumentList @('--set', 'lrclib_url', $modes[$mode], '--lyrics', ('"' + $t.query + '"')) `
            -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
        if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {} }
        $lines = @(Get-Content $log -Encoding UTF8)
        $res = $null; $int = $null
        foreach ($l in $lines) {
            if ($l -match 'selftest:\s{3}interim ' -and -not $int) { $int = $l }
            if ($l -match 'selftest:\s{3}\S+ in \d+ ms') { $res = $l; break }   # the first lookup's answer
        }
        Write-Output ("{0,-9} #{1,2} {2}{3}" -f $mode, $t.n, ($res -replace '^\S+ selftest:\s+', ''),
            $(if ($int) { ' || ' + ($int -replace '^\S+ selftest:\s+', '') } else { '' }))
    }
}
