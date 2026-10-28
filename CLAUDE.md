# Monolist — working notes for Claude Code

Read this first in every session. It is the project's memory: what the app is,
how to build and test it, the owner's decisions, the rules, and what is left.

## What this is

Monolist Player is a desktop music player: Qt 6.11 / QML + C++, libmpv for
playback, YouTube / YouTube Music as the catalogue (our own InnerTube client
plus yt-dlp), optional JioSaavn 320 kbps, synced lyrics, a local recommender,
downloads, Last.fm scrobbling and a YouTube Music account (by cookie import).
Windows and macOS share one code base; Linux is planned later.

It grew from Melody (old Electron app), Phono (Qt mockup, in `Design/`) and the
owner's iPhone app (AryaMusix / "Mediano", Swift, not in this repo).

- Code: `Monolist Player/` (`src/` C++, `views/` + `components/` QML,
  `Main.qml`, `Theme.qml`, `DESIGN.md`, `README.md`, `scripts/`, `macos/`).
- `ROADMAP.md` (repo root): every open item, ticked as work lands.
- `docs/engine/`: the engine study against the owner's BitChord research —
  `1-engine-map.md`, `2-scorecard-and-measurements.md`, `3-plan.md` (the
  approved, ranked plan; items and their status are listed below).
- `docs/research/connections-design.md`: Last.fm + YouTube Music account design.
- `docs/testing-your-youtube-account.md`: the owner's live test checklist
  (not written yet — see "Next", item 0; the account features are untested
  with a real account).
- GitHub: `github.com/droidboy08-hub/Monolist` (public, MIT, branch `main`).
  Recommendation data: `github.com/droidboy08-hub/Monolist-data` (tag `v1`,
  CC BY-NC data; the app downloads it from Settings → Recommendations).

## Build and run (Windows)

1. Toolchain in `C:\dev\monolist-deps` (Qt 6.11.2 mingw_64, MinGW 13.1, CMake,
   Ninja, libmpv x86_64, yt-dlp unpacked, FFmpeg, Deno, PortableGit):
   `powershell -ExecutionPolicy Bypass -File "Monolist Player\scripts\setup-windows.ps1"`
   (safe to re-run; `-Update` refreshes yt-dlp / FFmpeg / Deno). It downloads —
   ask the owner first, stating what and how big.
2. Build (Debug → `C:\dev\monolist-build\debug\monolist.exe`; runs qmllint and
   fails on real QML errors, then deploys):
   `powershell -ExecutionPolicy Bypass -File "Monolist Player\scripts\build-windows.ps1"`
   Add `-Config Release` for `C:\dev\monolist-build\release`. Only one build at
   a time. Keep the source on a LOCAL disk: building from a network share (the
   old Parallels setup) was a large part of why work was slow.
3. Run with logs: `$env:QT_FORCE_STDERR_LOGGING='1'; $env:MONOLIST_MPV_LOG='warn'`.
4. macOS (the owner's MacBook Pro, Apple silicon, macOS 26; Homebrew's Qt
   6.12, libmpv 0.41, FFmpeg 9): `Monolist Player/scripts/setup-macos.sh`
   once, then `scripts/build-macos.sh --install` (Release, self-contained,
   signed ad hoc, into /Applications). For quick work a Debug build:
   `cmake -S "Monolist Player" -B "Monolist Player/build-dev" -G Ninja
   -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix)"`
   then `cmake --build ... --parallel 10` (about 4 minutes from clean on the
   M-series, seconds after). Mac-only code lives in `src/macos/`. The clone
   is HTTPS (no SSH keys on the Mac); `gh` is installed for pushing.
5. Package and installer: `package-windows.ps1 -Installer` (needs
   `setup-windows.ps1 -Installer` once, for Inno Setup): Release build, a
   self-contained folder with real copies of the tools and the Vulkan loader,
   a DLL check that fails on anything neither in it nor in Windows, the zip
   and `Monolist-<version>.<build>-setup-win64.exe` in
   `C:\dev\monolist-build\package`.
6. Releases: push a tag `v<version>.<build>` (build = `git rev-list --count
   HEAD` of the tagged commit); `.github/workflows/release-windows.yml` builds
   the installer and zip on GitHub, runs offline self-tests on them and
   publishes the GitHub Release, which the app's update check reads.
   `.github/workflows/release-macos.yml` (2026-10-24) builds the Mac's
   `Monolist-<version>.<build>-macos-arm64.dmg` from the same tag on
   GitHub's Apple silicon runner, runs the offline self-tests, waits for the
   Windows job's release and adds the image and a Mac section to it. On a
   Mac the update check offers a release only once it has that image.

