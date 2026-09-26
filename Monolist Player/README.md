# Monolist — Qt 6 / QML music player

The Phono interface design, driven by the Melody playback and extraction
backend, rewritten in C++ against libmpv, yt-dlp and YouTube Music's own API.

The interface grew out of the Phono prototype and keeps its language: paper
and ink, one signal red, 2px rules, square corners, Archivo, photographs in
black and white except where a page is about one. On top of the prototype's
empty screens it now has YouTube Music's home feed, album and playlist pages,
a play queue with autoplay, the user's own playlists and likes, downloads, and
a full-size Now Playing view with synced lyrics, in a window whose title bar is
its own. Underneath, the simulated clock and the mock extractor are gone, and
the source ladder that Melody worked out in TypeScript runs natively.

## What "merging the backend" actually meant

Melody was a React app in an Electron shell. Its backend could not be copied
across, because most of it was not music-player logic at all — it was
workarounds for running in a browser:

| Melody did this | Why | In Monolist |
| :--- | :--- | :--- |
| Scraped `ytInitialData` out of search HTML with a regex | No API key, no Node in the renderer | YouTube Music's InnerTube API, with `yt-dlp ytsearch` as the fallback |
| Tunnelled every request through `api.codetabs.com` | Browser CORS | Deleted. A native app has no origin policy |
| Raced 5 Piped + 13 Invidious instances | Public instances fail constantly | **Kept** — as a fallback tier, not the primary |
| Fell back to a hidden YouTube iframe | A browser cannot play a URL that fails CORS | Deleted. mpv plays any URL that resolves |
| Downloaded bytes → base64 → IndexedDB → Blob → ObjectURL | Browsers cannot write files | yt-dlp writes to disk, FFmpeg tags it |
| `<audio>` element for playback | Only option available | libmpv |

The instance racing was worth carrying over and is the one piece kept close to
the original. "Ask twelve hosts, take the first answer" degrades far better than
any single endpoint, and `StreamResolver` is a direct translation of Melody's
`Promise.any` approach into `QNetworkAccessManager`.

### Search

`MediaExtractor` sends every search to YouTube Music's InnerTube API — the one
music.youtube.com itself calls — through `InnerTube`. It is one HTTPS request:
about 0.3 s on a warm connection, and the results are songs with their artists,
album and square cover art. Suggestions come from the same API while typing.

A yt-dlp search spends most of its ~8 s just starting (it unpacks a Python
runtime every time), so it is only the fallback: when InnerTube fails or comes
back empty, the same query runs through yt-dlp. The API is unofficial and
changes without notice; the parsers are written to degrade to "nothing found"
rather than crash, and the fallback covers the gap.

### The source ladder

`PlaybackController::beginTrack` decides where audio comes from:

```
1. downloaded file on disk       →  play it              (Melody: LOCAL)
2. source id present             →  StreamResolver       (Melody: STEALTH)
     cached link, still valid       instant
     tier 0   yt-dlp
     tier 1   Piped instance race
     tier 2   Invidious instance race
3. plain URL stored on the row   →  play it
```

Each resolver tier is tried only when the one before it fails outright, and a
tier fails only when every host in it fails. The tier that won is surfaced to
the UI as `Player.sourceLabel`.

Resolved links are cached until the expiry YouTube signs into them, and the
next song in the queue is resolved in the background while the current one
plays, so replaying or skipping forward rarely waits on yt-dlp. A link that
resolves but will not open in mpv is retried: a stale cached link is fetched
fresh, anything else moves on to the next tier.

### Downloads

`DownloadManager` runs up to three yt-dlp downloads at a time into
`<Music>/Monolist`, with FFmpeg doing the post-processing:

* **Format.** *Original* (the default) keeps the best stream exactly as
  published — usually Opus at 130–160 kb/s, unwrapped from its container but
  never re-encoded. *M4A* takes YouTube's AAC stream; *MP3* re-encodes (V0) for
  players that take nothing else.
* **Tags and cover art.** Title, artist, album and date are written into the
  file, and the thumbnail is cropped square and embedded as the cover.
* **Non-music parts.** With the option on, segments SponsorBlock users have
  marked as non-music (intros, skits) are cut out.

A finished download becomes a library row, so it plays offline straight away.
Queue state, progress and the offline set are in `Downloads.queue` and
`Downloads.library`; every track row asks `Downloads.stateFor(id)`.

