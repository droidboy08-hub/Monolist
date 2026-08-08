# Project Port Backup & AI Context Guide

## 🎯 Initial Project Goals
- **Core Objective:** Build a 100% ad-free music streaming application that pulls audio directly from YouTube, inspired by open-source apps like `metrolist`, `ViMusic`, and `InnerTune`.
- **Platform Strategy:** 
  1. **Phase 1 (Current):** Android and iOS with an identical, unified UI.
  2. **Phase 2 (Future):** Windows and macOS desktop applications.
  3. **Phase 3 (Future):** Linux support.

## 🛠️ Technology Stack Evolution
- **Initial Attempt (Flutter):** We initially planned to use Flutter (Dart) for true cross-platform UI. However, the local environment lacked the Flutter SDK and setting it up automatedly was unfeasible.
- **Current Stack (React + Capacitor):** We pivoted to a Web-Technology stack using **Vite + React (TypeScript)**. This allows us to build the UI identically once. We will use **Capacitor** to package this web app into native Android/iOS binaries, and later Electron/Tauri for Desktop.

---

## 🚧 Backend & Streaming: Failures, Fixes & Tricks

### 1. YouTube Search Implementation
- ❌ **Failed Attempt 1 (Node Libraries):** Tried using `yt-search` and `ytdl-core`. This crashed the Vite browser environment because these libraries rely heavily on Node.js core modules (`stream`, `vm`, `timers`). Polyfilling them caused massive bundle bloat and execution errors.
- ❌ **Failed Attempt 2 (Public APIs):** Tried routing requests through public Piped (`pipedapi.kavin.rocks`) and Invidious (`iv.ggtyler.dev`) API instances. These failed consistently due to severe rate-limiting, CORS blocks, and regional throttling.
- ✅ **The Fix (Direct Proxied Scraping):** We implemented a highly reliable "Direct Extraction" method. We use a public CORS proxy (`api.codetabs.com`) to fetch the raw HTML of a `youtube.com/results` search. We then use Regex to extract the hidden JSON object (`var ytInitialData = {...}`) and parse it manually to get video IDs, titles, artists, and high-res thumbnails. This mimics a real browser request and bypasses most API rate limits.

### 2. Ad-Free Audio Playback
- ❌ **Failed Attempt 1 (Direct Stream Extraction):** Tried to extract direct `.m4a` or `.webm` URLs. YouTube actively blocked these requests based on regional restrictions and ad-blocking detection ("Stealth Protocol blocked").
- ✅ **The Fix (The "Ghost Player" / Hybrid Protocol):** Since we couldn't steal the raw audio file, we decided to hijack the official YouTube player. 
  - **The Trick:** We embedded an official YouTube iframe using `youtube-nocookie.com` to bypass tracking cookies and reduce initial ads. 
  - **Parameters used:** `?autoplay=1&controls=0&modestbranding=1&rel=0&iv_load_policy=3`.
  - **Hiding the Video:** We applied CSS to render the iframe completely invisible (`opacity: 0`, `position: absolute`, moved off-screen). This allows the official YouTube engine to handle the heavy lifting (bypassing region blocks and decrypting streams) while giving the user a pure "audio-only" visual experience.

---

## 🎨 UI/UX Design & Logic Fixes

### Design System: "Adaptive Rhythm"
We implemented a custom design system focusing on a "Modern-Organic" aesthetic:
- **Colors:** Deep charcoal backgrounds (`#050505`, `#111`) with green accents (`#1DB954`) and bright white text.
- **Geometry:** High-radius rounding (`24px` to `40px`) on cards and thumbnails for a soft, premium feel.
- **Layout:** Replaced the tiny mini-player with an immersive, sticky "Now Playing" card featuring large album art (`aspectRatio: '1/1'`) that subtly scales up when music is playing.

### UI Logic & Bug Fixes
- ❌ **Bug: Custom Controls Not Syncing:** Initially, custom play/pause and progress bars didn't work because the audio was trapped inside the hidden YouTube iframe.
- ✅ **Fix (YouTube IFrame API):** We dynamically injected the YouTube IFrame API script. Instead of standard HTML `<audio>` controls, our custom React buttons now send commands (`ytPlayerRef.current.playVideo()`, `pauseVideo()`, `seekTo()`) directly to the hidden "Ghost" player.
- ❌ **Bug: Jittery/Weird Audio when Dragging Progress Bar:** When the user dragged the seek bar, it fired hundreds of seek commands per second, causing the audio engine to glitch and stutter.
- ✅ **Fix (Debounced Seeking):** We separated the visual progress state from the actual audio seek command. 
  - While dragging (`onMouseDown`), we update the UI instantly but pause the interval tracker.
  - We implemented a `setTimeout` (400ms delay) that only executes the actual `seekTo()` command *after* the user stops moving the slider.
- ✅ **Feature: Queue & Repeat Management:** Implemented automatic "Play Next" functionality tied to the YouTube API's `onStateChange` (`YT.PlayerState.ENDED`). Added a Repeat cycle button (Normal -> Repeat All -> Repeat One) that intercepts the end-of-track event to either seek to `0` or load the next song in the search array.

---

## ⚠️ CRITICAL ARCHITECTURAL INSTRUCTIONS FOR FUTURE AI

If you are an AI agent picking up this project, adhere strictly to the following:

**1. Modular Core Logic (UI vs. Engine Separation)**
- **The developer may request a completely new UI design or full custom UI rebuilds.**
- **CRITICAL:** You must extract and preserve the core working logic of the player. The UI is interchangeable, but the engine is precious. 
- Ensure that the `handleSearch` (Direct Proxied Scraping), `playSong` (Stealth/Hybrid protocol fallback), `initHybridPlayer` (Ghost Player initialization), debounced seeking, and repeat queue management remain perfectly intact and decoupled from the specific React UI components.
- By keeping this logic modular, the engine can be easily ported to any new UI design system, framework, or platform (Mac, iOS, Android, Linux).

**2. Proceed to the Next Phase**
- Ensure the YouTube IFrame API connection logic for the "Ghost Player" remains unbroken during any UI overhauls.
- Proceed to **Phase 4: Offline Engine**. Figure out how to cache or download audio using Capacitor's native File System plugins (this will be challenging due to iframe reliance and may require a dedicated native plugin/backend).
- Prepare Capacitor configurations for native compilation across the target platforms.