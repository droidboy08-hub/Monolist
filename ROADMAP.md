# Roadmap

What is left before Monolist is finished, from a full audit on 2026-09-25: the code, every request made in the build sessions, the iPhone app, Melody, the Phono and Lumen designs, the recommendation spec, and what a macOS build needs. Every item was checked against the code. Items are ticked as they land.

Priority runs from 1 (first) to 9. Size: S under an hour, M a few hours, L a day or more.

## Bugs

- [x] **B01** Every launch waits on three tool --version checks because Settings is built at startup *(P1, S)*
  Done when: The version checks run in the background through QProcess signals and start only the first time Settings is shown. The window appears at once, and Settings shows 'Reading versions…' until the answers arrive.
- [x] **B02** A retried InnerTube request can replace a newer one: search spins forever, or autoplay sticks on 'Finding more songs…' *(P1, S)*
  Done when: Each slot carries a generation token. A pending retry checks it before sending, and cancelling or starting a new request bumps it, so an old retry can never replace or silence a newer search or radio request.
- [x] **B04** A song started with Play after launch, or reached with Next/Previous while paused, is never recorded; launching offline shows an error toast unprompted *(P1, S)*
  Done when: The first time a paused-loaded track starts playing, it records history and opens its play event. A resolve failure for a track nobody asked to play only updates the status line.
- [x] **B07** Ctrl+F and the Search nav item don't put the cursor in the search field, and the field has no clear button *(P1, S)*
  Done when: TopBar.focusSearch() (forceActiveFocus plus selectAll) is called by the Find shortcut and the Search nav item. An X clears the text and suggestions and keeps focus in the field.
- [x] **B08** A failed search's error stays on the Search page after the field is cleared *(P1, S)*
  Done when: An empty query clears lastError, or the error Text also requires term.length > 0.
- [x] **B12** Changing the country while Home is still loading is ignored *(P2, S)*
  Done when: A refresh asked for during a load is queued, as Recommender does with m_refreshQueued, or restarts the load with the new region.
- [ ] **B14** Album and playlist pages read only one of YouTube's two header formats *(P5, S)*
  Done when: parseCollection falls back to musicDetailHeaderRenderer and the top-level header, so pages keep their title, artist and cover whichever form YouTube sends.

## Downloads

- [x] **B09** Track menu shows a greyed-out 'Downloading…' for failed downloads, and can't remove or show a finished one *(P1, S)*
  Done when: A failed track shows 'Retry download' (Downloads.retry). A finished one shows 'Show in folder' and 'Remove download' with the two-step confirmation (Downloads.remove).
- [x] **B13** Downloads stay disabled after the tools are installed or updated, until restart *(P2, S)*
  Done when: DownloadManager re-checks its tools after a tool update, and lazily on enqueue, through NOTIFY properties, so new tools work without a restart.
- [ ] **D01** The download queue is lost on quit, and partly downloaded files are deleted *(P5, M)*
  Done when: Queued and failed downloads are stored in a table and queued again at launch.
- [ ] **D02** The download folder can't be changed *(P5, M)*
  Done when: A 'Change…' action in Settings opens a native folder picker, saves the choice and rescans the stored files.

## Player

- [x] **B03** Player settings, including Settings' Autoplay switch, reset on every launch *(P1, S)*
  Done when: Volume, shuffle, repeat and autoplay are saved when they change and restored in main.cpp before QML loads.
- [x] **B05** After Next or Previous the old song keeps playing until the new one resolves, and its end can skip or fail the new track *(P1, S)*
  Done when: Starting a new track stops or pauses the old file, and mpv events are matched to the current playlist_entry_id from MPV_EVENT_START_FILE. Nothing from the previous file reaches the new one, and position starts at 0.
- [x] **B06** Player bar: the queue and Now Playing buttons vanish below 1040 px, volume has no fallback, and the queue button does nothing while Now Playing is open *(P1, S)*
  Done when: The Now Playing and queue toggles stay visible at every width, and only volume collapses to a button with a popup slider and mute. In Now Playing the queue button switches the right pane to UP NEXT.