### Your library

Kept in the local database and exposed as `Library`:

* **Playlists** hold songs by video id, so any song can go in one, from any
  song's menu (Add to playlist) or an album's (Add all to playlist). A playlist
  page is an album page with a mosaic of its songs' covers; the name is renamed
  in place and deleting is confirmed in place.
* **Likes** are library rows with the favourite flag. Liking a song that is not
  in the library adds it; unliking one that only the like put there takes it
  out again. Liked songs lists them, the latest first.
* **Saved albums and playlists** from YouTube Music, with Save on their pages.
* **History**: every song played, the latest first, clearable.

The sidebar shows Liked songs and the playlists, and the name the operating
system knows the user by (the account's full name, else the login name),
which the pencil there changes.

### Lyrics

`Lyrics` looks up the song playing while the Now Playing view shows them:

1. **LRCLIB** (lrclib.net), an open database of time-synced lyrics, searched by
   title and lead artist, the entry closest in length winning. Only an entry
   within 3 s of the recording's length is trusted for timing; one within 20 s
   is kept as plain text in case nothing better turns up.
2. **YouTube Music's own lyrics**, plain text from its partners (Musixmatch,
   LyricFind), credited as such.

What was found, or that nothing was, is stored per video id; "none" is asked
again after three days. Synced lines follow the playing position (a line lights
150 ms early, as it starts), and clicking one plays from there. LRCLIB can be
self-hosted; the `lrclib_url` setting points elsewhere.

The Now Playing view prints the cover on a field of its own dominant colour
(`CoverPalette`, measured from the cached artwork), with ink or paper type,
whichever contrasts better.

## Layout

    CMakeLists.txt             build; finds Qt 6 and libmpv
    cmake/FindMpv.cmake        libmpv locator (pkg-config, or -DMPV_ROOT)
    scripts/setup-windows.ps1  installs Qt, MinGW, CMake, Ninja, libmpv, yt-dlp,
                               FFmpeg, Deno and Git into C:\dev\monolist-deps
    scripts/build-windows.ps1  configures, builds and deploys a runnable build
    scripts/setup-macos.sh     installs Qt, libmpv, CMake, Ninja, FFmpeg, yt-dlp
                               and Deno through Homebrew
    scripts/build-macos.sh     builds Monolist.app with yt-dlp, Deno and FFmpeg
                               inside, or an Xcode project (--xcode)
    macos/                     Info.plist template, the app icon and its generator

    Main.qml                 window, view switching, breakpoints, shortcuts
    Theme.qml                design tokens
    Icons.js                 Lucide glyph outlines as path data
    components/              UI components: the prototype's, and the menus,
                             cards, queue panel, lyrics pane, title bar parts
    views/                   Home, Search, Library, Downloads, Page (album or
                             YouTube Music playlist), Playlist, Now Playing

    src/
      main.cpp               wiring; registers the QML singletons; self-tests
      appdatabase.*          SQLite schema, migrations
      library.*              the user's playlists, likes, saved albums, history
      playlistmodel.*  albummodel.*  trackmodel.*

      playbackcontroller.*   the facade QML binds to — driven by mpv
      queuemodel.*           the play queue
      mpvengine.*            libmpv wrapper, audio-only
      mediaextractor.*       search: InnerTube first, yt-dlp as fallback
      innertube.*            YouTube Music's API: search, suggestions, radio,
                             browse pages, lyrics
      catalog.*              Home's feed and album pages
      lyrics.*               LRCLIB and YouTube Music lyrics, synced to playback
      ytdlp.*                QProcess wrapper around yt-dlp; finds FFmpeg and Deno
      streamresolver.*       the tiered source ladder and its link cache
      downloadmanager.*      offline library: queue, options, files, DB rows
      downloadmodels.*       the queue and offline-set models for QML
      artworkcache.*         async disk-cached image provider + cover colours
      windowchrome.*         the window without the system title bar
      macos/                 the Mac's own parts: the title bar (macwindow.*), Now
                             Playing for the media keys (mediasession.*), and
                             yt-dlp and Deno kept current (toolstore.*)

### QML singletons

`Library`, `Player`, `Extractor`, `Downloads`, `Catalog`, `Lyrics`,
`CoverPalette` and `Chrome`. The image provider registers as
`image://artwork/<url>`.

