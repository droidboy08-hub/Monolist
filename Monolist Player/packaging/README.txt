Monolist for Windows
====================

A music player: YouTube Music's catalogue, synced lyrics, downloads,
recommendations from your own listening, and optionally your YouTube Music
account and Last.fm.

Starting it
-----------

Installed: Monolist is in the Start menu (and on the desktop, if you chose
that). The installer puts it in your user folder by default, so it needs no
administrator rights.

The portable zip: unzip it anywhere you can write to (your Documents, a USB
stick) and run monolist.exe in the Monolist folder.

Windows may say "Windows protected your PC", because this build is not
signed: choose "More info", then "Run anyway".

Everything it needs comes with it: the Qt runtime, mpv (playback), yt-dlp,
FFmpeg and Deno (in the tools folder). Nothing else has to be installed.

Where it keeps things
---------------------

- Your library, playlists, settings and the lyrics it has fetched:
  %APPDATA%\Monolist\Monolist
- Downloads: Music\Monolist in your user folder.
- A YouTube Music sign-in, if you import one, is kept encrypted for your
  Windows user alone, in %LOCALAPPDATA%\Monolist.

Uninstalling (Settings > Apps, or deleting the portable folder) removes the
program only. To remove your library and any sign-in too, delete
%APPDATA%\Monolist and %LOCALAPPDATA%\Monolist afterwards (sign out in
Monolist's Settings first to be told how to end a YouTube Music session at
Google as well).

The tools
---------

YouTube changes often, and an old yt-dlp can stop working. When songs that
used to play stop resolving, install the newest Monolist:
https://github.com/droidboy08-hub/Monolist/releases

Licences
--------

Monolist itself is MIT licensed. It bundles other people's software under
their own licences: see THIRD-PARTY-NOTICES.txt and the LICENSES folder.
