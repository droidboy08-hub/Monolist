# Monolist — Qt 6 / QML music player

The Phono interface design, driven by the Melody playback and extraction
backend, rewritten in C++ against libmpv and yt-dlp.

Nothing in the interface changed. Every QML component, view, spacing token and
breakpoint is byte-for-byte the prototype's, with the module renamed from
`Phono` to `Monolist`. What changed is everything underneath: the simulated
clock is gone, the mock extractor is gone, and the source ladder that Melody
worked out in TypeScript now runs natively.

## What "merging the backend" actually meant

Melody was a React app in an Electron shell. Its backend could not be copied
across, because most of it was not music-player logic at all — it was
workarounds for running in a browser:

| Melody did this | Why | In Monolist |
| :--- | :--- | :--- |
| Scraped `ytInitialData` out of search HTML with a regex | No API key, no Node in the renderer | `yt-dlp ytsearch` — same data, no parser to break |
| Tunnelled every request through `api.codetabs.com` | Browser CORS | Deleted. A native app has no origin policy |
| Raced 5 Piped + 13 Invidious instances | Public instances fail constantly | **Kept** — as a fallback tier, not the primary |
| Fell back to a hidden YouTube iframe | A browser cannot play a URL that fails CORS | Deleted. mpv plays any URL that resolves |
| Downloaded bytes → base64 → IndexedDB → Blob → ObjectURL | Browsers cannot write files | yt-dlp writes to disk directly |
| `<audio>` element for playback | Only option available | libmpv |

The instance racing was worth carrying over and is the one piece kept close to
the original. "Ask twelve hosts, take the first answer" degrades far better than
any single endpoint, and `StreamResolver` is a direct translation of Melody's
`Promise.any` approach into `QNetworkAccessManager`.

Everything else in the table above existed only to fight the browser.

### The source ladder

`PlaybackController::beginTrack` decides where audio comes from:

```
1. downloaded file on disk       →  play it              (Melody: LOCAL)
2. source id present             →  StreamResolver       (Melody: STEALTH)
     tier 0   yt-dlp
     tier 1   Piped instance race
     tier 2   Invidious instance race
3. plain URL stored on the row   →  play it
```

Each resolver tier is tried only when the one before it fails outright, and a
tier fails only when every host in it fails. The tier that won is surfaced to
the UI as `Player.sourceLabel`.

## Layout

    CMakeLists.txt           build; finds Qt 6 and libmpv
    cmake/FindMpv.cmake      libmpv locator (pkg-config, or -DMPV_ROOT)
    scripts/setup-windows.ps1  installs Qt, CMake, Ninja, libmpv, yt-dlp

    Main.qml                 window, view switching, breakpoints, shortcuts
    Theme.qml                design tokens
    Icons.js                 Lucide glyph outlines as path data
    components/              16 UI components, unchanged from the prototype
    views/                   HomeView, SearchView, LibraryView, DownloadsView

    src/
      main.cpp               wiring; registers the QML singletons
      appdatabase.*          SQLite schema, migrations, seeding
      library.*              read models exposed to QML
      playlistmodel.*  albummodel.*  trackmodel.*

      playbackcontroller.*   the facade QML binds to — now driven by mpv
      mpvengine.*            libmpv wrapper, audio-only
      mediaextractor.*       search, backed by yt-dlp; owns SearchResultModel
      ytdlp.*                QProcess + JSON wrapper around the yt-dlp binary
      streamresolver.*       the tiered source ladder
      downloadmanager.*      offline library, queue, progress, DB rows
      artworkcache.*         async disk-cached image provider + palette tool

### QML singletons

`Library`, `Player`, `Extractor` were already there. Added: `Downloads` and
`Palette`. The image provider registers as `image://artwork/<url>`.

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
| SwiftSoup — HTML parsing | Not needed; yt-dlp removed the scraping | — |
| SwifterSwift — utility extensions | Qt's own containers and algorithms | — |
| SwiftUI-Introspect | Not applicable outside SwiftUI | — |

NewPipeExtractor is a **Java** library. Calling it from C++ means shipping a JVM
alongside the app on all three platforms, for metadata yt-dlp already returns in
the same call that resolves the stream. It is not wired in. If it is ever
wanted, `MediaExtractor` is the seam — it already isolates search behind
signals, which is exactly what a second extractor would slot into.

## Build

Requires Qt 6.5+, CMake 3.21+, a C++17 compiler, and libmpv.
`yt-dlp` and `ffmpeg` are runtime dependencies, not build ones.

### Building without libmpv

libmpv is the only dependency with no clean Windows package — the SDK ships a
MinGW-style import library that MSVC cannot link, so it needs an extra
`lib.exe` step. To get everything else running first:

```
cmake -S . -B build -G Ninja -DMONOLIST_NO_MPV=ON -DCMAKE_PREFIX_PATH=<qt-prefix>
cmake --build build
```

This compiles `mpvengine_stub.cpp` in place of `mpvengine.cpp`. The header is
identical either way, so switching back is a configure flag, not a code change.

Working in this mode: the whole interface, the database, yt-dlp search, and
downloads — yt-dlp writes files itself and never goes through mpv. Not working:
audio output. The player bar reports "Built without libmpv" in the accent
colour rather than appearing to play silence.

**Windows** — run `scripts\setup-windows.ps1` first; it installs everything and
prints the configure command. Then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=<qt-prefix> -DMPV_ROOT=<libmpv-root>
cmake --build build
```

**Arch Linux**

```
sudo pacman -S qt6-base qt6-declarative qt6-svg mpv yt-dlp ffmpeg cmake ninja ttf-archivo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/monolist
```

**macOS**

```
brew install qt mpv yt-dlp ffmpeg cmake ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build
```

The UI names the **Archivo** family (400/600/800) directly. Without it Qt
substitutes the default sans; the layout still holds but the design will not
match.

## Runtime dependencies

* **libmpv** — required for audio, or build with `-DMONOLIST_NO_MPV=ON`. Even
  in a normal build, a libmpv that fails to initialise leaves the app running
  with `Player.engineAvailable` false rather than silently doing nothing.
* **yt-dlp** — required for search and downloads. Without it, search is
  disabled and playback falls through to the Piped/Invidious tiers. Found on
  PATH, next to the executable, or as `python -m yt_dlp`.
* **ffmpeg** — only for download remuxing to m4a.

## Storage

    downloads   <Music>/Monolist/<title> [<id>].m4a
    database    <AppData>/monolist.db
    artwork     <Cache>/artwork  (256 MB cap)

Downloads live in the user's real Music folder, the convention Melody settled
on, so they survive reinstalls and other players can see them.

## State of the merge

Written but **not yet compiled** — Qt, CMake and libmpv are not installed on
this machine, so none of the C++ has been through a compiler. Expect the
ordinary first-build fixes. Run `scripts\setup-windows.ps1`, then build, and
the errors that surface will be real ones.

Known gaps, all UI-side rather than backend:

* `DownloadsView` lists the library rather than filtering to downloaded rows;
  the manager exposes `storedTracks()` for a dedicated model.
* No download button in `TrackTable` yet — `Downloads.enqueue(...)` is
  callable, nothing calls it.
* Queue and device buttons in the player bar are still styled but unwired.
* `PaletteTool` is registered and working but nothing tints itself from it yet.
