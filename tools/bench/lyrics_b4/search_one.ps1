# One --search run; prints the top song results. Usage: search_one.ps1 -Query "<q>"
param([Parameter(Mandatory = $true)][string]$Query,
      [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe')
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$data = Join-Path $here 'data\search'
New-Item -ItemType Directory -Force $data, (Join-Path $here 'logs\search'), (Join-Path $here 'dl') | Out-Null
$env:MONOLIST_DATA_DIR = $data
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $here 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time process} %{message}'
$log = Join-Path $here 'logs\search\one.err'
$p = Start-Process -FilePath $Exe -ArgumentList '--search', ('"' + $Query + '"') `
    -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch {} }
Get-Content $log -Encoding UTF8 | Where-Object { $_ -match 'selftest:\s{3}[A-Za-z0-9_-]{11} \|' }
