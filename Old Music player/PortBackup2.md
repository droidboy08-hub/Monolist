# Project Port Backup Part 2: Advanced Logic & Desktop Integration

## 🛰️ Context Period
This document covers all project developments from the completion of the first **PortBackup.md** (UI Phase) until the successful creation of the **Portable Desktop Build**.

---

## ⚡ The Breakthrough: Fixing Download Restrictions

### 1. The Problem: "Stream Restricted"
- **Issue:** When trying to download audio for offline use, the browser consistently blocked the request with a **CORS error** (Stream Restricted). Even though we had the URL, the browser refused to let the app "touch" the raw bytes of the file for saving.
- **Why it failed:** Browser security prevents web apps from downloading large binary files from external domains (like YouTube's audio servers) without explicit server-side permission.

### 2. The Solution: Triple-Proxy Tunnelling
- **The Inspiration:** We looked at how `youtube_explode_dart` (Flutter) works—it extracts direct stream links. Since we are in a React/Capacitor environment, we mimicked this behavior using a **Binary Tunnel**.
- ✅ **The Fix:** We implemented a `downloadAndSaveSong` engine that:
  1. Rotates through **3 Stealth Gateways** to find the most stable raw `.m4a` link.
  2. Wraps the request in a **CORS Proxy Tunnel** (`api.codetabs.com`). This middle-man server fetches the bytes and serves them back to us from a "friendly" domain.
  3. Uses a **Streaming Reader** (`response.body.getReader()`) to track download progress in real-time (0% to 100%).
  4. Converts the final stream into a **Base64** string to be saved by the Capacitor Filesystem.

---

## 💾 Fixing Local Playback (Offline Mode)

### 1. The Problem: Memory Crashes & File Access
- **Issue:** Once the song was downloaded, clicking it in the Library would often **crash the app** or simply fail to play.
- **Why it failed:** 
  1. The browser cannot directly play a file from a mobile path like `/_data/song_id.m4a` for security reasons.
  2. Attempting to convert the entire file into memory at once caused a RAM overflow, freezing the device.

### 2. The Solution: Secure Blob URLs & Native Parsing
- ✅ **The Fix:** We refactored `playSong` for the **LOCAL** protocol:
  - We read the file from the filesystem as a Base64 string.
  - We used an **Optimized Native Parser** to convert the string into a `Uint8Array`.
  - We then wrapped that array in a **Blob** and created a **URL.createObjectURL(blob)**.
  - This "Blob URL" is a secure, temporary link that the browser's audio engine treats as a first-party file, allowing instant, memory-safe playback.

---

## 🖥️ Desktop Strategy (Portable .EXE)

### 1. Electron Integration
- We successfully integrated **Electron** to turn the React web app into a real Windows/Mac/Linux desktop application.
- **Main Script (`main.js`):** Configured to load the production `dist` folder and handle the window lifecycle.
- **Environment Logic:** Added a switch to detect if the app is in "Development" (loads localhost) or "Production" (loads local files).

### 2. The Portable Build
- ✅ **Success:** We generated an **Unpacked Desktop Build** located at `dist_electron\win-unpacked`.
- 🚧 **Challenge:** The single-file `.exe` compression failed due to Windows symbolic link restrictions (requires Admin rights). 
- **The Workaround:** The app is fully functional in the `win-unpacked` folder. Zipping this folder creates a "portable" distribution that can be sent to other users (like the developer's brother) and run via `Melody.exe`.

---

## ⚠️ CRITICAL SUMMARY FOR NEXT AI SESSION
- **Download Engine:** Uses the `api.codetabs.com` tunnel. If downloads stop working, the tunnel URL likely needs updating.
- **Local Playback:** Relies on `Filesystem.readFile` -> `Blob` -> `ObjectURL`. Do NOT revert to direct path playing; it will break browser security.
- **UI Architecture:** The player is currently a **Stable Fixed Card** at the top. The user requested this after the "Mini-to-Full" expansion logic caused instability. Keep this single-card design unless specifically asked to try expansion again.
- **Download Button:** It is now located in the **bottom control bar**, to the right of the Forward button, for ergonomic one-handed use.
