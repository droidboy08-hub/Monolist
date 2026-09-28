# Step 3: the engine plan, for your approval

**In short.** Step 2 showed that our first play of a new song is already level with BitChord: a median of 553.5 ms in a warm session. BitChord wins on what comes after that first play:
- replays: 421.5 ms for us, no network at all for BitChord;
- skips: 412.5 ms;
- the silence between songs: 334 ms;
- long tracks: 36.8 kB/s.

All four come from one missing piece: a disk cache of song bytes that sits in front of the resolver and is filled in advance with the next song. That cache tops the plan.

The launch delay is the cheapest big win, and it can be built now, before the JioSaavn merge. Today a play started the moment the window is ready waits a median 434.5 ms for visitorData.

For lyrics, the order is:
1. Stop storing a worse answer permanently.
2. Fix the search query.
3. Run the providers in parallel.
4. Add providers one at a time, each measured before it can rank above LRCLIB.

The YouTube ladder (InnerTube, then yt-dlp, muxed, Piped and Invidious) stays underneath every change. Each new layer has a switch that hands control back to the ladder.

**What the measurements overturned in the paper**
- **Range chunking (§11 row 1) is not a general speed-up for us.** googlevideo slows down only requests that ask for more than about 8-12 MiB.
  - Normal songs already arrive whole at 6.0 MB/s (n=93). 1 MiB ranges were slower on them (0.9-5.7 MB/s).
  - Only files over about 8 MiB (roughly 7.5 minutes or more of Opus) crawl, at 36.8 kB/s. Even those caused 0 stalls in 1,555 s of play.
  - So chunking is for long files only, and it goes inside the cache (rank 19).
- **The 500 ms start threshold (§11 row 5) is already beaten.** Audio starts a median 49 ms after the first bytes arrive (p90 209, n=176).
- **Matching the User-Agent or the per-client headers (§11 row 10) changed nothing** across 5 URLs x 8 variants.
- **A probe request before trusting a URL (§3.2.5) is not worth it.** It would add about 200 ms to every cold play in order to catch refusals on 1.1% of opens, and the first real read already catches every one of them.
- **Racing two InnerTube clients buys nothing we measured.** VISIONOS answered 96 of 96 /player calls. A second no-cipher client that is used only when VISIONOS fails is worth having (rank 9).

## Ranked plan

The ranking is by measured impact. The build order is different, because of the JioSaavn merge and dependencies between items; see the batches.

| # | Item | Change | Now → expected (evidence) | Effort | Main risk |
|---|---|---|---|---|---|
| 1 | PP-01 | A disk cache of song bytes, keyed by song and rendition, that mpv reads through a monolist:// stream. Built in three gated phases. | Replay, Previous and repeat-one: 421.5 ms (p90 592, n=24; 219 ms of it is a new connection) → ~120-175 ms. A cached song played at launch: 1,048 → ~150 ms. Network per replay: 3-6 MB → 0. | L (M + L + M) | mpv's stream_cb API is marked unstable. Media bytes move from mpv's curl to Qt, which has never been measured. Disk use. A stale pin plays the wrong recording. |
| 2 | PP-02 | Download the next song's bytes into the cache once the current song is complete. | Skip: 412.5 ms (p90 660, n=22) → ~90-130 ms. Silence at the end of a song: 334 ms (p90 581, n=10) → ~45-80 ms. | M | 3-6 MB wasted for each song jumped over. |
| 3 | PP-03 | One shared, stored visitorData and one shared cookie jar, in place of six home-page scrapes at launch. | Play at window-ready: 1,048 ms (p90 1,460, n=12) → ~615 ms (the 434.5 ms wait becomes 0). Launch download: 5.3 MB → ≤ 0.9 MB. | M | YouTube may stop honouring a stored id. A persisted id works like a tracking cookie. |
| 4 | LY-4 | Look lyrics up when a song starts, and for the next song, with today's two providers, instead of only while the pane is open. | Pane open to lines: 453 ms (p90 608, n=287) → 28 ms (p90 48, n=86). | S | Lookups for songs whose lyrics are never viewed. |
| 5 | LY-5 | Lyrics race: every provider starts at once, answers are taken in your priority order, losers are cancelled, and each provider's result is cached. | With LRCLIB down, the fallback answer is delayed 10,595 ms (host unreachable, n=3) or 4,564 ms (connection refused, n=58) → at most the patience window (~1.2 s to start with). "No lyrics": 860 ms → ~500 ms. | L | The largest lyrics change. An ordering bug would show the wrong provider. |
| 6 | LY-1 | An answer shown only because a better provider failed is stored as provisional and asked again later. | Songs stuck on worse lyrics after one failure (Blinding Lights; a transient LRCLIB failure in 1 of 15 cold passes) → 0. | S | A schema migration mistake could hide stored lyrics. |
| 7 | PP-04 | Write history and play events after the load starts, not before. | "Resolved to loadfile": 54.5 / 78.5 / 45.5 ms (new song / replay / skip) → < 5 ms. Only ~14 ms of this is on the launch path. | S | Recording a play that mpv rejected. |
| 8 | PP-05 (+QT3) | After a refusal, ask InnerTube for a fresh link once before falling to itag 18. A rescue link never sticks to the song. | Refusals hit 1.1% of opens: rescue 3.4-5.4 s → ~0.5-0.7 s (or ~0.45 s slower than today if the fresh link is also refused). Replays after a rescue: ~96 kbps AAC for up to 5 h → Opus 251. | S | Changes the order you asked for in U01. |
| 9 | NC-1 (new) | A second no-cipher InnerTube client, tried only when VISIONOS fails, before yt-dlp. | 175 of 178 opens rely on VISIONOS 1.02. If it breaks, every play costs yt-dlp's 2,913 ms; with this rung, ~200 ms extra. 1 of 24 launches already fell to yt-dlp (5,968 ms). | S | Client versions age, so it needs a canary. |
| 10 | LY-3 | One shared query cleaner and match scorer. | Query cleaning wrong on 9 of 18 test cases → 0. | S | Songs without artist credits need a string fallback. |
| 11 | LY-2 | LRCLIB's exact, database-only lookup before its search. | Correct text: 27/28 → 28/28. Answer size: 183 KB → 9 KB (median). | S | Its tight duration match sends more songs on to search. |
| 12 | QT2 | Gapless: the next song is appended to mpv's own playlist. | Silence at the end of a song, after PP-02: ~45-80 ms → ≤ 20 ms, and the ~180 ms of drained audio before it goes too. The skip gain is counted once, under PP-02. | L | State bugs in the controller: double counts, repeat, shuffle. |
| 13 | PP-07 | A deadline on every resolve, one hedged /player request, and yt-dlp never blocks the UI. | Worst case before "Couldn't play": ~49 s (derived) → ≤ 20 s. UI freeze when a skip cancels yt-dlp: up to ~3 s → 0. The normal path is unchanged (/player 199 ms). | M | A deadline that is too tight fails songs on slow networks. |
| 14 | PP-06 | An early end of file counts as a failure and playback resumes. A refused download streams instead. The failure counter resets only when audio actually starts. | Silent cut-offs: undetectable today → caught. A refused download: playback halts today → it continues. | M | False alarms on files whose duration is only an estimate. |
| 15 | QT1 | Even out loudness using YouTube's own per-format loudness value. This one is an estimate. | Loudness spread across 13 probed tracks: 12.45 dB → 4.68 dB. These are YouTube's numbers, not yet checked on our output. The median song plays ~5 dB quieter. | M | Everything is quieter by default. |
| 16 | LY-7 | Six keyless providers (Apple-TTML relays, LyricsPlus, SimpMusic, Unison, Portato), each measured before it can outrank LRCLIB. | Word-synced lyrics: 0/30 → unknown (§10.1 gives no hit rates). Line-synced or better must stay at 28/28. | L | Third-party services with no guarantees, and each one sees what you play. |
| 17 | LY-10 | A provider menu per song, and a Lyrics section in Settings. | Picking another provider: impossible today → one click, instant when that provider has already answered. | M | Contacting a disabled provider from the menu. |
| 18 | LY-6 + LY-8 | Word-timing model, parsers and a word highlighter, built only if LY-7 finds word timing. | Word-synced answers shown: 0 → every one that has word timing. CPU: 2.5% of one core → ≤ 3.5%. | M + M | Wasted work if word timing is rare. |
| 19 | PP-08 | Range reads only for files over 8 MiB, inside the cache, with a transparent link refresh. | Files over 8 MiB: 36.8 kB/s (n=9) → ~4-8 MB/s. That was 1 of 12 benchmark tracks, and there are 0 stalls today. | M | The "first request served, then 403" failure seen on 2 of 8 fresh long-track URLs. |
| 20 | QT4 | Opus first, skip DRC formats, and label the codec from the MIME type. | AAC picked although Opus was offered: 1 of 13 probed tracks → 0. | S | Occasionally the AAC stream (itag 140) is the better encode. |
| 21 | QT5 | One stream-info line built from what mpv confirms (the engine half of ROADMAP P06). | File starts whose delivered format you can see: 0% → 100%. | S | Log noise. |
| 22 | PP-09 | Join duplicate resolves, keep answers that arrive late, add per-source "unplayable" verdicts, and bound the link cache. | A song the whole ladder already failed on: ~5.8 s (derived) → skipped at once. Not measured. | S | A network outage recorded as "unplayable". |
| 23 | LY-11a | KuGou and YouTube captions for the long tail. | "No lyrics" on vocal tracks is 0/28 on our list; measure on a long-tail list (§10.1 rows 10, 12). | M | Captions that are not lyrics. |
| 24 | LY-9 | Translation on demand, with a cache. | 16 of our 28 vocal test tracks are not in English. None today → translated lines. | M | The Google endpoint is unofficial. |
| 25 | LY-13 | Read lyrics from local files, and write lyrics into downloads. | None today → yes (C42). | M | An interrupted remux could corrupt a download. |
| 26 | LY-14 | A lyrics sync offset. | None today → ±5 s in 100 ms steps (App. A). | S | A per-song offset surviving a change of provider. |
| 27 | LY-12 | Background vocals, duet voices and instrumental gaps. | None today → yes for TTML answers (C45). | S | The bracket split peeling off real lyrics. |
| 28 | LY-11b | JioSaavn lyrics, plus the Genius and Megalobiz scrapers (off by default). | Long tail; to be measured. | M | Scrapers break without notice. |
| 29 | QT6 | Optional crossfade using a second mpv (off by default). | None today → a 1-12 s equal-power fade (§8.1). | L | Two players, more memory, and listens counted twice. |
| 30 | LY-15 | PaxSenix routes for users who have their own key. | Off unless a key is set (§10.1 rows 5-6). | S | Needs the macOS Keychain (ROADMAP C05). |
| 31 | QT8 | Optional exclusive output, labelled bit-perfect only when that is confirmed. | 43 of 43 outputs are shared at 48 kHz → exclusive at the source rate when the option is on. | L | Device refusals, and it silences every other app. |
| 32 | LY-16 | The current lyric line in the OS media controls. | Waits for the ROADMAP media-session item (§10.9). | S | Re-sending the artwork on every line change. |

