# Step 1: Monolist's engine, stage by stage (in the BitChord paper's order)

All paths are under `Monolist Player/`. "Derived" means worked out from constants in the code, not measured; measuring is Step 2. Nothing was changed, built or run for this step. The only extra check was reading strings out of `libmpv-2.dll`.

## Overview (paper §2.3–2.4)

- **The queue holds identities, not links.** A `QueueTrack` holds a videoId plus title, artist, album and duration. `sourceUrl` is filled only for a local file or a direct URL (src/queuemodel.h:10-30). No googlevideo link is ever stored in the queue.
- **The source is chosen at play time**, in `PlaybackController::beginTrack` (src/playbackcontroller.cpp:927-1034), in this order:
  1. stop the old track (945-946);
  2. play a downloaded file if there is one (1001-1011);
  3. otherwise resolve the videoId (1013-1018);
  4. otherwise play the plain URL on the row (1021-1028).
- **Resolution finishes before mpv sees anything.** `handleResolved` passes the link to `MpvEngine::load`, which sends `loadfile <url> replace` (playbackcontroller.cpp:1036-1059; src/mpvengine.cpp:343-404).
- **libmpv does everything after that:** fetching, buffering, demuxing, decoding and output. The DLL is `mpv v0.41.0-1050-ge76a35ec9` with FFmpeg Lavf 63.7.100, at C:\dev\monolist-deps\libmpv\libmpv-2.dll.
- **The only identity-keyed cache is the link cache** (videoId → URL) inside `StreamResolver`. There is no byte cache. The only audio kept on disk is what the user downloaded.

**A cold play in one line:** click → `beginTrack` → stop the old track → `resolve(videoId)` → cache hit? answer at once : one InnerTube `/player` POST as the visionOS app → pick the best audio URL → cache the link → mpv `loadfile` → one HTTP request to googlevideo → FFmpeg decode → WASAPI.

## §3 Sources

**We have one catalogue: YouTube / YouTube Music.** The `Tier` enum lists five ways to get a link for the same video. They are not five catalogues (src/streamresolver.h:44-57).

| Order | Tier | What it does | Files |
|---|---|---|---|
| 0 | InnerTube | One in-process POST to `www.youtube.com/youtubei/v1/player`, sent as the visionOS app (VISIONOS 1.02). Its links are plain: no signature cipher to undo, no `n` throttling parameter, no PoToken. Formats that offer only a cipher are skipped. | src/innertube.cpp:134-147 (identity), 759-788 (request), 947-1038 (pick) |
| 1 | yt-dlp | A subprocess running `-f bestaudio/best --dump-single-json`, using Deno for YouTube's JS challenges. It is killed at 12 s, with its whole process tree. A code comment says it takes about 3.5 s, against "well under a second" for InnerTube. | src/ytdlp.cpp:432-459, 489-500; src/streamresolver.cpp:19-23, 345-386; src/streamresolver.h:30-33 |
| 2 | Muxed | yt-dlp as the `android,tv_simply` clients, format `18/b`: AAC at about 96 kbps plus a 360p picture that mpv discards. It keeps that client's headers so mpv fetches as the same client. | src/ytdlp.cpp:502-519; src/streamresolver.cpp:392-440 |
| 3–4 | Piped / Invidious | Races public instances. Both lists ship empty, so both tiers are skipped instantly. They can be revived through the `piped_instances` / `invidious_instances` settings rows. | src/streamresolver.cpp:38-72, 445-557; src/main.cpp:218-219 |

**Client identity.**
- Our strings are exactly InnerTubeX's `VISIONOS` 1.02 entry. InnerTubeX marks that entry PROBE_ONLY, with a note that it "can stall on clean sessions" (reference/sources/youtube.js:66-78, 174-189). BitChord leads with the AUTOMATIC `VISIONOS_0_1` instead.
- We post to www.youtube.com (innertube.cpp:762) and put `userAgent` in the context (146). InnerTubeX uses music.youtube.com and leaves the userAgent out.
- Only one no-cipher client is tried before the ~3.5 s yt-dlp step.

