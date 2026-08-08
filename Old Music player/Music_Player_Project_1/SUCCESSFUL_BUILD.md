# SUCCESSFUL BUILD: PROJECT MELODY (Music Player Project 1)

This document serves as the comprehensive "Source of Truth" for the **Melody** music player project. It details the initial architecture, the evolution of technical solutions, and the final stable build state.

---

## 🏗️ Phase 0: The Original Vision & Strategy
### Initial Goals
- **Objective:** Create a 100% ad-free, high-fidelity music streaming experience using YouTube as the backend.
- **Aesthetic:** Modern, "Adaptive Rhythm" design system (Organic shapes, immersive visuals).
- **Platform:** Cross-platform (Android, iOS, Desktop) using a single codebase.

### Initial Technical Stack
- **Framework:** React + Vite + TypeScript.
- **Styling:** Vanilla CSS (for maximum flexibility and performance).
- **Native Bridge:** Capacitor (for Mobile) and Electron (for Desktop).

---

## 🛰️ Phase 1: Search & Discovery (The Extraction Engine)
### 🔍 Problem: API Restrictions
Standard YouTube libraries (`ytdl-core`, `yt-search`) failed in the browser environment due to Node.js dependency issues. Public API instances (Piped/Invidious) were too unstable.

### ✅ Solution: Direct Proxied Scraping
- **Method:** Use a CORS proxy (`api.codetabs.com`) to fetch raw YouTube HTML.
- **Implementation:** Manual Regex parsing of the `ytInitialData` JSON object.
- **Result:** Highly resilient search that doesn't require API keys or complex Node polyfills.

---

## 🔊 Phase 2: Playback Engine Evolution
### 🔄 Approach A: The "Ghost Player" (Hybrid Protocol)
- **Concept:** Embed an invisible YouTube IFrame (`youtube-nocookie.com`).
- **Logic:** Custom React UI controls the hidden iframe via the YouTube IFrame API.
- **Outcome:** Reliable but restricted to online-only and subject to iframe limitations.

### 🔄 Approach B: Self-Healing Hybrid Engine (Current)
- **Concept:** A dual-engine "race" protocol for maximum reliability.
- **Engines:**
    1. **Piped Engine:** Direct audio streams from YouTube CDN (High quality, no iframe).
    2. **Invidious Engine (Proxied):** Tunnels audio through `local=true` instances to bypass CORS/Region blocks.
- **Logic:** `Promise.any` races multiple global instances. The first one to respond wins.
- **Outcome:** **SUCCESS.** This became the primary engine, enabling true custom audio controls and offline playback.

---

## 💾 Phase 3: Offline Engine (The "Stream Restricted" Fix)
### 🚧 Problem: CORS & Binary Access
Browsers block direct "fetching" of song bytes from YouTube's servers for security reasons, making downloads impossible.

### ✅ Solution: Triple-Proxy Tunnelling
- **The Fix:** We implemented a `downloadAndSaveSong` service.
- **Logic:**
    1. Rotate through **Stealth Gateways** to find a stable `.m4a` link.
    2. Wrap the request in a **CORS Proxy Tunnel** (`api.codetabs.com`).
    3. Stream the bytes, convert to **Base64**, and save via **Capacitor Filesystem**.
- **Playback Fix:** Local songs are loaded as Base64 -> `Uint8Array` -> `Blob` -> `ObjectURL`. This bypasses mobile security restrictions that block direct file path playing.

---

## 🖥️ Phase 4: Desktop & Distribution (The Final Build)
### Electron Integration & Installer
- **Engine:** Custom Electron wrapper (`main.js`) targeting the production `dist` build.
- **Installer:** Switched from portable to **NSIS** to provide a standard Windows installation experience.
- **Output:** `Melody Setup 0.0.0.exe` with support for custom install directories and desktop shortcuts.

### ✅ Persistent Local Storage
- **Implementation:** Refactored `downloadService.ts` to bypass IndexedDB on desktop.
- **Storage Path:** Songs are now saved directly to the user's native Music directory: `%USERPROFILE%\Music\Melody`.
- **Logic:** Uses Electron IPC (`save-song`, `read-song`) to communicate with the Node.js `fs` module, ensuring downloads persist across app updates and uninstalls.

---

## 🛠️ Key Technical Modules (For Search/Reference)

| Module Name | Purpose | Key File |
| :--- | :--- | :--- |
| `SearchEngine` | Proxied HTML Scraping | `App.tsx` |
| `StreamResolver` | Hybrid Piped/Invidious Racing | `streamService.ts` |
| `DownloadEngine` | CORS-Tunnelling & Base64 Storage | `downloadService.ts` |
| `AudioCore` | HTML5 Audio + Blob URL Logic | `App.tsx` |
| `DesktopBridge` | Electron IPC & Entry Point | `main.js` |

---

## ⚠️ Notes for Future AI Sessions
1. **Never Revert Playback Logic:** The current `Blob -> ObjectURL` flow is critical for security/performance.
2. **Download Proxy:** If downloads fail, check `api.codetabs.com` status or update `PIPED_INSTANCES` in `streamService.ts`.
3. **UI Consistency:** Keep the "Fixed Top Card" player design unless a full overhaul is specifically requested.
4. **Environment Detection:** Always use `isElectron()` or `Capacitor.isNativePlatform()` to switch between desktop/mobile/web logic.
