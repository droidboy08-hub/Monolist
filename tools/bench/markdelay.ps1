# How long each bench mark's synchronous print-text waited for mpv's core (the
# instrumentation's own cost), per mark kind, over every instrumented run.
param([string]$Filter = 'B_*.qt.log')
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$delays = @{}
foreach ($f in Get-ChildItem "$bench\logs" -Filter $Filter) {
    $mpvFile = $f.FullName -replace '\.qt\.log$', '.mpv.log'
    if (-not (Test-Path $mpvFile)) { continue }
    $qt = @{}
    foreach ($l in [IO.File]::ReadAllLines($f.FullName)) {
        $m = [regex]::Match($l, '^\s*([\d.]+) bench: \[\d+\] (.*)$')
        if ($m.Success) { $k = $m.Groups[2].Value; if (-not $qt.ContainsKey($k)) { $qt[$k] = New-Object System.Collections.Generic.Queue[double] }; $qt[$k].Enqueue([double]$m.Groups[1].Value) }
    }
    $pairs = @()
    foreach ($l in [IO.File]::ReadAllLines($mpvFile)) {
        $m = [regex]::Match($l, '^\[\s*([\d.]+)\]\[i\]\[cplayer\] BENCH (.*)$')
        if (-not $m.Success) { continue }
        $k = $m.Groups[2].Value
        if ($qt.ContainsKey($k) -and $qt[$k].Count -gt 0) { $pairs += , @($k, ([double]$m.Groups[1].Value - $qt[$k].Dequeue())) }
    }
    if ($pairs.Count -eq 0) { continue }
    $offset = ($pairs | ForEach-Object { $_[1] } | Measure-Object -Minimum).Minimum
    foreach ($p in $pairs) {
        $kind = ($p[0] -split ' ')[0..1] -join ' '
        if (-not $delays.ContainsKey($kind)) { $delays[$kind] = @() }
        $delays[$kind] += 1000 * ($p[1] - $offset)
    }
}
$delays.Keys | Sort-Object | ForEach-Object { $s = Get-Stats $delays[$_]; [pscustomobject]@{ mark = $_; n = $s.n; medianMs = $s.median; p90Ms = $s.p90; maxMs = $s.max } } | Format-Table -AutoSize | Out-String -Width 200