**visitorData** (the anonymous visitor ID YouTube expects on `/player`).
- It is scraped from the full www.youtube.com home page at startup (innertube.cpp:694-746). BitChord reads the much smaller `sw.js_data`.
- It is sent both in the context and as `X-Goog-Visitor-Id` (773-787).
- Six InnerTube objects are built at startup: src/main.cpp:215, 227, 240, 243, 250, 298, through streamresolver.h:167, playbackcontroller.h:286, mediaextractor.h:144, lyrics.h:110, catalog.h:174 and recommender.h:270. Each one fetches the home page and pre-opens two TLS connections, but only the resolver's visitorData is ever used for `/player`.

**Format choice.**
- InnerTube takes the audio-only format with a plain URL and the highest `bitrate` (innertube.cpp:979-1003). It does not prefer Opus, does not filter out DRC variants (dynamic-range-compressed copies), and does not read `loudnessDb`.
- If there is no audio-only format, it takes the highest progressive itag, normally 18 (1004-1029).
- yt-dlp's `bestaudio` normally picks Opus 251.
- There is no streaming quality setting.

**Missing from §3.3–3.6:** JioSaavn, addon (HTTP/JSON) sources, JS module sources, and a `TrackMatcher` for finding a lossless copy of a YouTube track. The recommender has a small catalogue→YouTube matcher (`pickResult`, src/recommender.cpp:94-133), but playback does not use it.

## §4 Resolution and orchestration

**Where it runs (§4.1).**
- Resolution is asynchronous on the Qt main event loop, driven by QNetworkReply and QProcess signals, and it completes before `loadfile`.
- mpv never resolves anything (`ytdl=no`, mpvengine.cpp:133-136).
- Late callbacks from an abandoned attempt are ignored through a per-job `generation` counter (streamresolver.h:116-129; streamresolver.cpp:586-603).

**Decision ladder (§4.2). The first rule that applies wins.**
1. A downloaded file exists, or `sourceUrl` is a local file → play the file (playbackcontroller.cpp:1001-1011, 1107-1117; src/downloadmanager.cpp:525-530).
2. A cached link has not expired → answer at once, delivered through the event queue (streamresolver.cpp:106-116).
3. A background prefetch for this id is already running → take it over (117-121).
4. Otherwise walk InnerTube → yt-dlp → Muxed → Piped → Invidious, one after another (124-127, 279-303, 577-584).
5. There is no id but the row has a plain URL → load it (playbackcontroller.cpp:1021-1028).

**Timeouts.**
- **`/player`:** Qt's 8 s transfer timeout, which fires after 8 s with no bytes. There is one retry after 1.2 s (innertube.cpp:61-63, 919-933; `retries = 1` default, src/innertube.h:407).
- **visitorData:** if it is still loading, `/player` waits for it first (748-757), under the same 8 s timeout (720).
- **yt-dlp and muxed:** a 12 s hard cap each.
- **Overall:** there is no deadline across tiers. The worst case is about 49 s before "Couldn't play … — skipping" (8 + 8 + 1.2 + 8 + 12 + 12; derived). It can be longer if bytes trickle in slowly, because Qt's timeouts measure silence, not total time.

**Racing (§4.3–4.4).** There is no race between catalogues, since there is only one. Only the Piped and Invidious tiers race their hosts, and both host lists are empty.

**Caches (§4.5).**

| Cache | Key → value | Lifetime and size | Files |
|---|---|---|---|
| Link cache | videoId → URL, tier, expiry, headers | The URL's `expire=` minus 10 min, capped at 5 h (30 min if there is no `expire`). No size limit. Never evicted, only invalidated per id. Written before a single byte is fetched (no probe). | streamresolver.h:131-139, 169; streamresolver.cpp:267-277, 559-575 |
| Video links | videoId → video/audio URLs + headers | Same rule | streamresolver.cpp:176-237 |
| visitorData | One per InnerTube object | For the session | innertube.cpp:710-746 |
| Refused tiers | Tiers mpv refused for this track | Until the track changes | playbackcontroller.h:293-295; playbackcontroller.cpp:938 |
| yt-dlp player JS | yt-dlp's own disk cache | yt-dlp's rules | We never pass `--no-cache-dir` (src/ytdlp.cpp:432-458) |
| "Unplayable" cache, StreamChoice-style pin | **none** | — | — |

