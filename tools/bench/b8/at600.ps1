# B8: the B0 test repeated on the B8 instrumented Release build (bench\build-b8) on the 13:32 track, seeking to 600 s once
# audio starts, over N fresh launches (the link cache is in memory, so each launch is a
# fresh /player resolve). mpv log at trace, so every libcurl request and response status
# is in the log. Logs stay in bench\b0\logs (they contain googlevideo URLs, i.e. the
# public IP: never print them). Summary: bench\b0\results_at600.json
param([string]$Data = '', [string]$Exe = '', [int]$Runs = 8, [int]$Seconds = 45, [string]$VideoId = 'Tu7oq3VNgpY', [int]$At = 600)
$b0 = Split-Path -Parent $MyInvocation.MyCommand.Path; if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $b0) 'build-b8\monolist.exe' }
$bench = Split-Path -Parent $b0
$logs = Join-Path $b0 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
$data = if ($Data) { $Data } else { Join-Path $b0 'data_at600' }
for ($r = 1; $r -le $Runs; $r++) {
    $log = Join-Path $logs ("at600_r{0}.log" -f $r)
    $o = & "$bench\run.ps1" -Exe $Exe -Log $log -AppArgs @('--play', $VideoId, "$Seconds", '--at', "$At") -MpvLog 'trace' -DataDir $data -TimeoutSec 120
    Write-Output ("run {0}: {1}" -f $r, ($o -join ' '))
    Start-Sleep -Seconds 2
}
