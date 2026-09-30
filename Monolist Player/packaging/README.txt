Monolist for Windows
====================

A music player: YouTube Music's catalogue, synced lyrics, downloads,
recommendations from your own listening, and optionally your YouTube Music
account and Last.fm.

Starting it
-----------

1. Unzip this folder anywhere you can write to (your Documents, a USB stick).
   It needs no installing and no administrator rights.
2. Run monolist.exe.

Windows may say "Windows protected your PC", because this build is not
signed: choose "More info", then "Run anyway".

Everything it needs is in this folder: the Qt runtime, mpv (playback),
yt-dlp, FFmpeg and Deno (in tools\). Nothing else has to be installed.

Where it keeps things
---------------------

- Your library, playlists, settings and the lyrics it has fetched:
  %APPDATA%\Monolist\Monolist
- Downloads: Music\Monolist in your user folder.
- A YouTube Music sign-in, if you import one, is kept encrypted for your
  Windows user alone.

To remove Monolist, delete this folder, and %APPDATA%\Monolist and
%LOCALAPPDATA%\Monolist if you want your library and any sign-in gone too
(sign out in Settings first to be told how to end a YouTube Music session at
Google as well).

The tools
---------

YouTube changes often, and an old yt-dlp can stop working. When songs that
used to play stop resolving, get a newer Monolist package.

Licences
--------

Monolist itself is MIT licensed. It bundles other people's software under
their own licences: see THIRD-PARTY-NOTICES.txt and the LICENSES folder.