**Headers.**
- Headers are cached only for the muxed tier (streamresolver.cpp:431).
- The InnerTube tier (341) and the yt-dlp audio tier (373-377) cache none. So mpv fetches their links with its default Chrome/Windows User-Agent (mpvengine.cpp:11-13, 351-353), although InnerTube minted the link with a Safari/visionOS User-Agent (innertube.cpp:57-59, 767, 783).

**One link per track, whichever tier made it.** The link cache is keyed by videoId alone. When the muxed itag-18 link rescues a track, it replaces the Opus link and is reused on replays until it expires.

**Next-track link warm-up (the URL half of §5.5).**
- `prefetchUpcoming` (playbackcontroller.cpp:758-770) takes the next row, or row 0 under repeat-all. It skips downloaded tracks and calls `StreamResolver::prefetch` (streamresolver.cpp:245-259).
- That runs a silent job over the full ladder, including a yt-dlp process if InnerTube fails.
- It is triggered:
  - after each resolve (1058);
  - after a local file starts (1008);
  - after queue edits (281, 688, 710, 716, 728, 744);
  - on a shuffle toggle (1329).
- It covers one track, fetches the link only, and starts with no delay.

**Duplicate requests (§11 row 8).**
- A foreground resolve takes over a running prefetch.
- But a second `resolve` or `resolveVia` for the same id cancels the running job (`start` → `cancel`, streamresolver.cpp:139-141).
- Skipping away cancels the pending job (playbackcontroller.cpp:934-936).
- When a job is cancelled, the InnerTube answer that arrives afterwards is thrown away, not cached (streamresolver.cpp:315-317).

**Quality upgrade (§4.6).** None. The building block exists: mpv's `start=` can resume at a position (mpvengine.cpp:386-389).

**Error recovery (§4.7).**
1. **A download has gone missing:** the track streams instead (downloadmanager.cpp:527-529).
2. **mpv refuses the link.** The trigger is a loadfile error or END_FILE with an error, which raises `loadFailed` (mpvengine.cpp:296-298, 318-319). The handler (playbackcontroller.cpp:133-187):
   - drops a video that never showed a frame (`abandonVideo`, 1145-1160);
   - if the refused link came from the cache, fetches a fresh one from the same tier;
   - otherwise marks the tier refused and walks the `afterRefusal` order. After an InnerTube refusal that order is Muxed → yt-dlp → Piped → Invidious (streamresolver.cpp:157-166);
   - invalidates the link and resumes at the same second.
3. **Every tier fails:** a toast, then the next track. It stops after 3 failures in a row (`kMaxConsecutiveFailures`, playbackcontroller.cpp:24-27, 1061-1105).
4. **Transport retries:**
   - `/player` retries once, 1.2 s later;
   - a country refused with HTTP 400 is dropped and the request is sent again (innertube.cpp:903-918).
5. **A self-test for this path:** `--play … --spoil` (src/main.cpp:405-410).

**Not present:**
- A permanent verdict for age-gated, private or blocked tracks. LOGIN_REQUIRED, UNPLAYABLE and AGE_VERIFICATION all simply fall through to yt-dlp (innertube.cpp:969-977).
- Any check that an "end of file" arrived near the real end of the song (see §5).

## §5 Transport, cache and buffering

**Our equivalent of the DataSource chain (§5.1).**
- We have no fetching layer of our own: libmpv fetches the googlevideo bytes.
- All we control is mpv options. They are set in `MpvEngine::applyBaseOptions` (mpvengine.cpp:122-166), plus four per-load settings: `user-agent`, `http-header-fields`, `audio-files` and `start` (343-404). Nothing else in the repo sets an mpv option.

