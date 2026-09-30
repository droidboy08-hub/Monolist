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
- [x] **D01** The download queue is lost on quit, and partly downloaded files are deleted *(P5, M)*
  Done 2026-09-30: the download_queue table, written a second after any change and as the app closes, in the order the downloads will run (those running first). At launch, or once yt-dlp is found, queued ones are queued again, and failed ones come back as failed with why. yt-dlp's partial file is kept on quit and carried on from; a finished download clears any stale one. `--download-queue-test` (offline) and `--download-resume-test <id>` (network): a real song stopped at 31% was finished by the next launch from the kept part.
  Done when: Queued and failed downloads are stored in a table and queued again at launch.
- [x] **D02** The download folder can't be changed *(P5, M)*
  Done 2026-09-30: CHANGE… in Settings → Downloads opens the system's folder picker, and DEFAULT goes back to Music\Monolist. The choice is kept (download_dir) and checked by writing to the folder first; it is refused while a download is running. Songs already downloaded stay where they are, and any whose file has gone missing are looked for in the chosen folder, so a folder moved by hand is found again and the library plays from there. `--download-folder-test`.
  Done when: A 'Change…' action in Settings opens a native folder picker, saves the choice and rescans the stored files.

## Player

- [x] **B03** Player settings, including Settings' Autoplay switch, reset on every launch *(P1, S)*
  Done when: Volume, shuffle, repeat and autoplay are saved when they change and restored in main.cpp before QML loads.
- [x] **B05** After Next or Previous the old song keeps playing until the new one resolves, and its end can skip or fail the new track *(P1, S)*
  Done when: Starting a new track stops or pauses the old file, and mpv events are matched to the current playlist_entry_id from MPV_EVENT_START_FILE. Nothing from the previous file reaches the new one, and position starts at 0.
- [x] **B06** Player bar: the queue and Now Playing buttons vanish below 1040 px, volume has no fallback, and the queue button does nothing while Now Playing is open *(P1, S)*
  Done when: The Now Playing and queue toggles stay visible at every width, and only volume collapses to a button with a popup slider and mute. In Now Playing the queue button switches the right pane to UP NEXT.