## Testing

- ALWAYS set `MONOLIST_DATA_DIR` and `MONOLIST_DOWNLOAD_DIR` to scratch folders
  for tests. Never touch the owner's real library, database or Music folder.
- Self-tests are flags on the app (grep `src/main.cpp` for `-test"`): e.g.
  `--saavn-test`, `--listen-test`, `--scrobble-test`, `--lyrics-race-test`,
  `--cookie-test`, `--account-guard-test`, `--ytm-session-test`, `--ytm-library-test`, `--visitor-test`, `--format-test`,
  `--download-cleanup-test`, `--download-queue-test`, `--download-folder-test`, `--loudness-test`, `--library-edit-test`, `--recovery-test`, `--session-test`,
  `--sound-test` (the sound effects with no libmpv: chain text, ramps, presets, clipping maths, what is kept;
  in the release gate), `--sound-live-test` (the effects on the real libmpv with `MONOLIST_MPV_AO=null`, no
  sound device; timed, so not in the release gate), `--icon-test` (the app icon choice; shortcuts made in the
  scratch folder only, never the real Start menu or desktop), `--search-test` (a search's pages after the
  first, on a stand-in YouTube Music), `--startup-test` (opening at sign-in, on a scratch registry key; a
  scratch library never writes the real Run key), `--tray-test` (closing to the system tray: the switch, the menu), `--sleep-test` (the sleep timer on a stand-in player), `--import-test` (playlist import on stand-in sources;
  asserts no cookie or account header is ever sent), `--just-played-test` (Home's just-played rows on a stand-in
  YouTube Music with a signed-in fake account: one cookieless look-up a settled song, none for a skip, the
  pacing, caps, rests and holds, and the rows built from the answers), `--just-played-probe <id>...` (live, up
  to 5 ids 20 s apart, cookieless; not in the gate), `--backup-test` (the library's backup file and its merge:
  a round trip, every merge rule both ways, a second merge changing nothing, refusals, nothing device-specific,
  times later than now left out),
  `--drive-test` (the Google Drive backup's sign-in, tokens and sync on stand-ins for Google and Drive, never
  Google; asserts no cookie sent, the token to Drive alone, no redirect elsewhere, no secret in the log),
  `--update-test` (the in-app update on a stand-in for GitHub and scratch copies: the file per kind of copy,
  the project's release files only, SHA-256, size cap, cancel/quit leaving no .part, a launch's cleanup
  leaving a download alone, a double-click's CANCEL let go, TRY AGAIN re-checking; on a Mac scratch bundles
  in disk images, a by-hand copy shown only a checked, quarantined image, and the helper's swap, second
  signature check and way back (cannot move in, will not open, does not start); on Windows a portable copy's
  zip and swap with the helper started inside its folder, an old folder with the user's files kept, the
  downloads inside left alone, and the setup's arguments; in the release gate), `--update-run` (About's
  CHECK, UPDATE and RESTART with no window, against `--set update.feed <stand-in>`: it REPLACES THE COPY IT
  RUNS FROM, so only ever a scratch copy under /private/tmp; `MONOLIST_UPDATE_TEST_FAULT=move-new` makes the
  Mac swap fail, to show the way back; `MONOLIST_TEST_VERSION=0.1.1` has one build play an older one,
  scratch libraries only, taken out of the environment at launch). `scripts/update-e2e-windows.ps1` (in
  release-windows.yml) installs an older setup made from the package, for the user and for everyone, and a
  portable copy, on GitHub's machine, and has each update itself from a loopback feed,
  `--ogg-test` (Mac only: the Ogg tag and cover reader on files it writes page by page, the app's own
  Spotlight importer loaded as Spotlight loads it, and a cover set as a scratch file's Finder icon) … Each
  prints `N checks, 0 failed`. All must pass, except `--scrobble-send-test`,
  which needs its local stand-in (`scripts/lastfm-mock.ps1`). `MONOLIST_MPV_AO=null` plays any run to no
  sound device (and `MONOLIST_MPV_VO=null` decodes a picture to nowhere, for an engine with no surface).
- Screenshots: `--shot <file.png> [seconds] [--shot-scroll <px>] [--quit]` (with `--view`, `--settings-section`) saves the
  window as the app draws it. Capturing the window from outside (PrintWindow) returns the last frame
  shown, which is stale while the window is behind others. Give a size at launch with Qt's own
  `-qwindowgeometry 1400x900+40+40` rather than resizing the window afterwards, which re-wraps the
  page after `--settings-section` scrolled to its part. A shot that must show a toast keeps the window
  in front (topmost): the toast's fade runs on the window's frames, and a window driven by posted
  clicks from behind others may be given none, so a toast missing from a shot is not proof that none
  was shown. Rows in a state for a look: `--ytm-demo <state>`, `--backup-demo <state>`.
  A window launched behind others (2026-10-21) never fired QML Timers either, so
  `--effects-popup` (opened by an 800 ms Timer) shot with no popup; with
  `QSG_RENDER_LOOP=basic` in the environment it opened every time.
- On the Mac: self-tests run with `QT_QPA_PLATFORM=offscreen` (no window),
  and the Mac helpers check for the Cocoa platform before touching AppKit;
  `--shot` works there and logs where the traffic lights are ("shot:
  traffic lights at x,y wxh"), since Qt's picture leaves them out and the
  shell has no Screen Recording permission for `screencapture`. The Keychain
  test (`--secret-test`) and the login agent's (`--startup-test`) only ever
  touch items for the scratch library. `--scrobble-kill-test` waits to be
  killed by hand, and `--library-test`, `--queue-test`, `--rec-test` need an
  argument (without one the app just starts).
  The bundled tools (yt-dlp, Deno) live in the REAL Application Support folder
  whatever MONOLIST_DATA_DIR says: a deployed app that carries them unpacks
  them there at launch, even on a scratch library (a scratch library starts
  no daily tool check since 2026-10-28). Copies for tests that launch a
  deployed app (the update's end-to-end run) are built with
  `build-macos.sh --no-tools`. The end-to-end run reopens the updated copy
  through LaunchServices (a window, briefly): close it by its PID, and
  `lsregister -u` the scratch copy afterwards, since it shares the owner's
  bundle id and is a newer build.
- Recommender checks (need the data): `--artist-test <EmbeddingData> <GraphData>`
  (baseline 17 graph / 5 sounds-like / 0 none, 0 leaks), `--graph-test` (22
  countries, 0 leaks), `--content-test <EmbeddingData> <report>` (0 of 19 and
  0 of 46 wrong).
- Live checks: `--play <videoId> <seconds> [--again] [--as "<title>" "<artist>" <seconds>]`,
  `--search "<q>"`, `--lyrics "<q>"`, `--view <view>`. JioSaavn needs
  `--set jiosaavn.enabled 1` AND a real title, artist and length (without a
  length it refuses on purpose). Royalty-free test id: `LrM_Y39Gmhk`.
- Timing benchmarks, fixed track lists and all measured results from the engine
  study are in `tools/bench/` (the instrumentation patch applies to a scratch
  copy, never to the repo). Re-measure speed items with the same scripts and
  lists; report median and p90. Raw research is in `docs/research/raw/`; Claude
  Code's memory notes from the old machine are in `docs/claude-memory/`.

## Git and GitHub

- Commit author email must be the GitHub no-reply address
  `276597617+droidboy08-hub@users.noreply.github.com` (set it with
  `git config user.email` in a new clone). Never commit with the Gmail address.
- Push after every finished piece of work: the owner wants everything on GitHub
  ("upload as much as you can"). Stage only files you changed.
- Never rewrite published history without asking.

## The owner's decisions (in force)

- Licence: the code stays **MIT**. The BitChord reference JavaScript, Metrolist,
  InnerTubeX are GPL-3.0: learn from them, never copy code from them; implement
  from the BitChord paper as a specification.
- Playback: the old YouTube ladder (InnerTube visionOS → yt-dlp → muxed itag 18 →
  Piped/Invidious) always stays as the fallback. After a googlevideo refusal:
  a fresh InnerTube link first, then itag 18, for that track only.
- JioSaavn is **opt-in**: Settings → Playback → Sound: *Standard* (default,
  JioSaavn never contacted) / *High · 320 kbps*. Under High it sends the
  Indian region headers (X-Forwarded-For / X-Real-IP 49.36.0.1) by default —
  the owner's explicit choice and risk; a switch turns them off.
- YouTube account: **the owner chose (2026-10-01) an in-app sign-in window**:
  Google's own page in WebView2 (`GoogleSignIn`), the engine untouched (no
  user-agent change, no script injection, no request interception, no
  automation or debugging port), a fresh private profile per sign-in deleted
  afterwards, only YouTube's cookies taken (into `YtmSession` as an import),
  and Google's refusal taken as final (no retry; the cookie import instead).
  The cookie import stays as the fallback. Still never read cookies out of
  browser profiles. The account is used for playback only *when needed*
  (login-required / age / bot-check), for the personal Home, library import,
  and listen reports to YouTube history (on by default).
- Search result click plays that song, then similar songs (autoplay radio).
- "Hide explicit titles" (suggestions only) exists and is off by default;
  Burzum is not blocked.
- Last.fm: the API key and secret come from the owner's environment variables
  `MONOLIST_LASTFM_API_KEY` / `MONOLIST_LASTFM_SHARED_SECRET`, injected at build
  time, never committed.

## Rules

- Never log, print, commit or store in the settings table any cookie value,
  SAPISID, session key, API key or token; secrets go through `SecretStore`.
- The owner's rule (2026-09-29): signing in must never put the user's Google
  account at risk. Every call that carries the account goes through
  `AccountGuard` (one at a time, spaced, counted per hour and day, and a rest
  of 1 h / 3 h / 24 h on a 429, a bot check or a 403); never add a call with
  the account that bypasses it, never retry one quickly, never write back to
  the account. 401 ends a session; 403 only rests it and has it checked. The
  evidence and the numbers: `docs/research/account-safety.md`.
- The Google Drive backup's sign-in (`DriveAuth`, `DriveSync`, QML `Backup`) is not the YouTube Music
  sign-in and never touches it: its own names (never `GoogleSignIn`/`SignIn`/`signin.*`), its own
  network manager with cookies Manual both ways, secret `backup.google`, settings `backup.*`, log prefix
  `backup:`; only `openid email drive.appdata`, never a YouTube scope; never AccountGuard (a Drive 429
  would rest YouTube); the token to www.googleapis.com only; signing out of one leaves the other.
  Its client id and secret come from `MONOLIST_GOOGLE_CLIENT_ID`/`_SECRET` at build time, never
  committed; tests use `setTestClient` and loopback stand-ins only (`--drive-test`).
- Home's just-played rows (`JustPlayed`) ask YouTube Music signed out and cookieless, never through
  `AccountGuard`: at most one look-up a settled song, paced and persisted (`home.justPlayed.pace`).
- The in-app update (`AppUpdater`, Settings › Updates; the owner's request, 2026-10-28) takes the release's
  files only over HTTPS from github.com/droidboy08-hub/Monolist/releases/download/, redirected only to
  objects.githubusercontent.com or release-assets.githubusercontent.com (plain HTTP only on loopback, for a
  loopback feed and a scratch library), never with a cookie, never past the feed's size, and nothing it has
  not matched against the published `.sha256`; on a Mac it keeps the copy's kind (Developer ID or ad hoc)
  and checks the staged app (codesign --deep --strict, identifier, newer build, team 7YWFR8A74F for
  Developer ID) before touching the old one, and before showing a by-hand copy its (then quarantined) disk
  image; RESTART stays a press of its own. The Mac helper is Monolist's own executable (`--update-swap`,
  dispatched first in main(), before any application object): App Management lets only the same developer
  change a notarised app, which a shell is not. Windows' portable swap never deletes a folder holding a file
  the new one lacks, and never swaps a folder holding the library or the downloads. Never run `--update-run`
  or the helper against /Applications, Program Files or the owner's copy.
- Keep everything portable: Windows-only code behind `Q_OS_WIN` with a compiling
  fallback; macOS-only code in `src/macos/`.
- Ask before downloading or installing anything (say what, where from, size).
  Never hard-delete the owner's files.
- Read `Monolist Player/DESIGN.md` before touching QML: flat, zero radius (but
  list-row covers and the Modern player bar's cover and play square, 6 px,
  which Classic modern squares and frames in ink; Now
  Playing's cover, 10 px with the app's one shadow), light and dark (`Theme.dark`; `Theme.ink`/`paper` for
  marks on pictures and red), ambient colour, 2 px
  rules, Archivo only, signal red; hover in instantly, fade out; grey at rest,
  ink under the pointer, red = on; no scale/rotate/overshoot.

## Traps met before

- 2026-10-01: the owner's library database was damaged: the schema root
  overwritten with Home's cache, other tables' pages crossed. It happened
  while a development build and the installed app had the same database open.
  It was repaired from the pages, and the damaged copy is kept beside it. Never
  run a build on the owner's real data folder while their app is open. Use
  MONOLIST_DATA_DIR scratch folders for every GUI check. The app now refuses a
  second instance per data folder (InstanceGuard), checks the database at
  launch, and keeps a daily backup.
- 2026-10-02: the owner's installed Monolist was found with no Settings > Apps
  entry. Release checks had installed each setup on this PC (into a test folder)
  and uninstalled it again: it shares the AppId, so its uninstall took the
  owner's entry with it. Never run the setup on the owner's PC to check a
  release; check the portable zip unpacked in a scratch folder (the same files),
  and leave installing to the owner.

- mpv list options: never `change-list <opt> set` with a URL (it splits on ':'
  on macOS/Linux — videos lost their sound); clear, then `append`.
- PowerShell 5.1: no `&&`; write commit messages to a file and `git commit -F`;
  don't round-trip source through Get-Content/Set-Content (mangles UTF-8).
- QML: `Window.window` is null inside Drag/TapHandler; a property named `on`+
  Capital is read as a signal handler; `Palette` is QtQuick's type (ours is
  `CoverPalette`); ids shadow same-named properties; `top` is FINAL on Item;
  replacing a ScrollBar's contentItem drops auto-hide (use MonoScrollBar).
- QML (2026-10-02): a plain TapHandler lets the press go on to whatever is
  underneath, even through a Popup with a press-eating MouseArea behind it;
  in a popup over a page, give taps `gesturePolicy:
  TapHandler.ReleaseWithinBounds` (ChoiceChip/ToggleRow: `takesPress`) or use
  a MouseArea. A ReleaseWithinBounds TapHandler on a Text never fired there;
  text links use a MouseArea, as the toast's does.
- QML (2026-10-19), why: a plain (DragThreshold) TapHandler that is topmost
  under a mouse press keeps the press accepted, so Qt skips every Item below
  it, press-eating MouseAreas included, and hands the press to the Pointer
  Handlers further down, none of which sees an exclusive grabber. A click on
  Now Playing's cover played the hidden song row beneath. Rules: Main.qml
  disables `shell` while Now Playing, the full-screen picture or the narrow
  sidebar covers it (and Now Playing under the full-screen picture); anything
  drawn over interactive content keeps its presses (an accepting MouseArea,
  or ReleaseWithinBounds taps) and has a `HoverHandler { blocking: true }`.
  Checked by real clicks (posted to Monolist's own window) on a scratch
  library; keys posted that way do not reach Qt's Shortcuts.
- 2026-10-24, colour fades: never animate a colour to "transparent". It is
  black at no opacity, and ColorAnimation interpolates the channels apart,
  so a hover grey fading out to it passed through a translucent mid grey:
  a dark band trailing the pointer down the sidebar (the owner's "weird
  black overlay"). Fade to `Theme.clear(c)`, the same colour at no opacity.
- 2026-10-24, menus made from a model (PlaylistSubmenu's Instantiator):
  an entry whose click changes that model (adding a song resets
  Library.playlists) is taken out of the menu inside its own click, with the
  connection that closes the menus: they stayed open. Act after the click
  (Qt.callLater).
- 2026-10-27, lists read from C++ (Qt 6.9 on, so the Mac's 6.12 and
  Windows' 6.11 alike): an element read from a QVariantList property
  (`JustPlayed.rows[i]`) is not a value but "element i of that property",
  read again whenever it is read. Kept in a JS array, an object's field or a
  closure, it turns into the property's next element i with nothing told;
  assigned to a `var` property or passed through a signal it is detached.
  Home's just-played rows kept them in `sessionShown`: the shelves went on
  drawing old cards, a press looked its row up in the new ones and played
  nothing (the owner's "Home cards don't play after a playlist"). Copy before
  keeping (`JSON.parse(JSON.stringify(...))`), and have a press act on what
  its delegate draws, never on a lookup in another copy.
- 2026-10-24, frame rate on the Mac: the app as shipped runs Qt's threaded
  render loop and draws at the display's rate (8 ms frames at 120 Hz while
  scrolling). QSG_RENDER_LOOP=basic (a test setting) caps animations at
  60 Hz, and --scroll-test counts frames only in the basic loop; measure the
  default with QT_LOGGING_RULES=qt.scenegraph.time.renderloop=true and the
  window in front (macOS draws nothing for a covered window).
- 2026-10-27, Opus (Ogg) in the Finder: macOS 26 reads no Ogg tags or
  cover, and app extensions cannot add them. Quick Look sends every audio
  file to its own "secure" Audiovisual thumbnailer (public.audiovisual-
  content), ahead of any app's thumbnail extension, even one for exactly
  org.xiph.ogg-audio that the user switched on (that extension did run for
  an unclaimed type); and a Core Spotlight import extension was called by
  neither `mdimport -t` nor indexing. What works: a legacy .mdimporter in
  Contents/Library/Spotlight (`MonolistOgg`), which Spotlight loads only
  once told (`mdimport -r <importer>`, done once per build by the app), and
  the cover as the file's custom icon (MacFinder, src/macos/finder.h).
  Testing: Spotlight never indexes a hidden folder (this worktree under
  .claude/), so check real indexing in a visible scratch folder; `mdimport
  -t` and `qlmanage` read from anywhere. Spotlight keeps the first path it
  learned for an importer's bundle id (`mdimport -L`), presumably until mds
  restarts (not yet seen): `mdimport -r` on another copy, or a newer
  version, did not replace it,
  and a user `mdworker` keeps the old binary loaded (it ignores SIGTERM).
  So an importer `mdimport -r`'d from a scratch or build folder is the one
  this Mac goes on using; don't announce one from a folder that will go. NSURLCustomIconKey answers nil for a custom icon: read the
  Finder info's kHasCustomIcon instead.
- 2026-10-28, volumes on a Mac: QStorageInfo matches mount points by the path's text, so a path under one
  of macOS's firmlinks (/Applications, /Users, /private) reads as on "/", the sealed read-only system
  volume: every scratch copy looked like one on a disk image. Ask `statfs` (MNT_RDONLY) and compare
  `stat`'s st_dev for "read-only" and "same volume". And `hdiutil attach` now and then attaches an
  image and mounts nothing of it: let it go by its device (`/dev/diskN` in the -plist answer), never
  leave it attached, and try once more.
- 2026-10-28, the update's review: QSettings opened on a registry path (`HKEY_CURRENT_USER\\...`) MAKES
  the key when it is not there, even to read; Inno Setup takes any key under Uninstall\\{AppId}_is1 for an
  installation, so a CHECK on a portable copy invented a per-user install. Read the registry with
  RegOpenKeyEx/RegGetValue. A Windows process holds its current folder (no FILE_SHARE_DELETE): a helper that
  renames Monolist's folder must not start in it (QProcess inherits it; Explorer starts an exe in its own
  folder). PowerShell's names ignore case: `$started` is the parameter `$Started`. And `QByteArray::contains`
  of an empty array is true: a test's "is this image attached" said yes for a file that was gone.
- Tooltips need the QtQuick.Controls module deployed (build-windows.ps1 copies
  it); importing QtQuick.Controls or QtQuick.Dialogs in QML makes windeployqt
  ship every style (~20 MB) — avoid.
- Qt binds a null QString as SQL NULL: wrap text bindings in `AppDatabase::text()`.
- 2026-10-12: NVIDIA's overlay took Monolist for a game (Alt+Z notice, frames
  hooked; `game[1]` in NVIDIA's ShadowPlay CaptureCore.log under ProgramData)
  because Qt's flip swap chain gets independent flip; the always-on-top mini
  player always did. main.cpp sets QT_D3D_NO_FLIP=1 before the application:
  keep it. Anything that moves for as long as a song plays must step, not
  animate (DESIGN 2.7): the playing bars' animation drew the whole window at
  the screen's 180 Hz, 36% of a core. Measure frames/s with
  `QT_LOGGING_RULES=qt.scenegraph.time.renderloop=true` ("frame rendered").
- 2026-10-20: two Google sign-ins now live in the app, and the names invite
  mixing them: `SignIn`/`GoogleSignIn` is YouTube Music's (cookies, WebView2),
  `Backup`/`DriveAuth` is the Drive backup's (OAuth in the browser, scope
  `drive.appdata`). Keep them apart (Rules). Why: the YouTube Music session is
  the one the owner must not risk, and AccountGuard counts every call it sees
  against the account's hourly share and pauses every use of the account for
  an hour or more on a 429 or a 403, so Drive going through it would spend
  that share and let a busy Drive stop the account; and a Drive request sent
  with YouTube's cookies would show that session to another Google service
  for nothing. Signing out of either leaves the other signed in.
- 2026-10-21: the backup merge (`LibraryBackup::merge`) compares times from
  other computers, so never take one as true because it parses: a clock
  running hours ahead (a dual-boot PC keeping UTC as local time) wins every
  comparison until real time catches up, and a Clear history it dated
  "tomorrow" emptied History on every computer at every merge. Anything later
  than now + 10 min is left out of a file read or written (dropped, never
  moved to now: a play's time is its identity), DriveSync stops syncing when
  Google's Date is more than 5 min from this clock, and History is cleared
  only by a tombstone the merge has just brought. And never merge a playlist
  by taking the newer copy whole: a pin on a computer that had not synced yet
  threw away songs added on another. Songs merge as a set; taking one out
  writes a `playlistEntry` tombstone (Library::removeFromPlaylist).
- 2026-10-21, the effects' chain: a biquad exactly at half the sound's rate
  (16 kHz in a 32 kHz file, 8 kHz in 16, 4 kHz in 8) is two poles on z = -1,
  and in the chain's floating point (afir makes the whole graph float) it
  rings at half the rate even at 0 dB. Studio clarity's 4, 8 and 16 kHz bands
  are written 1 Hz above for that (FFmpeg then passes them over, so commandsAt
  never sends them anything there). The equaliser's own 4, 8 and 16 kHz bands
  still sit exactly there: at 32 kHz the chain at rest, before Studio clarity
  too, put a 16 kHz tone at -22 dBFS beside a 63 Hz tone at -20 dBFS (-33 dBFS
  at 16 kHz, -37 at 8; not fixed yet, and no source Monolist plays today is
  32, 16 or 8 kHz; 1 Hz above fixed it in a scratch chain). And never FFmpeg's
  octave width (t=o) in
  the chain: it divides by sin(w0), and at 32 kHz a 16 kHz band, at any gain,
  turned all the sound to nothing.

## Where things stand (2026-09-29)

Done and on GitHub: the 13 core bugs; search radio; explicit filter; in-app
recommendation-data download; Last.fm (connect, listening time, offline queue,
sending — live test waits for the owner's key); artist pages; search by type;
reorder; menus everywhere; shelf actions; output device; video everywhere; the
engine study (steps 1-3) and plan items B0-B9 (shared visitor id, backup
client, Opus first, lyrics fixes and race, record after load, stream-info
line, fresh link before itag 18, early-end recovery, 20 s resolve deadline);
JioSaavn (opt-in, mid-song upgrade, 320 kbps downloads); the Mac app; the
account plumbing, personal Home (signed-in feed via FEmusic_home), signed-in
playback when needed, listen reports; and the step-by-step cookie-import guide
with clear sign-in states in Settings → Connections (check `git log`).

2026-09-29, on the native Windows PC (item 0, built and tested against the
stand-in; the live test with the owner's spare account is still to do,
`docs/testing-your-youtube-account.md`):
- `AccountGuard`: every call with the account paced, counted and rested (the
  owner's account-safety rule, see Rules); 403 now rests the account rather
  than ending the session; the yt-dlp account rung comes after yt-dlp signed
  out; F41 fixed (continuations carry the account).
- Read-only library import (C04, `YtmImport`, QML `AccountLibrary`): Liked on
  YouTube Music, the account's playlists (opened with the account, private
  ones too), its history; own tables, deleted on sign-out.
- Personalised Home both ways: the account's feed in its own order with
  "For <name>", mixes opened with the account, the check's feed reused (one
  call, not two); signed out, "Suggested for you" (Recs.homeShelves) on top.

Next, in the order the owner last agreed (ask which first):
0. The owner's live test of the account work with a spare account
   (`docs/testing-your-youtube-account.md`); tune AccountGuard's limits from
   what it shows (docs/research/account-safety.md lists what is unknown).
1. Engine plan items left (`docs/engine/3-plan.md`): next-song byte prefetch
   (PP-02) and gapless (QT2) first; the disk cache (PP-01) only if the owner's
   listening shows enough replays (B0 found 2.1 % replays, 1.2 % tracks over
   7.5 min, in 1.4 days); more keyless lyrics providers + a provider menu
   (LY-7, LY-10), then word-by-word lyrics (LY-6/LY-8); long-track range reads
   (PP-08, gate not met). Done 2026-09-30: loudness levelling (QT1, on by
   default).
2. The roadmap batch that was interrupted, what is left of it: Home
   continuation, charts and moods (F27, A05, X08), library Songs / Artists
   views and filters (L04, L05), virtualised track tables (F28) and the small
   F-items. Done 2026-09-30 and released in 0.1.122: P04, N01, P02, D01, D02,
   A06.
3. The rest of the roadmap: library export/import, local files, playlist import
   from links, preferences, in-app log, app icon, a Windows installer,
   automated tests, README/DESIGN.md refresh.

Sound effects (2026-10-02; the owner asked for an equaliser with Slowed +
reverb, High bass, 8D audio and Nightcore; ROADMAP P07): the engine
(`SoundChain`, one `@fx` lavfi chain in mpv's af, live changes by af-command
ramps; Slowed and Nightcore are mpv's speed with pitch correction off), the
model (`Sound` singleton, `SoundEffects`, sound.* settings) and the QML are
built and tested (`--sound-test`, `--sound-live-test`, screenshots). The QML:
`EffectsPanel` (one block, in the player bar's popup and in Settings >
Playback, `--settings-section effects`), `BandSlider`, the sliders glyph on
the bar (red while any effect is on; since the 2026-10-21 bar it never folds),
`--effects-popup` for screenshots, Now Playing's sound line. Studio clarity
(2026-10-21, the owner's, after LastWave's "Studio Master Clarity"; its numbers
only, LastWave has no licence): ten octave-wide bands of its own after the
equaliser's (`equalizer@c0..9`, `SoundChain::kClarityDb`), not added into
them; written as Qs for 48 kHz, with the song's own rate's widths sent by the
engine once the graph answers (`widthsAt`, `sendWidths`); measured in the
shipped libmpv within 0.02 dB of the octave bands at 48, 44.1 and 22.05 kHz.
The owner has not yet listened to the defaults on a real device (reverb
strengths, High bass +6 vs +9, Studio clarity). On the Mac (2026-10-24) Now
Playing publishes the playback rate, and `--sound-test` (131) and
`--sound-live-test` (115) pass against Homebrew's FFmpeg 9 and libmpv 0.41.

The Mac (2026-10-24, the owner: "this project is finished for Windows, you
have to work on the Mac version"): the Windows build is the reference
design; the Mac's own parts are the Keychain store, opening at login (a
LaunchAgent), the title bar (the traffic lights moved into the 64 px strip),
the menu bar, the Dock icon choice (the M. kept as a third icon, the
owner's), Mac wording in Settings, the icons from the note, and the
Google sign-in window in WebKit (`src/macos/googlesignin_mac.mm`: the
engine as it comes, a non-persistent data store per sign-in, only Google's
and YouTube's pages, only YouTube's cookies, Google's refusal final; seen
reaching Google's identifier page, not yet signed in with a real account,
and Google may refuse WebKit as it may WebView2). Signing (2026-10-24, the owner wants both
builds): `build-macos.sh --sign <identity> [--notarize <profile>]` makes
Monolist-unsigned.dmg and the Developer ID Monolist.dmg (README "Signed and
notarised"). Set up 2026-10-24: the owner's Developer ID Application
identity (team 7YWFR8A74F, SHA-1 4DBD0474…) in the login keychain, the
notary profile `monolist-notary` (an App Store Connect API key), and the
same certificate and key as GitHub secrets, so release-macos.yml signs,
notarises and publishes both images for macOS 15+ (a build on this Mac
needs macOS 26: Homebrew builds for the macOS it runs on). Apple's verdict
on the first: "Ready for distribution", no issues. Keep mpv's Lua scripts off
on a Mac (MpvEngine): under the hardened runtime LuaJIT is killed whatever
the entitlements; the app needs none. Left there: the Dock's
icon while Monolist is closed (a dock tile plug-in), font weights (G04: 88
`Font.Bold`, which no bundled face is), and the decisions on Developer ID
signing and Intel Macs.

Waiting on the owner: the Last.fm key (C01); a live test of the YouTube account
features with a spare account (C02-C04, see the checklist).

## Working with the owner

- Short messages, approves with "go"; wants momentum and measured evidence.
- Wants everything pushed to GitHub as it lands.
- Tell them EARLY when something about the environment makes work slow or
  expensive (they were unhappy to learn late that the emulated ARM VM + network
  share made builds 5-10 min and batches ~15 h).
- Agent batches: the owner found them too slow on the VM. Ask before launching
  long multi-agent runs; prefer doing work directly with builds, self-tests and
  targeted measurements, and use agents when the owner says so.