**Which HTTP code fetches the bytes.** This libmpv has two possible fetchers:
- mpv's own libcurl stream (the DLL's build line has `-Dlibcurl=enabled`);
- FFmpeg's `http` protocol, which mpv configures with `reconnect=1` and `reconnect_delay_max=7`.

Which one handles https here must be confirmed from the mpv log (`MONOLIST_MPV_LOG=v`, mpvengine.cpp:97-102).

**Range chunking (§5.2): not done.**
- Nothing sets `stream-lavf-o request_size` or `curl-max-request-size`, so each track is fetched as one open-ended request (`Range: bytes=0-`). This is the "unbounded GET" the paper measured at about 15 kB/s on googlevideo.
- mpv's own `ytdl_hook.lua`, embedded in our DLL, would copy yt-dlp's `http_chunk_size` (10 MiB) into both options. We switch that hook off (`ytdl=no`, mpvengine.cpp:136) and never replaced that part of it.

**HTTP clients (§5.3).**
- **Media:** one mpv connection per track, opened fresh after each `stop()`. `network-timeout` stays at mpv's default of 60 s.
- **Resolve side:** each InnerTube object has its own QNetworkAccessManager, with TLS pre-opened to music.youtube.com and www.youtube.com at startup (innertube.cpp:700-704). `StreamResolver::m_network` serves only Piped and Invidious (streamresolver.cpp:25-34, 74-80).
- **Two separate stacks:** the link is minted through Qt and fetched through mpv, so they do not share DNS, connections or the IPv4/IPv6 choice (§3.2.6, §11 row 10).

**Disk cache (§5.4): none.**
- mpv's buffer is in RAM and is dropped at every track change: `stop()` and then `loadfile replace` (playbackcontroller.cpp:945-946; mpvengine.cpp:397).
- Replay, Previous and repeat-one all download the track again.
- The only identity-keyed audio on disk is user downloads, and those do play before the network.
- The desktop equivalent of BitChord's cache in front of the resolver would be a custom mpv stream protocol (`stream_cb.h` ships in C:\dev\monolist-deps\libmpv\include\mpv\) or a local HTTP proxy. We have neither.

**Read-ahead (§5.5).** Link only, one track (see §4); no bytes. mpv's `prefetch-playlist` cannot help, because mpv's playlist never holds more than the current file.

**Buffering policy.** Media3's `LoadControl` corresponds to mpv's cache options:

| BitChord | Monolist (mpv) |
|---|---|
| Starts once 500 ms is buffered | `cache-pause-initial` is at its default, `no`: there is no explicit threshold, and playback starts once the decoder has data |
| Waits for 2 s after a stall | `cache-pause-wait` default, 1 s |
| Reads 15 min / 8 MiB ahead | `cache=yes`, `demuxer-max-bytes=64MiB` (mpvengine.cpp:150-151). The byte limit is the real bound, and a whole YouTube track fits. `demuxer-readahead-secs=20` (152) has no effect with the cache on, because mpv's far larger `cache-secs` default wins. |
| 30 s back buffer | mpv default `demuxer-max-back-bytes` |

**Stall signal.** mpv's `paused-for-cache` is observed (mpvengine.cpp:174, 240-247) and reaches `PlaybackController::buffering` (playbackcontroller.cpp:116-122). Only the Last.fm listen tracker uses it; nothing in the UI shows a stall.

**Failures mid-song.**
- Inside mpv, FFmpeg reconnects (with delays of up to 7 s) or curl retries, under the 60 s network timeout.
- An error end raises `loadFailed`, which re-resolves and resumes (§4.7).
- A stream that dies but is reported as a normal end raises `endOfFile` and goes straight to `next()`, with no check of position against duration (mpvengine.cpp:320-321; playbackcontroller.cpp:1162-1177). The song is cut short silently. This is the same kind of bug as BitChord's silent end after `MAX_EMPTY_RANGES`.

