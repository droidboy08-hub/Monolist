---
name: dev-environment
description: "Where the toolchain lives on this Windows ARM64 VM, and the quirks of building and testing here"
metadata: 
  node_type: memory
  type: project
  originSessionId: ef2a6719-0eac-48fc-a636-2c9ddb95cb0e
  modified: 2026-09-25T23:42:13.107Z
---

This VM is Windows 11 ARM64 (Parallels on Apple Silicon, 8 GB RAM). The project is on the Mac share (`\\psf\Home\Desktop\Multiplatform Music player`, also mapped as `Z:\Desktop\...`).

The toolchain is in `C:\dev\monolist-deps`:
- Qt 6.11.2 mingw_64 (`Qt\6.11.2\mingw_64`), MinGW 13.1, CMake 3.30.5 and Ninja (`Qt\Tools\...`), all x64 under emulation
- libmpv x86_64
- the unpacked yt-dlp in `yt-dlp\`
- native ARM64 FFmpeg, Deno and PortableGit

Build with `Monolist Player\scripts\build-windows.ps1`. It builds into `C:\dev\monolist-build\debug`, runs qmllint and fails on real QML errors, then deploys. The user runs `C:\dev\monolist-build\debug\monolist.exe` while I work; the script moves a running exe aside instead of failing the link, so a restart gets the newest build.

**Why:** Qt ships native ARM64 packages only for MSVC. cmd.exe cannot run Qt's generators from a UNC path, hence the Z: mapping.

**How to apply:**
- Run scripts with `powershell -ExecutionPolicy Bypass -File`.
- Run git as `C:\dev\monolist-deps\git\cmd\git.exe -c safe.directory=*`. Push with `push origin main`: SSH through PortableGit's ssh, key `C:\Users\droid\.ssh\id_ed25519` (a write deploy key on the Monolist repo only, so it cannot reach the user's other repos). Repo-local `user.email` is the GitHub no-reply address.
- Run the app with `QT_FORCE_STDERR_LOGGING=1` for logs, and `MONOLIST_MPV_LOG=warn` for mpv's own messages.
- Test against a scratch database with `MONOLIST_DATA_DIR=<scratchpad dir>`, never the user's real library.
- Screenshot with the scratchpad's `snap.ps1` (launch with `--view`, PrintWindow) and `snapclick.ps1` (also clicks, right-clicks, resizes; `-Screen` for popups).
- Qt binds a null QString as SQL NULL, so wrap text bindings in `AppDatabase::text()`.
- Test downloads with royalty-free tracks, e.g. `LrM_Y39Gmhk`.

QML traps met here, silent at runtime:
- `Window.window` is attached to Items only; inside a DragHandler/TapHandler it is null (read it on the Item, pass it in).
- A property named `on` + Capital (e.g. `onField`) is taken for a signal handler and reads as nothing.
- `Palette` resolves to QtQuick's type, not a singleton of that name (ours is `CoverPalette`).
- An element id shadows a same-named property in nested scopes (`side`).
- `top` is a FINAL Item property; custom properties cannot reuse it.
- Replacing a ScrollBar's `contentItem` drops its auto-hide; use `components/MonoScrollBar.qml`.

Related: [[monolist-project-goal]].
