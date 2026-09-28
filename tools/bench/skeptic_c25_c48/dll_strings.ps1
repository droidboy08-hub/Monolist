# Which mpv options, FFmpeg demuxers and decoders the bundled libmpv carries,
# found by plain string search in the DLL. Evidence for checklist rows C25
# (dither_method), C26 (audio-exclusive), C27 (no AudioToolbox / vendor audio
# decoders), C28 (replay-gain / R128 support, volume-gain), C31 (HLS and DASH
# demuxers), C47 (media-controls). Read-only; takes ~1 minute on the VM.
param([string]$Dll = 'C:\dev\monolist-deps\libmpv\libmpv-2.dll')
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Dll))
foreach ($s in @('Apple HTTP Live Streaming', 'Dynamic Adaptive Streaming over HTTP', 'dither_method',
                 'triangular_hp', 'audio-exclusive', 'audio-swresample-o', 'media-controls',
                 'stop-screensaver', 'R128_TRACK_GAIN', 'REPLAYGAIN_TRACK_GAIN', 'Applying replay-gain',
                 'volume-gain', 'reconnect_delay_max', 'dump-cache', 'aac_at', 'mp3float')) {
    $i = $text.IndexOf($s)
    '{0,-40} {1}' -f $s, $(if ($i -ge 0) { 'found' } else { 'NOT found' })
}