**Preventing sleep** (the desktop equivalent of Android's wake lock). There is no power request anywhere in src/: no SetThreadExecutionState and no IOPMAssertion. Only the OS's own hold while audio is actually rendering applies. It does not cover resolving, stalls or downloads.

## §6 Decoding

**The codec is decided by the resolver tier, not by mpv.**
- **InnerTube:** picks by peak bitrate. Usually that is Opus 251 at 48 kHz; AAC 140 at 44.1 kHz wins whenever its bitrate figure is higher.
- **yt-dlp:** `bestaudio`, usually Opus.
- **Muxed:** AAC in itag 18.
- **Piped:** prefers mp4 (streamresolver.cpp:480-483).
- **Invidious:** always itag 140 (518).

The muxed tier is the first rescue after mpv refuses an InnerTube link (streamresolver.cpp:160). So a refused Opus stream carries on as AAC at about 96 kbps.

**How decoding happens.**
- mpv demuxes with FFmpeg (lavf) and decodes with FFmpeg's software decoders, which are built into libmpv. The output is float.
- Nothing overrides `ad`, `audio-format`, `audio-samplerate` or `audio-channels`.
- Video is not decoded (`vid=no`, mpvengine.cpp:126) unless the video view turns it on (`hwdec=auto-copy-safe`, 433).

**Compared with the paper.**
- Our equivalent of "platform MediaCodec" (§6.1) is the same pinned FFmpeg on every machine, with no OS or vendor codecs, so vendor-specific decoder bugs cannot occur.
- We have no lossless sources, so the float-versus-16-bit question for hi-res audio (§6.3) never comes up, and our chain is float anyway.
- We have no container recovery like BitChord's (§6.2). An open failure is handled by re-resolving (§4.7).

## §7 PCM, DSP and output

- **Filters:** no `af` is set. mpv inserts a resampler (libswresample) only when the decoded rate or format differs from what the output accepted, for example AAC at 44.1 kHz on a 48 kHz Windows mix.
- **Volume.**
  - The path is: slider 0–1 (components/NowPlayingBar.qml:424, 503) → `setVolume` (playbackcontroller.cpp:1306-1316) → mpv `volume` 0–100 (mpvengine.cpp:465-471). mpv applies it in float on a cubic curve.
  - The default is 0.65 (playbackcontroller.h:338), about −11 dB, saved as `player.volume`.
  - The Windows per-app volume is a separate stage that we never touch.
- **Loudness (§7.3): effectively none.** `replaygain=track` is set (mpvengine.cpp:154-155), but nothing feeds it:
  - YouTube streams and our downloads carry no ReplayGain or R128 tags;
  - the `loudnessDb` value from `/player` is ignored;
  - `setReplayGainEnabled` (481-484) has no caller.
- **Output (§7.4).** Android's AudioTrack/AudioFlinger corresponds to mpv's audio output:
  - It uses mpv's default output: WASAPI on Windows, CoreAudio on macOS.
  - It runs in **shared mode**, because `audio-exclusive` is not set. The Windows mixer then applies the session volume (named "Monolist" via `audio-client-name`, mpvengine.cpp:157) and any sound enhancements (APOs), and converts to the device format.
  - There is no exclusive path (WASAPI exclusive, or CoreAudio hog mode on macOS).
  - libmpv ignores mpv.conf, so a user cannot switch exclusive mode on either.
- **Output device picker** (BitChord leaves this to Android).
  - mpv's `audio-device-list` is observed and parsed (mpvengine.cpp:31-57, 184, 273-282), and the menu is built from it (playbackcontroller.cpp:387-449).
  - The choice is saved (372-385) and switched mid-song without waiting (mpvengine.cpp:489-496).
  - There is a self-test: `--audio-devices` (src/main.cpp:2456-2556).
- **Speed and silence skipping (§7.5):** `MpvEngine::setSpeed` exists (473-479) but nothing calls it. There is no silence skipping.
- **Visibility:** mpv's log is off (`msg-level=all=no`, mpvengine.cpp:165) unless `MONOLIST_MPV_LOG` is set. Nothing reads `audio-params`, `audio-out-params` or `current-ao`, so the format actually delivered is not visible anywhere.

## §8 Gapless, crossfade, Automix

- **Every track boundary is a hard stop and reload:**
  1. mpv reports END_FILE (EOF), and the engine emits `endOfFile`.
  2. `handleEndOfFile` calls `next()`, which reaches `beginTrack`.
  3. `beginTrack` calls `stop()`.
  4. The resolver answers, usually from the link cache.
  5. mpv runs `loadfile replace`.

  See mpvengine.cpp:311-325, 397 and playbackcontroller.cpp:945-946, 1162-1177. Local files skip the resolve step.
- **What makes up the gap:** draining the old track, stopping, a new HTTP/TLS connection, the container probe, decoder and output setup, and the first packet. Downloaded albums have gaps too.
- **mpv's own gapless mode cannot engage.** Its `gapless-audio` (default `weak`) and `prefetch-playlist` both need more than one file in mpv's playlist, and ours only ever holds one.
- **Not present:** crossfade, a second player, Automix analysis and the version-swap aligner. Switching between video and sound reloads at the same second, which is a hard cut (playbackcontroller.cpp:1123-1141).
- ROADMAP.md:305 lists gapless, crossfade, EQ and speed as not yet assessed.

## §9 Downloads

- **Trigger.** The Download and Download all buttons (components/DownloadButton.qml:121, components/TrackMenu.qml:145, views/PlaylistView.qml:84, views/PageView.qml:84) call `DownloadManager::enqueue` / `enqueueAll` / `queueOne` (src/downloadmanager.cpp:191-243).
  - There is no metered-connection check, which is the desktop form of BitChord's Wi-Fi-only gate.
- **Queue.**
  - It lives in memory and runs up to 3 downloads at once (`kMaxConcurrent`, downloadmanager.h:164; `pump`, downloadmanager.cpp:245-257).
  - It is not saved. On quit, running jobs are cancelled and their partial files deleted (74-86), and queued items are lost.
- **Extraction.**
  - Each item is one yt-dlp process (src/ytdlp.cpp:544-621). That process does its own extraction, transfer and FFmpeg post-processing.
  - It never asks `StreamResolver`, so a link already in the cache is not reused (downloadmanager.cpp:283).
  - No cookies are passed, so Premium 256 kbps AAC is out of reach.
  - There is no overall deadline, only `--socket-timeout 15`, `--retries 2` and `--extractor-retries 1` (ytdlp.cpp:442-447).
- **Format.**
  - The default, "original", is `bestaudio` remuxed with no re-encode (`-x --audio-format best`, 578-584).
  - m4a (AAC 140) and mp3 (re-encoded) are the other options (585-596).
- **Transfer.**
  - yt-dlp's own downloader does the transfer, with its default YouTube chunking (believed to be 10 MiB ranges; to confirm with `-v`).
  - A 403 mid-download is not re-resolved: the job fails and the `.part` file is deleted (downloadmanager.cpp:397-420, 598-606, 645-670).
  - A manual retry restarts from byte 0 (444-457).
- **Tags.**
  - Written: `--embed-metadata` (with "- Topic" and "VEVO" stripped from the artist), a square-cropped cover, and an optional SponsorBlock "music_offtopic" cut, on by default (downloadmanager.h:175; ytdlp.cpp:599-618).
  - Not written: lyrics, ReplayGain, ISRC.
  - Small bug: yt-dlp's album is printed but ignored, so the tracks row is written with album '' (downloadmanager.cpp:358-373, 731).
- **Storage.** The folder is chosen in this order: `MONOLIST_DOWNLOAD_DIR`, then `MONOLIST_DATA_DIR/downloads`, then `<Music>/Monolist` (downloadmanager.cpp:608-629). Files are ordinary .opus, .m4a and .mp3.
- **Offline playback.**
  - The local file comes first, after an exists() check. If the file has vanished, the track streams instead.
  - Next-track prefetch is skipped for downloaded tracks (playbackcontroller.cpp:767-768, 1001-1011; downloadmanager.cpp:525-530).
- **Cancel** kills the whole yt-dlp/FFmpeg/Deno process tree (ytdlp.cpp:87-111, 144-177).

## §10 Lyrics

- **When a lookup runs:**
  - only while the lyrics pane is on screen (`Lyrics.active` is bound at Main.qml:574-579; src/lyrics.cpp:134-163);
  - the same video id never triggers a second lookup;
  - with the pane closed there is no lookup at track start, and nothing is fetched ahead for the next track.
- **Result cache** (BitChord has none).
  - An SQLite table `lyrics(video_id, synced, plain, source, fetched_at)` (src/appdatabase.cpp:193) is read first (lyrics.cpp:215-233).
  - A "none found" answer stands for 3 days (28-29). Lyrics that were found are kept forever and never refreshed.
- **Query cleaning** (`searchTitle` / `leadArtist`, lyrics.cpp:38-69):
  - strips an "Artist - " prefix;
  - strips packaging words, including "remastered";
  - strips feat/with credits;
  - takes the lead artist as the text before `,`, `&` or `feat`.
- **Providers, strictly one after another:**
  1. **LRCLIB** `/api/search?track_name&artist_name`, with a 10 s no-bytes timeout (lyrics.cpp:235-256).
     - The match is chosen by duration against the queue row: synced lyrics within 3 s first, then plain lyrics within 3 s, then the `instrumental` flag. An entry 3–20 s away is kept as a plain-text fallback (258-323).
     - Title and artist are not checked, and `/api/get` is never used.
     - If the search returns zero rows, one looser `q=` search runs.
  2. **YouTube Music's lyrics tab** (innertube.cpp:1244-1287):
     - a `next` call (12 s, one retry);
     - then the tab whose browseId starts `MPLYt…`;
     - then a `browse` call (20 s, one retry);
     - the result is plain text plus a "Source: Musixmatch" credit.

     These calls cannot be cancelled. A stale answer is dropped by comparing video ids only (lyrics.cpp:329).
  3. **Order of preference:** LRCLIB synced → LRCLIB plain → instrumental → YouTube Music plain → the loose LRCLIB plain fallback → "error" if both providers failed on the network (nothing is stored, and TRY AGAIN is offered) → otherwise "none", which is stored (lyrics.cpp:299-347).
- **Duration** comes only from the queue row (lyrics.cpp:274). When it is 0, the duration check is off entirely (279).
- **Time to "no lyrics":** a typical miss is 3–4 round trips in a row. The worst case is about 76 s when the first LRCLIB call times out, or about 86 s in an edge case (derived).
- **Formats.**
  - The LRC reader handles several stamps per line, `[offset:]`, 1–3 digit fractions and 3-digit minutes. Word-level stamps are removed (lyrics.cpp:423-463).
  - A line is just {timeMs, text} (lyrics.h:20-23).
  - There is no TTML, no word-by-word model and no translation.
- **Display.**
  - The current line is the playback position plus a 150 ms lead (lyrics.cpp:23, 394-410).
  - Clicking a line seeks to it (412-421).
  - The pane follows the song at 36 % of its height. Manual scrolling pauses that for 5 s, and a SYNC button returns to it (components/LyricsPane.qml:20-68, 192-215).
  - The source credit and "NOT TIME-SYNCED" are shown in views/NowPlayingView.qml:411-421.
  - There is no user sync offset.
- **Outside the pane (§10.9):** nothing.
  - There is no OS media session: no SMTC, MPNowPlayingInfoCenter or MPRIS code.
  - libmpv's built-in `media-controls` option is left at its default, which is off under libmpv.
- **Tests:** none for `parseLrc` or `searchTitle`. There is only the network self-test `--lyrics` (src/main.cpp:1957-2014).