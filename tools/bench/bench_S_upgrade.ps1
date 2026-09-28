# Step JS (QT7): the mid-song move measured on the 10 benchmark songs JioSaavn
# has (bench\playback_tracks.json, as matched in saavn\live12-final-on.log),
# each played with its JioSaavn answer held back -LateMs so YouTube starts it,
# a fresh data folder per run. Collects the engine's handover line and writes
# one JSON line per run to results_S_upgrade_<Tag>.jsonl.
param([int]$LateMs = 3000, [int]$Seconds = 16, [int]$Rounds = 1, [string]$Tag = 'js')
$bench = Split-Path -Parent $MyInvocation.MyCommand.Path
$list = (Get-Content "$bench\playback_tracks.json" -Raw | ConvertFrom-Json).tracks | Where-Object { $_.n -notin 4, 9 }
$out = "$bench\results_S_upgrade_$Tag.jsonl"
for ($r = 1; $r -le $Rounds; $r++) {
    foreach ($t in $list) {
        $p = $t.duration.Split(':'); $len = [int]$p[0] * 60 + [int]$p[1]
        $lines = & "$bench\js_upgrade.ps1" -VideoId $t.videoId -Title $t.title -Artist $t.artist -Length $len `
                    -Seconds $Seconds -LateMs $LateMs -Tag "b$($t.n)"
        $moved = $lines | Where-Object { $_ -match '^upgrade: .* moved from' } | Select-Object -First 1
        $stayed = $lines | Where-Object { $_ -match '^upgrade: .* (stays on YouTube|not moved)' } | Select-Object -First 1
        $o = [ordered]@{ round = $r; n = $t.n; videoId = $t.videoId; moved = [bool]$moved }
        if ($moved -match 'averages (\d+) kbps') { $o.newKbps = [int]$Matches[1] }
        if ($moved -match 'music (-?[\d.]+) ms later in the new file') { $o.contentOffsetMs = [double]$Matches[1] }
        if ($moved -match 'opened in (\d+) ms and buffered in (\d+) ms, aimed (\d+) time') {
            $o.openedMs = [int]$Matches[1]; $o.bufferedMs = [int]$Matches[2]; $o.aims = [int]$Matches[3]
        }
        if ($moved -match 'its sound started (-?\d+) ms after it was unpaused, (-?\d+) ms from the song.s, and was lined up to (-?\d+) ms in (\d+) ms with (\d+) speed') {
            $o.startMs = [int]$Matches[1]; $o.firstOffsetMs = [int]$Matches[2]; $o.alignedOffsetMs = [int]$Matches[3]
            $o.alignMs = [int]$Matches[4]; $o.nudges = [int]$Matches[5]
        }
        if ($moved -match 'new clock read ([\d.]+) s and the old ([\d.]+) s \((-?\d+) ms apart\)') {
            $o.handoverAt = [double]$Matches[1]; $o.handoverApartMs = [int]$Matches[3]
        }
        if ($moved -match 'mid-song at (\d+:\d+)') { $o.at = $Matches[1] }
        if (-not $moved) { $o.why = "$stayed" }
        ($o | ConvertTo-Json -Compress) | Add-Content -Encoding utf8 $out
        Write-Output ("#{0,-2} moved={1} kbps={11} content={12} open={2} buffered={3} start={4} first={5} aligned={6} in {7} ms nudges={8} handover={9} ms {10}" -f `
            $t.n, $o.moved, $o.openedMs, $o.bufferedMs, $o.startMs, $o.firstOffsetMs, $o.alignedOffsetMs, $o.alignMs, $o.nudges, $o.handoverApartMs, $o.why, $o.newKbps, $o.contentOffsetMs)
    }
}
