# Phono — Qt 6 / QML desktop player

A 1:1 QML implementation of the Phono interface design (Modernist design
system: flat surfaces, 2px rules, zero radius, Archivo, one signal red).
No visual decisions were changed; the only additions are the technical
requirements — responsive breakpoints, keyboard shortcuts, and a local
SQLite store.

## Requirements

* Qt 6.5 or newer (Quick, Quick Controls, Sql, Svg)
* CMake 3.21+, a C++17 compiler
* The **Archivo** font family (400 / 600 / 800). The UI names the family
  directly; without it Qt substitutes the default sans and the layout still
  holds, but the design will not match.

Arch Linux: pacman -S qt6-base qt6-declarative qt6-svg cmake ninja ttf-archivo

## Build

    cmake -S qt -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/phono

Windows (MSVC): use the Qt-provided toolchain file or open the folder in Qt
Creator. macOS: same commands; the bundle is produced at build/phono.app.

## Structure

    Main.qml                 window, view switching, breakpoints, shortcuts
    Theme.qml                design tokens, transcribed from the system CSS
    Icons.js                 Lucide glyph outlines as path data
    components/              reusable UI parts, one file each
      Icon, IconButton, NavItem, PlaylistRow, Sidebar, TopBar,
      PosterHero, PosterButton, SectionHeader, AlbumCard, Artwork,
      TrackTable, ProgressSlider, NowPlayingBar, HRule
    views/                   HomeView, SearchView, LibraryView, DownloadsView
    src/                     C++ backend
      appdatabase.*          SQLite schema, seeding, connection
      library.*              read models exposed to QML (Library singleton)
      playlistmodel.*, albummodel.*, trackmodel.*
      playbackcontroller.*   Player singleton — playback facade
      mediaextractor.*       Extractor singleton — stream resolution stub

## Responsive behaviour

* >= 900px  sidebar docked at 296px, as designed
* < 900px   sidebar becomes an overlay opened from a menu button in the top bar
* < 1040px  player bar drops the output/volume group
* < 760px   player bar keeps artwork and transport only
* Track table drops the album column below 900px and the artist column below 700px
* Album grid reflows from four columns down to one

## Connecting the real backend

The UI depends only on the three registered singletons, so nothing in QML
changes when the engines land:

* **Playback** — replace the simulated clock in `PlaybackController` with
  libmpv. Keep the property set (playing, position, duration, volume,
  shuffle, repeatMode) and drive `setPosition` / `setDuration` from mpv
  events. `loadIndex` is where a file or resolved stream URL is handed to
  the engine.
* **Extraction** — `MediaExtractor::search` / `resolve` currently return
  mock results after a timer. Swap in yt-dlp through `QProcess` (parse the
  JSON on stdout) or NewPipeExtractor for metadata, and emit the same
  `searchFinished` / `resolved` / `failed` signals.
* **Data** — the schema in `appdatabase.cpp` already carries playlists,
  albums, tracks, the featured poster, history, favourites and settings. Sample rows are inserted
  only when a table is empty, so real data simply takes their place.

## Content in the database

Nothing shown in the interface is hardcoded in QML except the "PHONO / V.2.6"
mark. Playlists, albums, tracks and the red poster statement are seeded into
SQLite on first run (only when a table is empty) and read back through the
models, so real data simply replaces them. The account name falls back to a
value in the `settings` table.

## Known placeholders

* Album artwork renders as a flat plate with a label until `artwork` paths
  are stored; the system prints photographs in black and white, so the
  grayscale layer hook is left in `Artwork.qml`.
* Queue and device buttons in the player bar are present and styled but not
  yet wired to a panel.
