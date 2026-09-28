# B9: splits each cold play's resolve (bench_A_play logs) into the visitor-id wait and the
# /player request: playing -> (visitor id ready) -> "chose itag". Per arm: medians / p90.
param([string[]]$Arms = @('b9base', 'b9new'))
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. "$bench\stats.ps1"
foreach ($arm in $Arms) {
    $wait = @(); $post = @(); $total = @(); $hedges = 0
    foreach ($log in Get-ChildItem "$bench\logs" -Filter "A_${arm}?_r1_*.log" | Where-Object { $_.Name -match '\.log$' }) {
        $lines = foreach ($l in [IO.File]::ReadAllLines($log.FullName)) {
            $m = [regex]::Match($l, '^\s*([\d.]+) (.*)$')
            if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; text = $m.Groups[2].Value } }
        }
        $play = $lines | Where-Object { $_.text -match '^selftest: playing \S+$' } | Select-Object -First 1
        if (-not $play) { continue }
        $id = ([regex]::Match($play.text, 'playing (\S+)')).Groups[1].Value
        $visitor = $lines | Where-Object { $_.text -match 'innertube: a new visitor id' } | Select-Object -First 1
        $chose = $lines | Where-Object { $_.t -ge $play.t -and $_.text -match "innertube: $([regex]::Escape($id)) chose itag" } | Select-Object -First 1
        if ($lines | Where-Object { $_.text -match 'asking once more on a connection of its own' }) { $hedges++ }
        if (-not $chose) { continue }
        $ready = if ($visitor -and $visitor.t -gt $play.t) { $visitor.t } else { $play.t }
        $wait += [Math]::Round(1000 * ($ready - $play.t))
        $post += [Math]::Round(1000 * ($chose.t - $ready))
        $total += [Math]::Round(1000 * ($chose.t - $play.t))
    }
    $w = Get-Stats $wait; $p = Get-Stats $post; $a = Get-Stats $total
    "{0}: n={1}  visitor wait {2}/{3}  /player {4}/{5}  play->chose {6}/{7}  hedges {8}" -f $arm, $a.n, $w.median, $w.p90, $p.median, $p.p90, $a.median, $a.p90, $hedges
}