- [ ] **U01** The itag 18 fallback doesn't run when a track actually fails to play (user request) *(P3, S)*
  Done when: When mpv fails on an InnerTube adaptive URL, the same track is retried once with its best progressive format (itag 18) before yt-dlp. This is exercised on a real track, with the itag logged.
- [ ] **P01** The output-device button in the player bar does nothing *(P4, M)*
  Done when: MpvEngine observes audio-device-list and sets audio-device. The button opens a menu of outputs (WASAPI on Windows, CoreAudio on macOS) with the current one marked, and the choice is saved.
- [ ] **P02** No media keys and no system Now Playing (Windows media flyout, macOS Control Center, AirPods) *(P4, L)*
  Done when: A small platform layer publishes title, artist, artwork and position, and handles play, pause, next, previous and seek. It uses the MediaPlayer framework through Objective-C++ on macOS and SMTC (or a WM_APPCOMMAND fallback) on Windows, with MPRIS later for Linux.
- [ ] **P04** The queue, current song and position are not restored after a restart *(P5, M)*
  Done when: The last queue, current index and position are saved on quit and restored, paused, at launch.
- [ ] **P05** No menu for the song that is playing, and no sleep timer *(P5, S)*
  Done when: A more button on the player bar and in Now Playing opens TrackMenu for Player.currentTrack, plus a sleep timer (15, 30, 45 or 60 minutes, or end of song) shown as a countdown that pauses playback.
- [ ] **P06** Buffering, streaming-versus-offline source and lyrics error reasons are never shown *(P5, S)*
  Done when: The player bar's status line shows buffering and a small source label such as 'Offline · local file' or 'Streaming · InnerTube'. The lyrics error says why it failed.

## Recommendations

- [x] **B10** Quitting while recommendations are being built can crash the app *(P2, S)*
  Done when: The worker checks a stop flag between scans, and the destructor waits until it stops, so quitting mid-build exits cleanly.
- [x] **B11** The recommender learns the wrong things: re-liked songs never count, skipped songs seed 'More like…', weekends use the UTC day *(P2, S)*
  Done when: The newest like or unlike per song decides. Only positively labelled plays seed song rails (label ≥0.6, liked, or unlabelled but heard for ≥30 s). The weekend check uses the local date.
- [ ] **R01** 'Not interested' is read by the taste profile, but nothing can record it *(P3, M)*
  Done when: 'Not interested' appears on suggestion rows, in TrackMenu and in Now Playing. It records a notInterested event, removes the row, keeps the song out of shelves and autoplay radio, and can be undone from the toast.
- [ ] **R02** Search is missing rails and scoring: weekend and 'back to' rails, Liked/playlist/genre rails, and skipped songs never count against *(P3, M)*
  Done when: Search shows 'Your weekend sound' and 'Back to a couple of months ago' when their minimum listening is met, plus 'More like your Liked songs', 'More like <playlist>' and genre shelves. Taste shelves rank with profile.score, so sounds the user skips sink.
- [ ] **R03** Suggestion shelves are tap-to-play only: no Play all, row menu, loading state or See all *(P3, M)*
  Done when: Each shelf header has Play all, which resolves the songs lazily in order, and See all, which loads more rows from the same source. Rows get TrackMenu on right-click and a spinner while resolving.
- [ ] **R04** Suggestions never rotate, and they include songs the user already owns *(P3, M)*
  Done when: A refresh on the Search page brings up different rows, and the page rebuilds after about 45 minutes. Songs in the library, playlists and downloads are left out, and that set is part of the rebuild check.
- [ ] **R05** Every suggestion press searches YouTube, even for songs already in the library, and the answer isn't remembered *(P5, M)*
  Done when: play() first plays a library or download copy of the song if one exists. Otherwise it searches and caches the pick for a while, and later presses and Play all use the cache.
- [ ] **R06** No 'Recommended' section on playlist pages, and queue radio never uses the recommender *(P5, L)*
  Done when: A playlist page ends with a Recommended list that is exactly what plays after it. A queue started from a playlist continues with a mix drawn from several of its songs, and falls back to the catalogue when YouTube doesn't answer.