Deferred: PP-10 (downloads reuse the cache). Parked with the JioSaavn work: QT7 (mid-song upgrade to JioSaavn). One slot pending research: [SIGN-IN SLOT].

**Build order in brief.**
- **Before the merge:** B0 measure first, then B1-B5: PP-03; PP-07a + NC-1; QT4; LY-1/3/2; LY-5 + LY-4.
- **The merge:** B6 merges jiosaavn and re-baselines.
- **After the merge:**
  - B7-B9: PP-04 + QT5; PP-05 + PP-06; PP-07b.
  - B10-B12: the PP-01 spike, then the PP-01 store, then PP-02.
  - B13-B20: LY-10 + LY-7, QT2, PP-08, QT1, LY-6/8, PP-09, then the rest.

## Item details

Each item lists **Change**, **Files**, **Test** and **Don't copy**. "Don't copy" names the §13 defect IDs the item must avoid.

#### 1. PP-01: Byte cache in front of resolution (L, in three phases)
- **Change:** An AudioBroker thread serves mpv through a registered monolist:// protocol (mpv_stream_cb_add_ro). It reads from disk entries keyed `yt:<videoId>:<itag>:<clen>` (later `saavn:<id>:<kbps>`), and a miss fills the entry while the song plays.
  - Video, local files, downloads and manifests keep today's direct path.
  - A Settings switch restores direct googlevideo URLs.
- **Phase 1, spike (M):** Read-through with no disk, using a Qt network manager owned by the broker thread. Compare HTTP/1.1 with HTTP/2.
  - read_fn returns whatever bytes it has and never waits to fill the buffer.
  - After 5 s with no bytes, it reconnects once at the same offset, then refreshes the link (bypassing the link cache), then returns an error so today's ladder runs. 20 s is only an absolute cap.
  - Log the IP family of the /player connection and of the media connection.
  - **Gate:** a new song must start within ±10 ms of the post-merge baseline, with no new 403s and the same pacing as curl.
  - **If the gate fails:** try the same design behind a 127.0.0.1 server (random port, a per-session token in the path, identity URLs only, never an open proxy). If that fails too, stop, and build QT2 on plain URLs to get the skip and gap gains.
- **Phase 2 (L):** Whole-file entries up to 8 MiB.
  - Files and their index live in `QStandardPaths::CacheLocation`, as ArtworkCache already does (src/artworkcache.cpp:185). Not in the database's AppDataLocation (src/appdatabase.cpp:21), which roams on Windows.
  - Persist only key, itag, clen, lmt and size. URLs (they carry your IP and expire=) and headers stay in memory.
  - "Complete" means file size = clen, checked at open with the same key function.
  - 1 GiB cap, whole-entry LRU with the current and next songs protected, and a Clear action.
  - The pin records the JioSaavn verdict's source, the matcher version and the JioSaavn settings, and is dropped when any of them changes. A complete pinned entry plays without running the JioSaavn race.
- **Phase 3 (M):** Partial entries, plus a mid-file refresh through a new `refresh(videoId, itag)`. It forces a new /player call and fails unless the same itag, clen and lmt come back. PP-08 lives here.
- **Files:**
  - new src/audiobroker.h/.cpp (+ CMakeLists.txt, and a guard in src/mpvengine_stub.cpp)
  - src/mpvengine.cpp:84-95 (register after mpv_initialize), 343-404 (load)
  - src/playbackcontroller.cpp:1001-1018 (a complete entry loads without a resolve), 1036-1059, 133-187
  - src/streamresolver.cpp:559-575 and .h:131-139 (itag and headers travel with each answer)
  - src/innertube.cpp:947-1038
  - src/main.cpp:211-227
- **Test:**
  - `--broker-test` against a QTcpServer fake googlevideo (the pattern of src/ytmselftest.cpp). Cases:
    - the assembled bytes match the source md5;
    - a seek into an unfilled span;
    - a 403 after the first request;
    - a changed clen must error and never splice;
    - a truncated body must error;
    - cancel during a blocked read;
    - a 30 s mid-body stall must resume within 8 s;
    - a JioSaavn pin flipped to no-match must play YouTube.
  - Real tracks: bench_B 'again' median < 200 ms with 0 network bytes.
  - Two launches of `--play <id> 30` must show cache hits across launches.
  - With the switch off, playback must reproduce the baseline numbers.
- **Don't copy:** D1, D6, D10, S3, S4, §5.2's silent end (D.3), §5.4's cache-lock stalls.

#### 2. PP-02: Next-track bytes (M)
- **Change:** Start filling the next row's rendition when the current entry is complete (a typical song ~0.75 s after it starts) or 3 s after start, whichever comes first.
  - Fill the whole file if it is ≤ 8 MiB (≤ 16 MiB once PP-08 lands), otherwise the first 4 MiB.
  - Start only once the next row's JioSaavn verdict is final: match, no match, or grace window expired.
  - The row after that gets a link only, from the InnerTube tier only.
  - The launch fill starts after PP-03 lands and after Home has loaded.
  - On a metered connection, fetch links only. Where Qt cannot tell (no Metered feature), treat the connection as unmetered.
  - Cancel on re-target, unless the fill is more than half done.
  - PP-02 is the only prefetch scheduler. QT2 later appends the same entry.
