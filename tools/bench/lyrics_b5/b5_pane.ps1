# B5's new measure: time from opening the lyrics to lines on show, as a
# listener meets it (monolist --lyrics-pane). For each track of the 30-track
# list, one process with a fresh data dir: the search's first two songs are
# queued and played, the lyrics opened 3 s after the first one's sound starts,
# closed, Next, and opened again 1 s after the second one's sound starts.
#   on:  the new build as shipped (background lookups on)
#   off: the same build with lyrics.background=0, the switch back: opening the
#        lyrics starts the lookup, as before LY-4 (with the race of LY-5)
# Arms are interleaved pass by pass. Logs: logs\pane_<arm>NN\NN.err
param([int]$Passes = 5, [int]$Start = 1, [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe', [int[]]$Only = @())
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bench = Split-Path -Parent $here
$list = (Get-Content -Raw -Encoding UTF8 (Join-Path $bench 'lyrics_tracks.json') | ConvertFrom-Json).tracks
New-Item -ItemType Directory -Force (Join-Path $here 'dl') | Out-Null
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $here 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time yyyy-MM-ddTHH:mm:ss.zzz} %{message}'
Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue
Write-Output ("pane start " + (Get-Date -Format o))
for ($i = $Start; $i -lt $Start + $Passes; $i++) {
    foreach ($arm in @('on', 'off')) {
        $pass = 'pane_{0}{1:D2}' -f $arm, $i
        $logDir = Join-Path $here "logs\$pass"
        New-Item -ItemType Directory -Force $logDir | Out-Null
        foreach ($t in $list) {
            if ($Only.Count -gt 0 -and $Only -notcontains [int]$t.n) { continue }
            $data = Join-Path $here ("data\{0}\{1:D2}" -f $pass, [int]$t.n)
            if (Test-Path $data) { Remove-Item -Recurse -Force $data }
            New-Item -ItemType Directory -Force $data | Out-Null
            $env:MONOLIST_DATA_DIR = $data
            $log = Join-Path $logDir ("{0:D2}.err" -f [int]$t.n)
            $appArgs = @()
            if ($arm -eq 'off') { $appArgs += @('--set', 'lyrics.background', '0') }
            $appArgs += @('--lyrics-pane', ('"' + $t.query + '"'))
            $p = Start-Process -FilePath $Exe -ArgumentList $appArgs -RedirectStandardError $log `
                -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
            if (-not $p.WaitForExit(120000)) { try { $p.Kill() } catch {} }
            $res = @(Get-Content $log -Encoding UTF8 | Where-Object { $_ -match 'selftest: pane-open ' } |
                     ForEach-Object { ($_ -replace '^\S+ selftest: pane-open ', '') -replace ' title=.*$', '' })
            Write-Output ("{0} #{1,2} {2}" -f $pass, $t.n, ($res -join ' | '))
        }
    }
}
Write-Output ("pane done " + (Get-Date -Format o))
