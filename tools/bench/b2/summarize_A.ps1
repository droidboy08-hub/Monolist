# B2: pools the interleaved bench_A_play results per arm (results_A_<tag>.json) and prints
# stats.ps1 medians / p90 for TTFA-cold (first play after launch), TTFA-warm (--again,
# link-cache hit) and the resolve times, plus the tiers.
param([string[]]$Arms = @('b2base', 'b2new', 'b2second'))
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. "$bench\stats.ps1"
$out = [ordered]@{}
foreach ($arm in $Arms) {
    $rows = @()
    foreach ($f in Get-ChildItem $bench -Filter "results_A_$arm*.json") {
        # PowerShell 5.1 hands a JSON array over as one object; unroll it.
        $parsed = Get-Content $f.FullName -Raw | ConvertFrom-Json
        foreach ($row in $parsed) { $rows += $row }
    }
    $cold = @($rows | Where-Object { -not $_.again })
    $warm = @($rows | Where-Object { $_.again })
    $out[$arm] = [ordered]@{
        files = @(Get-ChildItem $bench -Filter "results_A_$arm*.json" | ForEach-Object { $_.Name }) -join ' '
        ttfaColdLaunch = Get-Stats ($cold | ForEach-Object { $_.ttfaMs })
        resolveColdLaunch = Get-Stats ($cold | ForEach-Object { $_.resolveMs })
        ttfaWarm = Get-Stats ($warm | ForEach-Object { $_.ttfaMs })
        resolveWarm = Get-Stats ($warm | ForEach-Object { $_.resolveMs })
        tiers = ($rows | Group-Object tier | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' '
        missingTtfa = @($rows | Where-Object { $null -eq $_.ttfaMs }).Count
    }
}
$out | ConvertTo-Json -Depth 4
