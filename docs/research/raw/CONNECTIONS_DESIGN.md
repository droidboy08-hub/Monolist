# Connections design (Last.fm scrobbling, YouTube Music sign-in)

Produced by a research workflow on 2026-09-25 and ACCEPTED by the lead with every recommended option: Last.fm first, then YouTube Music read-only; Last.fm key and secret injected at build time from the environment variables MONOLIST_LASTFM_API_KEY / MONOLIST_LASTFM_SHARED_SECRET into a generated header (never committed, never logged); Last.fm desktop token flow in the system browser (retry getSession on window focus, poll every 3 s up to 10 min, plus an "I have approved it" button; never getMobileSession); an in-house SecretStore (DPAPI files on Windows; macOS Keychain later by the Mac session); YouTube Music by cookie import (file or pasted header) from a private window of the user's own browser; read-only scope; playback, search, lyrics and radio stay anonymous; liked songs import to a separate "Liked on YouTube Music" playlist; scrobble the primary artist, strip " - Topic", chosenByUser=0 for radio/autoplay; the "powered by AudioScrobbler" credit as a small link line in the Last.fm row.

The owner (not the app, not an agent) creates the Last.fm API account and sets the environment variables, and does any Google sign-in and cookie export personally. Until the variables exist, builds must work and show "no Last.fm key" in the Last.fm row.

## Last.fm

RECOMMENDED DESIGN: build Last.fm first. It changes nothing about how YouTube is contacted.
- Sign-in uses Last.fm's desktop token flow in the system browser.
- The session key is stored encrypted (DPAPI on Windows, Keychain on macOS).
- The API key and secret are injected at build time and never committed.
- A scrobble is decided by time actually heard, and an SQLite queue survives restarts.

CROSS-CHECK CORRECTIONS (verified)
- The "half of it" copy is at SettingsView.qml:417, not :416. The Connections section spans SettingsView.qml:394-432.
- ToS §4.3.3 forbids sub-licensing "Last.fm Data", not API keys. The ToS has no clause saying keys must be kept confidential (last.fm/api/tos). This corrects the monolist-fit researcher.
- Qt Network Authorization and Qt HTTP Server are GPLv3/commercial only (doc.qt.io/qt-6/licensing.html). A plain QTcpServer is Qt Network (LGPL). The design needs none of them.
- Last.fm's desktop-auth page says the user returns to the app afterwards (last.fm/api/desktopauth). Polling auth.getSession is established practice (the pylast README loops on it), but it is not official.

1. KEY HANDLING
- CMake reads MONOLIST_LASTFM_API_KEY and MONOLIST_LASTFM_SHARED_SECRET from -D or the environment.
- configure_file(src/apicredentials.h.in -> ${CMAKE_CURRENT_BINARY_DIR}/generated/apicredentials.h @ONLY). This sits next to buildinfo.h, whose include dir is already added (CMakeLists.txt:72-84).
- build*/ and CMakeCache.txt are gitignored (.gitignore:16-18). Only the value-free .h.in is committed. Never message() the values.
- Why not commit them: the key is issued to the owner (ToS §2.6), and Last.fm may limit, suspend or terminate it (§4.4, §9.2). Forks reusing a committed key draw on it (Strawberry forum post 9371; Navidrome dropped its shared key in v0.51.0).
- A shipped binary still exposes the secret (RFC 8252 §8.5). Treat it as an identifier. XOR-obfuscating it only stops a casual `strings` scan.
- A build with an empty key shows "This build has no Last.fm key" instead of a dead button. Later, add a "Use your own API account" option: the key goes in settings, the secret in SecretStore.
- There is no CI today (ROADMAP.md:220), so the values live as environment variables on the owner's build machines.