## Build

Requires Qt 6.6+, CMake 3.21+, a C++17 compiler and libmpv. yt-dlp, FFmpeg and
Deno are runtime dependencies, not build ones.

### Windows

```
scripts\setup-windows.ps1
scripts\build-windows.ps1 -Run
```

`setup-windows.ps1` installs everything under `C:\dev\monolist-deps` —
nothing system-wide, no installers, PATH untouched — and checks every download
against the checksum its project publishes. Qt and its tools come straight from
Qt's online repository (no account needed). Re-running skips what is already
there; `-Update` refreshes yt-dlp, FFmpeg and Deno, which should follow their
latest releases because YouTube keeps changing.

The kit is Qt 6.11 with MinGW 13.1, 64-bit x86. On ARM64 Windows (including
Parallels on Apple Silicon) it runs under Windows' x64 emulation, since Qt only
ships native ARM64 builds for MSVC; the runtime tools run as separate processes
and use native ARM64 builds.

`build-windows.ps1` builds out of the source tree into `C:\dev\monolist-build`
(a network share is slow, and cmd.exe cannot run Qt's generators from a UNC
path), runs `windeployqt`, copies libmpv, and links the runtime tools into
`tools\` beside the executable, where the app looks first. `-Config Release`,
`-NoMpv` and `-Run` do what they say.

To configure by hand instead:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=C:\dev\monolist-deps\Qt\6.11.2\mingw_64 ^
      -DMPV_ROOT=C:\dev\monolist-deps\libmpv
cmake --build build
```

### Building without libmpv

`-DMONOLIST_NO_MPV=ON` compiles `mpvengine_stub.cpp` in place of
`mpvengine.cpp`. The header is identical either way, so switching back is a
configure flag, not a code change. Everything works except audio output; the
player bar reports "Built without libmpv" rather than appearing to play silence.

### Arch Linux

```
sudo pacman -S qt6-base qt6-declarative qt6-svg qt6-imageformats mpv yt-dlp ffmpeg deno cmake ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/monolist
```

### macOS

