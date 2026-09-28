# Monolist — brief for implementation agents

## Where things are
- Repo root: `\\psf\Home\Desktop\Multiplatform Music player` (the same folder is mapped as `Z:\Desktop\Multiplatform Music player`; use Z: for anything that runs cmd, cmake or ninja).
- The app: `Monolist Player\` — `src\` (C++), `views\`, `components\`, `Main.qml`, `Theme.qml`, `DESIGN.md`, `README.md`, `ROADMAP.md` is at the repo root.
- Windows ARM64 VM, Qt 6.11.2 MinGW x64 under emulation. The Mac is only a file share: nothing can be run on macOS from here, so every change must stay portable (guard platform code with Q_OS_*; no .exe names, backslash paths or WinAPI outside guards). macOS must build too.

## Build and test
- Build (Debug, what the user runs): `powershell -ExecutionPolicy Bypass -File "Z:\Desktop\Multiplatform Music player\Monolist Player\scripts\build-windows.ps1"` — output `C:\dev\monolist-build\debug\monolist.exe`. It runs qmllint and fails on real QML errors. Add `-Config Release` for `C:\dev\monolist-build\release`. Incremental builds take a few minutes under emulation; allow a 10-minute timeout.
- Only one build may run at a time. Do not start a build while another process is building.
- Run the app for logs: set `$env:QT_FORCE_STDERR_LOGGING='1'`, `$env:MONOLIST_MPV_LOG='warn'`, and ALWAYS `$env:MONOLIST_DATA_DIR` to a scratch folder of your own (e.g. `C:\Users\droid\AppData\Local\Temp\claude\--psf-Home-Desktop-Multiplatform-Music-player\04bbb583-faaf-4a88-9a62-5827ef684e67\scratchpad\agentdata\<your-name>`) — never the user's real library.
- Self-test flags are listed in `Monolist Player\README.md` under "Self-tests and diagnostics" and implemented in `src\main.cpp` (e.g. `--play <videoId> [seconds]`, `--search "<q>"`, `--library-test`, `--diag`, `--view <view>`, `--events`, `--rec-test`, `--artist-test <cat> <graph>`, `--graph-test <cat> <graph>`, `--content-test <cat> <report>`). Royalty-free test video id: `LrM_Y39Gmhk`.
- Recommender data (licensed CC BY-NC — read in place only; NEVER copy, move or modify): catalogue `Z:\Desktop\AryaMusix\musicplayer\EmbeddingData`, graph `Z:\Desktop\AryaMusix\musicplayer\GraphData`.
- Screenshots: `...\scratchpad\snap.ps1 -View <view> [-Extra @('--now-playing')] -Out <png>` launches the debug exe on a view and captures the window; `snapclick.ps1` can also click/resize (read its header). Set `$env:MONOLIST_DATA_DIR` first — the scripts inherit it. Read the PNG with the Read tool to look at it.

## Lessons from earlier batches
- Downloads (F14, fixed): with MONOLIST_DATA_DIR set they go to `<MONOLIST_DATA_DIR>\downloads`; MONOLIST_DOWNLOAD_DIR names the folder outright and wins. Without either they go to the user's real Music\Monolist folder. A test may download only into one of those overrides (`--download` refuses without one), and still only with the user's say-so for a real download. Never delete or modify anything in the user's Music folder. `--download-cleanup-test` (needs MONOLIST_DATA_DIR) checks what a failed download may delete, on invented files.
- Self-test flags now include --lastfm-test, --cookie-test, --listen-test, --scrobble-test, --lastfm-connect-test, --ytm-session-test, --secret-test (each prints "N checks, 0 failed"), plus --rec-test/--artist-test/--graph-test/--content-test (baseline: 17 graph / 5 sounds-like / 0 none, 0 leaks, 22 countries, 0 of 19 content cases wrong, 0 of 46 explicit checks wrong). Rerun the ones covering what you touch.
- Settings is built lazily through a Loader in Main.qml; ServiceRow's state property is `serviceState` (Item already has `state`).
- Tooltips work only because build-windows.ps1 copies the QtQuick.Controls module after windeployqt; importing QtQuick.Controls or QtQuick.Dialogs in QML makes windeployqt ship every style (~17-23 MB) — avoid.
- snap.ps1/snapclick.ps1 pass -Extra arguments unquoted: a multi-word --query arrives as its first word.
- Secrets: never log, print or store in the settings table any key, session key or cookie value; secrets go through SecretStore only.

## Lessons from 2026-09-27
- The macOS work is MERGED into main now (src/macos/*, macos/, scripts/build-macos.sh, systempip*, toolstore, mediasession.mm). Keep every change portable; do not edit macOS-only files unless your task is about them; Windows-only code stays behind Q_OS_WIN with a compiling fallback.
- JioSaavn is merged and OPT-IN: Settings → Playback → Sound: Standard (default, JioSaavn never contacted) / High · 320 kbps (setting key jiosaavn.enabled = "1"). The Indian-headers switch (jiosaavn.india_headers) shows only under High and is on by default there. Tests that need JioSaavn must pass --set jiosaavn.enabled 1 and give the song's real title, artist and length (--play <id> <s> --as "<title>" "<artist>" <seconds>); without a length it refuses on purpose.
- mpv list options: never use "change-list <opt> set" for a path-list option with a URL (it splits on ':' on macOS/Linux); clear then "append".
- Do NOT build any in-app Google sign-in window or any code that captures cookies from a login web view: a safety check stopped that work and it is off the table. Signing in stays the existing cookie import (src/cookieimport.*, src/ytmsession.*).
- PowerShell: the tool's safety filter misreads `Remove-Item Env:X` and `Remove-Item $var` with odd paths; use `$env:X = $null` and `[IO.File]::Delete(...)`.
- The Parallels VM sometimes pauses itself; a long silent stretch followed by a clock jump is a pause, not a hang. Just carry on.
- The in-app recommendation-data download is live (github.com/droidboy08-hub/Monolist-data, tag v1).
- Commit author email must stay the GitHub no-reply address (repo config has it; never pass --author).

## Git
- `C:\dev\monolist-deps\git\cmd\git.exe -c safe.directory=* ...` (git is not on PATH).
- Commit your own work when it builds and is tested: stage only the files you changed (never `git add -A`; two untracked folders at the root must stay untracked). Write the message to a file and use `commit -F <file>` (PowerShell here-strings break on apostrophes). Subject: imperative, plain English. Body: what was wrong from the user's point of view, and what changed, briefly.
- Never push, amend, rebase, reset or rewrite history. The repo is configured with the right author email; do not pass --author or change git config.

## PowerShell 5.1 traps
- No `&&`; use `;` and `if ($?)`. Don't `2>&1` native tools unless piping to Out-String.
- Do not round-trip source files through Get-Content/Set-Content (mangles UTF-8). Use the Edit/Write tools. If you must script a rewrite, use `[IO.File]::ReadAllText/WriteAllText` with `New-Object Text.UTF8Encoding($false)`.

## Code conventions
- Match the surrounding code: Qt 6 idioms, naming, and comment density. Comments explain WHY in plain prose (this codebase's comments read like short essays on the reason; keep that voice, but be brief).
- QML singletons are registered in `src\main.cpp` with `qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, Name, &obj)`. Settings persist in the SQLite `settings` table through the existing helpers — find how `region` or `video height` is stored and do the same. Qt binds a null QString as SQL NULL, so wrap text bindings in `AppDatabase::text()`.
- Qt SQL connections are per-thread; the recommender worker runs on its own QThread.
- QML traps met here (silent at runtime): `Window.window` is null inside Drag/TapHandler; a property named `on` + Capital is taken for a signal handler; `Palette` resolves to QtQuick's type (ours is `CoverPalette`); an element id shadows a same-named property in nested scopes; `top` is FINAL on Item; replacing a ScrollBar's contentItem drops auto-hide (use `components/MonoScrollBar.qml`).

## Design rules (read `Monolist Player\DESIGN.md` before touching QML)
- Flat, zero radius, 2px rules, Archivo only, signal red accent. No scale/rotate/overshoot animations. Hover goes in instantly and fades out over `Theme.quick`. Icon states: grey (`Theme.neutral700`) at rest, ink (`Theme.text`) under the pointer, red (`Theme.accent`) = on; already-red glyphs brighten to `Theme.accent600` on hover. No hover plates behind icons.
- New controls reuse existing components (IconButton, MonoMenu/MonoMenuItem, ToggleRow, ActionButton, Toast…) rather than inventing new looks.

## Scope discipline
- Fix exactly the items you are given, completely. If you find another real problem, note it in your final report instead of fixing it.
- Your final text must report: what you changed (files), how you verified it (commands and what they printed), the commit hash, and anything you could not verify or left undone, stated plainly.