- [ ] **X09** The iPhone's listening history (play_events.jsonl) can't be imported *(P7, M)*
  Done when: Settings imports play_events.jsonl into play_events, skipping duplicates by title, artist and time, then rebuilds suggestions. Exporting in the same format is optional.

## Polish

- [ ] **U02** Slow-feeling scrolling was never investigated (user request) *(P3, S)*
  Done when: Scrolling is compared on a Release build and natively on the Mac. If it is still sluggish, wheel step and flick velocity are tuned once in a shared scroll component used by every view.
- [ ] **L06** Missing feedback and empty states: no page retry, silent download failures, a blank or bare Search page, 'Art' in an empty player bar *(P5, M)*
  Done when: Page errors get RETRY. Download failures, 'Download all' and 'Copy for a bug report' each show a toast. Search explains an empty or not-yet-personal page. The empty player bar says 'Nothing playing', with the heart, download and transport controls disabled.
- [ ] **N01** Back and forward have no keyboard or mouse-button bindings, and never grey out *(P5, S)*
  Done when: Shortcuts and the mouse's side buttons go back and forward, and the arrows dim when there is nowhere to go.
- [ ] **G04** Font weights and tooltips may look different on macOS *(P8, S)*
  Done when: Font.Bold becomes Theme.weightMedium or Theme.weightBlack (or a Bold face is bundled). The window sets the font and a palette from Theme, or a styled tooltip component is used, and the two TextFields match the other inputs.
- [ ] **G05** Home's 'Open album' button shows a play icon *(P8, S)*
  Done when: The button shows an arrow, or actually plays the release, and its label follows the release type.
- [ ] **G06** Six InnerTube instances each download youtube.com at startup for a visitor ID only one of them uses *(P8, S)*
  Done when: The visitor ID is fetched only by the instance that resolves streams, or shared, so it is downloaded once per session.
- [ ] **G07** The self-tests can't fail, and there are no automated tests *(P8, M)*
  Done when: Each self-test exits non-zero when an expectation fails. A CTest target checks the golden match keys, a taste profile built from fixed events, and fixed search results when a catalogue path is given, and GitHub CI can run it.
- [ ] **G08** If the database can't be opened, the app fails silently *(P8, S)*
  Done when: The app falls back to an in-memory database and tells the user that the library couldn't be opened, where the file is, and that changes won't be kept.

## Video

- [ ] **U03** Video works only in wide Now Playing: no switch in the narrow layout, no fullscreen or mini panel, and hidden video keeps decoding *(P3, M)*
  Done when: Narrow Now Playing gets the switch and a 16:9 plate, and hiding the last visible surface drops back to audio or shows a mini panel above the bar. F or double-click opens fullscreen video, and Esc leaves it.
- [ ] **P03** Music videos lose their video switch once liked, saved to a playlist or replayed from History *(P4, M)*
  Done when: A migration adds is_video, every insert path writes it and every reader exposes it, so a video played from anywhere offers the switch.

## Artist pages & search

- [ ] **A01** Artist pages are not built, and artist names are never links *(P4, L)*
  Done when: An 'artist:<UC id>' view shows top songs, albums and singles, and related artists, with Play and Shuffle. Tracks carry artist and album browse IDs, so names link to those pages, and TrackMenu gains Go to artist and Go to album.
- [ ] **A02** Search returns only songs or videos, though the field says 'Artists, albums, tracks…' *(P4, M)*
  Done when: ALBUMS and PLAYLISTS filters, and ARTISTS once A01 exists, show card grids that open PageView. Until then the placeholder no longer promises them.
- [ ] **A03** Long YouTube Music playlists stop at the first page *(P4, M)*
  Done when: Catalog::openPage follows continuation tokens, either as the user scrolls or up to a cap, so pageTracks holds the whole playlist.
- [ ] **A04** Shelves have no See all or Play all, cards can't be played without opening them, and Recently played has no Show all *(P5, M)*
  Done when: A shelf header opens a full view that loads more pages. Song shelves get Play all, album and playlist cards get a hover play button, and Recently played links to library:history.
