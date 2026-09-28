# Runs the app's own --search (songs filter, as the Search page) for every
# query in candidates.json and saves the top results, so the fixed lyrics test
# list is built from the same search the --lyrics self-test uses.
# Usage: powershell -ExecutionPolicy Bypass -File search_all.ps1 [-Exe <path>]
param(
    [string]$Exe = 'C:\dev\monolist-build\release\monolist.exe'
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$data = Join-Path $here 'data\search'
New-Item -ItemType Directory -Force $data, (Join-Path $here 'logs\search'), (Join-Path $here 'dl') | Out-Null
$env:MONOLIST_DATA_DIR = $data
$env:MONOLIST_DOWNLOAD_DIR = Join-Path $here 'dl'
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:QT_MESSAGE_PATTERN = '%{time process} %{message}'
Remove-Item Env:MONOLIST_MPV_LOG -ErrorAction SilentlyContinue

$candidates = Get-Content -Raw -Encoding UTF8 (Join-Path $here 'candidates.json') | ConvertFrom-Json
$out = @()
foreach ($c in $candidates) {
    $log = Join-Path $here ("logs\search\{0:D2}.err" -f $c.n)
    $p = Start-Process -FilePath $Exe -ArgumentList '--search', ('"' + $c.query + '"') `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Minimized
    if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch {} }
    $results = @()
    foreach ($line in (Get-Content $log -Encoding UTF8)) {
        if ($line -match 'selftest:\s{3}([A-Za-z0-9_-]{11}) \| (.*?) \| (.*?) \| (.*?) \| (\d+:\d{2}(?::\d{2})?) \| art:') {
            $results += [pscustomobject]@{ videoId = $Matches[1]; title = $Matches[2]; artist = $Matches[3];
                                           album = $Matches[4]; duration = $Matches[5] }
        }
    }
    Write-Output ("{0,2} {1} -> {2}" -f $c.n, $c.query, (($results | Select-Object -First 1 | ForEach-Object { "$($_.videoId) | $($_.title) | $($_.artist) | $($_.duration)" })))
    $out += [pscustomobject]@{ n = $c.n; query = $c.query; results = $results }
}
$json = $out | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $here 'search_results.json'), $json, (New-Object Text.UTF8Encoding($false)))
