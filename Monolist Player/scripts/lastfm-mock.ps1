<#
.SYNOPSIS
    A stand-in for Last.fm's web service, on this computer only, for trying
    Monolist's sending without a key or an account.

.DESCRIPTION
    Listens on http://127.0.0.1:<Port>/ and answers the four calls Monolist
    makes (auth.getToken, auth.getSession, track.updateNowPlaying and
    track.scrobble) the way Last.fm does, JSON included: a scrobble batch is
    answered item by item, one item as an object and several as an array.

    It knows only the self-tests' invented account (TESTAPIKEY..., the shared
    secret TESTSHAREDSECRET..., the session TESTSESSIONKEY0000) and checks every
    request against it as Last.fm would: the api_sig recomputed from the body as
    the server decodes it ('+' is a space), then the key, then the session.
    A wrong signature is error 13, a wrong key 10, a wrong session 9.

    A path with /err<N>/ in it answers every call with Last.fm error N, so
    MONOLIST_LASTFM_URL=http://127.0.0.1:8765/err26/ is a suspended key.

    One line per request goes to the console (and -Log): the method, how many
    scrobbles, and whether the signature, key and session checked out. Never a
    value. It stops after -Seconds, or when /quit is asked for.

.EXAMPLE
    # in one terminal
    .\lastfm-mock.ps1 -Seconds 300
    # in another, with a scratch data folder
    $env:MONOLIST_DATA_DIR = "$env:TEMP\monolist-send"
    $env:MONOLIST_LASTFM_URL = 'http://127.0.0.1:8765/2.0/'
    monolist --scrobble-send-test 120
    $env:MONOLIST_LASTFM_URL = 'http://127.0.0.1:8765/err26/2.0/'
    monolist --scrobble-send-test 120 --expect-kept
#>
[CmdletBinding()]
param(
    [int]    $Port    = 8765,
    [int]    $Seconds = 600,
    [string] $Log     = ''
)

$ErrorActionPreference = 'Stop'

# The self-tests' invented account; nothing real is ever used here.
$apiKey  = 'TESTAPIKEY0123456789abcdef012345'
$secret  = 'TESTSHAREDSECRET0123456789abcdef'
$session = 'TESTSESSIONKEY0000'

function Write-Line([string] $text) {
    $line = '{0}  {1}' -f (Get-Date -Format 'HH:mm:ss.fff'), $text
    Write-Host $line
    if ($Log) { Add-Content -LiteralPath $Log -Value $line -Encoding UTF8 }
}

# application/x-www-form-urlencoded, as a server reads it: '+' is a space,
# then %XX are bytes, and the bytes are UTF-8.
function Read-Form([string] $body) {
    $pairs = New-Object 'System.Collections.Generic.List[object]'
    foreach ($part in $body.Split('&')) {
        if ($part.Length -eq 0) { continue }
        $equals = $part.IndexOf('=')
        $name = if ($equals -lt 0) { $part } else { $part.Substring(0, $equals) }
        $value = if ($equals -lt 0) { '' } else { $part.Substring($equals + 1) }
        $pairs.Add([pscustomobject]@{
            Name  = [Uri]::UnescapeDataString($name.Replace('+', ' '))
            Value = [Uri]::UnescapeDataString($value.Replace('+', ' '))
        })
    }
    return ,$pairs
}

# last.fm/api/authspec: every parameter but format, callback and api_sig,
# sorted by name byte for byte, name then value, the secret on the end, MD5.
function Get-Signature($pairs) {
    $names = [string[]]@($pairs | Where-Object { $_.Name -notin 'format', 'callback', 'api_sig' } | ForEach-Object { $_.Name })
    [Array]::Sort($names, [StringComparer]::Ordinal)
    $text = New-Object System.Text.StringBuilder
    foreach ($name in $names) {
        $value = ($pairs | Where-Object { $_.Name -ceq $name } | Select-Object -First 1).Value
        [void]$text.Append($name).Append($value)
    }
    [void]$text.Append($secret)
    $md5 = [Security.Cryptography.MD5]::Create()
    $hash = $md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString()))
    return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-Value($pairs, [string] $name) {
    $found = $pairs | Where-Object { $_.Name -ceq $name } | Select-Object -First 1
    if ($found) { return $found.Value }
    return $null
}

