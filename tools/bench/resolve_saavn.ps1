# The JioSaavn stream for one song, as the app gets it: song.getDetails for the
# id the app's matcher accepted (see saavn\live12-final-on.log), the link
# decrypted (DES-ECB, the key every JioSaavn client uses), moved to HTTPS and
# to the 320 kbps rendition where the row says it has one. Writes the link to
# -UrlFile and prints one JSON line (id, kbps, bytes from a HEAD).
param([Parameter(Mandatory = $true)][string]$SaavnId, [Parameter(Mandatory = $true)][string]$UrlFile)
$ErrorActionPreference = 'Stop'
$ua = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36'
$api = "https://www.jiosaavn.com/api.php?__call=song.getDetails&_format=json&_marker=0&api_version=4&ctx=android&pids=$SaavnId"
$sw = [Diagnostics.Stopwatch]::StartNew()
$body = & curl.exe -s --max-time 10 -A $ua -H 'Accept: application/json' -H 'X-Forwarded-For: 49.36.0.1' -H 'X-Real-IP: 49.36.0.1' -H 'Cookie: explicit_content=1' $api
$apiMs = $sw.ElapsedMilliseconds
$json = ($body -join "`n")
$brace = $json.IndexOf('{')
if ($brace -gt 0) { $json = $json.Substring($brace) }
$root = $json | ConvertFrom-Json
$song = $null
if ($root.songs) { $song = $root.songs[0] } else { $song = $root.$SaavnId }
$more = $song.more_info
$enc = if ($more.encrypted_media_url) { $more.encrypted_media_url } else { $song.encrypted_media_url }
$has320 = (($more.'320kbps') -eq 'true') -or (($song.'320kbps') -eq 'true')
$des = [Security.Cryptography.DES]::Create()
$des.Mode = [Security.Cryptography.CipherMode]::ECB
$des.Padding = [Security.Cryptography.PaddingMode]::PKCS7
$des.Key = [Text.Encoding]::ASCII.GetBytes('38346591')
$cipher = [Convert]::FromBase64String($enc)
$plain = $des.CreateDecryptor().TransformFinalBlock($cipher, 0, $cipher.Length)
$url = [Text.Encoding]::UTF8.GetString($plain).Trim()
if ($url.StartsWith('http://')) { $url = 'https://' + $url.Substring(7) }
$kbps = 0
$m = [regex]::Match($url, '_(48|96|160|320)\.(mp4|aac|mp3)$')
if ($m.Success) {
    $kbps = [int]$m.Groups[1].Value
    if ($has320 -and $kbps -ne 320) { $url = $url.Substring(0, $m.Groups[1].Index) + '320' + $url.Substring($m.Groups[1].Index + $m.Groups[1].Length); $kbps = 320 }
}
[IO.File]::WriteAllText($UrlFile, $url)
$head = & curl.exe -s -I --max-time 10 $url
$len = 0
foreach ($l in $head) { if ($l -match '^(?i)content-length:\s*(\d+)') { $len = [long]$Matches[1] } }
$server = ($head | Where-Object { $_ -match '^(?i)(server|x-cache|via):' }) -join ' | '
[ordered]@{ saavnId = $SaavnId; title = $song.title; kbps = $kbps; host = ([Uri]$url).Host; bytes = $len; apiMs = $apiMs; server = $server } | ConvertTo-Json -Compress