- [x] **U01** *(Order changed in engine batch B8, as the owner agreed: a refused InnerTube link is first asked for afresh, once a track, and itag 18 follows; the itag 18 link then serves that play only. `playback.refused=muxed` puts it back. Since the batch's review: when the fresh link is refused too, the song is taken as refused for an hour, and its next plays start from the itag 18 link at once, with no fresh InnerTube link asked for; see F45 for how the fresh link fared on real refusals.)* The itag 18 fallback doesn't run when a track actually fails to play (user request) *(P3, S)*
  Done when: When mpv fails on an InnerTube adaptive URL, the same track is retried once with its best progressive format (itag 18) before yt-dlp. This is exercised on a real track, with the itag logged.
- [x] **P01** The output-device button in the player bar does nothing *(P4, M)*
  Done when: MpvEngine observes audio-device-list and sets audio-device. The button opens a menu of outputs (WASAPI on Windows, CoreAudio on macOS) with the current one marked, and the choice is saved.
- [x] **P02** No media keys and no system Now Playing (Windows media flyout, macOS Control Center, AirPods) *(P4, L)*
  Done 2026-09-30 for Windows (macOS had MacMediaSession already): WinMediaSession, Windows' System Media Transport Controls through the Windows Runtime, declared by hand since this MinGW's headers lack them. Title, artist, the cover (Windows fetches it from its address), playing or paused, and the timeline; play, pause, stop (a pause), next, previous and seek come back through the player's event loop. Without the controls, the keys still work while the window has the focus. The exe now has a version resource, so Windows names it "Monolist". Checked from outside through Windows' own session list: the song, its cover (a 21 KB JPEG), play, a seek to 40 s, pause and previous. MPRIS for Linux is still to do.
  Done when: A small platform layer publishes title, artist, artwork and position, and handles play, pause, next, previous and seek. It uses the MediaPlayer framework through Objective-C++ on macOS and SMTC (or a WM_APPCOMMAND fallback) on Windows, with MPRIS later for Linux.
- [x] **P04** The queue, current song and position are not restored after a restart *(P5, M)*
  Done 2026-09-30: PlaybackController::restoreSession, with the queue in player.queue (at most 500 rows around the song playing, the order shuffle hides, where it came from; written 2 s after a change) and the place in player.place (every 10 s while it moves, and on quit). A place within 5 s of the start or 10 s of the end starts the song from the top. `--session-test`, and a real launch and close.
  Done when: The last queue, current index and position are saved on quit and restored, paused, at launch.
- [ ] **P05** *(Partly done: the player bar has the song menu, from its dots or a right click on the song; Now Playing and the sleep timer are still to do.)* No menu for the song that is playing, and no sleep timer *(P5, S)*
  Done when: A more button on the player bar and in Now Playing opens TrackMenu for Player.currentTrack, plus a sleep timer (15, 30, 45 or 60 minutes, or end of song) shown as a countdown that pauses playback.
- [ ] **P06** *(Partly done: Now Playing's source note is now one line built from what mpv reports, such as 'Streaming · InnerTube · Opus · 48 kHz · 133 kbps' or 'Offline · Local file · AAC · 44.1 → 48 kHz', exposed as Player.streamInfo, and every file start writes a `stream:` line to the log. The player bar's buffering and source label and the lyrics error reasons are still to do.)* Buffering, streaming-versus-offline source and lyrics error reasons are never shown *(P5, S)*
  Done when: The player bar's status line shows buffering and a small source label such as 'Offline · local file' or 'Streaming · InnerTube'. The lyrics error says why it failed.

## Recommendations

- [x] **B10** Quitting while recommendations are being built can crash the app *(P2, S)*
  Done when: The worker checks a stop flag between scans, and the destructor waits until it stops, so quitting mid-build exits cleanly.
- [x] **B11** The recommender learns the wrong things: re-liked songs never count, skipped songs seed 'More like…', weekends use the UTC day *(P2, S)*
  Done when: The newest like or unlike per song decides. Only positively labelled plays seed song rails (label ≥0.6, liked, or unlabelled but heard for ≥30 s). The weekend check uses the local date.
- [x] **R01** 'Not interested' is read by the taste profile, but nothing can record it *(P3, M)*
  Done when: 'Not interested' appears on suggestion rows, in TrackMenu and in Now Playing. It records a notInterested event, removes the row, keeps the song out of shelves and autoplay radio, and can be undone from the toast.
- [ ] **R02** Search is missing rails and scoring: weekend and 'back to' rails, Liked/playlist/genre rails, and skipped songs never count against *(P3, M)*
  Done when: Search shows 'Your weekend sound' and 'Back to a couple of months ago' when their minimum listening is met, plus 'More like your Liked songs', 'More like <playlist>' and genre shelves. Taste shelves rank with profile.score, so sounds the user skips sink.
- [x] **R03** Suggestion shelves are tap-to-play only: no Play all, row menu, loading state or See all *(P3, M)*
  Done when: Each shelf header has Play all, which resolves the songs lazily in order, and See all, which loads more rows from the same source. Rows get TrackMenu on right-click and a spinner while resolving.
- [x] **R04** Suggestions never rotate, and they include songs the user already owns *(P3, M)*
  Done when: A refresh on the Search page brings up different rows, and the page rebuilds after about 45 minutes. Songs in the library, playlists and downloads are left out, and that set is part of the rebuild check.
- [ ] **R05** Every suggestion press searches YouTube, even for songs already in the library, and the answer isn't remembered *(P5, M)*
  Done when: play() first plays a library or download copy of the song if one exists. Otherwise it searches and caches the pick for a while, and later presses and Play all use the cache.
- [ ] **R06** No 'Recommended' section on playlist pages, and queue radio never uses the recommender *(P5, L)*
  Done when: A playlist page ends with a Recommended list that is exactly what plays after it. A queue started from a playlist continues with a mix drawn from several of its songs, and falls back to the catalogue when YouTube doesn't answer.
- [ ] **X09** The iPhone's listening history (play_events.jsonl) can't be imported *(P7, M)*
  Done when: Settings imports play_events.jsonl into play_events, skipping duplicates by title, artist and time, then rebuilds suggestions. Exporting in the same format is optional.

## Polish

- [x] **U02** Slow-feeling scrolling was never investigated (user request) *(P3, S)*
  Done when: Scrolling is compared on a Release build and natively on the Mac. If it is still sluggish, wheel step and flick velocity are tuned once in a shared scroll component used by every view.
- [ ] **L06** Missing feedback and empty states: no page retry, silent download failures, a blank or bare Search page, 'Art' in an empty player bar *(P5, M)*
  Done when: Page errors get RETRY. Download failures, 'Download all' and 'Copy for a bug report' each show a toast. Search explains an empty or not-yet-personal page. The empty player bar says 'Nothing playing', with the heart, download and transport controls disabled.
- [x] **N01** Back and forward have no keyboard or mouse-button bindings, and never grey out *(P5, S)*
  Done 2026-09-30: Alt+Left/Right, Cmd+[ and ] on a Mac, a keyboard's Back and Forward keys, and the mouse's side buttons anywhere in the window. Back leaves full-screen video and Now Playing first, as Esc does. The arrows dim at 0.3 when there is nowhere to go, as a shelf's do. A deleted playlist leaves the history both ways. Checked on the real window with posted side-button presses.
  Done when: Shortcuts and the mouse's side buttons go back and forward, and the arrows dim when there is nowhere to go.
- [ ] **G04** Font weights and tooltips may look different on macOS *(P8, S)*
  Done when: Font.Bold becomes Theme.weightMedium or Theme.weightBlack (or a Bold face is bundled). The window sets the font and a palette from Theme, or a styled tooltip component is used, and the two TextFields match the other inputs.
- [ ] **G05** Home's 'Open album' button shows a play icon *(P8, S)*
  Done when: The button shows an arrow, or actually plays the release, and its label follows the release type.
- [x] **G06** Six InnerTube instances each download youtube.com at startup for a visitor ID only one of them uses *(P8, S)*
  Done when: The visitor ID is fetched only by the instance that resolves streams, or shared, so it is downloaded once per session.
- [ ] **G07** The self-tests can't fail, and there are no automated tests *(P8, M)*
  Done when: Each self-test exits non-zero when an expectation fails. A CTest target checks the golden match keys, a taste profile built from fixed events, and fixed search results when a catalogue path is given, and GitHub CI can run it.
- [ ] **G08** If the database can't be opened, the app fails silently *(P8, S)*
  Done when: The app falls back to an in-memory database and tells the user that the library couldn't be opened, where the file is, and that changes won't be kept.

## Video

- [x] **U03** Video works only in wide Now Playing: no switch in the narrow layout, no fullscreen or mini panel, and hidden video keeps decoding *(P3, M)*
  Done when: Narrow Now Playing gets the switch and a 16:9 plate, and hiding the last visible surface drops back to audio or shows a mini panel above the bar. F or double-click opens fullscreen video, and Esc leaves it.
- [x] **P03** Music videos lose their video switch once liked, saved to a playlist or replayed from History *(P4, M)*
  Done when: A migration adds is_video, every insert path writes it and every reader exposes it, so a video played from anywhere offers the switch.

## Artist pages & search

- [x] **A01** Artist pages are not built, and artist names are never links *(P4, L)*
  Done when: An 'artist:<UC id>' view shows top songs, albums and singles, and related artists, with Play and Shuffle. Tracks carry artist and album browse IDs, so names link to those pages, and TrackMenu gains Go to artist and Go to album.
- [x] **A02** Search returns only songs or videos, though the field says 'Artists, albums, tracks…' *(P4, M)*
  Done when: ALBUMS and PLAYLISTS filters, and ARTISTS once A01 exists, show card grids that open PageView. Until then the placeholder no longer promises them.
- [x] **A03** Long YouTube Music playlists stop at the first page *(P4, M)*
  Done when: Catalog::openPage follows continuation tokens, either as the user scrolls or up to a cap, so pageTracks holds the whole playlist.
- [x] **A04** Shelves have no See all or Play all, cards can't be played without opening them, and Recently played has no Show all *(P5, M)*
  Done when: A shelf header opens a full view that loads more pages. Song shelves get Play all, album and playlist cards get a hover play button, and Recently played links to library:history.
- [ ] **A05** Search with nothing typed has no recent searches or moods & genres *(P5, M)*
  Done when: Recent searches (capped at about 50, each removable, with Clear all) appear under the empty field and on an idle Search page, along with YouTube Music's moods & genres as cards. The recommender shelves sit above them when a catalogue is set.
- [x] **A06** Home is blank at every launch and when offline, because the feed isn't cached *(P5, S)*
  Done 2026-09-30: the last good answers of the signed-out feed and of new releases, per country, in home_cache (compressed, as they came), read back through the same parsing at launch before anything is asked; Home then refreshes behind them. They stay on screen when the network fails, retried twice by themselves, with "Home as it was on <date>" beside RETRY. The account's feed is never kept: it is its own and always asked afresh. `--home-account-test` part 7b; a real second launch showed a full Home 0.4 s after its window.
  Done when: The last good Home result is saved, shown immediately at launch and refreshed in the background, and it stays on screen when the network fails.
- [ ] **X07** Video and fallback search results aren't re-ranked or cleaned of junk *(P7, S)*
  Done when: A shared ranking step, reusing pickResult's term list, reorders video and fallback results. YouTube Music's own song results keep their order.
- [ ] **X08** Home has no charts and no moods & genres *(P7, M)*
  Done when: Home adds chart shelves for the current country and a Moods & genres row whose chips open category shelves.

## Library & playlists

- [x] **L01** Playlist songs and the play queue can't be reordered *(P4, M)*
  Done when: Drag handles, plus Alt+Up/Down, on playlist rows and upcoming queue rows. They are backed by Library.movePlaylistEntry and QueueModel::move, which keep the current index and shuffle order consistent.
- [x] **L02** Right-click and more menus exist only in track tables *(P4, M)*
  Done when: Right-click and a more button open TrackMenu on download, queue and suggestion rows. Sidebar playlists get Play, Rename and Delete, saved cards get Remove from library, and Liked songs gets Add all to queue and Add all to playlist.
- [x] **L03** The song menu lacks Copy link, Open on YouTube, Remove from history and Remove from library *(P5, S)*
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
- [x] **M04** The build-without-libmpv option (-DMONOLIST_NO_MPV=ON) no longer compiles *(P6, S)*
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
  Windows done 2026-09-29: packaging/make-icon.ps1 draws "M." (Archivo, paper on ink, the point in red) into monolist.ico, built into monolist.exe (IDI_ICON1, the window's and the taskbar's) and the installer. Left: the .icns for the Mac bundle.
  Done when: An icon made from the 'MONOLIST.' wordmark ships as .ico on Windows (via an .rc file), .icns in the Mac bundle, and PNG for the window icon.
- [x] **G02** No Windows build that can be given to someone else *(P8, M)*
  Done 2026-09-29: package-windows.ps1 (-Installer), packaging/monolist.iss, .github/workflows/release-windows.yml. Installed and run with the toolchain out of PATH: self-tests, playback, a download through the bundled yt-dlp and FFmpeg; uninstalled clean.
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
- [ ] **F07** *(Seen in engine batch B9: on Qt 6.11 a timed-out /player came back as "Operation timed out" and its retry did run, 8 + 1.2 + 8 s, in `--bounds-test --before`. /player no longer retries that way, but the other calls still do: check them before closing this.)* Network timeouts may never be retried: Qt reports a transfer timeout as a cancel, which InnerTube skips *(P4, S)*
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

- [x] **F13** Data loss: when a download fails or is cancelled, a finished file for the same song that the database does not know about (after app data was reset, or a test run) is deleted with the partial files *(P1, S)*
  Done when: only yt-dlp's partial and intermediate files are removed, never a complete audio file, whatever the database says.
- [x] **F14** Tests and self-tests download into the user's real Music folder even with a scratch data folder *(P2, S)*
  Done when: MONOLIST_DATA_DIR (or a MONOLIST_DOWNLOAD_DIR override) keeps test downloads out of the real Music folder.
- [x] **F15** Radio tracks are never marked as radio: openPlayEvent's fromRadio test can never be true, so play_events and Last.fm's chosenByUser treat autoplay songs as chosen *(P2, S)*
  Done when: songs added by autoplay radio are recorded with source "radio" and scrobbled with chosenByUser=0.
- [x] **F16** Pin the published manifest's SHA-256 in the app once Monolist-data v1 is pushed, so a moved tag cannot swap the data *(P3, S)*
  Done when: RecData refuses a manifest whose hash differs from the one built into the app for that version.
- [ ] **F17** With "Hide explicit titles" on, pressing a clean suggestion can still play an explicit version, because the YouTube search that resolves it is not filtered *(P5, S)*
  Done when: with the switch on, the resolver prefers a non-explicit result when one exists.
- [x] **F18** The download button does nothing while a download is processing (FFmpeg), though the menu and Downloads page can cancel it *(P5, S)*
  Done when: the button cancels in every in-flight state.
- [x] **F19** With nothing loaded, the player bar's heart offers "Add to Liked songs" and does nothing *(P6, S)*
  Done when: the heart is disabled or hidden when nothing is loaded.
- [x] **F20** The build-without-libmpv stub (-DMONOLIST_NO_MPV=ON) no longer compiles: load(), setVideoEnabled and setVideoWatched are out of date *(P6, S)*
  Done when: that configuration builds. (Same as M04.)
- [ ] **F21** The README's self-test list lacks --rec-test, --graph-test, --artist-test, --content-test and the new --lastfm-test, --cookie-test, --listen-test, --scrobble-test, --lastfm-connect-test, --ytm-session-test, --secret-test *(P9, S)*
  Done when: every self-test flag is documented with what it checks.
- [ ] **F22** Downloading a song again in the "original" format while its finished file is still in the folder, unknown to the database, has FFmpeg rewrite that file in place, so a cancel or failure at that moment leaves it cut short *(P4, S)*
  Done when: a finished file already in the folder for that video id is taken into the library instead of being downloaded again, or the new copy is written under another name until it is complete.
- [ ] **F23** The video sometimes fails at once with HTTP 403 on a freshly resolved yt-dlp link (2 of about a dozen runs on LrM_Y39Gmhk), and the app goes straight back to sound with "This video would not play" *(P4, S)*
  Done when: a refused picture is resolved once more (a fresh link, or the android/tv_simply clients, whose streams played whole in September 2026) before the app gives up on it.
- [ ] **F24** Cards that open a page (albums, playlists, artists) carry the same red play triangle as cards that play (AlbumCard.qml footer), so an artist card promises to play and opens a page *(P8, S)*
  Done when: the footer mark says what a click does: an arrow for a page, the triangle for a song or video.
- [ ] **F25** A song queued from a track table's menu loses its first credit for Last.fm: TrackTable.trackOf leaves out `primaryArtist`, so "Play next" or "Add to queue" on "Lady Gaga & Bruno Mars" scrobbles the whole line as one artist (Scrobbler::scrobbleArtist falls back to `artist`) rather than "Lady Gaga" *(P6, S)*
  Done when: trackOf carries primaryArtist, and a joint-credit song queued from the menu scrobbles its first credit.
- [ ] **F26** YouTube Music's own playlists (RDCLAK…) stop at about a hundred songs: asked anonymously, every continuation answers the first hundred again ("'90s Alternative" lists 126 songs and 101 load; the page stops there rather than looping) *(P5, S)*
  Done when: the rest comes another way (the watch playlist, /next with the playlist id, or the signed-in session), or the page says how many of the total it could load.
- [x] **F27** Home shows only the first two or three shelves of YouTube Music's feed: the feed's own continuation is never followed, and asked anonymously it answers an empty page (token in the body, in the URL, or both) *(P5, M)*
  Done 2026-09-30: the empty page was the visitor. A signed-out continuation answers only when it names the visitorData its first page was answered to (checked on the live API: without it 0 shelves, with it the next 3), and browse calls named none. Home now keeps that visitor and the feed's token, and asks for the next page as the reader nears the end (Catalog.loadMoreHome into a ShelfModel, so only the new shelves are made). At most 6 pages signed out, and 3 for the account's own feed, whose pages are asked as the account through AccountGuard. `--home-account-test` part 7c.
  Done when: Home follows the feed's continuation as the reader scrolls (probably with the visitor cookie or the signed-in session), and shows the shelves below the first few.
- [ ] **F28** Track tables make every row up front (a Repeater in a Column): 100 rows cost 250-300 ms in the Debug build here, so a 5,000-song playlist would hold 5,000 heavy rows. Pages now add them 25 at a time (at most ~90 ms a batch), which keeps scrolling smooth but not the memory. Every view stays made, so those rows stay resident after the page is left. Play, Shuffle, Download all and Add all no longer wait for the rows, only for the songs (Catalog.pageFetching) *(P5, M)*
  Done when: rows are made only near what is on screen (a ListView kept in step with the page's scroll, or rows that load their controls lazily), measured with --page <id> --all.
- [ ] **F29** The library's cards (Liked songs, your playlists, saved albums and playlists) have no play plate, while YouTube Music's cards on Home, artist pages, search and "show all" now do *(P6, S)*
  Done when: every album and playlist card plays from its plate, your own playlists and Liked songs included.
- [ ] **F30** Play next and Add to queue say nothing: with the queue closed there is no sign either worked, and from a suggestion's menu the song is first looked up, so nothing happens for a second and then still nothing is said *(P5, S)*
  Done when: both answer in the toast ("Playing next: <title>", "Added to queue"), or the queue button shows that something was added.
- [ ] **F31** "Not interested" and "Don't suggest <artist>" can be taken back only from their toast: once it has gone they hold for good (they are play_events rows), and nothing lists them or clears them *(P5, S)*
  Done when: Settings lists the songs and artists turned down, each removable, with a Clear all, and the page rebuilds after a change.
- [ ] **F32** Two lists still scroll with Qt's own wheel handling, capped at about 600 px a second: Settings' country list and the lyrics in Now Playing. They sit inside something else that scrolls or swallows the wheel, and SmoothWheel, which the pages, the queue and the sidebar now use, keeps every wheel event it gets, so at the country list's ends the wheel would no longer move on to the page *(P6, S)*
  Done when: both use SmoothWheel, the country list passes the wheel on to the Settings page at its ends, and the lyrics still stop following the song when scrolled by hand (LyricsPane uses onMovementStarted, which a wheel glide does not raise).
- [ ] **F33** Now Playing's colour field decodes its cover on the interface's thread: PaletteTool::request reads, scales and measures the image in the network reply's slot (artworkcache.cpp), the same 10-190 ms per cover that --scroll-test measured for covers before they moved to a thread pool *(P6, S)*
  Done when: the decode and the histogram run on ArtworkFetcher's pool and only the colour comes back.
- [ ] **F34** "Add all to queue" on a long page (PageView, PlaylistView) still queues one song per call: each is a JS-to-C++ call with its map, a queue model insert that the queue panel's captions re-read, a scan for autoplay's first row and a prefetch, so a few thousand songs hold the window for seconds. Download all now goes in one call (Downloads.enqueueAll) *(P6, S)*
  Done when: Player takes the whole list in one call and inserts it as one block of rows, measured on a 5,000-song page.
- [ ] **F35** Every recorded play rebuilds the whole History table: `playRecorded` reloads `Library.history` (up to 200 rows) with a model reset, and LibraryView's track table, made at launch, makes all its rows again, visible or not (F28). With 480 songs in Recently played (the size of the owner's Mac library) that is 490-600 ms of the interface standing still at every song start, measured in the Release build; Home's own ten take 25-35 ms more. Since B7 the sound no longer waits for it, but the window, the clock and mpv's events still do, and in the B7 runs the time from googlevideo's answer to the first sound rose from ~30 to ~100 ms whenever the rebuild or its repaint fell just after the load (likely CPU contention; not proven) *(P4, S)*
  Done when: a play moves or adds one row at the top of History (row moves and inserts, not a reset) or History is refreshed only while it is shown, and a song start with 200+ songs in History costs under 20 ms on the interface thread.
- [ ] **F36** A song whose every link mpv refuses stops the listening: once the last rung is refused, PlaybackController's loadFailed halts with "Playback failed" and a toast, where a song that fails to resolve is skipped (counted toward the three-in-a-row stop) and the queue carries on. Since B8 a refused file with nothing to stream it from is skipped too, so this is the one failure left that halts *(P4, S)*
  Done when: the last refusal goes through the same skip-or-stop as a failed resolve (PlaybackController::failTrack), and --recovery-test checks it.
- [ ] **F37** A stored visitor id that googlevideo has turned against makes every song play from its muxed stream: every InnerTube link minted with it is refused with 403 on its first request, the fresh one B8 asks for included, so each song pays the muxed rescue (about 2.3 s more, then itag 18, 96-128 kbps AAC resampled to 48 kHz) until the id is replaced. Since B1 the id is kept across launches (renewed once it is a day old and something has played, used for up to 30 days), so one bad id can last a day. Seen on 2026-09-28 with an id fetched from sw.js_data during the B8 measurements: 8 of 8 launches on Tu7oq3VNgpY and two other songs refused, both links every time; the same songs played at once from InnerTube with a new id (`--set youtube.visitor_data_at 0`). /player itself answered OK throughout, so PP-03's LOGIN_REQUIRED renewal never fires *(P2, S)*
  Done when: a fresh InnerTube link refused after a first refusal of the same song renews the visitor id (as LOGIN_REQUIRED does), so at most one song pays for a bad id, checked against a stand-in that refuses one id's links.
- [ ] **F38** The picture's yt-dlp lookups are outside B9's one-yt-dlp-at-a-time queue: with the video switch on, every song start runs `StreamResolver::resolveVideo` (a yt-dlp process, ~3 s of Python under emulation) beside the sound's resolve, and prefetchUpcoming runs one more for the next song's picture. When the sound also falls to yt-dlp, two or three extractions run at once, the slowdown the queue exists to avoid, and the picture lookups have no 20 s limit either *(P6, S)*
  Done when: picture lookups take a turn in the same queue (the current song's sound first, then its picture, then anything ahead of time), measured with the video switch on and InnerTube failing (--bounds-test's stand-in).
- [ ] **F39** A JioSaavn link named for 320 kbps can serve a far thinner file, and the race plays it as "JioSaavn · 320 kbps" over YouTube's better stream: "Baibaba Bimba" (Tenniscoats, JioSaavn auQ6LDS0) comes as a "…_320.mp4" of 4.45 MB for 6:04, 98 kbps, where YouTube's Opus is 137 kbps (1 of the 10 benchmark songs JioSaavn has, engine step JS). The mid-song move and JioSaavn downloads now check the file's own size over its length and pass such a file by; the race and a remembered match do not. Since the batch's review, a file the mid-song move found thinner than the YouTube stream it was to replace (or not the length JioSaavn lists) is remembered as "not taken" for 7 days, so the race and downloads pass it over at the song's next start (checked on "Baibaba Bimba": the second play is YouTube's Opus, where it was JioSaavn's 98 kbps as "320 kbps"); a match the move never weighed still is not checked *(P3, S)*
  Done when: the race weighs JioSaavn's link by what it serves (the Content-Length its CDN sends for a HEAD, or mpv's file average once open) and YouTube plays where that is not at least what YouTube offers.