- **Files:** src/playbackcontroller.cpp:758-770 (prefetchUpcoming; the branch's prefetchTrack call is at its line 854); src/streamresolver.cpp:245-259; src/main.cpp:238; src/audiobroker.*
- **Test:**
  - bench_B 'skip': ≥ 90% cache hits, median < 150 ms.
  - bench_A_gap: median < 100 ms.
  - A skip 0.5 s after start, measured separately.
  - With metered forced on: 0 bytes prefetched.
  - A wasted-bytes counter per verdict.
- **Don't copy:** D1, D7, §5.4's lock probe.

#### 3. PP-03: Shared, stored visitorData (M)
- **Change:** One process-wide holder fetches visitorData and stores it, with its fetch time, in settings. It refreshes the value in the background after the first play.
  - Measure sw.js_data (§3.2.6) as a smaller source than the 885 KB home page.
  - The six InnerTube objects share one QNetworkCookieJar, so the VISITOR_INFO1_LIVE and YSC cookies still reach browse, search and next.
  - If /player answers LOGIN_REQUIRED with a stored id, refresh once and retry before falling to yt-dlp.
  - EU consent page: keep the stored id and log a warning.
  - Cap the id's age at 30 days, and clear it along with the other YouTube data.
  - The holder accepts a session-bound id from the sign-in work, which outranks the anonymous one.
- **Files:**
  - src/innertube.h:241-244, 422-424
  - src/innertube.cpp:694-708, 712-757, 773-787, 969-977
  - the six instances built from src/main.cpp:215/227/240/243/250/298
  - src/appdatabase.cpp:99
  - Check that the Recommender's InnerTube does not live on its worker thread (src/recommender.cpp:360): a cookie jar is not thread-safe.
- **Test:**
  - bench_B with `--settle 0` (the results_B_launch0 conditions): wait 0 ms in ≥ 95% of launches, TTFA median ≤ 700 ms, at most 1 home-page fetch.
  - With a corrupted id: the log shows refresh + retry, and the song still resolves on the InnerTube tier.
  - A first-ever launch behaves as today.
  - Compare browse, search and next with and without the cookie.
- **Don't copy:** D2, L4.

#### 4. LY-4: Lyrics before the pane opens (S)
- **Change:** Look up the playing song when it starts, and the next row, and pre-open TLS to the providers. Opening the pane then becomes a SQLite read.
  - Background lookups use only today's providers (LRCLIB and YouTube Music). Newer third-party providers run only while the pane is open, or with an opt-in.
  - No next-row lookup on a metered connection.
  - One in-flight lookup per videoId, and never a lookup while the duration is still 0.
  - Setting "Look up lyrics in the background", on by default.
  - Wire it to the queue's current and next rows from main.cpp, not through prefetchUpcoming, so it stays clear of the branch.
- **Files:** Main.qml:574-579; src/lyrics.cpp:134-163, 193-213; src/main.cpp:242-244; views/SettingsView.qml
- **Test:** New `--lyrics-prefetch-test`: two queue rows with the pane closed, then both must have stored rows. Time from pane open to lines: p90 ≤ 50 ms.
- **Don't copy:** L4, L8.

#### 5. LY-5: Lyrics race (L)
- **Change:** A LyricsProvider interface and a race coordinator.
  - Every enabled provider starts at once. Lazy ones start only if nothing is found.
  - Answers are harvested in your order, with a 6 s wall-clock deadline per provider.
  - The first timed answer that passes the gate shows at once. After a patience window, the best answer in hand shows.
  - A higher-ranked word-synced answer may replace the shown answer only while the pane is hidden or before the first sung line.
  - Line-synced and plain answers keep separate slots.
  - Losers are aborted.
  - Each provider's outcome is cached per (videoId, provider). Errors stay in memory only.
  - One shared network manager, and a generation counter to drop stale answers.
- **Dependencies and tuning:** It lands after PP-03 and uses its shared InnerTube holder. The YouTube Music leg gets a cancellable Slot. The patience window starts at 1.2 s and is tuned from the race test and live data.
- **Files:**
  - src/lyrics.h:52-127; src/lyrics.cpp:193-392
  - new src/lyrics/{lyricsprovider.h, lyricsrace.*, lyricsstore.*, providers/lrclibprovider.cpp, providers/ytmlyricsprovider.cpp}
  - src/innertube.h:387-407; src/innertube.cpp:1244-1287
  - src/appdatabase.cpp:190-198; CMakeLists.txt:49
- **Test:** Offline `--lyrics-race-test` with scripted fake providers. It checks harvest order, patience, the upgrade rule, the separate slots, error ≠ miss, aborted losers and stale generations. Then dead_provider.ps1 and batch.ps1.
- **Don't copy:** L1, L2, L3, L4, L6, L9, L11, L14, L15.

#### 6. LY-1: Stop pinning fallback answers (S)
- **Change:** Record miss versus error for each provider.
  - An answer shown only because a better provider errored is stored as provisional. It is asked again on the next view, or after 1 h.
  - Found answers get a refresh age (e.g. 60 days). "None" keeps its 3 days. A manual pick is never replaced automatically.
  - The reverse case too: when LRCLIB misses cleanly and YouTube Music errors, no "none" row may be stored (lyrics.cpp:340-347).
- **Files:** src/lyrics.cpp:28-29, 215-233, 265-268, 331-347, 351-365; src/lyrics.h:123-126; src/appdatabase.cpp:190-198 (ALTER TABLE ADD COLUMN, so existing rows read as final)
- **Test:**
  - dead_provider.ps1, then a real rerun on the same MONOLIST_DATA_DIR: #9 must come back synced from LRCLIB.
  - YouTube Music blackholed on a track LRCLIB lacks: no "none" row stored.
  - The cached pass holds at 28/48 ms.
  - An old database still loads.
- **Don't copy:** L14, L3.

#### 7. PP-04: Bookkeeping after the load (S)
- **Change:** In handleResolved, call load() first, then startListening and prefetchUpcoming.
  - Record the play only if the load was accepted.
  - For closePlayEvent: capture the position and ids synchronously where it happens today, then run the UPDATE in a `QTimer::singleShot(0)` right after stop(), so a failed resolve or a quit cannot lose it.
  - Collapse the double reloadRecent: playRecorded → reloadHistory → historyChanged → reloadRecent again (main.cpp:253-255).
- **Files:** src/playbackcontroller.cpp:823-831, 927-946 (closePlayEvent at 932), 1001-1011, 1036-1059; src/main.cpp:253-255
- **Test:**
  - Instrumented bench_B seq/skip/again: resolved-to-loadfile < 10 ms.
  - `--listen-test` and `--scrobble-test` pass.
  - history, recent and play_events counts are identical over 12 tracks.
  - Measure on a copy of a realistically sized history.
- **Don't copy:** D2.

#### 8. PP-05 with QT3 merged in: Refusal rescue (S)
- **Change:** When mpv refuses a fresh InnerTube link, ask /player once more (~0.2 s) before falling to itag 18. Once per track only.
  - Muxed stays ahead of yt-dlp.
  - Rescue links (muxed, yt-dlp) are stored under their own key and live for this play only; seeks within the same play reuse them.
  - A resolve from the top never answers with a rescue link, so the next play is Opus again.
- **Files:** src/streamresolver.cpp:99-128, 157-166, 267-277, 559-575; src/streamresolver.h:131-139, 169; src/playbackcontroller.cpp:154-178; src/playbackcontroller.h:291-295
- **Test:**
  - `--play <id> 30 --spoil` must log the fresh rung and play within ~1 s.
  - A replay after a rescue must log itag 251 or 140.
  - The 13:32 track with `--at 600`, over 8 or more fresh resolves: a seek beyond the buffer makes mpv send a second request, the likeliest real trigger of "one request, then 403". Log whether each seek ends in a rescue, a skip at end of file, or a clean seek.
  - Log every in-app 403 with its rung and outcome over ≥ 200 opens. Keep the rung only if it rescues ≥ 50%.
- **Don't copy:** S2, S4, D3.

#### 9. NC-1 (new): A second no-cipher client (S)
- **Change:** Add one of the paper's no-cipher clients as a rung inside InnerTube::player (§3.2.1; e.g. ANDROID_VR or the visionOS 0.1 identity, chosen by a curl test on the 12 benchmark ids). It is tried only when VISIONOS returns no plain URL, or LOGIN_REQUIRED after PP-03's retry, and always before yt-dlp. Add a canary id to the benchmark so a VISIONOS break shows up.
- **Files:** src/innertube.h:201 (Client enum); src/innertube.cpp:126-147, 947-1038
- **Test:**
  - setTestServer (innertube.cpp:689-692) with VISIONOS failing: the second client must answer at ~+0.2 s.
  - resolve_innertube.ps1 with the second client forced, over the 12 ids: plain URLs (no n=, no pot=) that play.
- **Don't copy:** D3. Do not walk several clients at 8 s each; PP-07's deadlines apply.

#### 10. LY-3: Shared query cleaner (S)
- **Change:** Replace searchTitle and leadArtist with a shared LyricsQuery.
  - Take the artists from QueueTrack::primaryArtist and the credits, and never split on '&' alone.
  - Keep version markers even inside noise brackets: '(Official Live Video)' becomes '(Live)'.
  - Strip '(with X)' and 'feat. X' only when X is a credited artist.
  - Stop an unbracketed 'feat.' at the next bracket.
  - The same module scores candidates on title, artist overlap and duration, for LY-5's gate.
- **Files:** src/lyrics.cpp:38-69, 237-248; src/queuemodel.h:21-24; src/queuemodel.cpp:51-52; new src/lyricsquery.h/.cpp; CMakeLists.txt:49
- **Test:**
  - Offline `--lyrics-query-test` built from bench\skeptic_c25_c48\lyrics_query_cases.json: 0 of 18 may fail.
  - Scorer cases: 'Anti-Hero' must not match 'Hero', and an artist-only hit must be rejected.
  - batch.ps1 stays at 28/28.
- **Don't copy:** L11, L13.

#### 11. LY-2: LRCLIB exact lookup first (S; after LY-3)
- **Change:** Query LRCLIB's database-only `/api/get-cached` with title, artist, album and duration, and take its synced, plain or instrumental answer.
  - Run today's `/api/search` only on a 404, or if get-cached has not answered within ~250 ms.
  - Tighten the plain fallback to ±10 s.
  - One wall-clock deadline for the whole provider.
- **Files:** src/lyrics.cpp:24-27, 31, 235-256, 258-323
- **Test:** lrclib_audit.ts and batch.ps1 -Extras:
  - #24 must return the right song's text (the one /api/get gives).
  - #16 must still come back synced through search.
  - 28/28 vocal tracks line-synced or better.
  - Cold median ≤ 453 ms, p90 ≤ 608.
- **Don't copy:** L9, L13.

#### 12. QT2: Gapless (L; after PP-01 and PP-02)
- **Change:** Once the next song's entry is known, append it with `loadfile <url> insert-next -1 <options>`.
  - Set prefetch-playlist=yes, behind a switch, because mpv calls it experimental.
  - Set gapless-audio=yes on Windows, where all 43 WASAPI output inits were shared at 48 kHz. On a Mac, measure before choosing yes or weak.
  - The controller treats the start of the appended entry as the advance, and runs only the bookkeeping half of a split beginTrack.
  - Every other action keeps today's replace path.
  - start, headers and audio-files become per-file options.
  - Appended entries are monolist:// URLs, so their headers live in the cache entry.
  - Add a libmpv ≥ 0.38 check to cmake/FindMpv.cmake.
  - If the PP-01 spike fails: append plain googlevideo URLs instead, and play rescue links that need headers through the replace path.
- **Files:** src/mpvengine.cpp:122-166, 304-325, 343-404; src/mpvengine.h:103-111; src/playbackcontroller.cpp:133-187, 758-770, 927-1034, 945-946, 1162-1177, 1222; cmake/FindMpv.cmake
- **Test:**
  - bench_A_gap median ≤ 20 ms.
  - A 1 kHz tone split into two files, loopback-recorded: no dropped samples at the seam.
  - 0 audio-output re-inits in the -v logs.
  - `--spoil` on the appended entry must still rescue it.
  - Listen and scrobble counts unchanged.
  - Also: a queue edit, Previous at 0:02, repeat-one, repeat-all at the end of the queue, shuffle, video on, and a resume after 35 min.
- **Don't copy:** D1, D7, S3.

#### 13. PP-07: Bounded resolve (M, in two parts)
- **7a, before the merge:** Make cancelling a resolve-only yt-dlp non-blocking on both Windows (the job wait) and Unix (waitForFinished(1000), ytdlp.cpp:110). Log why InnerTube failed and how long it took.
- **7b, after the merge:**
  - A 20 s overall deadline for each resolve.
  - A total (not idle) deadline of ~3 s for /player, with one hedged second /player at ~1.2 s on a separate connection: a second network manager, or HTTP/2 off. Otherwise Qt 6 multiplexes it onto the stalled connection; the existing 1.2 s retry at innertube.cpp:919-933 has the same flaw.
  - yt-dlp starts at ~3 s instead of after 17.2 s.
  - One yt-dlp process at a time for resolves: a foreground resolve goes first, and prefetch keeps its yt-dlp rung but waits. New download extractions do not start while a foreground resolve runs.
- **Files:** src/innertube.cpp:61-63, 759-825, 838-945, 947-1038; src/streamresolver.cpp:279-303, 308-343, 345-386, 577-584; src/ytdlp.cpp:87-111; src/downloadmanager.cpp:245-257
- **Test:**
  - /player delayed 10 s: the hedge at ~1.2 s, yt-dlp at ~3 s, an answer within ~6 s.
  - www.youtube.com blackholed: the skip notice within 20 s.
  - A skip during a yt-dlp resolve, under a UI watchdog: no event-loop gap over 100 ms.
  - bench_A_play medians unchanged.
- **Don't copy:** D2, D3, D10, §3.2.4's concurrency collapse.

#### 14. PP-06: Recovery gaps (M)
- **Change:**
  - On a stream, an end of file before MpvEngine's own duration minus max(5 s, 3%) counts as a failure: re-resolve the same rendition once and resume at the same position.
  - A refused downloaded or local file streams the song instead of halting.
  - A new "audio started" signal (PLAYBACK_RESTART, or the first time-pos > 0) is the only thing that resets the three-failures counter.
  - This also covers QT2's end-of-file check.
- **Files:** src/playbackcontroller.cpp:24-27, 133-187 (180-186 halts), 1001-1011, 1044, 1075, 1107-1117, 1162-1177; src/mpvengine.cpp:304-325
- **Test:**
  - A local HTTP server that closes at 60% (later `--broker-test`): playback must resume, and next() must not be called.
  - A junk file in a scratch MONOLIST_DOWNLOAD_DIR: the song streams instead.
  - Three failures, then one rescue: the counter behaves.
- **Don't copy:** §5.2 / D.3's silent end, S2, L14.

#### 15. QT1: Loudness (M; estimated)
- **Change:** Read the chosen format's loudnessDb in InnerTube::player. The top-level audioConfig.loudnessDb was missing on 13 of 13 probes; fall back to perceptualLoudnessDb + 14.
  - Store the value per videoId.
  - Pass clamp(-loudnessDb, -15, 0) dB to mpv as a per-file replaygain-fallback.
  - Keep replaygain=track so tagged files win, and add replaygain-preamp=+4 and replaygain-clip=yes.
  - Only ever turn songs down; a boost would need a limiter.
  - Setting "Even out loudness".
  - The ebur128 check in B0 decides whether the estimate holds.
- **Files:** src/innertube.cpp:947-1038; src/streamresolver.h:133-138; src/streamresolver.cpp:308-343, 559-575; src/appdatabase.cpp:190-198; src/mpvengine.cpp:154-155, 343-404; src/playbackcontroller.cpp:34-40, 317, 1001-1011, 1036-1059; views/SettingsView.qml:263-315
- **Test:**
  - A unit test of the gain function.
  - MONOLIST_MPV_LOG=v shows 'Applying fallback gain' = -loudnessDb for the 13 ids.
  - An ebur128 pass predicts the spread after gain: ≤ 4.7 dB.
  - A replay, a download, a --spoil rescue and a JioSaavn win all log the same gain.
  - Toggle off: 0 dB.
- **Don't copy:** Q6, Q7, A6.

#### 16. LY-7: Wave 1 providers (L)
- **Change:** BiniLyrics, BetterLyrics, LyricsPlus (the last good mirror first), SimpMusic, Unison and Portato. Also a trial of YouTube Music's lyrics browse sent as a mobile client, to see whether it returns line timing.
  - LRCLIB stays first by default until `--lyrics-providers` shows each provider's correct-recording rate on the 30-track list and on a long-tail list.
  - A word-timed answer may upgrade LRCLIB's line-synced answer (LY-5's rule), but never outranks it on the text.
  - BetterLyrics, Portato and Unison share one operator (boidu.dev), so they get one rate limit and one disclosure line.
  - None of them is used by background lookups (LY-4).
  - Per-provider toggles and credits.
