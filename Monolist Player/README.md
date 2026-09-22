# Monolist — Qt 6 / QML music player

The Phono interface design, driven by the Melody playback and extraction
backend, rewritten in C++ against libmpv, yt-dlp and YouTube Music's own API.

The interface is still the prototype's: every component, view, spacing token
and breakpoint came from the Phono design, with the module renamed from `Phono`
to `Monolist`. What was added on top of it is only what the backend needs to be
usable — download controls on every track, a real Downloads page, search
suggestions and a Songs / Videos switch. What changed underneath is everything:
the simulated clock is gone, the mock extractor is gone, and the source ladder
that Melody worked out in TypeScript now runs natively.

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
next track in the library is resolved in the background while the current one
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

## Layout

    CMakeLists.txt             build; finds Qt 6 and libmpv
    cmake/FindMpv.cmake        libmpv locator (pkg-config, or -DMPV_ROOT)
    scripts/setup-windows.ps1  installs Qt, MinGW, CMake, Ninja, libmpv, yt-dlp,
                               FFmpeg, Deno and Git into C:\dev\monolist-deps
    scripts/build-windows.ps1  configures, builds and deploys a runnable build

    Main.qml                 window, view switching, breakpoints, shortcuts
    Theme.qml                design tokens
    Icons.js                 Lucide glyph outlines as path data
    components/              UI components: the prototype's, plus
                             DownloadButton and ChoiceChip
    views/                   HomeView, SearchView, LibraryView, DownloadsView

    src/
      main.cpp               wiring; registers the QML singletons; self-tests
      appdatabase.*          SQLite schema, migrations
      library.*              read models exposed to QML
      playlistmodel.*  albummodel.*  trackmodel.*

      playbackcontroller.*   the facade QML binds to — driven by mpv
      mpvengine.*            libmpv wrapper, audio-only
      mediaextractor.*       search: InnerTube first, yt-dlp as fallback
      innertube.*            YouTube Music's API: search and suggestions
      ytdlp.*                QProcess wrapper around yt-dlp; finds FFmpeg and Deno
      streamresolver.*       the tiered source ladder and its link cache
      downloadmanager.*      offline library: queue, options, files, DB rows
      downloadmodels.*       the queue and offline-set models for QML
      artworkcache.*         async disk-cached image provider + palette tool

### QML singletons

`Library`, `Player`, `Extractor`, `Downloads` and `Palette`. The image provider
registers as `image://artwork/<url>`.

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

```
brew install qt mpv yt-dlp ffmpeg deno cmake ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build
```

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

## Self-tests and diagnostics

The debug build keeps its console. Start it with `QT_FORCE_STDERR_LOGGING=1` to
see the log there, and with `MONOLIST_MPV_LOG=warn` (or `info`, `v`) to add
mpv's own messages.

    monolist --play <videoId> [seconds] [--again]   resolve and play; --again replays from the cache
    monolist --download <videoId> [seconds]         one download through yt-dlp and FFmpeg
    monolist --search "<query>"                     one timed search, with suggestions
    monolist --view downloads                       open on a view (home, search, library, downloads)
    monolist --query "<text>"                       open on search with the text typed in

Each quits by itself and reports on stderr.

## Storage

    downloads   <Music>/Monolist/<artist> - <title> [<id>].<ext>
    database    <AppData>/monolist.db
    artwork     <Cache>/artwork  (256 MB cap)

Downloads live in the user's real Music folder, the convention Melody settled
on, so they survive reinstalls and other players can see them.

## State

Builds and runs on Windows 11 (ARM64, through x64 emulation) with Qt 6.11.2 and
MinGW 13.1. Verified end to end: InnerTube search and suggestions, streaming
through yt-dlp, the link cache, downloads with tags and cover art, and offline
playback.

Known gaps, all on the interface side:

* The play queue is the library. Playing a search result does not make the
  results a queue, so Next moves on through the library.
* Queue and device buttons in the player bar are still styled but unwired.
* `PaletteTool` is registered and working but nothing tints itself from it yet.
* The sidebar's account block and playlist list are prototype placeholders, and
  the Library view shows albums only.

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