- [ ] **F40** Where JioSaavn's file and YouTube's do not have the music at the same moment, moving between them "at the same second" skips or repeats the difference: JioSaavn's "Bohemian Rhapsody" has its music 840 ms later than YouTube's (and "Baibaba Bimba" 138 ms earlier), measured by cross-correlation (bench_S_align.ps1). The mid-song move lines the two up (AudioAlign), but the other moves do not: a refused JioSaavn link back to YouTube, the picture turned on over JioSaavn's sound, and the picture turned off back to it *(P5, S)*
  Done when: those moves use AudioAlign's lag (or the one the song already measured) to pick the moment in the other file, checked on "Bohemian Rhapsody" with --spoil-saavn.
- [ ] **F41** Since SI2, Home's feed is the account's, but whatever its cards and SHOW ALL links open is still asked for signed out: Catalog::openPage, openListing, playCollection and continueBrowse carry no account (InnerTube::browse(id, params) and continueBrowse take no Auth). A signed-in feed links to the account's own pages (its mixes, "Listen again", liked and private playlists), which signed out come back empty or refused, so Home would offer shelves that open to "YouTube Music did not return this page." Expected from the code, not yet seen: there is no account on the build machine *(P4, S)*
  Done when: the pages a signed-in Home links to open as the account (C04's step: Auth on browse(id, params), continueBrowse, page and listing opens), checked against a stand-in that answers a private playlist only with the account, and in C02's live test.
- [ ] **F42** Signed-out Home can come without Quick picks: between 11:23 and 11:26 (local time) on 2026-09-28, five launches in a row (fresh scratch data folders, US) showed New releases and the feed's card shelves ("Throwbacks"…) but no song list, four with SI2's Debug build and one with SI1's Release build, while a launch of SI1's Debug build at 11:10 had Quick picks. Catalog takes the first carousel of songs (musicResponsiveListItemRenderer) and parseShelves reads only musicCarouselShelfRenderer, so either YouTube Music's anonymous feed dropped the shelf, or it now sends it in a shape Monolist does not read (another renderer, or a later continuation of the feed) — not checked *(P4, S)*
  Done when: the raw FEmusic_home answer of such a launch is looked at, and Home shows the feed's song shelf whenever the answer has one (a parser test on that answer), or the missing shelf is shown to be YouTube's.
- [ ] **F43** A song that plays only with the account (C07) still has its picture and its download asked for signed out: `StreamResolver::resolveVideo` and `DownloadManager` run yt-dlp without the session's cookies, so the video switch on an age-restricted song ends in "This video would not play — back to audio", and its download fails with "Sign in to confirm your age". Expected from the code, not yet seen: there is no account on the build machine *(P5, S)*
  Done when: the picture and the download of a song the account had to play go through the same rung (its cookies file, its limit, its switch), checked against --account-play-test's stand-in.
- [ ] **F44** `--listen-test` passes only in a fresh data folder: run a second time in the same `MONOLIST_DATA_DIR` it fails "restarted at 0:05, skipped 20 s into the replay", apparently because it reads back the rows the earlier run left (its detail lists two pairs of 5000/20000 ms on the second run and three on the third; not traced further). Seen on the unmodified build of 680d00b as well as SI3's *(P8, S)*
  Done when: the test clears or scopes its own rows, and passes run after run in one folder.
- [ ] **F45** B8's fresh InnerTube link, asked for after googlevideo refuses a song's first, missed the plan's gate (keep the rung only if it rescues at least half of real refusals): over 468 fresh launches of bench_A_play since B8 (264 in the B9, SI1 and SI3 runs and the review's audit, 204 after the review's fixes), 24 songs had their first InnerTube link refused, and the fresh link was refused in all 24: it rescued none, and cost a median 263.5 ms (p90 372) before the muxed stream in the first set, 301 ms (p90 445) in the second. The same songs recur across launches that each fetched a brand-new visitor id (of the 54 benchmark launches that played each since engine step 2: 4D7u5KF7SP8 refused in 7, fsiPzT50ZiM 5, TiebZllW8As 4, DntZ3-yCaFs 3, phLb_SoPBlA 3), so these refusals look tied to the song more than to one link or one visitor id (compare F37). The order stays as the owner chose it (U01), and the replay it made ~13 times slower is fixed: a song both of whose InnerTube links are refused plays from its rescue link for an hour, and such a song's replay now starts in a median 276 ms (p90 366, 10 of 10 from the kept link), against 3,326 ms (p90 3,691, n=14) before the fix and 243.5 ms (p90 498, n=6) before B8 *(P4, S)*
  Done when: the owner decides whether `playback.refused=muxed` (no fresh link; ~0.26 s sooner on each refused song's first open) becomes the default, or a re-measure over at least 200 launches shows the fresh link rescuing real refusals (a seek's "one request served, then 403", which `--player-canary` met once on 2026-09-28, is the likeliest case).
- [ ] **F46** Picking Standard sound quality leaves a song already playing from JioSaavn to be fetched to its end from JioSaavn's CDN, and a JioSaavn download already running to finish from it, as the setting was designed ("from the next song on"). Since the batch's review everything else stops at once: a mid-song move under way, a late answer, a lookup a download waits on. If Standard is to mean that nothing more comes from JioSaavn from the moment it is picked, the song would have to move back to YouTube mid-song (with F40's alignment) and the download start again through yt-dlp *(P6, S)*
  Done when: the owner says which, and `--play … --saavn-off-at` shows it on a song playing from JioSaavn, and a download test on one being saved from it.

## Connections

Built so far (batch 2): Last.fm connect, listening-time tracking, offline scrobble queue and sending; YouTube Music session import (cookies file, header or cURL), SAPISIDHASH, signed-in browse requests; a DPAPI secret store. Engine step SI1 added the per-host jar, X-Goog-AuthUser, the account's visitor id, brand channels and the cookies.txt for yt-dlp (C06); SI2 made Home's feed the account's (C03, "Use my account for Home"); SI3 plays a song YouTube refuses signed out with the account (C07, "Play with my account when needed") and reports listens to the account's YouTube history (C08). All tested against fakes only.

- [ ] **C01** Last.fm live test: needs the owner's API key in MONOLIST_LASTFM_API_KEY / MONOLIST_LASTFM_SHARED_SECRET; then connect, scrobble, revoke *(P3, S)*
  Done when: a real scrobble shows on the owner's profile and revoking gives the Reconnect state.
- [ ] **C02** YouTube Music live test with the owner's spare account: import, account name, logged_in check *(P3, S)*
  Done when: an imported session shows the account name and survives a restart.
  The import is now a guide to follow alone (step import-ux, still by cookie import only): Settings → Connections → IMPORT SIGN-IN opens the steps (a private window, copy the sign-in out, close the window without signing out, and why), the two ways side by side (A, a cookies.txt file from an extension the user installs; B, the Cookie header or "Copy as cURL" from the developer tools) and what differs in Chrome, Edge, Firefox and Safari; every refusal the parser makes says what was read and what to do; the row says Checking…, Signed in as the name and @handle, Could not reach YouTube Music (kept, tried again at a time it names, CHECK NOW), Session expired or Not signed in (IMPORT AGAIN), and where a session is ended at Google (Google Account → Security → Your devices). Checked with --cookie-test, --ytm-session-test and screenshots of --ytm-demo states only; the browser steps themselves (menu names, shortcuts, the extensions' settings, Safari's Copy as cURL) were written from what the browsers are known to offer and not followed in them here, and a real paste of a multi-line cURL into the box was not tried on screen.
- [ ] **C03** YouTube Music personalised Home when signed in (design step 14) *(P4, M)*
  Done when: signed-in Home differs from signed-out and reports logged_in=1.
  2026-09-29: the account's feed is laid out as YouTube Music lays it out (its shelves in their order and under their own straplines, Quick picks among them, song shelves after the first kept as playable cards, immersive carousels read), under "FOR <NAME> · FROM YOUR YOUTUBE MUSIC", new releases after it; what it links to (a mix made for the account) opens with the account; the check's own FEmusic_home answer is shown on confirmation instead of asking again. Signed out (or the switch off), "Suggested for you" (Rec::homePicks: Made for you, then Because you like) heads Home. --home-account-test covers both.
  Built in engine step SI2, not yet seen with a real account: Home's feed is asked with Auth::IfSignedIn (new releases stay anonymous), YtmSession::sessionChanged and "Use my account for Home" (Settings → Connections, on by default, ytmusic.use_for_home) ask for the feed again when they change whose it is, and the log says "Home's feed, asked as the account, answered logged_in=…". --home-account-test passes against a stand-in; a signed-out run is unchanged. What is left is C02's live test: import a real session, and see Home change and log logged_in=1.
- [ ] **C04** Read-only imports: liked songs as "Liked on YouTube Music", library playlists, private playlists, history (design step 15) *(P4, L)*
  Done when: counts match the account.
  Built 2026-09-29, not yet seen with a real account: `YtmImport` (QML `AccountLibrary`) reads VLLM, FEmusic_liked_playlists and the first page of FEmusic_history with Auth::Required, one page at a time 3-6 s apart (a longer pause every ten), at most 50 pages of likes and 20 of playlists, on its own at most every 12 h (a minute or two after the session is confirmed) and SYNC NOW at most every 15 min, all through AccountGuard; a list replaces what was kept only when read to its end. Stored apart in ytm_tracks / ytm_playlists / ytm_lists (never the user's own likes, playlists, history, play_events or scrobbles); deleted on sign-out, the switch off (Settings → Connections, "Import my YouTube Music library", on by default) or another account; kept and marked as last read while a session is Rejected. Shown as "Liked on YouTube Music" (sidebar, Library, a read-only PlaylistView), the account's playlists in Library (opened with the account through Catalog::setAccountPages, so private ones open; F41 fixed for their continuations), and History → ON YOUTUBE MUSIC. --ytm-library-test passes against a stand-in; `--ytm-demo active+library` shows an invented one. Left for C02's live test: the real answers' shapes and page sizes.
- [ ] **C05** macOS Keychain backend for the secret store (secretstore_mac.mm) — for the macOS session *(P6, M)*
  Done when: the secret-test self-test passes on a Mac.
- [x] **C06** *(Engine step SI1, with no sign-in window: signing in stays the cookie import.)* Signed-in plumbing for the steps that use the account: the session's cookies kept for music, www and s.youtube.com, each call sent what a browser sends its host; X-Goog-AuthUser from a copied request (0 otherwise); the account's own visitor id, and a brand channel's onBehalfOfUser, learned from a signed-in answer and kept with the cookies; CookieImport::toNetscape writing youtube.com's cookies (never google.com's) for yt-dlp *(P4, M)*
  Done when: --cookie-test and --ytm-session-test pass, signed-out requests byte for byte unchanged, and signed-out playback benchmarks unchanged. Not yet seen against the real YouTube Music: whether its answers carry the DATASYNC_ID, and whether a brand channel is then served (C02's live test).
- [ ] **C07** A song YouTube refuses signed out (age check, "confirm you're not a bot", a private video) is skipped even with a YouTube Music session held *(P4, M)*
  Done when: with the owner's spare account imported, such a song plays as "YouTube · signed in", and signed-out playback benchmarks are unchanged.
  Built in engine step SI3, not yet seen with a real account: `StreamResolver::TierSignedIn`, used only when an anonymous rung is refused for a reason an account answers (LOGIN_REQUIRED, AGE_*, CONTENT_CHECK_REQUIRED, yt-dlp's "Sign in to confirm…"), then next; yt-dlp with `--cookies` (a per-lookup file of youtube.com's cookies under the app's local data, read back for rotated cookies, deleted, swept at start) and `player_client=tv_downgraded,web_embedded`; never ahead of time, one at a time, at most 120 songs an hour (`ytmusic.plays_per_hour`); no Cookie to mpv; the info JSON never logged; "cookies are no longer valid" has the session checked; sign-out or the switch forgets every signed-in link. Settings → Connections "Play with my account when needed" (`ytmusic.play_when_needed`, on). --account-play-test passes against a stand-in and this program standing in for yt-dlp. Left for C02's live test: whether tv_downgraded/web_embedded play real age-gated songs with the account, whether googlevideo serves their links to mpv without cookies, and how long a signed-in open takes (the design guessed 3-4 s).
- [ ] **C08** What is played here never reaches the account's YouTube history, so YouTube Music's recommendations for it learn nothing *(P4, S)*
  Done when: after a qualified listen with the owner's spare account, the song shows in its YouTube Music history.
  Built in engine step SI3, not yet seen with a real account: at `PlaybackController::listenQualified` (the Scrobbler's rule, half the song or four minutes, not the design's ~30 s), the account's WEB_REMIX `/player` (Auth::Required: never anonymous, never retried without the account, a 401/403 only has the session checked) names `playbackTracking.videostatsPlaybackUrl`, and a GET of it with ver=2, c=WEB_REMIX and a 16-character cpn reports the listen with that host's own cookies and the SID hashes; only `https://{s,www,music}.youtube.com/api/stats/playback`, no redirects. Settings → Connections "Send my listens to YouTube history" (`ytmusic.report_listens`, on). --account-play-test passes against a stand-in.

## Decisions waiting on the owner

- [x] **Decided: build both.** Connections: should YouTube Music sign-in (a cookie file kept in the Mac Keychain or Windows Credential Manager) and/or Last.fm scrobbling be built now, later, or should the 'Designed, not built' section be hidden until then? (SettingsView.qml:393-431)
- [x] **Decided 2026-09-29: yes, releases are published there** (the Windows installer, by the release workflow), and the app reads that feed by default. Update checks: the repo is public, so GitHub Releases on droidboy08-hub/Monolist can be the update feed (api.github.com/repos/droidboy08-hub/Monolist/releases/latest, appinfo.cpp:214-221). Will builds be published as Releases there?
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
- EU consent page: the visitor ID now comes from www.youtube.com/sw.js_data, with the home page as the fallback, and is stored for up to 30 days (innertube.cpp). A fetch that finds no ID (as a redirect to consent.youtube.com would) keeps the stored one and logs a warning, but a first launch in the EU could still start with none, which would push most tracks off the fast tier. The iPhone app has a YouTubeConsentCookie for this, and nobody tested it here.
- Performance with large libraries and long queues: whether track lists render only visible rows, memory use, and the cost of the artwork cache were not examined.
- Playback features a desktop user might expect were not assessed: gapless playback (designed in Lumen), crossfade, an equalizer and playback speed.
- Drag and drop (songs onto sidebar playlists), selecting several songs at once, desktop notifications on track change, and a system tray or mini-player window were not examined.
- Real macOS behaviour: the app has never been run on a Mac. Unchecked: the software video renderer on Metal, CoreAudio device switching, App Nap slowing timers while the window is hidden, and Gatekeeper and privacy prompts when opening data folders on ~/Desktop.
- Linux (planned later): the frameless window and resize-edge path, Wayland startSystemMove, MPRIS and packaging were only touched in passing.
- Build automation: there is no GitHub Actions workflow for Windows or macOS builds now that the code is on GitHub, and no script builds a native Windows ARM64 version (it runs under x64 emulation).
- Security and privacy: how a downloaded tool update would be verified, safe storage for future sign-in secrets, and the arguments passed to yt-dlp were not reviewed.
- Database robustness: recovery after a crash mid-write, repeated migrations, and backup of monolist.db were not examined.