- [ ] **A05** Search with nothing typed has no recent searches or moods & genres *(P5, M)*
  Done when: Recent searches (capped at about 50, each removable, with Clear all) appear under the empty field and on an idle Search page, along with YouTube Music's moods & genres as cards. The recommender shelves sit above them when a catalogue is set.
- [ ] **A06** Home is blank at every launch and when offline, because the feed isn't cached *(P5, S)*
  Done when: The last good Home result is saved, shown immediately at launch and refreshed in the background, and it stays on screen when the network fails.
- [ ] **X07** Video and fallback search results aren't re-ranked or cleaned of junk *(P7, S)*
  Done when: A shared ranking step, reusing pickResult's term list, reorders video and fallback results. YouTube Music's own song results keep their order.
- [ ] **X08** Home has no charts and no moods & genres *(P7, M)*
  Done when: Home adds chart shelves for the current country and a Moods & genres row whose chips open category shelves.

## Library & playlists

- [ ] **L01** Playlist songs and the play queue can't be reordered *(P4, M)*
  Done when: Drag handles, plus Alt+Up/Down, on playlist rows and upcoming queue rows. They are backed by Library.movePlaylistEntry and QueueModel::move, which keep the current index and shuffle order consistent.
- [ ] **L02** Right-click and more menus exist only in track tables *(P4, M)*
  Done when: Right-click and a more button open TrackMenu on download, queue and suggestion rows. Sidebar playlists get Play, Rename and Delete, saved cards get Remove from library, and Liked songs gets Add all to queue and Add all to playlist.
- [ ] **L03** The song menu lacks Copy link, Open on YouTube, Remove from history and Remove from library *(P5, S)*
  Done when: TrackMenu adds Copy link (music.youtube.com/watch?v=<id>), Open on YouTube, Remove from history (when shown in History) and Remove from library.
- [ ] **L04** The library has no Songs or Artists view *(P5, M)*
  Done when: A SONGS tab lists every distinct liked, playlist and downloaded song with Shuffle all. An ARTISTS tab groups them, and an artist opens their songs, or the A01 artist page once it exists.
- [ ] **L05** No filter or sort in the library, playlists or Downloads, and playlists can't be pinned or reordered *(P5, M)*
  Done when: A filter field and Recent / A–Z / Artist sort, remembered per view, on the library, playlists, Liked songs and Downloads, with clickable column headers. Playlists can be pinned and dragged into order, saved to playlists.position.