- **Files:** new src/lyrics/providers/{binilyrics, betterlyrics, lyricsplus, simpmusic, unison}.cpp; src/innertube.h:201; src/innertube.cpp:126-147, 1244-1287; views/SettingsView.qml:911; README.md:128-137
- **Test:** `--lyrics-providers "<query>"` over both lists, reporting found/miss/error, timing class and ms per provider. batch.ps1. A reachability check from your region.
- **Don't copy:** L2, L7, L11, L16, L17.

#### 17. LY-10: Provider menu (M)
- **Change:** Replace SEARCH AGAIN with a menu of the enabled providers, each with its state for this song:
  - not fetched;
  - fetching;
  - found, with its timing class;
  - not found;
  - error, with a retry.

  A found result applies at once from the cache. A provider not yet fetched starts a single-provider lookup. Your pick is saved. Settings gets reorder and on/off controls.
- **Files:** components/LyricsPane.qml:217-252; views/NowPlayingView.qml:411-421; src/lyrics.h; views/SettingsView.qml; settings key lyrics_providers
- **Test:** `--lyrics-race-test` covers the state machine. snap.ps1 screenshots. A manual pick must survive a restart and a background refresh.
- **Don't copy:** L14, L15, and §10.4's generation rule.

#### 18. LY-6 + LY-8: Word timing (M + M; conditional)
- **Condition:** Build these only if LY-7 finds word timing on, say, 10 or more of the 28 vocal tracks. The threshold is your call.
- **LY-6:** A Line gains words, background vocals and agent fields, and the raw payload is stored with a format tag. We write our own parsers:
  - enhanced LRC;
  - karaoke LRC, in both readings;
  - Apple TTML via QXmlStreamReader, reading ttm:agent namespace-aware and accepting every clock form;
  - LyricsPlus JSON.