function Get-ErrorBody([int] $code) {
    $messages = @{
        6  = 'Invalid parameters - Your request is missing a required parameter'
        9  = 'Invalid session key - Please re-authenticate'
        10 = 'Invalid API key - You must be granted a valid key by last.fm'
        11 = 'Service Offline - This service is temporarily offline. Try again later.'
        13 = 'Invalid method signature supplied'
        14 = 'Unauthorized Token - This token has not been authorized'
        15 = 'This token has expired'
        16 = 'There was a temporary error processing your request. Please try again'
        26 = 'Suspended API key - Access for your account has been suspended, please contact Last.fm'
        29 = 'Rate Limit Exceeded - Your IP has made too many requests in a short period'
    }
    $message = if ($messages.ContainsKey($code)) { $messages[$code] } else { 'Error' }
    return (@{ error = $code; message = $message } | ConvertTo-Json -Compress)
}

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
try {
    $listener.Start()
} catch {
    Write-Line "could not listen on 127.0.0.1:${Port}: $($_.Exception.Message)"
    exit 1
}
Write-Line "listening on http://127.0.0.1:$Port/ for $Seconds s"

$deadline = (Get-Date).AddSeconds($Seconds)
$served = 0
try {
    while ((Get-Date) -lt $deadline) {
        $pending = $listener.BeginGetContext($null, $null)
        while (-not $pending.AsyncWaitHandle.WaitOne(250)) {
            if ((Get-Date) -ge $deadline) { break }
        }
        if (-not $pending.IsCompleted) { break }
        $context = $listener.EndGetContext($pending)
        $request = $context.Request
        # Read now: the request is gone once the response is closed.
        $path = $request.Url.AbsolutePath
        $verb = $request.HttpMethod

        if ($path -eq '/quit') {
            $context.Response.StatusCode = 200
            $context.Response.Close()
            Write-Line 'asked to quit'
            break
        }

        $reader = New-Object IO.StreamReader($request.InputStream, [Text.Encoding]::UTF8)
        $pairs = Read-Form $reader.ReadToEnd()
        $reader.Close()

        $method = Get-Value $pairs 'method'
        $items = @($pairs | Where-Object { $_.Name -like 'artist`[*`]' }).Count
        $sigOk = (Get-Value $pairs 'api_sig') -eq (Get-Signature $pairs)
        $keyOk = (Get-Value $pairs 'api_key') -ceq $apiKey
        $needsSession = $method -in 'track.scrobble', 'track.updateNowPlaying'
        $skOk = (-not $needsSession) -or ((Get-Value $pairs 'sk') -ceq $session)

        $status = 200
        if ($path -match '/err(\d+)/') {
            $code = [int]$Matches[1]
            $status = if ($code -in 11, 16) { 503 } elseif ($code -eq 29) { 429 } else { 403 }
            $json = Get-ErrorBody $code
        } elseif (-not $sigOk) {
            $status = 403; $json = Get-ErrorBody 13
        } elseif (-not $keyOk) {
            $status = 403; $json = Get-ErrorBody 10
        } elseif (-not $skOk) {
            $status = 403; $json = Get-ErrorBody 9
        } else {
            switch ($method) {
                'auth.getToken' { $json = '{"token":"TESTTOKEN6789"}' }
                'auth.getSession' { $json = '{"session":{"name":"monolist-test","key":"' + $session + '","subscriber":0}}' }
                'track.updateNowPlaying' {
                    $json = '{"nowplaying":{"artist":{"corrected":"0","#text":""},"track":{"corrected":"0","#text":""},' +
                            '"ignoredMessage":{"code":"0","#text":""}}}'
                }
                'track.scrobble' {
                    $one = '{"ignoredMessage":{"code":"0","#text":""}}'
                    $list = if ($items -eq 1) { $one } else { '[' + ((1..$items | ForEach-Object { $one }) -join ',') + ']' }
                    $json = '{"scrobbles":{"scrobble":' + $list + ',"@attr":{"accepted":' + $items + ',"ignored":0}}}'
                }
                default { $status = 400; $json = Get-ErrorBody 6 }
            }
        }

        $bytes = [Text.Encoding]::UTF8.GetBytes($json)
        $context.Response.StatusCode = $status
        $context.Response.ContentType = 'application/json; charset=utf-8'
        $context.Response.ContentLength64 = $bytes.Length
        $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
        $context.Response.Close()
        $served++

        $what = if ($method -eq 'track.scrobble') { "$method, $items scrobbles" } else { "$method" }
        Write-Line ('{0} {1}: {2}; signature {3}, key {4}, session {5} -> {6}' -f $verb, $path, $what,
                    $(if ($sigOk) { 'ok' } else { 'WRONG' }), $(if ($keyOk) { 'ok' } else { 'WRONG' }),
                    $(if (-not $needsSession) { '-' } elseif ($skOk) { 'ok' } else { 'WRONG' }), $status)
    }
} finally {
    $listener.Stop()
    $listener.Close()
    Write-Line "stopped after $served requests"
}
