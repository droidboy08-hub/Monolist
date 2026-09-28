# Summarises the audio chain mpv actually built, from MONOLIST_MPV_LOG=v logs
# written by the Suite A / gap scripts (bench\logs\*.log). Evidence for
# checklist rows C25 (requantisation / dither), C26 (shared vs exclusive
# output), C27 (which decoder), C28 (replay-gain applied or not). Read-only.
# After Step 4:  .\audio_chain.ps1 -Pattern 'A_after*.log'
param(
    [string]$Logs = (Join-Path (Split-Path -Parent $PSScriptRoot) 'logs'),
    [string]$Pattern = 'A_before2_*.log'
)
$files = Get-ChildItem $Logs -Filter $Pattern
$decoders = @{}; $aos = @{}; $conv = @{}; $modes = @{}
$replay = 0; $dither = 0
foreach ($f in $files) {
    foreach ($line in Get-Content $f.FullName) {
        if ($line -match 'ad: Selected decoder: (.+)$') { $decoders[$Matches[1]]++ }
        elseif ($line -match 'cplayer: AO: \[(\w+)\] (.+)$') { $aos["$($Matches[1]) $($Matches[2])"]++ }
        elseif ($line -match 'swresample: (\d+Hz .+ -> .+)$') { $conv[$Matches[1]]++ }
        elseif ($line -match 'ao/wasapi: Accepted as .+\((shared|exclusive)\)') { $modes[$Matches[1]]++ }
        elseif ($line -match '(?i)replay-?gain') { $replay++ }
        elseif ($line -match '(?i)dither') { $dither++ }
    }
}
[ordered]@{
    logs = $files.Count
    pattern = $Pattern
    decoders = $decoders
    audioOutputs = $aos
    wasapiModes = $modes
    resamplerConversions = $conv
    replayGainLines = $replay
    ditherLines = $dither
} | ConvertTo-Json -Depth 3