Needs Homebrew (https://brew.sh) and Apple's Command Line Tools; the full Xcode
from the App Store only for an Xcode project. Apple silicon and Intel alike.

```
scripts/setup-macos.sh
scripts/build-macos.sh --install
```

`setup-macos.sh` installs Qt, libmpv, CMake, Ninja, pkgconf and FFmpeg to build
with, all through Homebrew, skipping what is already there; it only has to run
on the Mac that builds the app. It also installs yt-dlp and Deno, for the
builds that do not carry their own (Xcode's); `--update` updates those.

`build-macos.sh` makes a Release build in `build-macos/`, copies it to
`build-macos/dist/Monolist.app`, puts Qt, its QML modules, libmpv and FFmpeg
inside it with `macdeployqt` (so a `brew upgrade` cannot break it), adds
yt-dlp and Deno (below), signs it ad hoc and checks that nothing is still
loaded from Homebrew. The result needs nothing installed on the Mac it runs
on. `--install` then copies it into `/Applications` (`~/Applications` when
that is not writable), `--dmg` makes a disk image beside it, `--run` opens it,
`--debug` builds Debug, `--no-deploy` skips `macdeployqt` for a quicker build
that uses Homebrew's Qt, and `--no-tools` leaves yt-dlp and Deno out, for a
build with no network.

#### yt-dlp, Deno and FFmpeg inside the app

yt-dlp has to follow YouTube, which changes often, and a signed app may not
change: macOS calls one that did damaged. So the app carries yt-dlp and Deno
as the archives their projects publish, downloaded by `build-macos.sh` from
their GitHub releases and checked against the SHA-256 each publishes (a copy
that does not match is not bundled), in `Contents/Resources/tools` with their
versions in `tools.json`. On its first launch the app unpacks them into

    ~/Library/Application Support/Monolist/Monolist/tools

and runs them from there (`ToolStore`, in `src/macos/toolstore.*`). Updates go
to the same place, so they outlive the app that fetched them, and a newer app
replaces older unpacked copies.

* **Once a day** the app asks GitHub for the latest release of each. A newer
  yt-dlp is installed without asking, since an old one simply stops playing
  tracks; a newer Deno is announced, and Settings › Update components
  installs it. Each download is checked against its published SHA-256, and a
  failed update leaves the working copy in place.
* **FFmpeg** is Homebrew's, in `Contents/MacOS` beside the executable, where
  it shares its libraries with libmpv. It is updated with the app: YouTube's
  changes do not reach it.

Downloads are kept in `build-macos/tools`; with no network, the build uses the
newest one there.

#### In Xcode

```
scripts/build-macos.sh --xcode
```

generates `build-xcode/Monolist.xcodeproj` with CMake's Xcode generator and
opens it. Pick the **monolist** scheme and **My Mac**, then Product › Run (⌘R);
the log appears in Xcode's console. The project is generated, so change
`CMakeLists.txt` rather than the project, and run the command again after
adding a file. A build run from Xcode loads Qt and libmpv from Homebrew; for
the copy to keep, use `build-macos.sh --install`.

By hand, either way:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix)"
cmake --build build          # build/Monolist.app
```

#### What is different on a Mac

* **The window.** The design runs under a transparent title bar with the
  traffic lights in its corner, as Windows keeps its snap layouts: full
  screen, tiling and Mission Control work as for any other window. A double
  click on the bar does what System Settings › Desktop & Dock says. This needs
  Qt 6.9 or later; with an older Qt the system title bar simply stays.
* **Closing the window** leaves the app and the music running; the Dock icon
  brings the window back, and ⌘Q quits. ⌘W, ⌘M and ⌃⌘F do what they do in any
  Mac app.
* **Media keys and Now Playing.** The play, next and previous keys, AirPods,
  the Touch Bar, Control Center and the lock screen control Monolist and show
  the song, its cover and its position. Without this, the play key opens
  Apple Music.
* **Tools from Finder.** An app opened from Finder or the Dock does not get
  the shell's PATH, so Homebrew's folders (`/opt/homebrew/bin`,
  `/usr/local/bin`) and `~/.deno/bin` are added to it at startup: builds that
  do not carry their own tools (Xcode's) still find Homebrew's.
* **Light appearance.** The design draws its own colours, so the app keeps
  the light appearance in Dark Mode, as it looks on Windows.

The app is signed ad hoc ("Sign to Run Locally"), which is all a Mac needs to
run what was built on it. To give it to someone else, sign it with a Developer
ID and notarise it; without that, macOS will refuse to open a downloaded copy.

The icon is `macos/Monolist.icns`, drawn from the bundled Archivo by
`macos/make-icon.py` (Pillow); run that again if the palette changes.

### Typeface

**Archivo (400/600/800) is bundled** in `fonts/` and registered from Qt
resources at startup, so nothing needs installing on any platform. Google ships
Archivo as a variable font only; the three static instances here were cut from
it with `fonttools varLib.instancer` and renamed so a single family "Archivo"
carries all three weights. Archivo is OFL-1.1 and `fonts/OFL.txt` travels with
it, as the licence requires.

This matters more than it sounds: the design's letter-spacing (`Theme.tracking`,
including negative tracking on headings) is calibrated for Archivo's metrics, so
a fallback sans does not merely look different — the tracking is wrong for it.

## Runtime dependencies

* **libmpv** — required for audio, or build with `-DMONOLIST_NO_MPV=ON`. A
  libmpv that fails to initialise leaves the app running with
  `Player.engineAvailable` false rather than silently doing nothing.
* **yt-dlp** — stream resolution and downloads, and the search fallback.
  Without it, playback falls through to the Piped/Invidious tiers.
* **Deno** — the JavaScript runtime yt-dlp uses to solve YouTube's player
  challenges. Without one, YouTube hides most formats, the good audio ones
  included.
* **FFmpeg** — download post-processing: extraction, tags, cover art, trimming.
  Without it, downloads are saved as the raw stream.

All three tools are looked for in `tools\` next to the executable, next to the
executable itself, in `MONOLIST_TOOLS_DIR`, and then on PATH; bundled copies win.
On a Mac, yt-dlp and Deno are looked for first where the app unpacks and
updates them (above), FFmpeg next to the executable
(`Monolist.app/Contents/MacOS`), and PATH includes Homebrew's folders even when
the app is opened from Finder.

## Self-tests and diagnostics

The debug build keeps its console. Start it with `QT_FORCE_STDERR_LOGGING=1` to
see the log there, and with `MONOLIST_MPV_LOG=warn` (or `info`, `v`) to add
mpv's own messages.

    monolist --play <videoId> [seconds] [--again] [--at <s>]
                                                    resolve and play; --again replays from the cache,
                                                    --at jumps into the song
    monolist --download <videoId> [seconds]         one download through yt-dlp and FFmpeg
    monolist --search "<query>"                     one timed search, with suggestions
    monolist --lyrics "<query>"                     lyrics for the first three results, then one from the store
    monolist --library-test "<query>"               a playlist, likes and a saved album from a real search
    monolist --diag                                 what the database holds

Each quits by itself and reports on stderr. These open the window as it would
be, for a look at a state:

    monolist --view <view>                          home, search, downloads, library[:albums|:history],
                                                    page:<browse id>, playlist:<id>, playlist:liked
    monolist --query "<text>"                       search, with the text typed in
    monolist --open-queue  /  --now-playing         with the queue, or Now Playing, open
    monolist --set <key> <value>                    write a setting first (lrclib_url, piped_instances,
                                                    invidious_instances)

`MONOLIST_DATA_DIR` keeps the database somewhere else, so a test never touches
the real library.

## Storage

    downloads   <Music>/Monolist/<artist> - <title> [<id>].<ext>
    database    <AppData>/monolist.db  (library, playlists, history, lyrics, settings)
    artwork     <Cache>/artwork  (256 MB cap)

On a Mac, `<Music>` is `~/Music`, `<AppData>` is
`~/Library/Application Support/Monolist/Monolist` and `<Cache>` is
`~/Library/Caches/Monolist/Monolist`.

Downloads live in the user's real Music folder, the convention Melody settled
on, so they survive reinstalls and other players can see them.

## State

Builds and runs on Windows 11 (ARM64, through x64 emulation) with Qt 6.11.2 and
MinGW 13.1. Verified end to end: InnerTube search and suggestions, streaming
through yt-dlp, the link cache, downloads with tags and cover art, offline
playback, the queue and autoplay radio, Home and album pages, playlists, likes
and saved albums, and synced and plain lyrics.

Known gaps:

* Artist pages: an artist card searches for the name instead.
* The device button in the player bar is styled but unwired.
* Songs in a playlist cannot yet be reordered.
* macOS: prepared for, but not yet run on a Mac. The shared code compiles
  with Clang against libc++ (Apple's standard library) with the Mac code paths
  on; the Objective-C++ is checked against the AppKit and MediaPlayer
  declarations it uses; the build scripts are tested under macOS's bash 3.2
  with Homebrew, CMake and the signing tools stood in for. `ToolStore` has run
  for real on Linux: unpacking the bundled archives, updating from GitHub
  with the checksums checked, the daily check, and a failed update. The
  first real build is the test of the rest.
* Linux builds and starts (checked on Ubuntu 24.04 with Qt 6.11.2), but has
  not been used day to day and has no packaging yet.

## About the Swift libraries

Kingfisher, SwiftyJSON, SwiftSoup, SwifterSwift, ColorThiefSwift and
SwiftUI-Introspect are Swift/iOS libraries. They cannot be linked into a
Qt/C++ application on Windows or Linux — there is no binding path, and
Introspect has no meaning outside SwiftUI. Each one's *job* is covered:

| Wanted | Used instead | Where |
| :--- | :--- | :--- |
| Kingfisher — async image load + cache | `QQuickAsyncImageProvider` + `QNetworkDiskCache` | `artworkcache.*` |
| ColorThiefSwift — dominant colour | Weighted RGB histogram over a downsampled image | `PaletteTool` |
| SwiftyJSON — JSON parsing | `QJsonDocument` (built into Qt) | throughout |
| SwiftSoup — HTML parsing | Not needed; InnerTube and yt-dlp removed the scraping | — |
| SwifterSwift — utility extensions | Qt's own containers and algorithms | — |
| SwiftUI-Introspect | Not applicable outside SwiftUI | — |

NewPipeExtractor is a **Java** library. Calling it from C++ means shipping a JVM
alongside the app on all three platforms, for metadata InnerTube and yt-dlp
already return. It is not wired in. If it is ever wanted, `MediaExtractor` is
the seam — it already isolates search behind signals, which is exactly what a
second extractor would slot into.