2. AUTH FLOW (no password, no local server)
- auth.getToken (signed), then QDesktopServices::openUrl("https://www.last.fm/api/auth/?api_key=K&token=T"), then auth.getSession.
- getSession is tried (a) when the Monolist window becomes active again, (b) every 3 s while the row shows "Waiting for Last.fm", for up to 10 min, and (c) on an "I've approved it" button.
- Error 14 means keep waiting. Error 15 means the token expired (tokens last 60 min), so restart.
- Why not a loopback callback (Strawberry's cb=http://localhost:port):
  - it opens a port;
  - whether cb=http://127.0.0.1 works is untested;
  - it gains little.
  Keep it as a fallback idea only.
- Never use auth.getMobileSession, which takes the password.
- Storage:
  - session key: SecretStore("lastfm.session");
  - non-secret: lastfm.user and lastfm.enabled in the settings table.
- Disconnect deletes the key and links to https://www.last.fm/settings/applications so the user can revoke it there.

3. SIGNING AND TRANSPORT
- api_sig = lowercase-hex MD5 of: every parameter except format and callback, sorted by name in byte order, each concatenated as name+UTF-8 value, with the shared secret appended (last.fm/api/authspec). Add format=json after signing.
- POST to https://ws.audioscrobbler.com/2.0/ as application/x-www-form-urlencoded, percent-encoding every key and value with QUrl::toPercentEncoding.
- Do NOT use QUrlQuery for the body. Qt documents that it never encodes '+', so a title like "Simon + Garfunkel" would reach the server as a space and fail with error 13.
- User-Agent: "Monolist/<version> (+https://github.com/droidboy08-hub/Monolist)", using AppInfo's version (appinfo.cpp:293-294).
- Always parse the body; errors arrive as {"error":N,"message":...}.
- Never log sk, token, api_sig or the secret.

4. WHEN A TRACK COUNTS (PlaybackController)
- Add a heard-time accumulator to the positionChanged lambda (playbackcontroller.cpp:68-73). Add the delta only when m_playing && !m_buffering && 0 < delta <= 2500 ms.
  - Seeks are excluded because setPosition writes m_position first (:968-976).
  - So are the resume offsets used on video switches (:801, :866).
- Reset the accumulator per listen in beginTrack, right after closePlayEvent (:686).
- Do not reuse play_events.listened_ms. It is the playhead, and seeking to the end counts as a full listen (appdatabase.cpp:130-133).
- A listen starts at its first counted tick; started_at = now_utc - heard. The obvious hooks are wrong (verified):
  - beginTrack is too early, because resolving can still fail (:806-846);
  - openPlayEvent is skipped for the track queued paused at launch (main.cpp:138 -> beginTrack(autoPlay=false), :723-726), because play() only unpauses (:892-902).
- New signals: listenStarted(track, startedAtSecs, chosenByUser) and listenQualified(...). Do not use currentTrackChanged; it also fires on queue index moves.
- chosenByUser=0 when the fromRadio test is true (:601-603). Otherwise omit it (the default is 1).
- The track qualifies when mpv's duration > 30000 ms and heard >= min(duration/2, 240000) (last.fm/api/scrobbling). An unknown or zero duration waits.
- Enqueue once, at the moment the threshold is crossed.
- Repeat-one is a new listen (:881-885). A video toggle or a stale-link refresh stays the same listen.
- Metadata:
  - Send the primary artist. InnerTube joins artists with ", " (innertube.cpp:186), and Track has only a string (innertube.h:34-44). Capture a primaryArtist field at parse time rather than splitting on ", ", which breaks "Tyler, The Creator".
  - Add album when known.
  - Strip " - Topic" from channel-derived artists.
  - Skip items with no artist or title.
  - Never auto-apply Last.fm corrections.

5. NOW PLAYING
- Send track.updateNowPlaying (artist, track, album, duration) at listen start.
- It is fire-and-forget and not queued.
- Re-send it on resume after a pause longer than 5 min. The docs are silent on this; it is my design choice.

6. OFFLINE QUEUE
- Add a table in AppDatabase::createSchema, after lyrics (appdatabase.cpp:191); the change is additive only:
  scrobble_queue(id INTEGER PRIMARY KEY, account TEXT, artist, track, album, album_artist, duration_s, started_at INTEGER, chosen_by_user INTEGER, video_id, attempts INTEGER, last_error TEXT, queued_at INTEGER).
- The account column means a backlog is never sent to a different Last.fm user after a reconnect.
- Record only while connected and enabled. Keep queueing while the session is expired.
- Flushing:
  - one request in flight, oldest first, up to 50 per track.scrobble with [i] parameters;
  - triggers: 5 s after an enqueue, 20 s after launch, QNetworkInformation going Online (the qnetworklistmanager plugin is in the kit), and the backoff timer.
- Response handling:
  - accepted: delete;
  - ignored codes 1-4: delete, logging counts only;
  - ignored code 5: keep, and pause until the next UTC day;
  - network error, HTTP 5xx, 11, 16: exponential backoff from 30 s to 30 min, with jitter;
  - 29: pause 15 min;
  - 9: delete the session key, set state Expired ("Reconnect"), keep the queue;
  - 10, 13, 26: pause and keep the queue (tributary #335, 23 Sep 2026: deleting on these lost scrobbles);
  - 6, 8 or another request-level error on a batch: resend its items one at a time, once, and drop only those that fail non-retryably.
- Cap the queue at 10,000 rows. The age limit is undocumented; "14 days" is community lore.

7. UI
- ServiceRow gains:
  - a `state` property: off | waiting | connected | expired | error | unavailable;
  - `accountName` and `statusLine` ("12 scrobbles waiting");
  - `built`, which hides the "Not built yet" footer (ServiceRow.qml:125-134);
  - signals connectRequested, disconnectRequested and cancelRequested.
- The button is no longer disabled when connected (:65); it becomes DISCONNECT.
- Replace SettingsView.qml:417 with: "A track counts as played once you have heard half of it or four minutes, whichever comes first; tracks of 30 seconds or less never count. While a track plays, Last.fm is also told what is playing now. Nothing else is sent."
- Make the Note at :405-408 reflect the real state.
- Add a small "powered by AudioScrobbler" link (ToS §2.7) and a Scrobble on/off toggle.

8. FILES
- Add:
  - src/lastfm.h/.cpp: signing, getToken, getSession, nowPlaying, scrobble, response parser;
  - src/scrobbler.h/.cpp: listen -> queue -> flush and backoff, as QML singleton "Scrobbler";
  - src/secretstore.h and src/secretstore_win.cpp: DPAPI CryptProtectData into QSaveFile files under AppDataLocation/secrets;
  - src/secretstore_mac.mm (macOS session);
  - src/apicredentials.h.in.
- Change:
  - CMakeLists.txt: sources, configure_file, crypt32 on WIN32 next to dwmapi netapi32 (:188-192), and OBJCXX + Security.framework on APPLE;
  - src/appdatabase.cpp;
  - src/playbackcontroller.h/.cpp;
  - src/innertube.h/.cpp (primaryArtist);
  - src/main.cpp (construct and register near :172-191);
  - components/ServiceRow.qml, views/SettingsView.qml, .gitignore.

9. TEST PLAN
- Self-tests with no network, in the self-test style (ROADMAP.md:92):
  - api_sig known-answer vectors, including UTF-8 ("Sigur Rós"), '+', '&' and '=';
  - form-body round-trip;
  - the response parser on canned JSON for accepted, ignored 1-5, and errors 6/9/10/11/13/16/26/29.
- Listen tracker with a fake engine:
  - a 25 s track never scrobbles;
  - a 3:00 track scrobbles at 1:30 heard; a 10:00 track at 4:00;
  - seeking to the end does not count; pauses and buffering do not count;
  - repeat-one twice gives 2 scrobbles; an audio-to-video toggle gives 1;
  - launch-paused then Play counts; a resolve failure gives none.
- Queue:
  - kill the process after the threshold: the row survives;
  - 120 offline rows go out as 3 requests (50/50/20), oldest first;
  - a faked error 26 leaves the queue intact.
- Live, on the owner's account:
  - connect, play, and check "Scrobbling now" and the history on last.fm;
  - revoke at last.fm/settings/applications: the next send gets error 9 and the row shows Reconnect;
  - Disconnect removes the secret file.

## YouTube Music

RECOMMENDED DESIGN: import a cookie session from the user's own browser (private window). No embedded browser.
- This works today on Windows with the current MinGW kit and adds no dependency.
- The Google password is only ever typed into Google's own page, in the user's own browser.
- macOS reuses all of the C++; only the secret store changes (Keychain).
- An embedded sign-in window is deferred to an optional spike.
- Signed out stays the default, and every authed feature falls back to anonymous.

WHY THIS AND NOT THE ALTERNATIVES (cross-checked)
- Reading the browser profile:
  - Chrome, Edge and Brave on Windows use v20 app-bound encryption, which yt-dlp cannot decrypt (yt-dlp #10927, still reported Sep 2026).
  - Firefox can be read, but that shares a live, rotating main-browser session, which the yt-dlp wiki and Tunelio (7 Sep 2026) warn against.
  - Not in v1.
- OAuth is dead: the yt-dlp wiki says "logging in with OAuth no longer works", and yt-dlp's _base.py says it is "no longer supported" (both verified). The app's own Google client would fall under the YouTube API Services policies and a 100-user cap.
- An embedded web view is not available on MinGW:
  - Qt WebEngine does not compile with MinGW.
  - Qt WebView's WebView2 backend is gated on `WIN32 AND MSVC AND WebView2_FOUND` (verified in qtwebview 6.11 configure.cmake).
  - Qt WebView never exposes cookie values, and the kit has no webview plugin.
  - A hand-hosted WebView2 is possible, but Google has blocked sign-in from embedded browsers since 2021 (WebView2Feedback #1584, #2552). The iPhone app only gets through by spoofing the Mobile Safari user agent (GoogleAccountSheet.swift:94-98).
- Private-window cookie export is the method the yt-dlp wiki documents (verified). A 7 Sep 2026 guide still reports days-to-weeks lifetimes.

1. CAPTURE (Settings > Connections > YouTube Music > CONNECT)
1. In your own browser, open a private window and sign in at music.youtube.com with an account you can afford to lose. Firefox is suggested: Google's DBSC post names Chrome on Windows only.
2. Get the cookies one of two ways:
   - (a) Open https://www.youtube.com/robots.txt in that same tab and export the youtube.com cookies with a cookies.txt extension.
   - (b) With no extension: open DevTools > Network on music.youtube.com, pick any `browse` request, and copy its `cookie` request header. This is ytmusicapi's documented browser method.
3. In Monolist, choose the file ("Choose file..." uses QtQuick.Dialogs, which is in the kit) or paste into a masked field.
4. Close the private window without using it again. Monolist offers "Delete the exported file now" but never deletes a user's file by itself.

2. PARSE AND VALIDATE (new src/cookieimport.*)
- Auto-detect the format:
  - Netscape: 7 TAB-separated fields, LF or CRLF. Lines starting `#HttpOnly_` are cookies, not comments (curl convention). Getting this wrong silently drops SID cookies.
  - a raw "Cookie: a=b; c=d" header;
  - a pasted "Copy as cURL" (-H 'cookie: ...' or -b '...').
- Keep only cookies for youtube.com and its subdomains; header-form cookies are treated as .youtube.com, path /, secure. Drop google.com and everything else.
- Deduplicate by name, most specific domain winning (the iPhone app hit this bug: YouTubeAccountSync.swift:1123-1143).
- Require LOGIN_INFO plus SAPISID or __Secure-3PAPISID. This is yt-dlp's own _has_auth_cookies rule (_base.py, verified).
- Log cookie names and counts only, never values.
- Only show Connected after a live check (section 4).

3. STORAGE
- SecretStore("ytmusic.cookies") holds a JSON jar: name, value, domain, path, expires, secure, httpOnly.
- Windows: CryptProtectData with CRYPTPROTECT_UI_FORBIDDEN and app entropy, written to AppDataLocation/secrets via QSaveFile. Links crypt32; dpapi.h and libcrypt32.a are already in the kit.
- Not Credential Manager. MS docs cap a blob at 2,560 bytes, and MinGW's wincred.h:112 defines CRED_MAX_CREDENTIAL_BLOB_SIZE as 512 (verified). A jar can exceed either.
- macOS: Keychain SecItem generic password.
- Linux later: libsecret. Until then, keep the jar in memory only and tell the user it won't be remembered.
- The settings table gets only non-secret keys (ytmusic.account_name). It is readable from any QML (library.h:65), and `--set` echoes values to the log (main.cpp:98).
- Sign out deletes the secret, clears memory, and refreshes Home anonymously.

4. SESSION MODEL, REFRESH AND GRACEFUL DEGRADE (new src/ytmsession.*, QML singleton "Account")
- States: SignedOut | Checking | Active | Rejected | Unavailable, plus accountName and avatar.
- Validation runs at import, once at launch, and at most every 6 h while running:
  - POST account/account_menu for the name and avatar (ytmusicapi get_account_info; iPhone YouTubeAccountSync.swift:72-93);
  - read `logged_in` from responseContext.serviceTrackingParams on the authed FEmusic_home (iPhone AuthDiagnostics.swift:212, YouTubeAccountSync.swift:251-283).
- Only a definitive server answer moves the state to Rejected: 401/403, or logged_in=0 seen twice. A network failure means Unavailable, and the session is kept.
- Bad auth often comes back as HTTP 200 with a generic feed (YouTubeAccountSync.swift:25). The status code alone proves nothing.
- On Rejected, stop sending cookies, show "Your YouTube Music session ended - import again", and keep working anonymously.
- Rotation: authed requests set CookieSaveControlAttribute=Manual. YtmSession merges reply->header(SetCookieHeader) into the jar and persists it (debounced 30 s), much as yt-dlp writes its jar back.
  - Whether this extends the session's life is unknown; measure it.
  - No calls to accounts.google.com/RotateCookies and no keep-alive traffic.

5. AUTHENTICATED INNERTUBE REQUESTS
- Add `enum class Auth { Anonymous, IfSignedIn }` to post/send/browse, defaulting to Anonymous, so signed-out traffic stays byte-identical.
- A static session hook, modelled on g_region (innertube.cpp:77-79) and set in main.cpp beside :105-108. It must be static because there are six InnerTube objects, each with its own QNAM (mediaextractor.h:114, catalog.h:68, lyrics.h:110, recommender.h:111, playbackcontroller.h:220, streamresolver.h:141; verified).
- When the call is IfSignedIn, the session is Active, and the client is Music:
  - CookieLoadControlAttribute=Manual, so the anonymous jar is never mixed in. Qt does not document how a raw Cookie header merges with the jar.
  - Cookie: the session cookies.
  - Authorization: the schemes space-joined as yt-dlp does (_base.py, verified), each `<scheme> <ts>_<sha1-lowercase-hex("<ts> <sid> https://music.youtube.com")>`:
    - SAPISIDHASH from SAPISID, or __Secure-3PAPISID if SAPISID is absent;
    - SAPISID1PHASH from __Secure-1PAPISID;
    - SAPISID3PHASH from __Secure-3PAPISID.
  - X-Origin: https://music.youtube.com, and X-Goog-AuthUser: 0.
  - No `_u` user-session suffix in v1: it needs DATASYNC_ID, and ytmusicapi works without it.
  - Brand accounts (X-Goog-PageId / onBehalfOfUser) are out of scope for v1.
- Errors in send() for an authed call:
  - 401/403: reportRejected(), then retry once anonymously.
  - 400: retry anonymously, and never enter the branch at innertube.cpp:470-478. Otherwise a broken session would wipe the user's chosen country through Library::dropRegion (main.cpp:106-108; verified).

6. WHICH CALLS CARRY THE ACCOUNT
- Authed:
  - FEmusic_home (catalog.cpp:51);
  - account_menu;
  - album and playlist page browse (catalog.cpp:155), so private playlists open;
  - the new library reads.
- Always anonymous:
  - VISIONOS /player and prefetch;
  - the visitorData scrape (innertube.cpp:371-405);
  - suggestions, search, lyrics, the recommender and radio;
  - all of yt-dlp.
- Keeping these anonymous holds per-account volume low and keeps the promise that signing in "buys none of the speed" (SettingsView.qml:430).
- yt-dlp with cookies would switch to _DEFAULT_AUTHED_CLIENTS = ('web_embedded','tv_downgraded','web'). That was verified in current _video.py; the monolist-fit researcher's list is stale. Those clients bring PO-token and SABR costs (#16229, closed as a duplicate of #16212).
- yt-dlp's info JSON also embeds cookies (GHSA-v8mc-9377-rwjj), while ytdlp.cpp:211-212 logs stderr on success.

7. WHAT IT UNLOCKS, IN ORDER
- (1) Personalised Home, plus the account name in the row. Catalog::refresh runs on session change, as regionChanged already does (main.cpp:155).
- (2) Read-only import:
  - Liked songs (browse VLLM) become a separate "Liked on YouTube Music" playlist, which is reversible and not mixed into local likes;
  - library playlists (FEmusic_liked_playlists) become saved playlists via Library::setSaved (library.cpp:638).
- (3) History (FEmusic_history), read-only.
- Later, each an opt-in and an owner decision:
  - two-way likes through Library::setLiked (library.cpp:540);
  - playlist edits;
  - a signed-in yt-dlp rung, only for LOGIN_REQUIRED or age-gated tracks. It uses a temporary cookie file per process in a private folder, reads it back, deletes it, and suppresses stderr and JSON logging;
  - Premium 256 kbps.

8. COPY FIXES
- SettingsView.qml:428: "kept in this computer's keychain" becomes "encrypted with your Windows sign-in (the Keychain on a Mac)".
- SettingsView.qml:429: "the file and the key are both deleted" becomes "Monolist's encrypted copy is deleted; it offers to delete your exported file as soon as it has read it".
- Add: "A session lasts days to weeks; when it ends Monolist says so and keeps playing signed out."
- The caution at :425 stays.

9. MACOS PATH
- The parser, YtmSession, InnerTube auth and Scrobbler are shared C++.
- secretstore_mac.mm uses SecItemAdd, CopyMatching, Update and Delete (kSecClassGenericPassword, service = bundle id). CMake adds OBJCXX and Security.framework.
- UNVERIFIED: whether ad-hoc or unsigned builds re-prompt for Keychain access after every rebuild. TN3137 says the data-protection keychain needs entitlements. The macOS session should test this; Developer ID signing likely makes it stable.
- An optional later macOS-only sign-in sheet (Objective-C++ WKWebView, WKHTTPCookieStore getAllCookies into the same YtmSession) could reuse the iPhone GoogleAccountSheet flow. It relies on a user-agent spoof against Google's embedded-browser policy, so it is the owner's call.

10. DEFERRED WINDOWS EMBEDDED SIGN-IN
- Only if cookie import proves too painful.
- A half-day spike: webview/webview (MIT, MinGW listed as working) with ICoreWebView2CookieManager::GetCookies(""), checking whether Google blocks sign-in without a user-agent spoof.
- WebView2 Runtime 147 reportedly ships DBSC (researcher-reported from Edge 147 notes, not re-verified), which would bind the session to the web view.

11. FILES
- Add: src/ytmsession.h/.cpp, src/cookieimport.h/.cpp, SAPISIDHASH helpers (inside ytmsession), reusing src/secretstore*.
- Change:
  - src/innertube.h/.cpp: Auth flag, session hook, headers, error branches, logged_in reader;
  - src/catalog.h/.cpp: authed Home and pages, imports, refresh on session change;
  - src/library.*;
  - src/main.cpp;
  - components/ServiceRow.qml, plus a small connect panel with file and paste input;
  - views/SettingsView.qml, CMakeLists.txt;
  - .gitignore: add cookies*.txt, *.cookies, Secrets.plist, secrets/.

12. TEST PLAN
- Parser self-tests on synthetic fixtures only; never commit real cookies:
  - Netscape LF and CRLF, `#HttpOnly_` lines, spaces-instead-of-tabs rejected;
  - header form and cURL form;
  - duplicate names across domains;
  - missing LOGIN_INFO rejected.
- SAPISIDHASH known-answer test: fixed ts, sid and origin, with the expected SHA-1 computed once offline.
- Signed-out regression: with no session, every request's headers are identical to today's.
- SecretStore: round-trip; a tampered blob fails cleanly; delete removes the file.
- A faked 400 on an authed call leaves the region unchanged and retries anonymously.
- Live, on the owner's throwaway account:
  - import goes Checking, then Active with the name;
  - Home differs from signed-out and reports logged_in=1;
  - a restart restores the session;
  - a mangled SAPISID leads to Rejected, and Home still loads;
  - sign out removes the secret;
  - record the jar's size in bytes and its cookie names only;
  - log the days until Rejected, with and without Set-Cookie absorption, to measure the real lifetime.

## Risks

- The Last.fm key belongs to the owner's account. Last.fm can rate-limit (error 29), suspend (error 26) or terminate it (ToS §4.4, §9.2) because of all users' traffic combined.
- Mitigation: build-time injection so forks don't reuse it, low call volume (no Last.fm read APIs at first), backoff, a queue that survives key errors, and a later bring-your-own-key option.
- Last.fm ToS §5.1.8 prohibits use associated with 'illegal or unauthorised use or sharing of content'. Whether it reaches a player that streams YouTube through InnerTube and yt-dlp is uncertain. It is worded broadly; Pear Desktop and Web Scrobbler scrobble YouTube Music openly. This is the owner's risk call.
- Google may restrict or ban an account used from outside clients. The yt-dlp wiki warns of temporary or permanent bans, and it is unclear whether a ban reaches Gmail or Drive (yt-dlp #15724, unanswered).
- Mitigation: the 'account you can afford to lose' caution, read-only scope, authed calls limited to a few low-volume browse requests, and playback kept anonymous.
- Cookie sessions are short-lived: days to weeks according to the yt-dlp wiki procedure and Tunelio (Sep 2026).
- DBSC is on by default for personal accounts in Chrome on Windows with no off switch (Google post, 28 May 2026), and could shorten exported sessions further.
- The post says nothing on incognito or other browsers, so the impact is uncertain.
- Mitigation: clear 'session ended' state, one-click re-import, Firefox suggested for export, and signed-out behaviour unchanged.
- Silent auth failure: bad cookies can return HTTP 200 with the generic feed. Only logged_in=1 in serviceTrackingParams proves it worked (iPhone app findings). Trusting the status code would show 'Connected' when it is not.
- Region wipe: send() treats any 400 as a refused country and calls Library::dropRegion (innertube.cpp:470-478, main.cpp:106-108). An auth failure answered with 400 would erase the user's chosen country unless authed calls bypass that branch.
- Secret leakage through logs:
- yt-dlp's info JSON contains cookies (GHSA-v8mc-9377-rwjj), and ytdlp.cpp:211-212 logs stderr on success;
- `--set` echoes values (main.cpp:98);
- the settings table is readable from QML (library.h:65).
Mitigation: never pass cookies to yt-dlp in v1, never put secrets in settings, and log cookie names and counts only.
- Parser pitfalls that silently break sign-in or scrobbling:
- Netscape `#HttpOnly_` lines dropped as comments, which loses the SID cookies;
- QUrlQuery not encoding '+', so the server decodes it as a space and returns error 13;
- duplicate cookie names across domains, which gives logged_in=0.
Each has a planned test.
- Credential Manager's 2,560-byte blob cap (MS docs), and the MinGW wincred.h:112 macro of 512. Avoided by using DPAPI files.
- On macOS, ad-hoc or unsigned builds may re-prompt for Keychain access after every rebuild. Unverified.
- Undocumented Last.fm limits: how old a timestamp may be before code 3, and the daily cap (about 2,800 is community lore). Old queue items may be ignored.
- Qt does not document how a raw Cookie header merges with the jar. The design sidesteps this with CookieLoadControlAttribute=Manual, which should be confirmed with a header-capture test.
- An embedded sign-in, if ever built, works against Google's embedded-browser policy (Google blocked such sign-in from Jan 2021) and needs a user-agent spoof. It could break without notice, which is why it is deferred.
- The YouTube Terms of Service forbid automated access and circumventing security features. The whole app already carries this risk; signing in ties it to a real account.

## Build order

- 1. Repo hygiene and UI scaffolding.
- Add cookies*.txt, *.cookies, Secrets.plist and secrets/ to .gitignore.
- Give ServiceRow its state, accountName, statusLine and built properties and its connect/disconnect signals.
- Fix the copy at SettingsView.qml:417, :428 and :429.
- Test: the Settings screenshot shows the new copy, and the rows still say 'not built'.
- 2. SecretStore with the Windows DPAPI backend (crypt32).
- Test: self-test round-trip; a tampered blob fails cleanly; delete removes the file.
- 3. Build-time Last.fm credentials: apicredentials.h.in plus configure_file into build/generated.
- Test: a build with no environment variables shows 'no Last.fm key'; a build with them reports that a key is present; git status stays clean.
- 4. LastFmApi, with no network yet.
- Build: signing, form encoding with QUrl::toPercentEncoding, and a response parser.
- Test: known-answer api_sig vectors (UTF-8, '+', '&') and canned JSON for every error and ignored code.
- 5. Last.fm connect and disconnect.
- Flow: getToken, open the browser, retry getSession on focus, poll and the manual button; store the session key; show the username.
- Test: the owner connects, restarts, disconnects, and revokes on last.fm.
- 6. Listen tracking in PlaybackController.
- Build: the heard-time accumulator, listenStarted and listenQualified, and primaryArtist from InnerTube.
- Test: fake-engine scenarios (30 s, half, 4 min, seek, pause, buffering, repeat-one, video toggle, launch-paused Play).
- 7. The scrobble_queue table, with enqueue at the threshold but no sending.
- Test: rows appear with the right started_at and chosen_by_user, and survive killing the process.
- 8. Sending.
- Build: updateNowPlaying, batched track.scrobble (50), backoff, and the error mapping (5 / 9 / 10-13-26 / 11-16 / 29 / 6).
- Test: 120 offline rows go out as 50/50/20; a faked error 26 keeps the queue; a live scrobble shows on the owner's profile.
- 9. Optional separate commit: record play_events for the track queued paused at launch and then played.
- 10. Cookie import parser: Netscape (including #HttpOnly_), raw header and cURL; youtube.com filter, dedupe, and the required-cookie check.
- Test: self-tests on synthetic fixtures only.
- 11. SAPISIDHASH builder (three schemes, lowercase hex).
- Test: a known-answer vector.
- 12. YtmSession and the InnerTube auth path.
- Build: YtmSession (states, SecretStore persistence, Set-Cookie absorption), plus InnerTube's Auth flag, static session hook, Manual cookie attributes, and 401/403/400 handling that never touches the region.
- Test: signed-out headers are unchanged; a faked 400 keeps the region.
- 13. Settings wiring for YouTube Music.
- Build: Choose file, masked paste, validation through account_menu and logged_in, the account name, sign out, and an offer to delete the exported file.
- Test: the owner imports from a throwaway account; a mangled cookie leads to Rejected, with Home still loading.
- 14. Personalised Home: authed FEmusic_home, with Catalog refreshed on session change.
- Test: signed-in Home differs from signed-out and reports logged_in=1. Start the session-lifetime log.
- 15. Read-only imports.
- Liked songs (VLLM) as a separate playlist; library playlists (FEmusic_liked_playlists) as saved playlists; authed page browse so private playlists open; then FEmusic_history.
- Test: counts match the account.
- 16. macOS (other session).
- Build: secretstore_mac.mm (SecItem) with CMake OBJCXX and Security.framework; run the same self-tests.
- Test: whether Keychain prompts come back after a rebuild.
- 17. Later opt-ins, each needing an owner decision:
- bring-your-own Last.fm key;
- two-way likes and playlist edits;
- a signed-in yt-dlp rung for age-gated or LOGIN_REQUIRED tracks;
- Premium quality;
- the embedded sign-in spike (WebView2 through webview/webview, or macOS WKWebView).

## Sources

- Monolist Player/views/SettingsView.qml:394-432 (Connections section; Last.fm 'half' copy at :417; YouTube Music copy at :424-430)
- Monolist Player/components/ServiceRow.qml:15-22, :59-67 (disabled when connected, :65), :125-134 (the 'Not built yet' footer)
- Monolist Player/src/innertube.cpp:371-405 (visitorData scrape), :418-446 (post headers), :448-499 (send), :470-478 (400 drops the region)
- Monolist Player/src/innertube.h:34-44 (Track has only a joined artist string), :87 (Client enum), :104 (region handler), :112-113 (browse), :164-172 (post/send)
- Monolist Player/src/innertube.cpp:186 (artists joined with ', ')
- Monolist Player/src/main.cpp:93-100 (--set echoes values), :105-108 (region handler -> Library::dropRegion), :138 (library queued paused), :151-155 (Catalog refresh wiring), :172-191 (QML singletons)
- Six InnerTube instances: src/mediaextractor.h:114, src/catalog.h:68, src/lyrics.h:110, src/recommender.h:111, src/playbackcontroller.h:220, src/streamresolver.h:141
- Monolist Player/src/playbackcontroller.cpp:68-73 (position lambda), :77-86 (pause and buffering), :601-603 (fromRadio), :630-677 (closePlayEvent uses the playhead), :681-777 (beginTrack; play event only when autoPlay, :723-726), :881-885 (repeat-one), :892-902 (play() only unpauses), :968-976 (setPosition)
- Monolist Player/src/appdatabase.cpp:99 (settings table), :130-135 (listened_ms is the playhead), :191 (lyrics table, last in the schema)
- Monolist Player/src/library.h:65 (settingValue is Q_INVOKABLE); src/library.cpp:540 (setLiked), :638 (setSaved); src/catalog.cpp:51 (FEmusic_home), :155 (page browse)
- Monolist Player/src/ytdlp.cpp:211-212 (stderr logged even on success)
- Monolist Player/src/appinfo.cpp:293-294 (User-Agent Monolist/<version>); src/artworkcache.cpp:207 (Monolist/0.1)
- Monolist Player/CMakeLists.txt:19 (Qt modules), :72-93 (generated header pattern), :188-192 (Windows libraries)
- .gitignore:16-18 (build*/, CMakeCache.txt), :85-88 (.env only)
- ROADMAP.md:193 (Connections decision), :194 (repo droidboy08-hub/Monolist), :220 (no GitHub Actions)
- C:\dev\monolist-deps\Qt\Tools\mingw1310_64\x86_64-w64-mingw32\include\wincred.h:112 (#define CRED_MAX_CREDENTIAL_BLOB_SIZE 512)
- C:\dev\monolist-deps\Qt\6.11.2\mingw_64\plugins\networkinformation\qnetworklistmanager.dll and qml\QtQuick\Dialogs\qmldir (both present)
- iPhone app, \\psf\Home\Desktop\AryaMusix\musicplayer: AuthDiagnostics.swift:212 and YouTubeAccountSync.swift:25, 103, 251-283 (logged_in check); GoogleAccountSheet.swift:94-98 (user-agent spoof, per researcher). Secrets.plist exists and was not opened.
- https://www.last.fm/api/desktopauth
- https://www.last.fm/api/show/auth.getSession
- https://www.last.fm/api/scrobbling
- https://www.last.fm/api/authspec
- https://www.last.fm/api/tos (§2.6, 2.7, 3.1, 4.3.3, 4.4, 5.1.8; no key-confidentiality clause)
- https://www.last.fm/api/errorcodes
- https://github.com/jm2/tributary/issues/335 (23 Sep 2026: queue deleted on errors 10/13/26 and 5)
- https://raw.githubusercontent.com/pylast/pylast/main/README.md (polls get_web_auth_session_key in a loop)
- https://forum.strawberrymusicplayer.org/post/9371 and https://github.com/navidrome/navidrome/releases/tag/v0.51.0 (researcher-cited, on shared keys)
- https://datatracker.ietf.org/doc/html/rfc8252#section-8.5
- https://doc.qt.io/qt-6/licensing.html (Network Authorization and HTTP Server are GPLv3)
- https://doc.qt.io/qt-6/qnetworkrequest.html (CookieLoadControl and CookieSaveControl attributes)
- https://doc.qt.io/qt-6/qurlquery.html (never encodes '+')
- https://raw.githubusercontent.com/qt/qtwebview/6.11/src/webview/configure.cmake (WebView2 requires WIN32 AND MSVC)
- https://doc.qt.io/qt-6/qtwebengine-platform-notes.html (WebEngine does not compile with MinGW; researcher-cited)
- https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/yt_dlp/extractor/youtube/_base.py (SAPISIDHASH, 1P and 3P schemes, _has_auth_cookies, OAuth unsupported)
- https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/yt_dlp/extractor/youtube/_video.py (_DEFAULT_AUTHED_CLIENTS = web_embedded, tv_downgraded, web)
- https://github.com/yt-dlp/yt-dlp/wiki/Extractors (private-window export, ban warning, OAuth no longer works, 300 vs 2,000 per hour)
- https://tunelio.dev/blog/yt-dlp-cookies/ (7 Sep 2026: days to a few weeks)
- https://github.com/yt-dlp/yt-dlp/issues/16229 (cookies reduce formats; closed as a duplicate)
- https://github.com/yt-dlp/yt-dlp/issues/10927 (Chrome app-bound encryption; researcher-cited)
- https://github.com/advisories/GHSA-v8mc-9377-rwjj (cookies field in the info JSON; researcher-cited)
- https://workspaceupdates.googleblog.com/2026/05/prevent-account-takeovers-with-DBSC-now-generally-available-in-the-Chrome-browser-for-Windows.html (28 May 2026: personal accounts, Chrome on Windows, cannot be disabled)
- https://developers.googleblog.com/guidance-to-developers-affected-by-our-effort-to-block-less-secure-browsers-and-applications/ (embedded sign-in blocked since 2021; researcher-cited)
- https://github.com/MicrosoftEdge/WebView2Feedback/issues/1584 and https://github.com/MicrosoftEdge/WebView2Feedback/issues/2552
- https://ytmusicapi.readthedocs.io/en/stable/setup/browser.html (copying the request-header method; researcher-cited)
- https://everything.curl.dev/http/cookies/fileformat.html (#HttpOnly_ prefix)
- https://learn.microsoft.com/en-us/windows/win32/api/wincred/ns-wincred-credentialw (2,560-byte blob cap; researcher-cited)
- https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata
- https://developer.apple.com/documentation/technotes/tn3137-on-mac-keychains (researcher-cited)
