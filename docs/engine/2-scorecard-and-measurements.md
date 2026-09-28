# Scorecard

Status per the verified rows, which override the Step 1 draft. Paths are under `Monolist Player/`; `bench\` is the scratchpad bench folder. The measured figures come from the Step 2 runs on 2026-09-26.

| # | Capability | BitChord | Monolist | Evidence (file:line) and note |
|---|---|---|---|---|
| C1 | Queue items are identities, resolved at open time | ✅ | ✅ | src/queuemodel.h:10-30 (videoId, no stream URL). The id is resolved at play time: src/playbackcontroller.cpp:999-1018, 1036-1059. A refused link is re-resolved and resumed at the same second (133-178). mpv never holds more than the current file (src/mpvengine.cpp:397). |
| C2 | Disk cache keyed by identity + rendition, above the resolver | ✅ | ❌ | mpv's cache is RAM only (src/mpvengine.cpp:148-152) and is dropped at every track change (src/playbackcontroller.cpp:945-946; mpvengine.cpp:397). The only disk audio checked first is a user download (src/downloadmanager.cpp:525-530). Measured: 178 plays opened the URL 178 times (bench\logs). |
| C3 | Per-rendition cache keys | ✅ | ❌ | No byte cache. The link cache has one slot per videoId (src/streamresolver.cpp:568), so a 403 rescue's itag-18 link (~96 kbps AAC) replaces the Opus link. That lower link is then replayed until it expires, which can be up to 5 h (267-277). This was seen after 2 of 178 opens. |
| C4 | Decision pinning (retries and prefetch fill the same file) | ✅ | ⚠️ | Link level only: prefetch and playback share m_cache (src/streamresolver.cpp:106-121, 245-259, 559-575), and seeks stay inside one mpv load. There is no StreamChoice-style pin, and with no byte cache there is nothing to pin. |
| C5 | Range-chunked googlevideo reads (≤ 1 MiB) | ✅ | ❌ | No curl-max-request-size or request_size is set (src/mpvengine.cpp:122-166), and ytdl=no (136) switches off the hook that would set them. Each track is fetched with one `Range: bytes=0-` GET. Measured: that is fast for files ≤ ~8 MiB (6.0 MB/s median in-app) but paced to ~36 kB/s for the 14.6 MB track. 1 MiB ranges ran at 4.3-6.8 MB/s (curl). |
| C6 | Range chunking for other CDNs | ❌ (D5) | ❌ | Invidious, Piped and direct URLs go through the same unbounded fetch (src/streamresolver.cpp:459-550; src/playbackcontroller.cpp:1021-1028). No effect today: both host lists ship empty (streamresolver.cpp:64-72). |
| C7 | Unciphered, token-free client tried first | ✅ (visionOS 0.1) | ✅ | The InnerTube tier comes first (src/streamresolver.h:44-57), as VISIONOS 1.02 (src/innertube.cpp:134-147), taking plain URLs only (979-1003). Measured: 175 of 178 opens came from it, no URL carried n= or pot=, and the tier's resolve median is 199 ms. |
| C8 | Parallel or hedged client requests | ❌ (D3) | ❌ | Tiers run strictly one after another (src/streamresolver.cpp:139-149, 279-303, 577-584). /player gets one retry after 1.2 s (src/innertube.cpp:919-933). The only race is between Piped/Invidious hosts, and it is disabled (445-557, 64-72). |
| C9 | Signature/n solver with a persistent cache | ✅ | ⚠️ | The solver exists only in the yt-dlp tiers, via Deno (src/ytdlp.cpp:449-451). yt-dlp keeps only youtube-sigfuncs/*.json on disk and downloads the player JS again in every process (bench\skeptic_c1_24). The primary tier never needs a solver (0 of 178 URLs had n=). |
| C10 | PoToken generation | ✅ WebView | ❌ | There is no BotGuard/PoToken code, and yt-dlp reports "PO Token Providers: none". The primary path does not need one (no pot= in 178 URLs). |
| C11 | Probe before trusting a URL | ✅ (D6) | ❌ | Links are cached untested (src/streamresolver.cpp:559-575), and recovery happens only after mpv fails (src/playbackcontroller.cpp:133-178). Measured: 2 of 178 opens got a 403 on the first request and were rescued in 3.4-5.4 s. With curl, 2 of 8 fresh long-track URLs served one request and then refused every later one. |
| C12 | URL cache (~20 min) + negative cache + single-flight | ✅ | ⚠️ | The link cache works: a hit resolves in ~45 ms (n=46) against 203.5 ms for a new track. But its TTL is expire minus 10 min, capped at 5 h, and it is unbounded and unprobed (src/streamresolver.cpp:267-277). There is no negative cache (296-300; src/innertube.cpp:969-977). A duplicate foreground resolve cancels and restarts the running one (139-141), and the cancelled job's answer is thrown away (315-317). |
| C13 | Lossless lookup raced against a fast fallback | ✅ | ❌ | YouTube is the only source: the five tiers all reach the same catalogue (src/streamresolver.h:44-57). |
| C14 | Mid-song upgrade with pre-buffered audition | ✅ | ❌ | Absent. The only building block is resume-at-position (src/playbackcontroller.cpp:168, 1055-1057; src/mpvengine.cpp:386-389). |
| C15 | Cross-catalogue matching (duration, artist, version) | ✅ (no ISRC) | ⚠️ | Two partial, one-way matchers, and neither picks a playback source: src/recommender.cpp:94-133, 694; src/rec/matchkey.cpp:212-216, 495. |
| C16 | ISRC-based matching | ❌ | ❌ | 'isrc' does not occur anywhere in src/. |
| C17 | Next-track full prefetch to disk | ⚠️ (D1) | ❌ | prefetchUpcoming fetches a link only (src/playbackcontroller.cpp:758-770; src/streamresolver.cpp:245-259). 'loadfile replace' keeps mpv's playlist at one file (src/mpvengine.cpp:397). |
| C18 | URL warm-up for the next 1-2 tracks | ✅ | ✅ | The next row (or row 0 under repeat-all) is prefetched after every resolve and every queue edit (src/playbackcontroller.cpp:755-770, 1058). Measured: 22 of 22 skips hit the link cache; skip median 412.5 ms against 553.5 ms for a new track. |
| C19 | Low start threshold (≤ 500 ms) | ✅ | ✅ | mpv's defaults apply (cache-pause-initial=no, cache-pause-wait 1 s); nothing is set in src/mpvengine.cpp:148-152. Measured proxy: from the first response bytes to audio start, median 49 ms, p90 209, max 481 (n=176). |
| C20 | Bitrate-aware buffer byte target | ❌ (D4) | ❌ | A constant demuxer-max-bytes=64MiB (src/mpvengine.cpp:151). The readahead-secs=20 setting (152) is overridden by mpv's cache-secs default. Harmless today: 64 MiB is ~55 min of Opus, and there were 0 stalls in 2,319.7 audible seconds. |
| C21 | Non-blocking resolution (not on the loader thread) | ❌ (D2) | ✅ | Resolution runs async on the Qt event loop (src/streamresolver.cpp:308-440; src/innertube.cpp:947-1038). mpv only receives a finished URL, sent with mpv_command_async (src/mpvengine.cpp:397-398; ytdl=no at 136). Caveats: there is no overall deadline (~49 s worst case, derived), and cancelling yt-dlp can block the UI thread for up to ~3 s (src/ytdlp.cpp:87-111). |
| C22 | Float decode for hi-res on capable routes | ✅ (opt-in, Q1) | ✅ | Float by default, since no ad or audio-format override is set (src/mpvengine.cpp:122-166). The logs show every output init as wasapi 48000 Hz stereo float, shared. The capability is latent: there is no hi-res source, and hi-res audio would be resampled to the 48 kHz mix. |
| C23 | DSP in float, outside a 16-bit branch | ✅ | ✅ | The af chain is empty. The only processing is volume and swresample, both in float (logs; src/mpvengine.cpp:465-471). Trivially satisfied, since there is no EQ. |
| C24 | Zero-cost DSP bypass for exactness | ✅ | ⚠️ | mpv bypasses idle stages. But the default volume of 0.65 (src/playbackcontroller.h:338) means ~-11.2 dB of software gain on every sample (every log shows volume=65). 44.1 kHz AAC is resampled in the app (19 of 176 opens). |
| C25 | Dither on re-quantisation | ❌ (Q4) | ❌ | No audio-swresample-o dither is set (src/mpvengine.cpp:122-166). No effect today, because the path stays float end to end (43 of 43 output inits float). |
| C26 | Bit-perfect output | ❌ (Q2) | ❌ | audio-exclusive is not set (src/mpvengine.cpp:122-166), and the session is named for shared mode (157). All 43 WASAPI inits were shared at 48 kHz. |
| C27 | Vendor-codec blacklist | ✅ | N/A | Only FFmpeg's own software decoders are used (opus 41, aac 3 of 44 decoder opens); the DLL contains no OS or vendor audio codec. hwdec is set for video only (src/mpvengine.cpp:433). |
| C28 | ReplayGain / loudness normalisation | ⚠️ YouTube only | ❌ | replaygain=track (src/mpvengine.cpp:154-155) has no tags to read, loudnessDb from /player is never read, and setReplayGainEnabled has no caller (481-484). There were 0 replay-gain lines in 22 verbose logs. |
| C29 | Dual-player crossfade | ✅ | ❌ | One mpv handle (src/mpvengine.h:103). Every track boundary is a stop plus a 'loadfile replace' (src/playbackcontroller.cpp:945-946, 1162-1177). Measured silence at a natural track end: median 334 ms, p90 581 (n=10). |
| C30 | Beat-matched transitions (Automix) | ✅ | ❌ | No analysis, no second player and no tempo stretch. setSpeed has no caller (src/mpvengine.cpp:473-479). |
| C31 | Container self-healing | ✅ | ✅ | mpv detects the container from the bytes; no MIME hint is passed (src/mpvengine.cpp:391-398). The DLL has FFmpeg's HLS and DASH demuxers, and open failures go down the tier ladder (src/playbackcontroller.cpp:133-179). A real manifest URL was not tested. |
| C32 | Recovery ladder (fallback, retry, skip) | ✅ | ⚠️ | Present for streams: a missing download streams instead (src/downloadmanager.cpp:525-530); a refused link is re-resolved or moved to another tier and resumed (src/playbackcontroller.cpp:133-179); a failed resolve skips the track, stopping after 3 failures in a row (1061-1105, 24-27). Gaps: there is no permanent "unplayable" verdict (src/innertube.cpp:969-977), and any successful resolve resets the failure counter (1044). A refused local file stops playback (937, 180-186), and an EOF is not checked against the track's duration (1162-1177). |
| C33 | Download range resume and persistent queue | ❌ (S6) | ❌ | The queue lives in memory (src/downloadmanager.h:166-173). Partial files are deleted on failure and on quit (src/downloadmanager.cpp:74-86, 397-420), and a retry starts from scratch (444-457). |
| C34 | Download reuses the playback cache | ❌ | ❌ | DownloadManager never asks the resolver or mpv (src/downloadmanager.h:142-181; src/downloadmanager.cpp:283). |
| C35 | Wake lock for network playback | ❌ (D12) | ❌ | There is no power request anywhere (grep for SetThreadExecutionState, PowerCreateRequest, IOPMAssertion and freedesktop inhibit). Whether the OS holds the system awake while audio renders is unverified (powercfg needs admin). |
| C36 | ≥ 5 lyrics providers behind one interface | ✅ 16 | ❌ | Two hard-wired providers: LRCLIB (src/lyrics.cpp:235-256) and the YouTube Music lyrics tab (src/lyrics.cpp:325-349; src/innertube.cpp:1244-1287). |
| C37 | Providers started in parallel, taken in priority order | ✅ | ❌ | The chain is serial (src/lyrics.cpp:212, 318-322). Measured: a miss took 860 ms. A dead LRCLIB delays the YouTube Music answer by 10.6 s (blackholed host) or 4.6 s (refused port). |
| C38 | Word/syllable-synced lyrics | ✅ | ❌ | A line is only {timeMs, text} (src/lyrics.h:20-23), and word stamps are stripped (src/lyrics.cpp:431, 451). Measured: 0 of 30 tracks word-synced. |
| C39 | Recording identified (ISRC) before searching by name | ✅ (L2) | ❌ | No ISRC step. LRCLIB is searched by title and lead artist with a duration gate (src/lyrics.cpp:24-27, 243-248, 274-297). The YouTube Music leg is keyed on the videoId. |
| C40 | Cleaning keeps version markers | ✅ (L13) | ⚠️ | searchTitle/leadArtist (src/lyrics.cpp:38-69) got 9 of 18 fixed cases wrong (bench\skeptic_c25_c48\regex_check.ps1). Markers are lost inside noise brackets, for example '(Official Live Video)'. The L13 cases reproduce, 'Simon & Garfunkel' becomes 'Simon', and 'Feathers' becomes 'hers'. |
| C41 | Manual provider switching with per-provider results cached | ✅ | ❌ | The only control is SEARCH AGAIN, which reruns the same chain (components/LyricsPane.qml:240-251; src/lyrics.cpp:170-173). |
| C42 | Embedded/local lyrics read first (offline) | ⚠️ (L5) | ❌ | Nothing reads tags or .lrc files (src/lyrics.cpp:193-213), and downloads write no lyrics (src/ytdlp.cpp:599-618). |
| C43 | Lyrics result cache across plays | ❌ | ✅ | An SQLite lyrics table (src/appdatabase.cpp:190-198; src/lyrics.cpp:209, 215-233, 351-365). Measured: cached answer median 28 ms (n=86). Caveat: found answers never expire (src/lyrics.cpp:229-230). |
| C44 | Winner shown without waiting for slower providers | ❌ (L1) | ✅ | An answer is shown as soon as the serial chain settles on it (src/lyrics.cpp:299-311, 328-339, 351-392), and there are no losers to wait for. The cost of the serial chain belongs to C37. |
| C45 | Background vocals and duet layout | ✅ | ❌ | Line has no field for either (src/lyrics.h:20-23), and the pane is a single left-aligned column (components/LyricsPane.qml:95-165). |
| C46 | Lyrics translation with on-disk cache | ✅ | ❌ | Nothing in src/lyrics.*, components/LyricsPane.qml or views/NowPlayingView.qml. |
| C47 | Current lyric line on lock screen / car display | ⚠️ (L8) | ❌ | No SMTC, MPRIS or MPNowPlayingInfoCenter code, and lookups run only while the lyrics pane is visible (Main.qml:574-579). |
| C48 | One shared lyrics lookup per track | ❌ (L4) | ✅ | One Lyrics singleton (src/main.cpp:242-243, 282); a track change to the same id is ignored (src/lyrics.cpp:144-149); answers are shared through the table. This holds only because there is a single consumer. Stale YouTube Music callbacks are matched by videoId alone (src/lyrics.cpp:328-330). |

**Totals.** Monolist: 11 ✅, 7 ⚠️, 29 ❌, 1 N/A. BitChord (paper §15.2): 31 ✅, 4 ⚠️, 13 ❌.
- **Ahead of BitChord on 4 rows:** C21, C43, C44, C48.
- **Level on 16:** both ✅ on C1, C7, C18, C19, C22, C23, C31; both ❌ on C6, C8, C16, C20, C25, C26, C33, C34, C35.
- **Behind on 27.**
- **Not comparable:** C27.

Five rows changed from the Step 1 draft: C2, C20 and C42 went from partial to no; C32 went from yes to partial; C23 went from partial to yes.

# Metrics

**Conditions.** Saturday 2026-09-26, 18:23-20:52 EDT.
- **Machine:** a Windows 11 ARM64 VM (the app runs under x64 emulation), 4 logical cores, behind Parallels NAT on a home connection; the ISP is not identified.
- **Network:** RTT-level latencies 40-110 ms; one connection moved 4-14 MB/s; DNS and RTT were slower early in the session.
- **Other load:** other agents shared the VM for most runs. Suite A was repeated on a quiet machine: cold median 700.5 ms against 677 contended.
- **Builds:** the release exe, plus an instrumented copy outside the repo for the stage splits.

**Proxies.**
- "First audio" is mpv's 'starting audio playback'. Sound follows about 10-22 ms later.
- "Tap" is a self-test call, not a UI click.
- "TTFA-warm" is a link-cache hit, because there is no byte cache.

BitChord has no measured timings in the paper. The reference column quotes §15.3's code-comment figures or other paper sections, or says "not reported".

| Metric (§15.1) | Our median | Our p90 | Runs | BitChord reference | Note |
|---|---|---|---|---|---|
| TTFA-cold, first play after launch (500 ms after UI load) | 677 ms | 1,365 ms | 24 | not reported | Unmodified exe. Includes 2 fallbacks: yt-dlp at 5,968 ms, and a 403 rescued via muxed at 3,875 ms. Without them: 662 / 953 (n=22). Instrumented: 665.5 / 1,167 (n=18). |
| TTFA-cold, play the moment the window is ready | 1,048 ms | 1,460 ms | 12 | not reported | Waits a median 434.5 ms (p90 816) for visitorData. Six InnerTube objects fetch the 885 KB home page at launch. |
| TTFA-cold, new track in a warm session | 553.5 ms | 761 ms | 22 | not reported | Stage medians: resolve 203.5, resolved to loadfile 54.5, HTTP open 200.5, response to audio 45. |
| TTFA-warm (link-cache hit; bytes downloaded again) | 421.5 ms | 592 ms | 24 | not reported; a disk hit costs "zero network and zero resolve" (§11 row 2) | Not the paper's definition. The HTTP open (219 ms) is the largest stage; the cache hit itself takes 49 ms. |
| TTFA-warm, immediate replay on a reused connection | 196 ms | 525 ms | 24 | not reported | mpv's curl reuses the open connection to the same googlevideo host. |
| Skip latency (next track's link prefetched) | 412.5 ms | 660 ms | 22 | not reported | 22 of 22 were link-cache hits. The HTTP open (247 ms) is ~60% of the total. A skip before the prefetch finished was not tested. |
| Track-change silence at a natural end (§8; same path as a skip) | 334 ms | 581 ms | 10 | gapless by default (§8.1), no figure | Unmodified exe. About 180 ms of buffered audio drains before this silence. |
| Resolve time, InnerTube tier alone | 199 ms | 247 ms | 12 | not reported for InnerTubeX; cold fallback walk ~7.9 s vs ms warm (§15.3) | 0 failures. In the app: 267 / 395 at launch (n=18), 203.5 / 283 in a warm session (n=22). BitChord also probes every new URL (§3.2.5, D6); we do not. |
| Resolve time, link-cache hit | 42.5-49 ms | 51-64 ms | 46 | "ms" (§15.3) | Main-thread event handling only. |
| Resolve time, yt-dlp tier alone | 2,913 ms | 3,207 ms | 12 | NewPipe fast path 2.3 s; cold player-JS solve 8.7 s (§15.3) | Subprocess, about 15x the InnerTube tier. |
| Resolve time, muxed tier alone (itag 18) | 2,742.5 ms | 3,198 ms | 12 | no equivalent | The first rescue after a 403. |
| Resolve split: visitorData wait / /player POST | 0 / 169 ms (warm session); 434.5 / 180 ms (window-ready) | 0 / 254; 816 / 226 | 22; 12 | not reported | curl cross-check on fresh connections: /player takes 309.7 ms after TLS (n=17). |
| Throughput in-app, tracks ≤ 6.3 MB (11 of 12 tracks) | 6.02 MB/s (whole file in 754 ms) | 11.49 MB/s | 93 | bounded Range ~5.7 MB/s (§15.3) | Proxy: file size / (request start to demuxer EOF). mpv downloads each file in one burst, so there is no steady-state rate. Measured on one open-ended GET, not chunked. |
| Throughput in-app, 14.6 MB track (13:32 Opus) | 36.8 kB/s | 37.3 kB/s | 9 plays (1 whole file) | open-ended GET ~15 kB/s (§15.3) | Paced for the whole file, at about 1.85x real time. |
| Throughput, curl, long track: open-ended vs 1 MiB ranges | 35.5 kB/s vs 4.3-6.8 MB/s (by UA) | 35.7-35.8 kB/s vs 7.8-8.3 MB/s | 40 (n=4 per variant) | ~15 kB/s vs ~5.7 MB/s (§15.3) | The paper's ratio reproduces (~180x). Pacing starts when one request asks for more than 8-12 MiB. The UA made no difference. |
| Rebuffer ratio | 0 | 0 | 2 continuous runs (1,555.2 s, 6 tracks) | not reported | 0 stalls in 2,319.7 s across all instrumented plays, including the paced long track. |
| googlevideo refusals (not a §15.1 row) | 2 of 178 opens (1.1%) | n/a | 178 | not reported | Both on the first request of a freshly resolved link; rescued via itag 18 in 3.4-5.4 s. One more happened in the CPU suite (1 of 12; HTTP code not logged). |
| Lossless hit rate / time to lossless | N/A | N/A | 0 | not reported | No lossless source (C13). |
| Delivered format | float32, 48 kHz stereo, WASAPI shared mode | - | 43 output inits | default PCM16, usually 48 kHz (§12) | From mpv -v logs, the desktop equivalent of dumpsys. |
| Resampling | in-app 44.1 to 48 kHz on 19 of 176 opens (AAC itag 140); none for Opus | - | 176 | the app never resamples; the mixer usually does (§6.3, §12) | What the Windows mixer does afterwards is not visible. |
| CPU, steady playback (15-45 s after launch) | 2.5% of one core | 2.8% | 12 | not reported | Whole process under x64 emulation, so native cost is likely lower. App open with nothing playing: 0% / 0.1% (n=4). Suite D (bench_D_cpu.ps1), quiet machine, 20:35-20:52 EDT. |
| Memory while playing | peak working set 359.5 MB (private 324.2 MB) | 369 MB (334.3 MB) | 12 | not reported | App open and idle: 175.6 MB (private 148.6 MB), n=4. Whole process, including autoplay filling 25 songs in each run. The ~180 MB rise is not attributed to a component. |
| Time to lyrics, cold | 453 ms | 608 ms | 287 | not measured; L1 analysis gives 6-8 s typical (§13.5) | Proxy: lookup() until lines are in the model. It leaves out the pane gate: lookups run only while the lyrics pane is open (Main.qml:574-579). |
| Time to lyrics, warm connection | 247 ms | 391 ms | 494 | not reported | LRCLIB synced answers only. |
| Time to lyrics, cached (SQLite) | 28 ms | 48 ms | 86 | no result cache (§10.4 step 9) | 10 / 20 ms for a repeat in the same process (n=287). |
| Time to "no lyrics" | 860 ms | 932 ms | 5 | not reported | LRCLIB, then YouTube Music, one after the other. |
| Winning provider and slowest provider | LRCLIB leg 447 ms; LRCLIB was both winner and slowest in 145 of 145 | 595 ms | 145 | the slowest provider sets time to lyrics (L1) | Proxy: measured through a local pass-through. The YouTube Music leg alone: 512 / 610 ms (n=86). |
| Dead-LRCLIB penalty | 10,595 ms (blackholed host); 4,564 ms (refused port) | 10,630; 4,650 | 3; 58 | not reported | The cost of the serial fallback. |
| Lyrics hit rate, 30 fixed tracks | word 0/30, line 28/30, plain 0/30, none 2/30 | - | 30 tracks x 15 runs | not reported | Line text was correct on 27 of 28 vocal tracks (#24 had the wrong text, #23 came in romaji). Both instrumentals were correctly none. YouTube Music alone: plain on 25 of 28. |

Not measured: loudness spread between tracks, sync accuracy of lyric lines, and battery (there is no battery on this VM).

# Faster?

No for the first play of a new song: on this network we are probably level with BitChord or slightly ahead. Yes for everything that comes after it: replays, Previous, skips, the change from one song to the next, and long tracks.

The paper gives no BitChord start-up or skip timings (§15.3 has only figures from code comments). So wherever BitChord comes out faster below, the size given is what we measured for the step BitChord avoids. That is an upper bound, not a head-to-head number.

**1. New song: level or slightly ahead of BitChord.**
- **Our path:** one /player POST as the visionOS client (tier median 199 ms), then one HTTP request to googlevideo.
  - Total in a warm session: median 553.5 ms, p90 761 (n=22).
  - First play after launch: median 677 ms, p90 1,365 (n=24).
- **Download speed:** mpv's single open-ended request brought typical 3-6 MB songs down whole in a median 754 ms, at 6.0 MB/s (n=93). That matches BitChord's chunked figure (~5.7 MB/s, §15.3).
- **Start:** audio starts a median 49 ms after the first bytes arrive (C19).
- **BitChord's path:** it uses the same kind of unciphered client (§3.2.5, §11 row 3), but adds a probe request before it trusts each new URL (§3.2.5, D6). On our network, a request on a new connection took a median 200.5 ms. That figure is only an estimate of what the probe would cost.

**2. Replays, Previous and repeat-one: BitChord is faster.**
- BitChord plays these from a disk cache placed in front of the resolver, with no network at all (§5.1, §11 row 2).
- We have no byte cache (C2), so a replay downloads the song again. It takes a median 421.5 ms (p90 592, n=24).
- Of that, the new connection is 219 ms and the link-cache lookup 49 ms. That is about half the time, and it is what BitChord saves.

**3. Skip to the next song: BitChord is faster when its read-ahead has already put the next file on disk (§5.5).**
- That read-ahead is broken when a lossless source is configured (D1) and does not run in the first 8 s of a song (D7).
- We prefetch only the next song's link (C17, C18). A skip takes a median 412.5 ms (p90 660, n=22), of which the new connection is 247 ms (~60%).

**4. From the end of one song to the start of the next: BitChord is clearly faster.** It plays the queue gaplessly (§8.1). We stop and reload at every boundary (C29), leaving a median 334 ms of silence (p90 581, n=10), on top of ~180 ms spent draining buffered audio.

**5. Long tracks: BitChord downloads about 180x faster.**
- Once a single request asks for more than roughly 8-12 MiB (about 10 minutes or more of Opus), googlevideo paces our one request to 36.8 kB/s. The 14.6 MB test track took 396.7 s to arrive.
- 1 MiB ranges, which BitChord uses (§5.2), got 4.3-6.8 MB/s on the same URLs.
- Not audible yet: 36.8 kB/s is 1.85x real time, and there were 0 stalls in 1,555 s of continuous play. The margin is thin, though: 2.1 s of audio was buffered after 1 s of playback.

**6. Launch: we lose time, but BitChord's cost here is unknown.**
- A play started the moment the window is ready takes a median 1,048 ms (p90 1,460, n=12).
- It waits a median 434.5 ms for visitorData, which six InnerTube objects each scrape from the 885 KB home page (5.3 MB in total).
- BitChord takes visitorData from the small sw.js_data (§3.2.6) and pre-warms 2 s after launch (§11 row 12). The paper gives no figure for either.

**7. Failures: roughly the same cost on both sides.**
- A refused link was rescued in 3.4-5.4 s (2 of 178 opens). Our fallback tiers alone take 2.9 s (yt-dlp) and 2.7 s (muxed).
- BitChord's cold fallback walk is ~7.9 s, and its NewPipe fast path 2.3 s (§15.3).
- Neither engine races clients (C8, D3).

**Why.**
- BitChord's speed comes from its network and cache layers, not its audio stack (§11): a disk cache in front of the resolver, whole-file read-ahead, a gapless player and 1 MiB range reads. We have none of these (C2, C5, C17, C29).
- Our first-play speed comes from:
  - the same unciphered-client choice (C7);
  - resolving asynchronously before mpv is involved, with no probe (C21);
  - mpv's near-zero start threshold;
  - its 64 MiB buffer, which holds a typical song whole.

# Quality?

For the same YouTube song on default settings, our signal path is at least as clean as BitChord's. But BitChord gives the better listening result overall: it can play true lossless files, it evens out loudness between songs, and it leaves no gap between them.

**Source (§3, §12).**
- Both play YouTube's lossy streams.
- BitChord can also substitute FLAC or ALAC (up to 24-bit/96 kHz) from addon or module servers, or 320 kbps AAC from JioSaavn.
- We have one catalogue (C13; src/streamresolver.h:44-57). This is the largest quality gap, but it only matters for a user who configures such sources.

**Stream choice (§3.2.5).**
- BitChord's automatic pick favours Opus 251.
- We take whichever audio format has the highest `bitrate` field, whatever the codec (src/innertube.cpp:989-1003). As a result, 19 of 176 opens were 44.1 kHz AAC that the app had to resample to 48 kHz.
- After a 403, our first rescue is itag 18 (~96 kbps AAC). It is kept for the rest of the song and for replays until the link expires (C3). This happened on 2 of 178 opens.

**Decoding and bit depth (§6.3).**
- BitChord outputs 16-bit PCM by default and uses float only if the user opts in (Q1).
- Ours is float32 from the FFmpeg decoder all the way to WASAPI. That held in all 43 output inits in the logs, and no code forces it (src/mpvengine.cpp:122-166 sets nothing; C22, C23).
- With no hi-res source to protect, this advantage is mostly theoretical today.

**Processing (§7.1-7.2).**
- BitChord has a well-built float EQ, a spatializer and transition filters, each of which switches itself off when idle.
- We have only the volume control and the resampler (C23, C24).
- Our default volume of 0.65 is about -11 dB of software gain on every sample (src/playbackcontroller.h:338; src/mpvengine.cpp:465-471). It is applied in float, so nothing is truncated.

**Loudness (§7.3).**
- BitChord corrects YouTube tracks by -loudnessDb, clamped to -15 to +3 dB.
- We apply nothing (C28): replaygain=track finds no tags, loudnessDb is never read, and 0 of 22 verbose logs show a replay-gain line.
- So songs play at their mastered loudness. How much that varies between songs was not measured.

**Output (§7.4, §12).** The two engines end up the same here:
- both go through a shared system mixer at 48 kHz;
- neither has a bit-perfect path (C26);
- neither dithers (C25), and we don't need to, since nothing is converted back to integers inside the app.

**Song-to-song transitions (§8).**
- BitChord is gapless by default, with crossfade as an option.
- We leave a median 334 ms of silence (p90 581) at every boundary (C29). That is audible on albums whose tracks run into each other.

**Codec risk (§6.1).** We use FFmpeg's own decoders pinned inside libmpv, so vendor decoder bugs like BitChord's Samsung FLAC case cannot happen here (C27, N/A).

# Lyrics?

BitChord has better lyrics; we have faster ones. Its lyrics are richer: more providers, word-by-word timing, translation, duet layout and a line on the lock screen. Ours arrive in about half a second, are kept across restarts, and were line-synced for every vocal track on our 30-song list.

**Coverage and detail (§10.1, C36-C38).**
- BitChord asks 16 providers; we ask 2, LRCLIB and the YouTube Music lyrics tab (src/lyrics.cpp:235-256, 325-349).
- We show one timed line at a time at best, and we delete word timing when a source has it (src/lyrics.cpp:431, 451).
- Results on 30 fixed tracks, 15 runs each:
  - word-synced 0, line-synced 28, plain 0, none 2;
  - both "none" were instrumentals, which is the right answer.
- The line text was right on 27 of the 28 vocal tracks:
  - For one Japanese song, LRCLIB's search returned another song's text in every row. LRCLIB's exact-match lookup (/api/get), which BitChord tries first (§10.5), returns the right text.
  - One song came back in romaji with a credit line first.
- Every non-English vocal track got native-script line sync, but the list is weighted towards well-known songs.
- We did not measure BitChord's hit rate on this list.

**Speed (§13.5 L1, C44).**
- Our times (median / p90):

| Case | Median | p90 | Runs |
|---|---|---|---|
| Cold | 453 ms | 608 ms | 287 |
| Warm connection | 247 ms | 391 ms | 494 |
| From the cache | 28 ms | 48 ms | 86 |

- BitChord shows its winner only after its slowest provider finishes. The paper's analysis puts that at 6-8 s typically (L1).
- Caveat: we look lyrics up only while the lyrics pane is open (Main.qml:574-579), so the user's wait starts when the pane opens.

**Misses and outages (§10.2, C37).**
- Our providers are asked one after the other. "No lyrics" took a median 860 ms (p90 932, n=5).
- If LRCLIB is down, the YouTube Music answer is delayed by 10.6 s (host unreachable, n=3) or 4.6 s (connection refused, n=58).
- BitChord starts all its providers at once, so a dead provider adds nothing beyond its L1 wait.

**Persistence (C43, §10.4 step 9, L4).**
- We keep every answer in SQLite across restarts. BitChord runs its whole race again on every play.
- But a found answer is never refreshed. After a single LRCLIB failure, YouTube Music's plain text is stored permanently (src/lyrics.cpp:229-230, 265-268, 331-334). This was seen for Blinding Lights, which LRCLIB has synced.

**Query cleaning (C40, L13).** Our title cleaning reproduces the paper's L13 mistakes and adds some of its own: 9 of 18 fixed test cases came out wrong.

**Missing entirely:**
- word timing (C38);
- choosing a provider by hand (C41);
- lyrics stored with downloads (C42);
- background vocals and duets (C45);
- translation (C46);
- a current line outside the lyrics pane (C47).

# Surprises

- googlevideo's pacing depends on how much one request asks for, not on the request being open-ended. The paper (§5.2, §11 row 1) and Step 1 expected our one-request fetch to be paced to ~15 kB/s on every track. Instead, open-ended GETs served 11 of 12 tracks (≤ 6.3 MB) at a median 6.0 MB/s in the app (n=93). With curl, single requests up to 8 MiB were fast, requests of 12 MiB or more were paced, and '10000000-' (the last 4.6 MB, open-ended) was fast (bench\results_C_rangesize_before.jsonl).
- The paced rate was ~36 kB/s, not ~15 kB/s (§15.3). That is ~1.85x the Opus bitrate, so the repeated stalls §5.2 predicts did not happen: 0 stalls in 1,555 s of continuous play, including the full 13:32 track. The buffer was thin, though: 2.1-10.6 s ahead during the first 10 s.
- The User-Agent mismatch that Step 1 and paper §11 row 10 flagged as a 403 risk (links minted as visionOS, fetched with a Chrome UA) made no measurable difference. Pacing was identical and no 403 depended on the UA across the Chrome, visionOS, no-UA and mpv's-exact-headers variants (curl, 5 URLs x 8 variants).
- mpv fetches https through its own libcurl stream, not FFmpeg's http protocol (curl trace in bench\logs). The FFmpeg reconnect options Step 1 described (reconnect=1, reconnect_delay_max=7) therefore do not apply, and chunking would have to be set with curl-max-request-size.
- The VISIONOS 1.02 identity, which InnerTubeX marks probe-only and Step 1 suspected could stall, was reliable. /player never failed in 96 instrumented calls, and 175 of 178 stream opens used it. The one unexplained fall-through to yt-dlp came in 1 of 24 launches.
- Launch overhead delays the first play. Six InnerTube objects fetch the 885 KB home page in parallel (5.3 MB), and a play issued when the window is ready waits a median 434.5 ms (p90 816) for visitorData. Cold TTFA is then 1,048 ms instead of 665.5 ms. Step 1 noted the duplication but not this delay, and the --play self-test's 500 ms head start hides it.
- yt-dlp does not keep the player JS on disk. Only youtube-sigfuncs/*.json survives, and each process downloads the player JS again (bench\skeptic_c1_24). Step 1's cache table listed 'yt-dlp player JS' as held in yt-dlp's own disk cache.
- The link cache saves less than expected. A hit resolves in ~45 ms against 203.5 ms, yet a warm play still takes a median 421.5 ms, because the per-track connection (median 219 ms) is paid every time. The 196 ms immediate replay is fast because mpv reuses an open connection, not because of any cache.
- One LRCLIB failure pins a song to unsynced lyrics permanently. YouTube Music's plain answer is stored, and only 'none' ever expires (src/lyrics.cpp:229-230, 265-268, 331-334). Blinding Lights, which LRCLIB has synced, stayed plain. Step 1 credited us with telling network errors from misses (L14 'mostly' fixed), but that holds only when both providers fail.
- A refused connection is not instant on Windows. A closed local port cost ~4.5 s per LRCLIB call (median 4,564 ms, n=58), where Step 1 expected 'refused at once'.
- LRCLIB's one wrong answer came from its data, not from our matching. For アイドル (YOASOBI), all 20 /api/search rows carry another song's text, while /api/get returns the right one. Every entry we chose had the right title and artist, so the matching errors the paper warns about (L11) did not appear.
- LRCLIB gave native-script line sync for all 16 non-English vocal tracks on the list (Korean, Arabic, Punjabi, Hindi, Japanese, Spanish, French, German). Step 1 expected non-English songs to be where our two providers fall short. The list is weighted towards well-known songs.
- The 'serves the first request, then 403' refusal the paper describes (§3.2.5) reproduced with curl on 2 of 8 fresh long-track URLs, whatever the UA or range. We never saw it in the app, because mpv makes one request per file. The in-app refusals (2 of 178, plus 1 of 12 in the CPU suite) were all on the first request, so a switch to range reads could expose this new failure mode.
- Five checklist rows moved from the Step 1 draft on verification: C2, C20 and C42 from partial to no; C32 from yes to partial; C23 from partial to yes. C32 dropped because of two new gaps: a successful rescue resets the 3-failures-in-a-row stop, and a refused local or downloaded file halts playback instead of streaming or skipping.
- Step 1's §6 verdict says BitChord filters out DRC (dynamic-range-compressed) variants. The paper says it does not (§13.2 Q7), so on that point the two engines are the same.
- Playing one song roughly doubles the app's memory: peak working set a median 359.5 MB against 175.6 MB with nothing playing (n=12 vs 4, bench\results_D_before.json). That is far more than mpv's buffer for the song (3-15 MB). The cause is not attributed, and autoplay's radio fill ran in every run.