- **LY-8:** WordLine.qml: a Flow of words with a wipe driven by FrameAnimation, and its own word lead of 0-50 ms. No scaling or glow, per DESIGN.md.
- **Files:** src/lyrics.h:20-24; src/lyrics.cpp:21-23, 91-114, 394-410, 423-486; new src/lyrics/lyricsformats.*; components/LyricsPane.qml:100-165; new components/WordLine.qml; CMakeLists.txt:225
- **Test:**
  - `--lyrics-format-test` with our own fixtures.
  - snap.ps1 at 720 px, 480 px and narrow widths, plus an Arabic line.
  - The QML profiler.
  - bench_D_cpu with the pane open: at most +1 point.
- **Don't copy:** L5, L8, L12, and §10.6's agent bug (reading ttm:agent by qualified name).

#### 19. PP-08: Long-file range reads (M; inside the cache only)
- **Change:** Keep a single request while ≤ 8 MiB remains to fetch. Above that, read sequential 4 MiB ranges on the kept-alive connection.
  - A mid-file 403, 404 or 410 calls refresh(videoId, itag) and continues at the same offset.
  - A short read is reopened at most 3 times, then fails loudly.
  - The mpv curl-max-request-size stop-gap is dropped.
  - JioSaavn's CDN gets a single request until its pacing is measured.
  - Build this only if B0 shows that long tracks are a real share of your listening.
- **Files:** src/audiobroker.*; src/streamresolver.* (refresh)
- **Test:**
  - bench_C_throughput and bench_C_rangesize through the app, on the 13:32 track and two more tracks over 10 min: the whole file arrives in < 10 s.
  - The fake server refuses after the first range: no mpv error, and the entry's md5 equals a whole-file download.
  - Re-measure the 8-12 MiB threshold on ≥ 3 long tracks.
- **Don't copy:** §5.2's silent end, D5, D6.

#### 20. QT4: Format choice (S)
- **Change:** An ordered rule: Opus 774, then Opus 251, then AAC 141, then AAC 140, then the rest by bitrate.
  - Skip isDrc formats and 'drc' xtags.
  - Keep the itag 18 fallback.
  - The choice is deterministic, and itag, codec and kbps are logged with every resolve.
  - It lands before the merge and before PP-01. Passing kbps into the JioSaavn race waits for the merge.
- **Files:** src/innertube.cpp:979-1003, 1035
- **Test:**
  - Saved /player fixtures with the URLs stripped:
    - IPeJ7iM55hc gives 251;
    - CmThpha4Hoo gives 140;
    - a synthetic DRC 251 is skipped;
    - a response with only formats[] gives 18.
  - -v logs over the 12 ids show swresample only where no Opus exists.
- **Don't copy:** Q7, Q8, S4.

#### 21. QT5: Stream info (S)
- **Change:** Observe audio-codec-name, audio-params, audio-out-params, current-ao and the gain actually set. Write one log line per file start and one status line in Now Playing. It is built as ROADMAP P06's engine half: one line, not a second label.
- **Files:** src/mpvengine.cpp:168-185, 194-341; src/playbackcontroller.cpp:1036-1059; views/NowPlayingView.qml:411-421
- **Test:** An Opus track, an AAC track (CmThpha4Hoo) and a download: the line must match mpv -v's 'ao/wasapi' and swresample lines.
- **Don't copy:** Q3, Q8.

#### 22. PP-09: Resolver hygiene (S)
- **Change:**
  - Join a running resolve for the same id instead of restarting it.
  - A late /player answer fills the link cache only if no newer entry exists for that key.
  - A 10-minute "unplayable" verdict per source, written only for UNPLAYABLE or ERROR together with a yt-dlp "unavailable" answer. A song is skipped only when every enabled source, JioSaavn included, has a verdict.
  - Verdicts clear on a sign-in change or a network change.
  - The link cache is capped at ~256 entries (LRU), and links that have not yet served bytes live 20 min.
- **Files:** src/streamresolver.cpp:99-149, 245-259, 279-303, 308-343, 611-619; src/streamresolver.h:116-139, 169; src/playbackcontroller.cpp:758-770, 1061-1105
- **Test:**
  - A fake UNPLAYABLE /player plus a failing yt-dlp: the second resolve fails in < 50 ms with no process started.
  - Network off: no verdict stored.
  - A double resolve logs a single /player.
  - Skip away mid-resolve, then Previous: a cache hit.
- **Don't copy:** §4.4's SharedCalls leak, L14, S4, §4.5.

#### 23-28. Remaining lyrics items (one build each)
- **LY-11a:**
  - KuGou: search, then download, with duration within 8 s. Credit lines are stripped only from bounded head and tail windows.
  - YouTube captions: get_transcript on our videoId, lazy, last among the timed providers, with '[Music]' cues dropped.
  - Files: new providers kugou.cpp and ytcaptions.cpp; src/innertube.cpp:130-132.
  - Test: a 31-line KuGou fixture ending in a credit keeps every lyric line; count "none" before and after on a long-tail list.
  - Don't copy: L6, L17.
- **LY-9:** A Translate toggle.
  - Use the payload's own translation when it has one; otherwise a Translator interface.
  - Batches of up to 3,500 characters, a piece-count mismatch fails the whole batch, and results are cached per (videoId, payload hash, language).
  - A DeepL key needs SecretStore, which means the Keychain on a Mac (ROADMAP C05).
  - Files: new src/lyrics/translator.*; src/lyrics.h; components/LyricsPane.qml:127-153; src/appdatabase.cpp; src/secretstore.h:49-54; views/SettingsView.qml.
  - Test: an offline batching test; the 16 non-English tracks, with the first translation ≤ 1 s and cached ≤ 50 ms.
  - Don't copy: L4, L14.
