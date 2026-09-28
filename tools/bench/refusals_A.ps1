# Real googlevideo refusals in bench_A_play runs (logs\A_<tag>_*.log), for any build:
#   launches, refused launches (a "would not play from InnerTube" line), refused InnerTube opens,
#   fresh-link asks and what each led to, what rescued each refused open, the fresh rung's cost
#   (its ask to the muxed stream being asked for), and the cold and replay TTFA of refused launches
#   (parse_play.ps1's ttfaMs: the "selftest: playing" line to mpv's first audio).
# Usage: refusals_A.ps1 -Tags b9newA,b9newB,...   (or -Pattern 'A_fix*')
param([string[]]$Tags = @(), [string]$Pattern = '', [switch]$PerLog)
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$bench\stats.ps1"
$files = @()
if ($Pattern) { $files += Get-ChildItem "$bench\logs" -Filter "$Pattern.log" }
foreach ($t in $Tags) { $files += Get-ChildItem "$bench\logs" -Filter "A_${t}_*.log" }
$files = @($files | Where-Object { $_.Name -match '\.log$' } | Sort-Object FullName -Unique)
$rows = @()
foreach ($f in $files) {
    $lines = foreach ($l in [IO.File]::ReadAllLines($f.FullName)) {
        $m = [regex]::Match($l, '^\s*([\d.]+) (.*)$')
        if ($m.Success) { [pscustomobject]@{ t = [double]$m.Groups[1].Value; text = $m.Groups[2].Value } }
    }
    $refusedOpens = @($lines | Where-Object { $_.text -match 'would not play from InnerTube' })
    $asks = @($lines | Where-Object { $_.text -match 'asking InnerTube once more' })
    $costs = @()
    foreach ($a in $asks) {
        $next = $lines | Where-Object { $_.t -gt $a.t -and $_.text -match 'trying \S+ again as its muxed stream|rescued: its sound came from' } | Select-Object -First 1
        if ($next -and $next.text -match 'muxed stream') { $costs += [Math]::Round(1000 * ($next.t - $a.t)) }
    }
    $rescues = @($lines | Where-Object { $_.text -match 'rescued: its sound came from (.*), (\d+) ms after' } | ForEach-Object {
        $null = $_.text -match 'rescued: its sound came from (.*), (\d+) ms after'
        [pscustomobject]@{ by = ($Matches[1] -replace '[^\x20-\x7e]', '.'); ms = [int]$Matches[2] } })
    $kept = @($lines | Where-Object { $_.text -match 'plays from the link that rescued it' }).Count
    $skipped = @($lines | Where-Object { $_.text -match "InnerTube links were refused lately, so InnerTube is not asked again" }).Count
    $plays = & "$bench\parse_play.ps1" -Log $f.FullName | ConvertFrom-Json
    $cold = @($plays | Where-Object { -not $_.again }) | Select-Object -First 1
    $warm = @($plays | Where-Object { $_.again }) | Select-Object -First 1
    $rows += [pscustomobject][ordered]@{
        log = $f.BaseName; refusedOpens = $refusedOpens.Count; freshAsks = $asks.Count
        freshRescues = @($rescues | Where-Object { $_.by -match '^InnerTube' }).Count
        muxedRescues = @($rescues | Where-Object { $_.by -match 'muxed' }).Count
        otherRescues = @($rescues | Where-Object { $_.by -notmatch '^InnerTube' -and $_.by -notmatch 'muxed' }).Count
        rescueMs = ($rescues | ForEach-Object { $_.ms }) -join '/'
        freshCostMs = $costs -join '/'
        keptLinkPlays = $kept; freshSkipped = $skipped
        coldTtfa = if ($cold) { $cold.ttfaMs } else { $null }; coldTier = if ($cold) { $cold.tier } else { $null }
        replayTtfa = if ($warm) { $warm.ttfaMs } else { $null }; replayTier = if ($warm) { $warm.tier } else { $null }
        costs = $costs; rescueList = $rescues
    }
}
$refused = @($rows | Where-Object { $_.refusedOpens -gt 0 })
if ($PerLog) { $refused | Select-Object log, refusedOpens, freshAsks, freshRescues, muxedRescues, otherRescues, rescueMs, freshCostMs, keptLinkPlays, freshSkipped, coldTtfa, replayTtfa, replayTier | Format-Table -AutoSize | Out-String -Width 300 | Write-Host }
$allCosts = @($refused | ForEach-Object { $_.costs } | ForEach-Object { $_ })
$allRescueMs = @($refused | ForEach-Object { $_.rescueList } | ForEach-Object { $_.ms })
[ordered]@{
    launches = $rows.Count
    refusedLaunches = $refused.Count
    refusedInnerTubeOpens = ($refused | Measure-Object refusedOpens -Sum).Sum
    freshAsks = ($refused | Measure-Object freshAsks -Sum).Sum
    freshRescues = ($refused | Measure-Object freshRescues -Sum).Sum
    muxedRescues = ($refused | Measure-Object muxedRescues -Sum).Sum
    otherRescues = ($refused | Measure-Object otherRescues -Sum).Sum
    keptLinkPlays = ($refused | Measure-Object keptLinkPlays -Sum).Sum
    freshSkipped = ($refused | Measure-Object freshSkipped -Sum).Sum
    freshRungCostMs = Get-Stats $allCosts
    refusalToSoundMs = Get-Stats $allRescueMs
    refusedColdTtfaMs = Get-Stats ($refused | ForEach-Object { $_.coldTtfa })
    refusedReplayTtfaMs = Get-Stats ($refused | ForEach-Object { $_.replayTtfa })
    refusedReplayTiers = ($refused | Group-Object replayTier | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' '
} | ConvertTo-Json -Depth 3
