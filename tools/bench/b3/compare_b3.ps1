# B3: TTFA-cold (b) (new track in a warm session) and TTFA-warm per run and pooled, per arm,
# with the plays googlevideo refused (HTTP 403, rescued as the muxed stream) counted
# apart. Selection as summarize_B.ps1: seq runs, play-first from a resolve, not the
# first play of the process (that one is a launch play).
$bench = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
. "$bench\stats.ps1"
function Load($tag) {
    $rows = @()
    foreach ($f in Get-ChildItem "$bench\logs" -Filter "B_${tag}_seq_*.parsed.json") {
        $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
        $first = ($j.records | Sort-Object press | Select-Object -First 1).press
        foreach ($r in $j.records) {
            $r | Add-Member -NotePropertyName file -NotePropertyValue ($f.Name -replace '\.parsed\.json$', '') -Force
            $r | Add-Member -NotePropertyName launch -NotePropertyValue ($r.press -eq $first) -Force
            $rows += $r
        }
    }
    $rows
}
function Line($label, $sel, $field = 'ttfaMs') {
    $s = Get-Stats @($sel | ForEach-Object { $_.$field })
    "{0,-58} n={1,2}  median {2,7}  p90 {3,7}  min {4,6}  max {5,6}" -f $label, $s.n, $s.median, $s.p90, $s.min, $s.max
}
foreach ($tag in 'b3opus', 'b3rate') {
    $rows = Load $tag
    $coldB = @($rows | Where-Object { $_.kind -eq 'play-first' -and $_.linkSource -eq 'resolve' -and -not $_.launch })
    $warm = @($rows | Where-Object { $_.kind -eq 'play-again' })
    "== $tag"
    foreach ($file in ($coldB | Select-Object -ExpandProperty file -Unique | Sort-Object)) {
        Line "  (b) $file" @($coldB | Where-Object { $_.file -eq $file })
    }
    Line "  (b) pooled, all" $coldB
    Line "  (b) pooled, not refused" @($coldB | Where-Object { -not $_.httpErrors })
    Line "  (b) resolve stage, not refused" @($coldB | Where-Object { -not $_.httpErrors }) 'resolveMs'
    Line "  (b) HTTP open to response, not refused" @($coldB | Where-Object { -not $_.httpErrors }) 'httpOpenToResponseMs'
    Line "  (b) response to audio, not refused" @($coldB | Where-Object { -not $_.httpErrors }) 'responseToAudioMs'
    Line "  (b) rot0+rot6 only, not refused" @($coldB | Where-Object { -not $_.httpErrors -and $_.file -notmatch 'rot3' })
    Line "  warm (link cache), all" $warm
    Line "  warm, links not from a rescue" @($warm | Where-Object { $_.tier -eq 0 })
    Line "  warm rot0+rot6 only" @($warm | Where-Object { $_.file -notmatch 'rot3' })
    "  refused first plays: " + @($coldB + @($rows | Where-Object { $_.launch }) | Where-Object { $_.httpErrors }).Count + " of " + @($rows | Where-Object { $_.kind -eq 'play-first' }).Count
}