- **LY-13:** Read .lrc sidecars, and mpv's 'metadata' lyrics, for local files.
  - After a download, write a .lrc file, and embed the lyrics with `-map 0 -c copy -map_metadata 0` into a temp file, then rename it. This keeps the cover.
  - Files: src/lyrics.cpp:205-208; src/mpvengine.cpp:~170; src/downloadmanager.cpp:31-52, 259, 343-395, 459-493, 631-670; src/ytdlp.cpp:575, 599-618.
  - Test (real downloads, with your OK): .opus, .m4a and .mp3 each show their lyrics and cover in ffprobe and play offline; `--download-cleanup-test` passes with a .lrc present.
  - Don't copy: L5, L10, §9.2.
- **LY-14:**
  - A global offset of ±5,000 ms in 100 ms steps, plus an optional per-song override that is dropped when the provider changes. The offset is applied to the clock.
  - Files: src/lyrics.cpp:21-23, 394-421; components/LyricsPane.qml.
  - Test: highlighting and tap-to-seek stay symmetric under an offset.
  - Don't copy: §10.7 (the offset applied to the lines), L8.
- **LY-12:**
  - TTML x-bg background vocals, or failing that a trailing balanced '(…)'.
  - Duet voices marked by an indent and a tick, not by right alignment, per DESIGN.md.
  - Gap lines at pauses of 4 s or more.
  - Files: src/lyrics/lyricsformats.cpp; src/lyrics.h; components/LyricsPane.qml:104-153.
  - Test: fixtures, and a screenshot of a duet.
  - Don't copy: §10.6's agent bug.
- **LY-11b:**
  - JioSaavn plain lyrics, through the merged branch's verified match.
  - Genius and Megalobiz as lazy scrapers, off by default.
  - Test: parsing fixtures.
  - Don't copy: L11.

#### 29-32. Optional items
- **QT6 crossfade (off by default):**
  - Needs QT2's beginTrack split.
  - No crossfade between adjacent tracks of an album, for tracks under 45 s, or in video mode, and none in exclusive mode.
  - The incoming player's clock drives the fade.
  - Files: src/main.cpp:211; src/playbackcontroller.h:280; new src/crossfade.*; src/playbackcontroller.cpp:387-449, 927-1034, 1306-1316; src/videosurface.cpp:66.
  - Test: the incoming song starts within ±50 ms of plan; the loopback sum stays within ±1 dB; pause and skip mid-fade; --spoil on the incoming song; memory with one vs two players.
  - Don't copy: A5, A6, D1, §8.3's title guard.
- **LY-15:**
  - PaxSenix's keyed routes, off unless you paste a key.
  - The key lives in SecretStore only. The field is hidden where there is no backend, and the key never goes into the settings table.
  - One 15 s deadline for the whole route.
  - Test: `--secret-test`; no contact without a key.
  - Don't copy: L1, L15, L16.
- **QT8:**
  - Exclusive output, off by default.
  - It keeps software volume, and reads "exclusive, not bit-perfect" whenever the volume is below 100. Driving the device's own volume instead would change your system volume, and the change would persist after the app exits.
  - gapless-audio=weak, and triangular dither when the output is integer.
  - A shared-mode fallback with a notice.
  - Verify on a Mac before promising CoreAudio hog mode.
  - Files: src/mpvengine.cpp:122-166, 168-185, 465-471; src/playbackcontroller.cpp:372-449; src/main.cpp:2456-2556.
  - Don't copy: Q1-Q5.
- **LY-16:**
  - After the ROADMAP media-session item (ROADMAP.md:48): publish only the current synced line as the subtitle, only when it changes, and never for plain lyrics.
  - Don't copy: L4, L8.

**Deferred: PP-10 (downloads reuse the cache).** SponsorBlock "music_offtopic" is on by default (downloadmanager.h:175, ytdlp.cpp:618) and still needs yt-dlp, so this path would rarely run. Revisit once PP-01 is proven, and only when skipNonMusic is off and the cached itag matches the chosen format.

## Where I disagreed with the skeptic

I accepted most of the review; the corrections are written into the items above. On these points I disagreed, in part or in full:
1. **QT4 timing.** The skeptic wants it after the merge. The selection rule lives in src/innertube.cpp, which the branch does not touch: `git diff --stat main...jiosaavn` lists 18 files, and none of them is innertube.*, lyrics.*, mpvengine.*, ytdlp.* or downloadmanager.*. Landing it before PP-01 also means the cache keys never shift under a new ranking. Only the JioSaavn race wiring waits for the merge.
2. **LY-2 in parallel.** Running /api/get and /api/search side by side on every lookup brings back the 183 KB search payload that LY-2 exists to remove, and it doubles the load on a volunteer-run service. Instead: get-cached first, then search on a 404 or after ~250 ms without an answer. LY-4 hides the remaining serial cost.
3. **PP-07, pausing downloads.** I would not pause a running download: stopping yt-dlp mid-transfer means starting again, because partial files are deleted today (C33). Holding back new download extractions while a foreground resolve runs gives the same priority at no cost.
4. **The macOS pass.** It should not block the Windows work. It is required before the constants (the 8 MiB threshold, the gapless mode, the watchdog) are trusted on a Mac, which belongs with ROADMAP M06.
5. **PP-01 as XL.** I rate it as three phases (M + L + M) with a hard gate after the spike. The gate matters more than the label.
6. **QT1 "not measured".** Only partly true. The 13-track input is YouTube's own measurement, the one it uses for its own normalisation. What is unmeasured is the result on our output. So it is labelled as an estimate and scheduled after the speed items, but it still ranks above the lyrics items that have no numbers at all.
7. **LY-7 relay policy.** I agree it is your call. My recommendation: allow relays that serve Apple or QQ data from their own servers, as long as no secret ships in our app. Each gets its own toggle and a disclosure, and none is used by background lookups.

## Decisions for you
1. Licence: stay MIT and write everything from the paper (recommended), or relicense to GPL-3.0 and port the reference. See below.
2. U01: may a fresh InnerTube link go before itag 18 (PP-05)?
3. Loudness (QT1): on by default? And raise the default volume from 0.65 at the same time?
4. The lyrics relay policy for LY-7 (disagreement 7).
5. Background lyrics lookups (LY-4): on by default, with LRCLIB and YouTube Music only?
6. Cache size (PP-01): 1 GiB by default?
7. The word-timing threshold that unlocks LY-6 and LY-8 (suggested: 10 of 28 tracks).
8. Default translator (LY-9): Google's keyless endpoint, or LibreTranslate/DeepL?
9. Crossfade default (QT6): off?
10. Permission for B0 to run read-only counts on your real library, and for LY-13's tests to make real downloads.

## In flight (not re-planned)
**JioSaavn (branch jiosaavn, worktree C:\dev\monolist-wt\saavn, commits ac22ba1 and 855317d, 18 files, +3,435 lines).**
- **Must wait for the merge:** every item that edits handleResolved, the loadFailed handler, StreamResolver::succeed or prefetchUpcoming. That is PP-01, PP-02, PP-04, PP-05, PP-06, PP-07b, PP-09, QT1's wiring and QT2.
- **Can land before it**, with only small textual conflicts in main.cpp and appdatabase.cpp: PP-03, PP-07a, NC-1, QT4 and the lyrics items.
- **Re-baseline right after the merge.** The race holds YouTube's answer for up to 1.2 s (kSaavnGraceMs, branch streamresolver.cpp:28) for songs with no cached verdict, so every "must not regress" gate is reset from the new numbers.
- **What this plan needs from the branch:** the verdict's source, matcher version and settings (to key PP-01's pin), and a "verdict final" signal (for PP-02).
- **Handed to the JioSaavn work:** the mid-song upgrade (QT7; first count its "did not answer within 1200 ms" lines per race), measuring the JioSaavn CDN's pacing before any chunking applies there, and JioSaavn lyrics (LY-11b).

