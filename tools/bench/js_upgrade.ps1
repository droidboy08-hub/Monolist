# Step JS (QT7): one --play of a song JioSaavn has, with JioSaavn's answer held
# back so YouTube wins the race and the song moves over mid-song. A fresh data
# folder per run, so no remembered verdict short-cuts the race.
param(
    [string]$VideoId = 'fsiPzT50ZiM', [string]$Title = 'Tum Hi Ho', [string]$Artist = 'Arijit Singh',
    [int]$Length = 262, [int]$Seconds = 30, [int]$LateMs = 3000, [string]$Tag = 'run', [string[]]$Extra = @(),
    [string]$MpvLog = 'warn'
)
$s = 'C:\Users\droid\AppData\Local\Temp\claude\--psf-Home-Desktop-Multiplatform-Music-player\04bbb583-faaf-4a88-9a62-5827ef684e67\scratchpad'
$data = "$s\agentdata\js\up_$Tag"
if (Test-Path $data) { Get-ChildItem $data -Recurse -File | ForEach-Object { [IO.File]::Delete($_.FullName) } }
New-Item -ItemType Directory -Force $data | Out-Null
$env:MONOLIST_DATA_DIR = $data
$env:MONOLIST_DOWNLOAD_DIR = "$data\dl"
$env:QT_FORCE_STDERR_LOGGING = '1'
$env:MONOLIST_MPV_LOG = $MpvLog
$log = "$s\bench\logs\js_up_$Tag.log"
$a = @('--set', 'jiosaavn.enabled', '1', '--play', $VideoId, "$Seconds", '--as', $Title, $Artist, "$Length")
if ($LateMs -gt 0) { $a += @('--saavn-late', "$LateMs") }
$a += $Extra
$quoted = ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '
$p = Start-Process -FilePath 'C:\dev\monolist-build\debug\monolist.exe' -ArgumentList $quoted -NoNewWindow -Wait -PassThru -RedirectStandardError $log -RedirectStandardOutput "$log.out"
"exit $($p.ExitCode)"
Get-Content $log -Encoding UTF8 | Where-Object { $_ -match '^(upgrade|jiosaavn|stream:|selftest: (\+|position|sound)|mpv\[(warn|error)\])' }