- [ ] **X01** No library export or import (the iPhone app's backup JSON) *(P7, M)*
  Done when: Settings exports playlists and likes in the iPhone's format and imports such a file, with a Merge or Replace choice and a result toast.
- [ ] **X02** No local music files *(P7, L)*
  Done when: 'Add files…' and 'Add folder…' open native dialogs, read tags and cover art with ffprobe, add the songs to the library, and show a playable 'Local files' collection.
- [ ] **X03** No way to import a playlist from a YouTube, YouTube Music or Spotify link *(P7, L)*
  Done when: An Import playlist dialog takes a YouTube Music playlist link (loading every page) or a Spotify playlist or album link, matches each song by title, artist and length, creates a playlist, and lists the songs it couldn't find.
- [ ] **X04** Playlists have no description and no 'Add songs' panel *(P7, M)*
  Done when: An editable description appears under the playlist title, and an Add songs panel on the playlist page searches the library and YouTube and adds or removes songs in place.

## Settings

- [ ] **S01** *(Partly done: the data now downloads to a default location from Settings; the manual fields still have no picker or ~ expansion.)* Recommendation folders must be typed by hand: no picker, no ~ expansion, no default location *(P5, S)*
  Done when: A 'Choose…' button opens a native folder picker, a leading ~ is expanded, and with nothing set the app looks in its data folder and beside the executable (Contents/Resources in a Mac bundle).
- [ ] **X05** No in-app log, and release builds have no console *(P7, M)*
  Done when: A message handler keeps recent log lines in memory and in a rotating log file in the app's data folder. About adds 'Copy playback log' and 'Open log folder'.
- [ ] **X06** Missing small preferences: start page, prefer video, confirm before clearing the queue, reset all data *(P7, M)*
  Done when: Settings adds a start-page choice, 'Prefer video when available', 'Confirm before clearing queue' and a two-step 'Reset all data' that keeps downloaded files.

## macOS build

- [ ] **M01** libmpv will refuse to start on macOS because LC_NUMERIC is never reset to 'C' *(P6, S)*
  Done when: std::setlocale(LC_NUMERIC, "C") is called right after QGuiApplication and before MpvEngine, and audio plays on macOS when launched from both Terminal and Finder.
- [ ] **M02** The macOS title-bar flags need Qt 6.9, but CMake and the README allow 6.6 *(P6, S)*
  Done when: CMake requires 6.9 on APPLE and the README says so, or the flags are guarded with QT_VERSION_CHECK(6,9,0) and fall back to the standard title bar.
- [ ] **M03** yt-dlp, FFmpeg and Deno aren't found when the app is opened from Finder, and any python3 makes yt-dlp look installed (Windows too) *(P6, S)*
  Done when: On macOS the lookup also searches the bundle's tools folder, ~/Library/Application Support/Monolist/tools, /opt/homebrew/bin and /usr/local/bin. The python fallback is used only if `-m yt_dlp --version` succeeds, and missing-tool messages give the right step for each platform.
- [ ] **M04** The build-without-libmpv option (-DMONOLIST_NO_MPV=ON) no longer compiles *(P6, S)*
  Done when: The stub matches the header and VideoSurface's mpv code is guarded, so a NO_MPV build compiles and shows 'Built without libmpv'.
- [ ] **M05** The Mac build is a bare executable, not a Monolist.app *(P6, M)*
  Done when: On APPLE, CMake sets MACOSX_BUNDLE and OUTPUT_NAME Monolist and uses a cmake/Info.plist.in (bundle ID, version from PROJECT_VERSION, build number, minimum macOS, Music category, icon). cmake --build then produces a Monolist.app that opens from Finder.
- [ ] **M06** No macOS setup or build script, and the README's Mac recipe is three lines *(P6, M)*
  Done when: scripts/setup-macos.sh installs pinned Qt, CMake, Ninja, pkgconf and libmpv, plus arm64 yt-dlp_macos, ffmpeg/ffprobe and deno, with checksum checks. scripts/build-macos.sh configures, builds, runs the QML lint check, deploys into Monolist.app and supports --run. The README documents both.
- [ ] **M07** Nothing turns the Mac build into a self-contained, signed .app or .dmg *(P6, L)*
  Done when: The build script runs macdeployqt with -qmldir, copies libmpv and its libraries into Contents/Frameworks, places ffmpeg, ffprobe, deno and yt-dlp where code signing allows, signs the app (ad-hoc or Developer ID, per the decision), and writes Monolist-<version>-arm64.dmg that runs on a clean Mac.
- [ ] **M08** The Mac title bar has never been run: fixed 78 px inset, traffic lights off the 64 px bar, a gap in full screen, and double-click ignores the system setting *(P6, M)*
  Done when: On a Mac, the inset comes from QWindow::safeAreaMargins and disappears in full screen. The traffic lights are centred in the bar with a small Objective-C++ helper, or the bar's contents line up with them. Double-click follows the system's double-click setting, checked with a screenshot.
- [ ] **M09** No Mac menu bar or Cmd shortcuts, and closing the window quits and stops the music *(P6, M)*
  Done when: Native menus: App (About, Settings… Cmd+,, Quit), Controls (Play, Next, Previous) and Window (Minimize, Zoom). Closing the window hides it while music keeps playing, a Dock click brings it back, and Cmd+W and Cmd+M work.
- [ ] **M10** 'Update components' only works from the Windows development folder *(P6, M)*
  Done when: A cross-platform updater downloads yt-dlp, FFmpeg and Deno for the current OS and architecture, with checksums, into a writable tools folder in the app's data folder that is searched before the bundled copies. A Homebrew development install instead suggests 'brew upgrade'.

## Distribution

- [ ] **G01** The app has no icon on any platform *(P8, S)*
  Done when: An icon made from the 'MONOLIST.' wordmark ships as .ico on Windows (via an .rc file), .icns in the Mac bundle, and PNG for the window icon.
- [ ] **G02** No Windows build that can be given to someone else *(P8, M)*
  Done when: A -Package option, or an Inno Setup/CPack step, produces a self-contained zip or installer with real copies of the tools, a Start-menu shortcut and the icon.
- [ ] **G03** *(Partly done: the recommendation data is credited in Settings and About; bundled components still have no notices.)* No licence notices for bundled components, and no credit for the recommendation data *(P8, S)*
  Done when: About → Acknowledgements lists every component with its licence text, and the texts ship in the Windows package and inside the Mac app. A credit line for the data appears whenever a catalogue or graph is loaded.

## Docs

- [ ] **DOC1** The README describes the old source order and leaves out Settings, the recommender, video and macOS *(P9, S)*
  Done when: The README matches the code: the source order, singletons, layout, self-tests, the recommender data and its licence, Known gaps, and the macOS build.
- [ ] **DOC2** DESIGN.md and some code comments disagree with the code *(P9, S)*
  Done when: Each deliberate exception is written into DESIGN.md with its reason (or the code changes), the macOS title bar is described, and the stale comments and tokens are fixed.

## Found while fixing

Seen by the agents that fixed B01–B13 and by their reviewers, and not yet fixed.

- [x] **F01** Player-bar tooltips never appear: hovering logs "QQmlComponent: Component is not ready" *(P2, S)*
  Done when: every ToolTip in the player bar shows on hover with no warning.
- [x] **F02** With nothing loaded, the centre button shows Pause because Player.playing is true at launch *(P2, S)*
  Done when: an empty player shows Play, and playing is false until something plays.
- [x] **F03** The search box overlaps the forward arrow at narrow window widths (TopBar.qml:134) *(P3, S)*
  Done when: the search box shrinks or moves so nothing overlaps at any width down to the minimum.
- [x] **F04** Pressing Previous twice records the song as heard for 0 seconds *(P3, S)*
  Done when: restarting a song closes its play event with the time actually heard.
- [x] **F05** The track menu shows a greyed "Downloading…" for queued and running downloads, with no Cancel *(P3, S)*
  Done when: queued, downloading and processing tracks offer "Cancel download".
- [ ] **F06** IconButton's iconSize has no effect: every glyph is drawn at the button's full size *(P4, S)*
  Done when: iconSize sets the glyph size and existing buttons look the same or better.
- [ ] **F07** Network timeouts may never be retried: Qt reports a transfer timeout as a cancel, which InnerTube skips *(P4, S)*
  Done when: a timed-out InnerTube request is retried once, and a real cancel is not.
- [ ] **F08** A country rejected during a Home load makes Home load twice, and Catalog's own retry timer survives a refresh *(P5, S)*
  Done when: one refresh at a time, whatever triggered it.
- [ ] **F09** "Update components": a timeout's message is overwritten by the exit-code message that follows it *(P5, S)*
  Done when: the user sees "took too long" when that is what happened.
- [ ] **F10** Unmuting after a restart that happened while muted goes to 65% instead of the old level *(P6, S)*
  Done when: the level before muting is remembered across restarts.
- [ ] **F11** Esc may be claimed by both Now Playing and an open menu or the volume popup *(P6, S)*
  Done when: Esc closes the topmost thing only.
- [ ] **F12** Compiler warnings: deprecated QDateTime::setTimeSpec (taste.cpp), unchecked QFile::open in --content-test (main.cpp) *(P8, S)*
  Done when: the changed files build without warnings.

- [ ] **F13** Data loss: when a download fails or is cancelled, a finished file for the same song that the database does not know about (after app data was reset, or a test run) is deleted with the partial files *(P1, S)*
  Done when: only yt-dlp's partial and intermediate files are removed, never a complete audio file, whatever the database says.
- [ ] **F14** Tests and self-tests download into the user's real Music folder even with a scratch data folder *(P2, S)*
  Done when: MONOLIST_DATA_DIR (or a MONOLIST_DOWNLOAD_DIR override) keeps test downloads out of the real Music folder.
- [ ] **F15** Radio tracks are never marked as radio: openPlayEvent's fromRadio test can never be true, so play_events and Last.fm's chosenByUser treat autoplay songs as chosen *(P2, S)*
  Done when: songs added by autoplay radio are recorded with source "radio" and scrobbled with chosenByUser=0.
- [ ] **F16** Pin the published manifest's SHA-256 in the app once Monolist-data v1 is pushed, so a moved tag cannot swap the data *(P3, S)*
  Done when: RecData refuses a manifest whose hash differs from the one built into the app for that version.
- [ ] **F17** With "Hide explicit titles" on, pressing a clean suggestion can still play an explicit version, because the YouTube search that resolves it is not filtered *(P5, S)*
  Done when: with the switch on, the resolver prefers a non-explicit result when one exists.
- [ ] **F18** The download button does nothing while a download is processing (FFmpeg), though the menu and Downloads page can cancel it *(P5, S)*
  Done when: the button cancels in every in-flight state.
- [ ] **F19** With nothing loaded, the player bar's heart offers "Add to Liked songs" and does nothing *(P6, S)*
  Done when: the heart is disabled or hidden when nothing is loaded.
- [ ] **F20** The build-without-libmpv stub (-DMONOLIST_NO_MPV=ON) no longer compiles: load() and setVideoEnabled are out of date *(P6, S)*
  Done when: that configuration builds. (Same as M04.)
- [ ] **F21** The README's self-test list lacks --rec-test, --graph-test, --artist-test, --content-test and the new --lastfm-test, --cookie-test, --listen-test, --scrobble-test, --lastfm-connect-test, --ytm-session-test, --secret-test *(P9, S)*
  Done when: every self-test flag is documented with what it checks.

## Connections

Built so far (batch 2): Last.fm connect, listening-time tracking, offline scrobble queue and sending; YouTube Music session import (cookies file, header or cURL), SAPISIDHASH, signed-in browse requests; a DPAPI secret store. All tested against fakes only.

- [ ] **C01** Last.fm live test: needs the owner's API key in MONOLIST_LASTFM_API_KEY / MONOLIST_LASTFM_SHARED_SECRET; then connect, scrobble, revoke *(P3, S)*
  Done when: a real scrobble shows on the owner's profile and revoking gives the Reconnect state.
- [ ] **C02** YouTube Music live test with the owner's spare account: import, account name, logged_in check *(P3, S)*
  Done when: an imported session shows the account name and survives a restart.
- [ ] **C03** YouTube Music personalised Home when signed in (design step 14) *(P4, M)*
  Done when: signed-in Home differs from signed-out and reports logged_in=1.
- [ ] **C04** Read-only imports: liked songs as "Liked on YouTube Music", library playlists, private playlists, history (design step 15) *(P4, L)*
  Done when: counts match the account.
- [ ] **C05** macOS Keychain backend for the secret store (secretstore_mac.mm) — for the macOS session *(P6, M)*
  Done when: the secret-test self-test passes on a Mac.

## Decisions waiting on the owner

- [x] **Decided: build both.** Connections: should YouTube Music sign-in (a cookie file kept in the Mac Keychain or Windows Credential Manager) and/or Last.fm scrobbling be built now, later, or should the 'Designed, not built' section be hidden until then? (SettingsView.qml:393-431)
- [ ] Update checks: the repo is public, so GitHub Releases on droidboy08-hub/Monolist can be the update feed (api.github.com/repos/droidboy08-hub/Monolist/releases/latest, appinfo.cpp:214-221). Will builds be published as Releases there?
- [ ] Country shelf title: keep 'From <country>' (current; the list is ranked by MusicBrainz ratings, not plays, shelves.h:61-69) or go back to 'Popular in <country>' as you first asked? The Settings note (SettingsView.qml:371) will be changed to match either way.
- [x] **Decided: Burzum is not blocked; add a "Hide explicit titles" setting.** Should Burzum be added to the suggestion block list (suitable.cpp:73), and do you want a 'Hide explicit titles' setting (suitable.h:29)?
- [ ] Should 'Clear history' also reset recommendations by deleting the listening events, or keep them and add a separate 'Reset recommendations'? Today it clears only Recently played and History (library.cpp:697-705), so Search still says 'Because you played…' for cleared songs.
- [x] **Decided: play that song, then similar songs.** When you click a search result, should the app keep queueing all the results (SearchView.qml:155-156), or play just that song and continue with similar songs like the iPhone app does?
- [ ] Appearance: stay a single paper-and-ink theme by design, or add a dark theme that follows the system? A Mac in dark mode currently gets a light app.
- [ ] iPhone and desktop libraries: is a JSON backup file enough (export on one, import on the other), or do you want live sync? iCloud sync needs a signed Mac app, and Windows would need a web service or your own server.
- [ ] *(Handled by the separate macOS session.)* Mac distribution: personal use only (ad-hoc signed, opened with right-click → Open), or shareable with others (Apple Developer ID at $99 a year plus notarization)?
- [ ] *(Handled by the separate macOS session.)* Mac architecture: Apple Silicon only (simplest, matches your Mac) or universal with Intel Macs (needs universal Qt, libmpv and tools)?
- [ ] The two untracked extracted folders ('Desktop Multiplatorm Music Player' and 'Spotify-inspired desktop player') and their .zip files are still at the repo root. They are not in git. Will you delete them from Finder (deleting on the network share is permanent), and should the zips go too?
- [ ] Will Monolist stay non-commercial? The recommendation catalogue is licensed CC BY-NC 4.0, which forbids commercial use and requires a credit in the app.

## Not yet examined

Areas the audit did not cover, or covered thinly. Each needs a look before the list above can be called complete.

- Accessibility: nothing in the project sets Accessible.name or Accessible.role (grep finds none). Nobody checked keyboard navigation inside lists, visible focus on page buttons, or how the app works with VoiceOver on macOS or Narrator on Windows.
- Window size and position are not remembered: Main.qml:11-12 opens at a fixed 1512×945, and no window-geometry saving was found. Behaviour across multiple monitors and mixed display scaling was not checked.
- Running the app twice: there is no single-instance guard (no QLockFile or QLocalServer), so a second launch opens another window on the same SQLite database and a second libmpv.
- Localisation: no qsTr anywhere, so every string is hard-coded English. Date and number formats were not checked.
- EU consent page: fetchVisitorData downloads www.youtube.com (innertube.cpp:371-405). In EU countries that can redirect to consent.youtube.com with no visitor ID, which would push most tracks off the fast tier. The iPhone app has a YouTubeConsentCookie for this, and nobody tested it here.
- Performance with large libraries and long queues: whether track lists render only visible rows, memory use, and the cost of the artwork cache were not examined.
- Playback features a desktop user might expect were not assessed: gapless playback (designed in Lumen), crossfade, an equalizer and playback speed.
- Drag and drop (songs onto sidebar playlists), selecting several songs at once, desktop notifications on track change, and a system tray or mini-player window were not examined.
- Real macOS behaviour: the app has never been run on a Mac. Unchecked: the software video renderer on Metal, CoreAudio device switching, App Nap slowing timers while the window is hidden, and Gatekeeper and privacy prompts when opening data folders on ~/Desktop.
- Linux (planned later): the frameless window and resize-edge path, Wayland startSystemMove, MPRIS and packaging were only touched in passing.
- Build automation: there is no GitHub Actions workflow for Windows or macOS builds now that the code is on GitHub, and no script builds a native Windows ARM64 version (it runs under x64 emulation).
- Security and privacy: how a downloaded tool update would be verified, safe storage for future sign-in secrets, and the arguments passed to yt-dlp were not reviewed.
- Database robustness: recovery after a crash mid-write, repeated migrations, and backup of monolist.db were not examined.