**[SIGN-IN SLOT: pending the Metrolist-style Google sign-in research.]** One slot replaces PP-11, QT9 and LY-15's slot. Nothing is built until the research reports.
- **Streaming:** where a signed-in client sits in the InnerTube tier (first, or a quality upgrade after an anonymous start). The anonymous VISIONOS → yt-dlp → muxed ladder stays underneath.
- **Formats and loudness:** QT4's order already prefers 774 and 141, and QT1 reads their per-format loudnessDb. The JioSaavn race must compare real kbps.
- **visitorData:** PP-03's holder accepts a session-bound id that outranks the anonymous one.
- **Cache:** PP-01's keys carry the rendition and the auth class, so a signed-in link never serves a signed-out play.
- **Lyrics:** whether signed-in YouTube Music returns timed lyrics.
- **Revisit:** the C9/C10 rejections (cipher and player-JS cache, PoToken) if the research needs a WEB or WEB_REMIX client.
- **Rules:** cookies and auth headers are never logged and never written to the cache index.
- **Gate:** signed-out benchmark medians unchanged.

## Not recommended
- **1 MiB chunking for every read** (§5.2, §11 row 1). Normal songs are faster as one request (6.0 MB/s against 0.9-5.7 MB/s), and every extra request risks "first request served, then 403" (2 of 8 fresh long-track URLs). The mpv curl-max-request-size stop-gap is dropped for the same reason.
- **A probe request before trusting a URL** (C11, D6). It costs ~200 ms on every cold play and would catch 1.1% of opens, all of which the first real read catches.
- **mpv's own disk-cache options as the byte cache.** They are per-file, discarded at close, not byte-exact and not keyed by identity.
- **Sending the minting client's UA or per-client headers, or unifying the IP family, as a fix.** No effect across 5 URLs x 8 variants. The IP family is only logged, in the PP-01 spike.
- **Changing the start threshold or buffer sizes** (C19, C20). Audio already starts 49 ms after the first bytes, with 0 stalls in 2,319.7 s. The only change is removing the dead demuxer-readahead-secs=20 (mpvengine.cpp:152).
- **Racing two InnerTube clients on every play.** 96 of 96 /player calls succeeded. NC-1 and PP-07's single hedge cover the tail.
- **A cipher or player-JS disk cache, and PoToken** (C9, C10). 0 of 178 URLs needed them. Revisit if the sign-in research needs a web client.
- **Other BitChord mechanisms:**
  - the fixed 8 s prefetch delay (D7);
  - the 20-minute TTL that ignores expire= (0 of 46 cached links were refused);
  - the span evictor (whole-entry LRU does the same job);
  - 16 connections per host;
  - NewPipe fast paths;
  - a wake lock (whole songs download in about a second; measure first).
- **Audio-path items:**
  - resampler changes (the shared mixer runs at 48 kHz anyway; QT4 removes the needless AAC picks);
  - Automix (AGPL-derived, ~7 s of analysis per track);
  - porting PrecisionAudioSink (our path is float already);
  - 16-bit output;
  - standalone dither;
  - a global normaliser;
  - a +3 dB boost without a limiter;
  - DRC filtering as its own project;
  - acrossfade inside one mpv;
  - the version-swap aligner;
  - a vendor-codec blacklist;
  - EQ and spatializer (app work, in the ROADMAP).
- **Lyrics routes that depend on secrets taken from other apps:**
  - Musixmatch's desktop API (a scraped HMAC secret);
  - PaxSenix's keyless Apple route (a scraped Apple JWT);
  - Spotify's internal API, KuGou KRC and NetEase eapi.
- **Lyrics behaviours that are BitChord defects:**
  - a blocking ISRC step before the race (L2);
  - waiting for every provider (L1);
  - racing all LyricsPlus mirrors (L7);
  - dropping our result cache (C43 is where we lead);
  - separate lookups per consumer (L4);
  - captions ranked 10th (L17);
  - the "prioritize syllable sync" toggle (L3).
- **Porting the reference JavaScript or its 546 fixtures.** See the licence section.

## Licence
**Recommended: stay MIT and write our own from the paper as a specification.**
- We use facts: endpoint names, parameters, field names, thresholds and constants.
- We never open or copy the reference JavaScript or its fixtures. We write our own test fixtures.
- **Cost:** extra work where a reference module exists, mostly in the lyrics race, parsers and providers, and in the chunked transport.
- **Why this is the better choice:** the reference is JavaScript and Monolist is C++/Qt, so a "close port" would still be a rewrite. It would save design and debugging, not typing, and §10.2's pseudocode plus §10.6 already specify most of what those modules do.
- **Relicensing costs:** it would bind every future contributor and fork, and it needs the consent of every copyright holder.
- **A note on binaries:** the libmpv-2.dll we build against appears to be a GPL build. Its build string has no -Dgpl=false and enables dvdnav and Rubber Band, which are GPL. As the README's licence section already warns, a packaged release that ships it falls under GPL terms either way. So relicensing mainly changes the source licence. Staying MIT keeps the source reusable, and keeps an LGPL libmpv open as an option.

