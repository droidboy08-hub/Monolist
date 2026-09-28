# Replays Monolist's lyrics query cleaning (src/lyrics.cpp:38-69) on the fixed
# cases in lyrics_query_cases.json, for checklist row C40. Read-only: it does
# not touch the repo or the app. The patterns below are copied verbatim from
# lyrics.cpp; if Step 4 changes them, copy the new ones here and rerun.
param([string]$Cases = (Join-Path $PSScriptRoot 'lyrics_query_cases.json'))

$ci = [Text.RegularExpressions.RegexOptions]::IgnoreCase
$separators   = New-Object Text.RegularExpressions.Regex('\s*(?:,|&|\bfeat\.?|\bft\.?)\s*', $ci)
$noise        = New-Object Text.RegularExpressions.Regex('\s*[\(\[][^\)\]]*\b(?:official|video|audio|lyrics?|visuali[sz]er|mv|hd|hq|4k|remaster(?:ed)?)\b[^\)\]]*[\)\]]', $ci)
$featuring    = New-Object Text.RegularExpressions.Regex('\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with)\s[^\)\]]*[\)\]]', $ci)
$featuringTail= New-Object Text.RegularExpressions.Regex('\s+(?:feat\.?|ft\.?|featuring)\s.*$', $ci)

function Lead([string]$artist) {
    $parts = $separators.Split($artist) | Where-Object { $_ -ne '' }
    if ($parts) { return (@($parts)[0]).Trim() } else { return '' }
}
function Simplified([string]$s) { return ([regex]::Replace($s, '\s+', ' ')).Trim() }
function SearchTitle([string]$title, [string]$artist) {
    $dash = $title.IndexOf(' - ')
    $lead = Lead $artist
    if ($dash -gt 0 -and $lead -ne '' -and $title.Substring(0, $dash).IndexOf($lead, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        $title = $title.Substring($dash + 3)
    }
    $title = $noise.Replace($title, '')
    $title = $featuring.Replace($title, '')
    $title = $featuringTail.Replace($title, '')
    return Simplified $title
}

$data = Get-Content -Raw -Encoding UTF8 $Cases | ConvertFrom-Json
$bad = 0; $n = 0
foreach ($c in $data.cases) {
    $n++
    $gotTitle = SearchTitle $c.title $c.artist
    $gotArtist = Lead $c.artist
    $ok = $true
    if ($c.PSObject.Properties.Name -contains 'wantTitle' -and $gotTitle -ne $c.wantTitle) { $ok = $false }
    if ($c.PSObject.Properties.Name -contains 'wantArtist' -and $gotArtist -ne $c.wantArtist) { $ok = $false }
    if (-not $ok) { $bad++ }
    "{0}  title='{1}' artist='{2}'  ->  title='{3}' lead='{4}'   [{5}]" -f $(if ($ok) { 'ok  ' } else { 'MISS' }), $c.title, $c.artist, $gotTitle, $gotArtist, $c.note
}
"$n cases, $bad not as wanted"
