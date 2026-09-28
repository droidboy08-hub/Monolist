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
4. macOS: `Monolist Player/scripts/setup-macos.sh`, then `build-macos.sh`
   (Monolist.app, Xcode project). Mac-only code lives in `src/macos/`.

## Testing

- ALWAYS set `MONOLIST_DATA_DIR` and `MONOLIST_DOWNLOAD_DIR` to scratch folders
  for tests. Never touch the owner's real library, database or Music folder.
- Self-tests are flags on the app (grep `src/main.cpp` for `-test"`): e.g.
  `--saavn-test`, `--listen-test`, `--scrobble-test`, `--lyrics-race-test`,
  `--cookie-test`, `--ytm-session-test`, `--visitor-test`, `--format-test`,
  `--download-cleanup-test`, `--library-edit-test`, `--recovery-test` … Each
  prints `N checks, 0 failed`. All must pass, except `--scrobble-send-test`,
  which needs its local stand-in (`scripts/lastfm-mock.ps1`).
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
- YouTube account: signing in is **only by cookie import** (the user signs in on
  Google's own page and imports the session). Do NOT build an in-app sign-in
  window, and do not read cookies out of browser profiles — a safety check
  ruled both out. The account is used for playback only *when needed*
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
- Keep everything portable: Windows-only code behind `Q_OS_WIN` with a compiling
  fallback; macOS-only code in `src/macos/`.
- Ask before downloading or installing anything (say what, where from, size).
  Never hard-delete the owner's files.
- Read `Monolist Player/DESIGN.md` before touching QML: flat, zero radius, 2 px
  rules, Archivo only, signal red; hover in instantly, fade out; grey at rest,
  ink under the pointer, red = on; no scale/rotate/overshoot.

## Traps met before

- mpv list options: never `change-list <opt> set` with a URL (it splits on ':'
  on macOS/Linux — videos lost their sound); clear, then `append`.
- PowerShell 5.1: no `&&`; write commit messages to a file and `git commit -F`;
  don't round-trip source through Get-Content/Set-Content (mangles UTF-8).
- QML: `Window.window` is null inside Drag/TapHandler; a property named `on`+
  Capital is read as a signal handler; `Palette` is QtQuick's type (ours is
  `CoverPalette`); ids shadow same-named properties; `top` is FINAL on Item;
  replacing a ScrollBar's contentItem drops auto-hide (use MonoScrollBar).
- Tooltips need the QtQuick.Controls module deployed (build-windows.ps1 copies
  it); importing QtQuick.Controls or QtQuick.Dialogs in QML makes windeployqt
  ship every style (~20 MB) — avoid.
- Qt binds a null QString as SQL NULL: wrap text bindings in `AppDatabase::text()`.

## Where things stand (2026-09-28)

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

Next, in the order the owner last agreed (ask which first):
0. Finish the YouTube account work that was cut short:
   - read-only YouTube Music library import (ROADMAP C04): liked songs (browse
     `VLLM`) into a separate "Liked on YouTube Music" playlist, the account's
     playlists (`FEmusic_liked_playlists`, private ones opened with the
     account, continuations), and its history (`FEmusic_history`); "Sync now"
     plus refresh when a session becomes Active; never write back to YouTube;
     hide on sign-out;
   - personalised Home both ways: signed in, render YouTube Music's personal
     shelves properly (Quick picks = musicResponsiveListItemRenderer rows in a
     musicCarouselShelfRenderer, Listen again, Mixed for you) with play
     actions and "For <name>"; signed out (or the switch off), put one or two
     of Monolist's own recommender shelves ("Made for you", "Because you like
     …") at the top of Home when the data is installed and there is history;
   - `docs/testing-your-youtube-account.md`: a numbered live-test checklist for
     the owner's spare account (import, Settings state, Home, library import,
     an age-restricted song, a listen in YouTube history, sign out).
1. Engine plan items left (`docs/engine/3-plan.md`): next-song byte prefetch
   (PP-02) and gapless (QT2) first; the disk cache (PP-01) only if the owner's
   listening shows enough replays (B0 found 2.1 % replays, 1.2 % tracks over
   7.5 min, in 1.4 days); loudness normalisation from YouTube's loudnessDb
   (QT1); more keyless lyrics providers + a provider menu (LY-7, LY-10), then
   word-by-word lyrics (LY-6/LY-8); long-track range reads (PP-08, gate not met).
2. The roadmap batch that was interrupted: restore queue after restart (P04),
   back/forward keys (N01), Windows media keys + system media controls (P02),
   Home continuation/cache/charts/moods (F27, A06, A05, X08), library Songs /
   Artists views and filters (L04, L05), download queue persistence and folder
   choice (D01, D02), virtualised track tables (F28) and the small F-items.
3. The rest of the roadmap: library export/import, local files, playlist import
   from links, preferences, in-app log, app icon, a Windows installer,
   automated tests, README/DESIGN.md refresh.

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
