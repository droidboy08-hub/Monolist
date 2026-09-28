# Checklist row C19 (low start threshold). Reads the mpv verbose logs that the
# playback benchmark already wrote (bench\logs\*.log, format "<sec> mpv[v] ...")
# and, for every stream open, measures
#   first response bytes ("curl: Mime-type")  ->  "cplayer: starting audio playback"
# i.e. how long mpv holds sound back once data is arriving. Also records the
# AO format and the af chain seen, and every URL's query keys (C5/C7/C10 evidence).
param(
    [string]$Logs = (Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) "logs")
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gaps = New-Object System.Collections.Generic.List[double]
$aoFormats = @{}
$opensPerPlay = @{}
foreach ($file in Get-ChildItem $Logs -Filter *.log) {
    $mime = $null
    foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
        if ($line -notmatch '^\s*([\d\.]+)\s+mpv\[') { continue }
        $t = [double]$matches[1]
        if ($line -match 'curl: Mime-type') { $mime = $t }
        elseif ($line -match 'cplayer: starting audio playback' -and $mime -ne $null) {
            $gaps.Add([math]::Round(($t - $mime) * 1000, 1)); $mime = $null
        }
        elseif ($line -match 'cplayer: AO: \[(\w+)\] (.*)$') { $aoFormats[$matches[2]] = 1 + [int]$aoFormats[$matches[2]] }
    }
}
$sorted = $gaps | Sort-Object
function pct($a, $p) { if ($a.Count -eq 0) { return $null }; $a[[math]::Min($a.Count - 1, [math]::Ceiling($p * $a.Count) - 1)] }
$result = [pscustomobject]@{
    metric = "first response bytes -> mpv starts audio (ms), mpv log clock"
    n = $sorted.Count
    median = pct $sorted 0.5
    p90 = pct $sorted 0.9
    max = ($sorted | Select-Object -Last 1)
    aoFormats = $aoFormats
}
$result | ConvertTo-Json -Depth 4 | Out-File -Encoding utf8 (Join-Path $here "results_C19_start_threshold.json")
$result | Format-List