**Alternative: relicense to GPL-3.0 and port closely.** A close port would have been possible for these items; none of the modules was opened:
- PP-03: sources/youtube.js (if it holds the visitorData code)
- PP-05: youtube.js onPlaybackRefused
- PP-06: transport/chunkedFetch.js TruncatedStreamError
- PP-07: lib/http.js timeoutSignal and sources/resolve.js raceWithFallback
- PP-08: chunkedFetch.js chunkedStream and rangeBytesFor
- PP-09: sources/cache.js ttlLru, singleFlight and sharedCalls
- NC-1: the client table in youtube.js (the client names are facts either way)
- QT4: youtube.js format selection
- QT7: resolve.js worthSwapping, sameRecordingAs and upgradeFor
- LY-2: the lrclib provider
- LY-3: lyrics/query.js
- LY-5: lyrics/repository.js and index.js
- LY-6: lyrics/formats/ttml.js, lrc.js and provider-payload.js
- LY-7, LY-11 and LY-15: lyrics/providers/*.js
- LY-10: createLyricsController
- LY-12: lyrics/postprocess.js

There is nothing to port for PP-01 (the reference has no disk cache), PP-02, PP-04, PP-10, QT1, QT2, QT5, QT6, QT8, LY-1, LY-4, LY-8, LY-9, LY-13, LY-14 or LY-16.

## Implementation order

- B0. Measure first; no app changes:
- With your OK, read-only counts from your real library: the share of plays that repeat a song heard within the last ~1 GiB of listening (PP-01's value), and the share of plays longer than 7.5 min (PP-08's value).
- Today's app on the 13:32 track with --at 600, over 8 fresh resolves: does a seek hit 'one request, then 403'?
- ffmpeg ebur128 over the 13 loudness-probe tracks (QT1's estimate).
- A curl test of the candidate no-cipher clients on the 12 benchmark ids (NC-1).
- B1. PP-03, before the merge: one stored, shared visitorData with a 30-day cap, one shared cookie jar across the six InnerTube objects, and a LOGIN_REQUIRED refresh-and-retry (innertube.*, plus construction in main.cpp).
- B2. PP-07a + NC-1, before the merge: non-blocking yt-dlp cancel on Windows and Unix; logging of why InnerTube failed; a second no-cipher client used only when VISIONOS fails, plus a canary id in the benchmark (innertube.cpp, ytdlp.cpp).
- B3. QT4, before the merge: Opus first, a DRC guard, and a deterministic choice with itag, codec and kbps logged (innertube.cpp). It lands before PP-01 so the cache keys stay stable.
- B4. Lyrics quick fixes, before the merge, in this order: LY-1 (no pinned fallbacks, in both directions), LY-3 (shared query cleaner and scorer), LY-2 (LRCLIB get-cached, then search).
- B5. LY-5 lyrics race, then LY-4 background lookups limited to LRCLIB and YouTube Music. LY-4 is wired from main.cpp to the queue's current and next rows, not through prefetchUpcoming. Needs B1.
- B6. Merge the jiosaavn branch, then re-baseline: re-apply instrumentation.diff, rerun every Step 2 suite, and record how long the race holds YouTube's answer. The new medians replace 553.5, 421.5, 412.5 and 334 ms as the gates.
- B7. PP-04 + QT5: history and play-event writes after the load (singleShot write, one reloadRecent); the stream-info line, which is ROADMAP P06's engine half and the instrument for later batches; remove the dead demuxer-readahead-secs=20.
- B8. PP-05 (with QT3) + PP-06: a fresh InnerTube link before itag 18, and rescue links scoped to one play; early end-of-file detected against mpv's own duration; refused downloads stream on; the failure counter resets only when audio really starts. Confirm the U01 order first.
- B9. PP-07b: a 20 s resolve deadline; a total /player deadline with one hedged request on a separate connection; yt-dlp at ~3 s; one yt-dlp at a time, foreground first.
- B10. PP-01 phase 1 spike: a monolist:// read-through with no disk (partial reads, 5 s watchdog), measured against the B6 baseline for open time, pacing, 403s and IP family. Decide there: continue, switch to the 127.0.0.1 variant, or drop the byte cache and build QT2 on plain URLs.
- B11. PP-01 phase 2: whole-file entries up to 8 MiB in CacheLocation with no URLs on disk; a pin carrying the JioSaavn verdict metadata; 1 GiB LRU, a Clear action and a kill switch.
- B12. PP-02: fill the next song once the current entry is complete and the next verdict is final; the row after gets a link only (InnerTube tier); the launch fill starts after Home loads; metered connections fetch links only.
- B13. LY-10 provider menu and Settings section, then LY-7's wave-1 providers one at a time behind --lyrics-providers. LRCLIB stays first until measured. Needs your ruling on the relay policy.
- B14. QT2 gapless on monolist:// entries: FindMpv >= 0.38 check, split beginTrack, per-file options, prefetch-playlist behind a switch.
- B15. PP-01 phase 3 + PP-08: partial entries, refresh(videoId, itag), and 4 MiB ranges above 8 MiB. Only if B0 shows long tracks matter.
- B16. QT1 loudness, after your decision on default-on and the default volume.
- B17. LY-6 + LY-8 word timing, only if B13 found word timing on enough tracks.
- B18. PP-09 resolver hygiene: joined resolves, guarded late answers, per-source unplayable verdicts, a bounded link cache.
- B19. Lyrics extras, one per build: LY-11a (KuGou, captions), LY-14 offset, LY-12 background vocals and duets, LY-9 translation, LY-13 lyrics in files (real downloads only with your OK), LY-11b (JioSaavn lyrics, scrapers off by default).
- B20. Optional, on request: QT6 crossfade (off by default), QT8 exclusive output, LY-15 (after ROADMAP C05), LY-16 (after the ROADMAP media-session item), PP-10 (once PP-01 is proven).
- SIGN-IN SLOT: fill it when the research reports; best after B1 and B11. Gate: signed-out benchmark medians unchanged.

## Re-measurement

All scripts are in the scratchpad's bench\ folder. Instrumented runs re-apply instrumentation.diff through build-bench.ps1.

- **B0:** itself a measurement. Record the library counts, the --at 600 outcomes (rescue, end-of-file skip or clean seek), the ebur128 LUFS per track, and the curl results per client.
- **B1 (PP-03):** bench_B.ps1 with --settle 0 (the results_B_launch0.json conditions). Check the visitorData wait (434.5 / 816 ms → 0 in ≥ 95% of launches), window-ready TTFA (1,048 / 1,460 → ≤ 700 median), home-page fetches per launch (6 → ≤ 1) and the googlevideo rate at window-ready (4.9 → ~6 MB/s). Also bench_A_play.ps1: 677 ms cold after launch must not regress.
- **B2 (PP-07a, NC-1):** bench_A_play.ps1 medians unchanged (677 cold, 553.5 new track). resolve_innertube.ps1 with the second client forced: 12 of 12 plain URLs that play. A UI-thread watchdog during a skip over a yt-dlp resolve: no gap over 100 ms.
- **B3 (QT4):** bench_B.ps1 seq with MONOLIST_MPV_LOG=v over the 12 ids. swresample appears only where no Opus exists (resampled opens were 19 of 176), and the seq median is unchanged.
- **B4 (LY-1/3/2):**
  - lyrics\dead_provider.ps1, then a rerun on the same data dir: #9 synced.
  - lyrics\batch.ps1: cold ≤ 453 / 608 ms, cached 28 / 48 ms, 28/28 line-synced; plus -Extras.
  - lyrics\lrclib_audit.ts: 28/28 correct text; response median 183 KB → 9 KB.
  - skeptic_c25_c48\regex_check.ps1: 9 of 18 wrong → 0.
- **B5 (LY-5, LY-4):**
  - dead_provider.ps1: 10,595 / 4,564 ms → ≤ the patience window.
  - batch.ps1: 'no lyrics' 860 ms → ~500; cold 453 / 608 must not regress.
  - New pane-open-to-lines timing: 453 → ≤ 50 ms p90.
  - --lyrics-race-test.
- **B6 (merge):** the full set, instrumented. bench_A_play.ps1 (cold after launch), bench_A_gap.ps1 (song-end silence), bench_B.ps1 seq/skip/again and --settle 0 (TTFA, skip, stage splits), bench_C_throughput.ps1 and bench_C_rangesize.ps1, bench_D_cpu.ps1 (CPU, memory). Also count the grace-timer lines. These become the new baselines.
- **B7 (PP-04, QT5):** instrumented bench_B.ps1 seq/skip/again. Resolved-to-loadfile goes from 54.5 / 78.5 / 45.5 ms to < 10, and the TTFA medians fall by the same amount. --listen-test and --scrobble-test pass, and table row counts are identical. The QT5 line must match mpv -v on an Opus track, an AAC track and a download.
- **B8 (PP-05, PP-06):**
  - --play <id> 30 --spoil: plays within ~1 s (was 3.4-5.4 s).
  - A replay after a rescue logs itag 251 or 140.
  - Every 403 is logged with its rung over ≥ 200 opens (bench_B.ps1 runs).
  - The --at 600 test from B0, repeated.
  - Truncation and junk-download tests.
- **B9 (PP-07b):** fake /player delayed 10 s: answer within ~6 s. Blackholed host: skip notice within 20 s. bench_A_play.ps1 medians unchanged.
- **B10 (PP-01 spike):** bench_B.ps1 seq within ±10 ms of the B6 baseline. bench_C_rangesize.ps1 and bench_C_throughput.ps1 through the broker: same pacing as curl, 6 MB/s on normal songs. 403 count and IP family per connection.
- **B11 (PP-01 store):** bench_B.ps1 again: TTFA-warm 421.5 → < 200 ms median, with 0 network bytes. Two launches of --play <id> 30 show cross-launch hits. The kill switch reproduces the B6 numbers. bench_D_cpu.ps1 for memory against the 359.5 MB peak.
- **B12 (PP-02):**
  - bench_B.ps1 skip: ≥ 90% cache hits; 412.5 → < 150 ms median.
  - bench_A_gap.ps1: 334 → < 100 ms.
  - Wasted-bytes counter; a metered run fetches 0 bytes.
- **B13 (LY-10, LY-7):** --lyrics-providers over the 30-track list and a long-tail list (found, miss, error and timing class per provider; word-synced share against 0/30). batch.ps1: line-synced or better stays ≥ 28/28, cold not slower. snap.ps1 screenshots.
- **B14 (QT2):**
  - bench_A_gap.ps1: ≤ 20 ms median.
  - bench_B.ps1 skip: no regression from B12.
  - -v logs: 0 audio-output re-inits.
  - Loopback tone test; listen and scrobble counts.
- **B15 (PP-08):** bench_C_throughput.ps1 and bench_C_rangesize.ps1 through the app on the 13:32 track and two more tracks over 10 min: 36.8 kB/s → whole file in < 10 s. The md5 check against a whole-file download.
- **B16 (QT1):** MONOLIST_MPV_LOG=v 'Applying fallback gain' per track, and the ebur128 spread after gain: 12.45 dB → ≤ 4.7.
- **B17 (LY-6, LY-8):** --lyrics-format-test. bench_D_cpu.ps1 with the pane open: 2.5% → ≤ 3.5% of one core. snap.ps1.
- **B18 (PP-09):** fake-UNPLAYABLE test: < 50 ms, no process. bench_B.ps1 unchanged.
- **B19:** per item.
  - LY-11a: --lyrics-providers on the long-tail list ('none' count).
  - LY-9: translation timing (≤ 1 s first, ≤ 50 ms cached).
  - LY-13: ffprobe on the downloads.
  - LY-14 and LY-12: fixtures and screenshots.
- **B20 (QT6):** the transition bench with a 4 s crossfade (±50 ms), and bench_D_cpu.ps1 memory with two players.
- **SIGN-IN SLOT:** the full B6 set signed out must match the then-current baselines.