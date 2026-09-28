# Dot-source: . .\stats.ps1 ; Get-Stats @(1,2,3)
# median = middle value (mean of the two middle ones for an even count);
# p90 = nearest-rank 90th percentile (ceil(0.9 n)-th smallest).
function Get-Stats([object[]]$Values) {
    $v = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    $n = $v.Count
    if ($n -eq 0) { return [pscustomobject]@{ n = 0; median = $null; p90 = $null; min = $null; max = $null } }
    $median = if ($n % 2) { $v[($n - 1) / 2] } else { ($v[$n / 2 - 1] + $v[$n / 2]) / 2 }
    $p90 = $v[[Math]::Max(0, [Math]::Ceiling(0.9 * $n) - 1)]
    [pscustomobject]@{ n = $n; median = [Math]::Round($median, 1); p90 = [Math]::Round($p90, 1); min = [Math]::Round($v[0], 1); max = [Math]::Round($v[$n - 1], 1) }
}
