# Monolist — Qt 6 / QML music player

The Phono interface design, driven by the Melody playback and extraction
backend, rewritten in C++ against libmpv, yt-dlp and YouTube Music's own API.

The interface grew out of the Phono prototype and keeps its language: paper
and ink, one signal red, 2px rules, square corners, Archivo, photographs in
black and white except where a page is about one. On top of the prototype's
empty screens it now has YouTube Music's home feed, album and playlist pages,
a play queue with autoplay, the user's own playlists and likes, downloads, and
a full-size Now Playing view with synced lyrics, in a window whose title bar is
its own. Underneath, the simulated clock and the mock extractor are gone, and
the source ladder that Melody worked out in TypeScript runs natively.

## What "merging the backend" actually meant

Melody was a React app in an Electron shell. Its backend could not be copied
across, because most of it was not music-player logic at all — it was
workarounds for running in a browser:

| Melody did this | Why | In Monolist |
| :--- | :--- | :--- |
| Scraped `ytInitialData` out of search HTML with a regex | No API key, no Node in the renderer | YouTube Music's InnerTube API, with `yt-dlp ytsearch` as the fallback |
| Tunnelled every request through `api.codetabs.com` | Browser CORS | Deleted. A native app has no origin policy |
| Raced 5 Piped + 13 Invidious instances | Public instances fail constantly | **Kept** — as a fallback tier, not the primary |
| Fell back to a hidden YouTube iframe | A browser cannot play a URL that fails CORS | Deleted. mpv plays any URL that resolves |
| Downloaded bytes → base64 → IndexedDB → Blob → ObjectURL | Browsers cannot write files | yt-dlp writes to disk, FFmpeg tags it |
| `<audio>` element for playback | Only option available | libmpv |

The instance racing was worth carrying over and is the one piece kept close to
the original. "Ask twelve hosts, take the first answer" degrades far better than
any single endpoint, and `StreamResolver` is a direct translation of Melody's
`Promise.any` approach into `QNetworkAccessManager`.

### Search

`MediaExtractor` sends every search to YouTube Music's InnerTube API — the one
music.youtube.com itself calls — through `InnerTube`. It is one HTTPS request:
about 0.3 s on a warm connection, and the results are songs with their artists,
album and square cover art. Suggestions come from the same API while typing.

A yt-dlp search spends most of its ~8 s just starting (it unpacks a Python
runtime every time), so it is only the fallback: when InnerTube fails or comes
back empty, the same query runs through yt-dlp. The API is unofficial and
changes without notice; the parsers are written to degrade to "nothing found"
rather than crash, and the fallback covers the gap.

The chips over the results pick what to search for: songs and videos come back
as rows, albums, artists and playlists as cards that open their pages (a
playlist search shows YouTube Music's own playlists and its listeners' as two
sections). Cards exist only on YouTube Music, so they have no fallback.

### Long lists and shelves

A YouTube Music playlist arrives a hundred songs at a time. The page asks for
the next hundred as the reader scrolls towards the end (`Catalog::loadMorePage`),
and anything that takes the whole playlist — Play, Shuffle, Download all, Add
all — first loads the rest (`loadRestOfPage`, up to YouTube's 5,000). A part
that repeats songs already listed ends the list: some of YouTube Music's own
playlists answer every "more" with their first hundred again.

A shelf's header offers SHOW ALL where YouTube Music has a page of the whole
shelf (a playlist, an artist's albums, the week's new releases, opened as
`shelf:<browse id>[|<params>]`) and PLAY ALL where the shelf is songs. An
album's or a playlist's card plays from a plate on its cover, without opening
the page.

### The source ladder

`PlaybackController::beginTrack` decides where audio comes from:

```
1. downloaded file on disk       →  play it              (Melody: LOCAL)
2. source id present             →  StreamResolver       (Melody: STEALTH)
     cached link, still valid       instant
     tier 0   yt-dlp
     tier 1   Piped instance race
     tier 2   Invidious instance race
3. plain URL stored on the row   →  play it
```

Each resolver tier is tried only when the one before it fails outright, and a
tier fails only when every host in it fails. The tier that won is surfaced to
the UI as `Player.sourceLabel`.

Resolved links are cached until the expiry YouTube signs into them, and the
next song in the queue is resolved in the background while the current one
plays, so replaying or skipping forward rarely waits on yt-dlp.

When a song will not keep playing, the player tries to keep it going from
the same second:

* **A link mpv refuses** (googlevideo's 403, now and then): a stale cached
  link is fetched fresh. A fresh InnerTube link is asked for once more
  (~0.2 s) before the song's muxed stream (itag 18, ~3 s), then yt-dlp and
  the public instances. InnerTube is asked again at most once a track. That
  second link rescues a link spoiled by `--spoil`, but it has not yet
  rescued a real refusal: in 468 fresh benchmark launches since B8,
  googlevideo refused 24 songs' first InnerTube link and refused the fresh
  one every time too; the muxed stream rescued them all, about 3.1-3.6 s
  after the first refusal, as 96-128 kbps AAC, and the fresh link cost
  ~0.3 s on the way (ROADMAP F45). A link found below the song's own rung
  this way is a *rescue link*: it serves the rest of that play. If a single
  refusal was rescued, the song's next play starts from InnerTube again. If
  both InnerTube links were refused, the refusal is the song's, not the
  link's (a replay 15 s later was refused again every time), so for an hour
  the song's next plays start from the rescue link at once (a median 276 ms
  to sound, where both refusals again took 3.3 s), and a further refusal
  goes straight to the muxed stream. `playback.refused=muxed` skips the
  second InnerTube link.
* **A stream that ends early**, more than 5 s (or 3%) before the length mpv
  read from it (a connection that gave out, or a reconnect googlevideo
  refused), is taken as its link failing: a fresh link from the same rung,
  and the song carries on from where it stopped, once a track. Streams only:
  a file's own length can be an estimate. A row's own http link is loaded
  again from where it stopped only when its container carries its length
  (MP4, WebM and Matroska, Ogg, FLAC, WAV): an MP3 with no index, or ADTS
  AAC, has a length guessed from its first frames and is sought by the same
  guess, so a reload would play part of it again.
* **A downloaded or local file mpv will not open** is streamed instead, or,
  with no source id to stream it from, passed over.
* Three songs in a row that will not play stop the queue. The count starts
  again only once a song's sound starts, not when its link arrives.

How long a song may take to resolve is bounded too:

* **20 s for the whole resolve**, JioSaavn's part included; past it the
  player says it could not play the song and moves on. Walked to its end,
  the ladder could take ~49 s before.
* **3 s for /player**, the whole call, the visitor id and the second client
  included. A first request with no answer after 1.2 s (or one that fails
  before then) is sent once more beside it, on a connection of its own: a
  second network manager, so HTTP/2 cannot put it on the connection that
  stalled. Past 3 s the song goes to yt-dlp, which used to wait 8 s or more.
* **One yt-dlp resolve at a time**: the song someone is waiting for goes
  first, and a prefetch's lookup is stopped for it and started again after.
  While such a song resolves no new download starts; running ones carry on.

One rung is used only when a song needs it: the **YouTube Music account**
(`TierSignedIn`, Settings → Connections → "Play with my account when needed",
on by default while a session is confirmed). When an anonymous rung is
refused for a reason an account answers (`LOGIN_REQUIRED`, which is how
"Sign in to confirm you're not a bot" and a private video arrive, an age or a
content check, or yt-dlp's words for the same), the song is asked for once
more, next, through yt-dlp with `--cookies` and
`player_client=tv_downgraded,web_embedded`; the rest of the ladder stays
below it. The cookies go in a `cookies-<random>.txt` file of their own under
the app's local data (`yt-dlp-cookies`, readable by this user alone),
youtube.com's only, written for that one lookup, read back afterwards for the
cookies yt-dlp rotated (taken into the session) and deleted; leftovers are
swept at start. Never ahead of time, one lookup with the account at a time,
at most 120 songs an hour (`ytmusic.plays_per_hour`), and the link goes to
mpv without a Cookie header. yt-dlp's answer is never logged (it can carry
the cookies), and "cookies are no longer valid" has the session checked. It
plays as "YouTube · signed in". Signing out, or turning the switch off,
forgets every link fetched with the account and stops a lookup under way. A
refused signed-in link is followed by the anonymous rungs, not the account
again. Signed out, nothing changes.

With the same session, a listen that counts (the Scrobbler's rule: half the
song or four minutes) is reported to the account's YouTube history, as
YouTube Music's own player reports it ("Send my listens to YouTube history",
on by default): the account's WEB_REMIX `/player` answer names
`playbackTracking.videostatsPlaybackUrl`, and a GET of it with `ver=2`,
`c=WEB_REMIX` and a fresh 16-character `cpn`, carrying what a browser sends
that host and path, reports it (ytmusicapi's `add_history_item` does the
same). Only `https://{s,www,music}.youtube.com/api/stats/playback`, never a
redirect, never without a confirmed session.

Each newer part has a switch back in the settings table (`--set <key>
<value>`): `playback.refused=muxed` puts the muxed stream straight after a
refused InnerTube link, `playback.rescue_link=keep` keeps a rescue link as
the song's link, and `playback.early_end=next` takes an early end as the end;
`playback.resolve_deadline=off` drops the 20 s limit,
`youtube.player_deadline=off` asks /player as before (8 s a request and one
retry, no hedge, no limit on the whole), and `ytdlp.resolves=parallel` runs
yt-dlp resolves side by side with downloads never waiting.
`ytmusic.play_when_needed=0` and `ytmusic.report_listens=0` (the two
Connections switches) keep playback and listens signed out.

### JioSaavn

JioSaavn has much of the same music as AAC at up to 320 kbps. While it is on
(Settings → Playback → Sound: High; off until chosen, and on Standard it is
never contacted), `StreamResolver::resolveTrack` asks it
and YouTube at the same moment: one search for "title lead-artist", whose
rows already carry their links (DES-encrypted, decrypted by `des.*`). A row
is taken only when it is the same recording, and the matcher leans towards
refusing: the same title once asides, credits and release noise are set
aside; the same version (live, remix, acoustic, sped up, a cover, a part, a
style, "(Arijit Singh Version)"… must agree both ways, a remaster may
differ); the same language, whether the title names it, bare or inside
`(From "Film (Telugu)")`, or the album does; any aside words nothing
recognises ("(Synthwave)", an alternate title, a bare year) found somewhere
on the other side; whose remix or which concert agreeing where either side
names one; an artist credited on both sides; and a length within 3 s (a row
with no length is refused when ours is known; with ours unknown, only a lone
candidate is taken). Then, among the rows that fit: one lacking a name of
ours that another listing of the song credits is dropped (the Hindi song,
for its Telugu dub's singer); where our album names theirs, only its rows
are left; if those are in two languages by JioSaavn's own account, all are
refused. Of those
left, the uncensored one, then the one at 320 kbps, then the closest in
length. A link at 96 kbps or less is refused, since YouTube does better, and
so is any link that is not HTTPS on JioSaavn's CDN.

If JioSaavn answers first, or within 1.2 s of when both were asked, its link
plays; otherwise YouTube's does, and JioSaavn's answer is kept for the next
play. Answers are remembered in the `saavn_matches` table: a match for a
week, "not there" for a day, each only for the title, artist, album and
length it was judged against and the matcher version that judged it; Clear
history empties the table. A lookup JioSaavn did not properly answer (an
error sent as HTTP 200, a details call that failed) is not remembered. A
JioSaavn link mpv refuses is forgotten and the song carries on from
YouTube's ladder at the same second, from a link fetched beside it. Requests
carry an Indian `X-Forwarded-For` while "Send Indian region headers" is on,
because some songs are offered only in India, and follow redirects only to
JioSaavn's own hosts. Now Playing's sound badge says what mpv says arrived
("AAC · 321 KBPS": the file's own average, container included), and under the
pointer which source it came from and the rest ("Streaming · JioSaavn · AAC ·
44.1 → 48 kHz · 321 kbps").

A JioSaavn match that comes after YouTube has started the song (it missed the
1.2 s) moves the song over mid-song (QT7; "Switch to JioSaavn mid-song",
`jiosaavn.upgrade`, on). Only when its bitrate is at least 96 kbps above what
plays (by the link's name, and again by the file's own size over its length
once it is open: one "_320" link served 98 kbps) and its length within 2 s
of the stream's as mpv read it, never with the picture on, and once a play.
First `AudioAlign` finds where the music sits in each file (FFmpeg decodes a
few seconds of each from its start; a cross-correlation finds the lag to a
quarter of a millisecond): the copies are not always the same file, and one
had its music 138 ms earlier on JioSaavn. Then `MpvEngine::startUpgrade`
opens JioSaavn's link in a second, silent, paused mpv 5 s ahead of the song
and buffers 12 s past that point; when the song gets there the second player
starts silent, its clock is brought onto the first's plus that lag (its
speed nudged, unheard, until they are within 10 ms), the two crossfade over
80 ms and the second becomes the player. Nothing is heard of an attempt that
fails, and none is made or finished in the last 15 s. A song rescued by its
muxed stream after a refused link is weighed once that sounds. Each move
logs `upgrade: <id> moved from InnerTube to JioSaavn …`, with the timings and
how far apart the two were. JioSaavn's CDN paces nothing (a whole song
arrives in one request at about 35 MB/s from here, 2.2 MB/s when it is not
yet in the CDN's cache), so it is fetched in one request.

With High on, a download of a song JioSaavn has comes from it too
(`saavndownload.*`): the 320 kbps AAC, rewrapped into
`<Artist> - <Title> [<id>].m4a` (or re-encoded for MP3) with the same tags
and square YouTube cover a yt-dlp download gets, through files the failure
cleanup already treats as unfinished; anything that goes wrong, a file that
averages under 256 kbps included, hands the song to yt-dlp. On Standard,
`StreamResolver::findSaavn` answers "none" without asking JioSaavn. Lyrics
do not use it.

### Downloads

`DownloadManager` runs up to three yt-dlp downloads at a time into
`<Music>/Monolist`, with FFmpeg doing the post-processing:

* **Format.** *Original* (the default) keeps the best stream exactly as
  published — usually Opus at 130–160 kb/s, unwrapped from its container but
  never re-encoded. *M4A* takes YouTube's AAC stream; *MP3* re-encodes (V0) for
  players that take nothing else.
* **Tags and cover art.** Title, artist, album and date are written into the
  file, and the thumbnail is cropped square and embedded as the cover.
* **Non-music parts.** With the option on, segments SponsorBlock users have
  marked as non-music (intros, skits) are cut out.

A finished download becomes a library row, so it plays offline straight away.
Queue state, progress and the offline set are in `Downloads.queue` and
`Downloads.library`; every track row asks `Downloads.stateFor(id)`.

### Your library

Kept in the local database and exposed as `Library`:

* **Playlists** hold songs by video id, so any song can go in one, from any
  song's menu (Add to playlist) or an album's (Add all to playlist). A playlist
  page is an album page with a mosaic of its songs' covers; the name is renamed
  in place and deleting is confirmed in place.
* **Likes** are library rows with the favourite flag. Liking a song that is not
  in the library adds it; unliking one that only the like put there takes it
  out again. Liked songs lists them, the latest first.
* **Saved albums and playlists** from YouTube Music, with Save on their pages.
* **History**: every song played, the latest first, clearable.
* **Imported playlists**: Import a playlist (Library) takes a YouTube, YouTube
  Music, Spotify, JioSaavn, Deezer or Apple Music link, a pasted list or a CSV,
  makes the playlist at once and fills it as each song is found on YouTube
  Music, signed out and a few seconds apart. Songs it was unsure of, could not
  find, or never looked for wait on the playlist's page.
* **Backup and sync** (Settings > Backup and sync, QML `Backup`): playlists,
  likes, saved albums and playlists, plays and turn-downs as one file,
  `monolist-library.json`. EXPORT… saves it anywhere and IMPORT… merges one
  back, with no sign-in, in any build. SIGN IN WITH GOOGLE keeps the same file
  in the user's own Google Drive, in the hidden folder only Monolist can open
  (scopes `drive.appdata`, `openid` and `email`, nothing else), and merges it
  on every computer that signs in: right after signing in, 15 s after a
  launch, a minute after an edit, when the window comes to the front a few
  minutes after the last sync, every half hour (which is when plays go up),
  and on SYNC NOW. A merge only adds, except what was deleted on another
  computer later than it was last added or changed here (each deletion is
  kept, with its time, in `sync_removed`), so bringing back an old file never
  takes away what was added since. A playlist's songs merge as a set (a song
  taken out of one is a deletion of its own), so songs added to the same
  playlist on two computers are both kept; its name, pin and order are the
  newer copy's. Nothing dated later than now is read or written (a clock
  that ran ahead would win every comparison), and a computer whose clock is
  more than five minutes from Google's does not sync until it is set right.
  The file names no computer. The
  sign-in is Google's page in the user's own browser, answered on 127.0.0.1
  (PKCE), and has nothing to do with the YouTube Music sign-in: its own
  client, network manager and secret (`backup.google`), no cookies, never
  AccountGuard. No server of Monolist's is involved; signing out leaves the
  file in Drive (Drive's Settings → Manage apps → Monolist → Delete hidden app
  data removes it).

The sidebar shows Liked songs and the playlists, and the name the operating
system knows the user by (the account's full name, else the login name),
which the pencil there changes.

### Lyrics

`Lyrics` looks up the song playing from two providers, asked at once and
answered in this order (`lyrics/lyricsrace.*`): the first timed lines in the
order show the moment they arrive, and the other provider is called off; a
provider lower down never pre-empts one above it, except that after 1.2 s
(`lyrics.patience`) the best answer in hand shows while the one above is still
out, to be replaced if its timed lines still come. Each provider has 6 s on
the wall clock. `lyrics.race=serial` goes back to one after the other.

1. **LRCLIB** (lrclib.net), an open database of time-synced lyrics. First its
   exact lookup (`/api/get`: title, lead artist, album, length); on a 404, or
   when that has not answered 250 ms after it was sent, its search (the exact
   answer still wins if it comes within a second of being sent), the entry
   closest in length winning among those that are this song
   (`LyricsQuery::match`: "Anti-Hero" is not "Hero"). Only an entry within 3 s
   of the recording's length is trusted for timing; one within 10 s is kept as
   plain text in case nothing better turns up. LRCLIB gets 6 s in all.
2. **YouTube Music's own lyrics**, plain text from its partners (Musixmatch,
   LyricFind), credited as such.

The title is asked for as databases file it and the artists as the song's
credits name them (`lyricsquery.*`): noise such as "(Official Video)" goes,
version markers stay ("(Official Live Video)" is asked as "(Live)"), and an
artist line is never cut at "&" or "," on a guess.

The song playing is looked up once its sound starts, and the song after it
once that is done (not on a metered connection), so opening the lyrics reads
them from the database; Settings' "Look up lyrics in the background" turns
this off. One lookup per song at a time, whoever asked for it, and never for
a song whose length is not known yet.

What was found, or that nothing was, is stored per video id, and each
provider's own answer beside it, so a provider that has answered is not
asked again while that is fresh. An answer shown
only because a better source failed (LRCLIB out of reach, so YouTube Music's
plain text) is stored as provisional: shown at once the next time, and asked
for again behind it until that source answers. "None" is stored only when
every source answered that it has none, and is asked again after three days;
found lyrics are checked again after 60 days. Synced lines follow the playing
position (a line lights 150 ms early, as it starts), and clicking one plays
from there. LRCLIB can be self-hosted; the `lrclib_url` setting points
elsewhere, and `lyrics.lrclib=search` goes back to its search alone, taken by
length, as before.

The Now Playing view (the owner's "Ambient stage") lays one calm field of the
cover's own colour (`CoverPalette`, measured from the cached artwork) over the
whole sheet, deep in dark and pale in light, and prints everything on it in
tints of the same hue, each found by its contrast against the field, so they
read on any cover's colour.

### Sound effects

`Sound` (`soundeffects.*`) holds six effects, each kept from one launch to
the next (the `sound.*` settings):

* **Slowed + reverb**: every song at 0.80, 0.85 or 0.90 of its speed, the
  pitch going down with it like a record played slow, in a hall (reverb off,
  light, medium or heavy).
* **Nightcore**: 1.20, 1.25 or 1.30 times, the pitch going up. Slowed and
  Nightcore both set the speed, so turning one on turns the other off.
* **8D audio**: the song carried round the head once every ten seconds, in a
  light room of its own; best with headphones.
* **High bass**: a shelf under 100 Hz, +3, +6, +9 or +12 dB.
* **Studio clarity**: the curve of LastWave's "Studio Master Clarity", which
  the owner asked for: ten peaking bands on the octave centres, one octave
  wide, at +2.7, +2.2, +0.6, -2.6, -0.6, +1.2, +3, +4, +4.3 and +4.8 dB from
  31 Hz to 16 kHz. A stage of its own after the equaliser, not added into its
  bands, so each keeps what it is set to (the equaliser's bands are narrower
  at the top, and the curve on them lost 2.5 dB of its air). Its bands are an
  octave wide at the song's own rate: the chain is written for 48 kHz, and at
  44.1 kHz the engine sends seven of them their widths once the graph runs.
  Measured inside the shipped libmpv, the result is the octave bands' curve
  within 0.02 dB. Only the numbers come from LastWave, never its code.
* **Equaliser**: ten bands from 31 Hz to 16 kHz, up to 12 dB up or down each
  in 0.5 dB steps, with 16 curves to start from.

The speed is mpv's own `speed` with its pitch correction off, so times stay the
song's (a 3:00 song still shows 3:00, and takes about 3:32 slowed), the seek
bar and the lyrics follow it as written, and Windows' media controls are told
the rate. Everything else is one lavfi chain in mpv's `af`, built only from
fixed templates (`soundchain.*`); a change is ramped into the running chain
with `af-command` rather than rebuilding it, so nothing clicks. The sound is
turned down as far as the boosts need, and a limiter at -1 dBFS catches what
is left. If mpv will not take the chain, the song plays without it and the
effects stay off until Monolist restarts, which a toast says.

They are on the player bar: the sliders glyph beside the output button is red
while any effect is on, and opens them all in a popup standing on the bar.
Settings > Playback has the same block, with a line of explanation under the
effects that need one (`--settings-section effects`), and a tag beside Now
Playing's sound badge names the effects that are on. The mini player has them as switches over
its cover.

### Updating Monolist

Monolist updates itself from Settings › Updates (`AppUpdater`, behind the
About singleton); the release page on GitHub stays as the last resort.

* **CHECK** asks the release feed (GitHub's latest release, or the setting
  `update.feed`). A Mac counts a release only once it has a disk image for
  that Mac. A newer release shows **UPDATE**, or **GET IT** (its page) for a
  copy that cannot update itself (a development build, or Linux).
* **UPDATE** takes, from the release's own list of files, the one for this
  copy and the `.sha256` published beside it: on a Mac the disk image of the
  same kind as the running app (the Developer ID one for a Developer ID copy,
  `-unsigned` for an ad hoc one); on Windows the setup for a copy the setup
  installed (its uninstall key names this folder; the key is only read,
  never made), the zip for a portable one. Both come over HTTPS from the
  project's release files on `github.com`
  (`/droidboy08-hub/Monolist/releases/download/`), redirected only to
  GitHub's two download hosts (`objects.githubusercontent.com`,
  `release-assets.githubusercontent.com`), every hop checked, with no
  cookie. Plain HTTP is allowed only on this computer, only when the feed is
  there too, and only for a scratch library (the tests). The file is
  streamed to `<name>.part`, hashed as it arrives, stopped if it grows past
  the size the feed gave, and renamed only when it matches the published
  SHA-256; a mismatch, a cut, CANCEL or quitting deletes the part. A file
  already downloaded and still matching is used again. Links are logged
  without their query (GitHub's signed tokens).
* It is then got ready. **Mac**: the image attached hidden and read-only,
  Monolist.app copied out with `ditto` beside the update on the app's own
  volume, the image let go of, and the copy checked before anything is
  touched: `codesign --verify --deep --strict`, the same bundle identifier, a
  newer `CFBundleVersion`, and for a Developer ID copy the Developer ID
  requirement for team 7YWFR8A74F. A download that fails these is deleted. A
  copy that cannot be replaced in place (opened from where it was downloaded,
  from a disk image, from a folder the user may not change, from its build
  folder) is put through the same checks; only then is its disk image marked
  as downloaded (quarantined, so Gatekeeper checks what is dragged out of
  it) and opened in the Finder, with the words to drag it to Applications.
  **Windows**: the setup needs nothing; the zip is unpacked beside the
  portable copy. A portable copy whose folder holds the library or the
  downloads is never swapped: the zip is shown, to be copied in by hand. The
  Windows builds are not code-signed: HTTPS from GitHub and the SHA-256 are
  what vouch for them there.
* **RESTART** is a press of its own (it stops the music; a toast offers it
  when the download ends). A second press starts nothing more. It starts a
  helper, every path an argument, and quits. The helper waits for Monolist
  to end, then:
  * **Mac**: the helper is Monolist's own executable (`--update-swap`),
    signed as the app is, by the same developer as the app it replaces,
    which macOS's App Management asks of whatever changes a notarised app.
    It checks the new app's signature once more, moves the old app aside
    and the new one into its place, registers it with LaunchServices, opens
    it, waits for it to start (the word `started` it leaves once its window
    is up, or it still running after a minute), and only then deletes the
    old one. If any step fails, or the new one ends before it has started,
    the old one is put back and opened.
  * **Windows**: for an installed copy, it hashes the setup again, holds it
    open so that nothing can change it, and runs it quietly with this
    folder and `/RELAUNCH` (packaging/monolist.iss starts Monolist again when
    it is done); a copy installed for everyone makes Windows ask first. For
    a portable copy it swaps the folder for the new one, starts it, and
    puts the old one back if it ends with an error at once. The old folder
    is deleted only when every file in it is also in the new one; otherwise
    it is kept beside it (`Monolist.old-<n>`) and the next launch says
    where. The helper runs in the updates folder, never in Monolist's own.
  
  It leaves a word in `result.txt`; the next launch waits for it, says how
  it went ("Monolist is now 0.1.191", or why the old copy is still the one
  running, with TRY AGAIN and the release page), keeps that sentence in
  `last.txt`, and clears what is left (never while an update is under way):
  half downloads, staged copies, a disk image a crash left attached, a zip a
  crash left unpacked.
* **TRY AGAIN** looks for the release again before downloading, as a newer
  one may have come out.

The updates folder is `updates` in the cache (`~/Library/Caches/Monolist/
Monolist`, `%LOCALAPPDATA%\Monolist\Monolist\cache`), or in
`MONOLIST_DATA_DIR`. Nothing of the user's lives in the program's folder or
the bundle unless they put it there, which the update checks for.

## Layout

    CMakeLists.txt             build; finds Qt 6 and libmpv
    cmake/FindMpv.cmake        libmpv locator (pkg-config, or -DMPV_ROOT)
    scripts/setup-windows.ps1  installs Qt, MinGW, CMake, Ninja, libmpv, yt-dlp,
                               FFmpeg, Deno and Git into C:\dev\monolist-deps
    scripts/build-windows.ps1  configures, builds and deploys a runnable build
    scripts/setup-macos.sh     installs Qt, libmpv, CMake, Ninja, FFmpeg, yt-dlp
                               and Deno through Homebrew
    scripts/build-macos.sh     builds Monolist.app with yt-dlp, Deno and FFmpeg
                               inside, or an Xcode project (--xcode)
    macos/                     Info.plist template, the app icon and its generator

    Main.qml                 window, view switching, breakpoints, shortcuts
    Theme.qml                design tokens
    Nav.qml                  where a link in any list asks to go (an artist, a page)
    Icons.js                 Lucide glyph outlines as path data
    components/              UI components: the prototype's, and the menus,
                             cards, queue panel, lyrics pane, title bar parts,
                             ArtistLine (an artist line whose names are links),
                             EffectsPanel and BandSlider (the sound effects),
                             MiniPlayer (the mini player's own window),
                             NowPlayingBar and NowPlayingBarClassic (the
                             player bar, Modern, Classic or Classic modern,
                             which is Modern `framed`: Settings > Appearance >
                             Player bar, appearance.player_bar, switched at
                             once; Main builds only the one chosen)
    views/                   Home, Search, Library, Downloads, Page (album or
                             YouTube Music playlist), Artist, Shelf (a shelf's
                             "show all"), Playlist, Now Playing

    src/
      main.cpp               wiring; registers the QML singletons; self-tests
      appdatabase.*          SQLite schema, migrations
      library.*              the user's playlists, likes, saved albums, history
      libraryeditselftest.*  --library-edit-test
      librarybackup.*        the library as one file, and its merge: Export,
                             Import and the Google Drive backup
      drivesync.*            the backup as QML sees it (Backup): Export, Import,
                             and the Google Drive sync (find, read, merge,
                             upload; when, backoff, a file held unread)
      driveauth.*            the Drive backup's own Google sign-in: loopback
                             PKCE in the browser, tokens, revoke
      backupselftest.*       --backup-test
      driveselftest.*        --drive-test
      appupdater.*           the in-app update (Settings › Updates): the file
                             for this copy, from GitHub only, checked against
                             its SHA-256, got ready, and the helper; Windows'
                             part in appupdater_win.cpp, the Mac's in
                             macos/appupdater_mac.*
      appupdaterselftest.*   --update-test, --update-run
      playlistmodel.*  albummodel.*  trackmodel.*

      playbackcontroller.*   the facade QML binds to — driven by mpv
      queuemodel.*           the play queue
      mpvengine.*            libmpv wrapper, audio-only
      soundchain.*           the sound effects' lavfi chain, its ramps and the
                             equaliser's curves (no mpv)
      soundeffects.*         the effects as kept and shown to QML (Sound)
      soundselftest.*        --sound-test and --sound-live-test
      appicon.*              the app icon chosen in Settings: the window's,
                             and the app's own shortcuts' (AppIcon)
      appiconselftest.*      --icon-test
      searchselftest.*       --search-test
      startup.*              opening at sign-in, off / open / minimised / mini player (Startup)
      startupselftest.*      --startup-test
      tray.*                 the window closed to the system tray, the music on (Tray)
      trayselftest.*         --tray-test
      sleeptimer.*           the sleep timer: minutes or the end of the song, a fade, a pause
      sleepselftest.*        --sleep-test
      playlistimport.*       importing a playlist: the jobs, their pacing and review (Importer)
      importsource.*         reading a link, a pasted list or a CSV into songs
      importmatcher.*        whether a search result is the song on the list
      importselftest.*       --import-test
      mediaextractor.*       search: InnerTube first, yt-dlp as fallback
      innertube.*            YouTube Music's API: search, suggestions, radio,
                             browse pages, artist pages, lyrics
      catalog.*              Home's feed, album, playlist, artist and "show all"
                             pages, and a card's play button
      artistlinks.*          which page an artist's name opens, learnt from every
                             answer that links one and kept in the database
      artistselftest.*       --artist-links-test
      lyrics.*               LRCLIB and YouTube Music lyrics, synced to playback
      lyricsquery.*          what lyrics are asked for, and which answer is the song
      lyrics/                the providers (providers/*), their race and what is kept
      lyricsselftest.*       --lyrics-query-test, --lyrics-flow-test, --lyrics-race-test,
                             --lyrics-prefetch-test and --lyrics-pane
      ytdlp.*                QProcess wrapper around yt-dlp; finds FFmpeg and Deno
      streamresolver.*       the tiered source ladder and its link cache, and the
                             race against JioSaavn
      jiosaavn.*  des.*      JioSaavn's search, links and the same-recording matcher;
                             DES (FIPS 46-3) for its encrypted links
      saavnselftest.*        --saavn-test and --saavn
      downloadmanager.*      offline library: queue, options, files, DB rows
      saavndownload.*        one download from JioSaavn's copy, tagged as yt-dlp's
      audioalign.*           how far apart in time two copies of a song are (QT7)
      downloadmodels.*       the queue and offline-set models for QML
      downloadselftest.*     --download-cleanup-test and --saavn-download-test
      artworkcache.*         async disk-cached image provider + cover colours
      windowchrome.*         the window without the system title bar
      macos/                 the Mac's own parts: the title bar (macwindow.*), Now
                             Playing for the media keys (mediasession.*), and
                             yt-dlp and Deno kept current (toolstore.*)
      recdata.*              downloads the recommendation data, checked against
                             its manifest, into the data folder

### QML singletons

`Library`, `Player`, `Sound`, `Extractor`, `Downloads`, `Catalog`, `Artists`,
`Lyrics`, `CoverPalette` and `Chrome`. The image provider registers as
`image://artwork/<url>`. In QML, `Nav` carries a link's request to the window,
and `Menus` a right click's or a "more" button's: the window keeps one
`TrackMenu`, `CardMenu` and `PlaylistMenu` and opens it with what was clicked.

### Menus and order

A song has the same menu everywhere it is shown — track tables, the queue,
suggestion rows, Downloads, the player bar, song cards — from its dots or a
right click: play next, queue, like, add to a playlist, remove it from this
playlist, from History or from the library (every playlist and the like, on a
second click), go to its artists and album, copy its link or open it on
YouTube, and its download. On the player bar, its "…" heads that menu with
what the bar keeps behind it: the mini player, the sleep timer, the video, and
in a narrow window the lyrics and the output. Albums, playlists and artists on cards have Play,
Open, Save or Remove from library and their link; the user's playlists (on
their page, their card and in the sidebar) have Play, Shuffle, Add all to
queue or to another playlist, Rename and Delete. A suggestion is only a name
until it is looked up, so its menu searches for the song first, as pressing it
does. Songs in a playlist and what is still to come in the queue can be
reordered: dragged by the grip that takes the row's number (or margin) under
the pointer, with Alt+Up and Alt+Down once the grip has been pressed, or with
Move up and Move down in the row's menu.

### Artist links

Every artist's name in the interface is a link: in track tables, the player
bar, Now Playing, album pages, the queue, Downloads and the suggestion rows.
A song fresh from YouTube Music carries its credits piece by piece with each
name's page (`credits` in the song models), so a joint credit links each name
to its own artist. A song kept by name only (Liked songs, playlists, History,
downloads, the catalogue's suggestions) is matched by `Artists` against every
name an answer has linked before, kept in the `artist_links` table; a line it
cannot split wholly into known names stays one link, and a name nobody has
linked is looked up among YouTube Music's artists, falling back to Search when
there is no artist of that name. The view names are `artist:<channel id>` and
`artistname:<name>` (a lookup, replaced by the page once found).

## Build

Requires Qt 6.6+, CMake 3.21+, a C++17 compiler and libmpv. yt-dlp, FFmpeg and
Deno are runtime dependencies, not build ones.

### Windows

```
scripts\setup-windows.ps1
scripts\build-windows.ps1 -Run
```

`setup-windows.ps1` installs everything under `C:\dev\monolist-deps` —
nothing system-wide, no installers, PATH untouched — and checks every download
against the checksum its project publishes. Qt and its tools come straight from
Qt's online repository (no account needed). Re-running skips what is already
there; `-Update` refreshes yt-dlp, FFmpeg and Deno, which should follow their
latest releases because YouTube keeps changing.

The kit is Qt 6.11 with MinGW 13.1, 64-bit x86. On ARM64 Windows (including
Parallels on Apple Silicon) it runs under Windows' x64 emulation, since Qt only
ships native ARM64 builds for MSVC; the runtime tools run as separate processes
and use native ARM64 builds.

`build-windows.ps1` builds out of the source tree into `C:\dev\monolist-build`
(a network share is slow, and cmd.exe cannot run Qt's generators from a UNC
path), runs `windeployqt` (and adds the `QtQuick.Controls` module it leaves
out, which the tooltips need), copies libmpv, and links the runtime tools into
`tools\` beside the executable, where the app looks first. `-Config Release`,
`-NoMpv` and `-Run` do what they say.

To configure by hand instead:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=C:\dev\monolist-deps\Qt\6.11.2\mingw_64 ^
      -DMPV_ROOT=C:\dev\monolist-deps\libmpv
cmake --build build
```

### Building without libmpv

`-DMONOLIST_NO_MPV=ON` compiles `mpvengine_stub.cpp` in place of
`mpvengine.cpp`. The header is identical either way, so switching back is a
configure flag, not a code change. Everything works except audio output; the
player bar reports "Built without libmpv" rather than appearing to play silence.

### Arch Linux

```
sudo pacman -S qt6-base qt6-declarative qt6-svg qt6-imageformats mpv yt-dlp ffmpeg deno cmake ninja
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/monolist
```

### macOS

Needs Homebrew (https://brew.sh) and Apple's Command Line Tools; the full Xcode
from the App Store only for an Xcode project. Apple silicon and Intel alike.

```
scripts/setup-macos.sh
scripts/build-macos.sh --install
```

`setup-macos.sh` installs Qt, libmpv, CMake, Ninja, pkgconf and FFmpeg to build
with, all through Homebrew, skipping what is already there; it only has to run
on the Mac that builds the app. It also installs yt-dlp and Deno, for the
builds that do not carry their own (Xcode's); `--update` updates those.

`build-macos.sh` makes a Release build in `build-macos/`, copies it to
`build-macos/dist/Monolist.app`, puts Qt, its QML modules, libmpv and FFmpeg
inside it with `macdeployqt` (so a `brew upgrade` cannot break it), adds
yt-dlp and Deno (below), signs it ad hoc and checks that nothing is still
loaded from Homebrew. The result needs nothing installed on the Mac it runs
on. `--install` then copies it into `/Applications` (`~/Applications` when
that is not writable), `--dmg` makes a disk image beside it, `--run` opens it,
`--debug` builds Debug, `--no-deploy` skips `macdeployqt` for a quicker build
that uses Homebrew's Qt, and `--no-tools` leaves yt-dlp and Deno out, for a
build with no network.

#### yt-dlp, Deno and FFmpeg inside the app

yt-dlp has to follow YouTube, which changes often, and a signed app may not
change: macOS calls one that did damaged. So the app carries yt-dlp and Deno
as the archives their projects publish, downloaded by `build-macos.sh` from
their GitHub releases and checked against the SHA-256 each publishes (a copy
that does not match is not bundled), in `Contents/Resources/tools` with their
versions in `tools.json`. On its first launch the app unpacks them into

    ~/Library/Application Support/Monolist/Monolist/tools

and runs them from there (`ToolStore`, in `src/macos/toolstore.*`). Updates go
to the same place, so they outlive the app that fetched them, and a newer app
replaces older unpacked copies.

* **Once a day** the app asks GitHub for the latest release of each. A newer
  yt-dlp is installed without asking, since an old one simply stops playing
  tracks; a newer Deno is announced, and Settings › Update components
  installs it. Each download is checked against its published SHA-256, and a
  failed update leaves the working copy in place.
* **FFmpeg** is Homebrew's, in `Contents/MacOS` beside the executable, where
  it shares its libraries with libmpv. It is updated with the app: YouTube's
  changes do not reach it.

Downloads are kept in `build-macos/tools`; with no network, the build uses the
newest one there.

#### In Xcode

```
scripts/build-macos.sh --xcode
```

generates `build-xcode/Monolist.xcodeproj` with CMake's Xcode generator and
opens it. Pick the **monolist** scheme and **My Mac**, then Product › Run (⌘R);
the log appears in Xcode's console. The project is generated, so change
`CMakeLists.txt` rather than the project, and run the command again after
adding a file. A build run from Xcode loads Qt and libmpv from Homebrew; for
the copy to keep, use `build-macos.sh --install`.

By hand, either way:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$(brew --prefix)"
cmake --build build          # build/Monolist.app
```

#### What is different on a Mac

* **The window.** The design's 64 px top strip is the title bar, under a
  transparent one, as Windows keeps its snap layouts: full screen, tiling and
  Mission Control work as for any other window. The traffic lights are moved
  into the strip, centred in it at the sidebar's edge, with the name after
  them (the version beside the name gives way; Settings' About has it), and
  put back whenever AppKit lays them out again. A click on the name folds
  the sidebar to its rail, as on Windows: the lights then stand across the
  rail's top, with the "M." that opens it again under them. A double click on the strip
  does what System Settings › Desktop & Dock says. This needs Qt 6.9 or
  later; with an older Qt the system title bar simply stays. (Qt 6.9's
  ApplicationWindow pads its content by the safe area, which under the
  transparent title bar is the title bar: WindowChrome takes that back.)
* **The menu bar.** Monolist (About, Settings… ⌘,, Hide, Quit), Controls
  (Play or Pause, Next ⌘→, Previous ⌘←, Mini Player ⇧⌘M) and Window
  (Minimize ⌘M, Zoom, Bring All to Front, the windows), in
  `src/macos/menubar.*`. Space stays the window's, so typing in the search
  field is never taken for it.
* **Closing the window** leaves the app and the music running; the Dock icon
  brings the window back (not over the mini player, which stands in for it),
  and ⌘Q quits. ⌘W and ⌃⌘F do what they do in any Mac app.
* **Sign-ins** (Last.fm, YouTube Music, the Drive backup) are kept in the
  login Keychain, one item per secret; a run with `MONOLIST_DATA_DIR` keeps
  its own, apart from the library in use. YouTube Music's sign-in window is
  WebKit's, as Windows' is WebView2, under the same rules: the engine as it
  comes, its data in memory only, Google's and YouTube's pages only, and
  Google's refusal taken as final, with the import offered instead.
* **Opening at login** is a LaunchAgent of the user's own
  (`~/Library/LaunchAgents/io.github.droidboy08-hub.monolist.login.plist`),
  as Windows' is a Run value, with the same choices; System Settings' Login
  Items & Extensions can turn it off, and Settings says so.
* **Media keys and Now Playing.** The play, next and previous keys, AirPods,
  the Touch Bar, Control Center and the lock screen control Monolist and show
  the song, its cover and its position. Without this, the play key opens
  Apple Music.
* **Opus and Ogg files from the Finder.** Monolist claims `.opus`, `.ogg`
  and `.oga` (the system's `org.xiph.ogg-audio`, in `macos/Info.plist.in`)
  at Default rank, so a double-click can open them in it; where another app
  already has them, Get Info › Open with › Change All makes Monolist the
  one. Open With and a file dropped on the Dock icon work either way. The
  file plays at once, whether Monolist was open or not (the running app gets
  it, never a second copy), several in the order selected. One of its own
  downloads (named `… [<video id>].opus`, and the very file Downloads keeps)
  plays as its song, and autoplay carries on from it; any other plays as a
  file of its own, with the title, artist, album and cover its tags give
  (ffprobe; the cover is copied out by FFmpeg into `file-covers` in the data
  folder) and no radio after it. M4A and MP3 stay with the apps that have
  them. On Windows the same files can be given on the command line (Open
  With › Choose another app › `monolist.exe`), and a second start hands them
  to the one running.
* **Tools from Finder.** An app opened from Finder or the Dock does not get
  the shell's PATH, so Homebrew's folders (`/opt/homebrew/bin`,
  `/usr/local/bin`) and `~/.deno/bin` are added to it at startup: builds that
  do not carry their own tools (Xcode's) still find Homebrew's.
* **Appearance.** Light, Dark or Match macOS, as Windows has Match Windows.
* **Opus in the Finder.** Downloads in the original format are Opus in Ogg
  (.opus), whose tags and cover macOS cannot read: Spotlight imports Ogg
  "with no plugIn", so Get Info shows no title, artist or album, and the
  Finder draws a blank note. The app fills both in:
  * its tags come from `MonolistOgg.mdimporter`, a Spotlight importer inside
    the app (`Contents/Library/Spotlight`, `src/macos/oggimporter.c`): the
    title, artists, album, length, genre, track, year, codec, rate and
    channels, the attributes Apple's own importer gives an M4A. Get Info's
    More Info and Spotlight searches show them. Spotlight is told of the
    importer once per build (`mdimport -r`), which has it read every Ogg
    file on the Mac again, so songs saved before show their tags too;
  * its cover becomes the file's own Finder icon as the download is saved
    (`src/macos/finder.mm`): 256 px in the file's resource fork, about
    200 KB beside the song, the audio untouched. Opus downloads saved
    before get theirs at the first launch of a build that does this.
    An app extension could not do it: Quick Look gives every audio file to
    macOS's own thumbnailer, which reads no Ogg cover (on macOS 26 a
    Quick Look thumbnail extension for Ogg is never asked, and a Core
    Spotlight import extension is never called either).

  Quick Look itself (the space bar, the Gallery view's preview) still shows
  no cover or title for Opus; M4A shows both everywhere. Both are read by
  one small parser of Ogg pages, OpusTags and FLAC picture blocks
  (`src/macos/oggtags.c`, no dependencies), checked by `--ogg-test`.

The app is signed ad hoc ("Sign to Run Locally"), which is all a Mac needs to
run what was built on it, and is what `build-macos.sh --dmg` makes: the
unsigned disk image. Another Mac opens a downloaded copy of it only after its
user chooses Open Anyway in System Settings › Privacy & Security, once. It
needs no Apple account, and never stops working.

#### Signed and notarised, for other Macs

Signed with a Developer ID and notarised by Apple, the app opens on any Mac
like any other. Nothing about it expires: the signature carries Apple's
timestamp, so it stays valid after the certificate (five years) runs out, and
copies already signed keep opening if the Apple Developer Program is not
renewed; only new versions need it. Once, on the Mac that builds:

1. Join the Apple Developer Program (developer.apple.com/programs, 99 USD a
   year), as the account holder.
2. Xcode › Settings › Accounts › your team › Manage Certificates › + ›
   **Developer ID Application** (not Apple Development, which signs for this
   Mac alone; not a cloud-managed one, which cannot be exported for CI).
3. The notary service's login, kept in the keychain under a name of your
   choosing: `xcrun notarytool store-credentials monolist-notary`, with your
   Apple ID, team ID and an app-specific password made at account.apple.com.

Then:

```
scripts/build-macos.sh --sign "Developer ID Application: Your Name (TEAMID)" --notarize monolist-notary
```

makes both disk images in `build-macos/dist`: `Monolist-unsigned.dmg` (the ad
hoc build, as without `--sign`) and `Monolist.dmg` (signed, notarised, its
ticket stapled, and the app inside stapled too, so it opens offline). Notary
logs are kept in `build-macos/notary-*.json`. Without `--notarize`, the signed
image is made but not notarised.

What signing does (`build-macos.sh`, "Developer ID"): every piece of code
signed again from the inside out with the hardened runtime and a timestamp:
Qt's libraries, plugins and frameworks, FFmpeg, the Spotlight importer, and
yt-dlp inside its archive (its Python.framework's links, packed as copies by
its project, put back first); Deno is signed and notarised by its makers and
left alone. The app asks for no entitlements, and neither does the importer
(the script checks both): Spotlight loads it into its own worker. It needs mpv's Lua scripts off (MpvEngine does so
on a Mac): LuaJIT's run-time code is killed under the hardened runtime.

The first launch of a Developer ID build on a Mac that ran an ad hoc one asks
once for each sign-in kept in the Keychain (Always Allow): the Keychain ties
each item to the signature that made it.

GitHub's release workflow (`.github/workflows/release-macos.yml`) signs and
notarises too when the repository has these secrets, and then publishes both
images; without them it publishes the unsigned one, as before:

| Secret | What |
| :--- | :--- |
| `MONOLIST_DEVELOPER_ID_P12` | The Developer ID Application certificate with its key, exported from Keychain Access as .p12, base64 (`base64 -i cert.p12 \| gh secret set MONOLIST_DEVELOPER_ID_P12`) |
| `MONOLIST_DEVELOPER_ID_P12_PASSWORD` | That export's password |
| `MONOLIST_NOTARY_KEY_P8` | An App Store Connect **team** API key (Users and Access › Integrations), the .p8 file, base64 |
| `MONOLIST_NOTARY_KEY_ID` | Its key ID |
| `MONOLIST_NOTARY_ISSUER` | Its issuer ID |

The icon is `macos/Monolist.icns`: the owner's note
(`packaging/icons/note.png`) on Apple's grid, drawn by `macos/make-icon.py`
(Pillow), which also draws each choice for the Dock (`icons/app/mac-*.png`)
and the first icon, the "M.", kept on the Mac as a third choice beside the
note and the red note. Settings' choice sets the Dock's icon while Monolist
runs; closed, the Dock shows the bundle's.

### Last.fm key

Scrobbling needs a Last.fm API account, which belongs to whoever builds the app
and is never committed. Set `MONOLIST_LASTFM_API_KEY` and
`MONOLIST_LASTFM_SHARED_SECRET` in the environment before building (or pass
both with `-D`), and they are written to `generated/apicredentials.h` in the
build tree, read again at every build. Without them the app builds all the same,
and Settings says the build has no Last.fm key. CMake reports only whether there
is a key, never its value; `monolist --lastfm-test` and `--diag` say the same.

### Google sign-in client

The Google Drive backup signs in with an OAuth client of type "Desktop app" from
the builder's own Google Cloud project (with the Drive API enabled and the
`drive.appdata` scope on its consent screen). Set the consent screen's
publishing status to **In production** (Google Auth Platform → Audience →
Publish app): `openid`, `email` and `drive.appdata` are all non-sensitive
scopes, so publishing needs no verification review. Left in **Testing**, only
the people listed as test users can sign in at all (anyone else is told
"Access blocked"), they see Google's "hasn't verified this app" warning, and
their sign-in ends after 7 days, because `drive.appdata` is not one of the
basic sign-in scopes: the row then says the sign-in has ended, every week.
Like the Last.fm key the client is never
committed: set `MONOLIST_GOOGLE_CLIENT_ID` (`<number>-<letters>.apps.googleusercontent.com`)
and `MONOLIST_GOOGLE_CLIENT_SECRET` before building, both or neither, and they
go into the same generated header, seen only by `driveauth.cpp`. Without them
Settings says this copy was built without Google sign-in and offers the backup
file only. `build-windows.ps1` and `package-windows.ps1` take all four values
from the Windows user's environment when the shell has none of its own, so
`[Environment]::SetEnvironmentVariable('MONOLIST_GOOGLE_CLIENT_ID', '…', 'User')`
once is enough; nothing prints them, and `--diag` says only whether there is a
client. On a Mac, `build-macos.sh` takes any of the four the environment lacks
from the login Keychain, kept there once with
`security add-generic-password -U -a "$USER" -s MONOLIST_GOOGLE_CLIENT_ID -w`
(and likewise `MONOLIST_GOOGLE_CLIENT_SECRET`, `MONOLIST_LASTFM_API_KEY`,
`MONOLIST_LASTFM_SHARED_SECRET`), which asks for the value rather than taking
it on the command line. GitHub's release workflows read repository secrets of
the same four names (`gh secret set MONOLIST_GOOGLE_CLIENT_ID`, which also
asks for the value).

### Typeface

**Archivo (400/600/800) is bundled** in `fonts/` and registered from Qt
resources at startup, so nothing needs installing on any platform. Google ships
Archivo as a variable font only; the three static instances here were cut from
it with `fonttools varLib.instancer` and renamed so a single family "Archivo"
carries all three weights. Archivo is OFL-1.1 and `fonts/OFL.txt` travels with
it, as the licence requires.

This matters more than it sounds: the design's letter-spacing (`Theme.tracking`,
including negative tracking on headings) is calibrated for Archivo's metrics, so
a fallback sans does not merely look different — the tracking is wrong for it.

## Runtime dependencies

* **libmpv** — required for audio, or build with `-DMONOLIST_NO_MPV=ON`. A
  libmpv that fails to initialise leaves the app running with
  `Player.engineAvailable` false rather than silently doing nothing.
* **yt-dlp** — stream resolution and downloads, and the search fallback.
  Without it, playback falls through to the Piped/Invidious tiers.
* **Deno** — the JavaScript runtime yt-dlp uses to solve YouTube's player
  challenges. Without one, YouTube hides most formats, the good audio ones
  included.
* **FFmpeg** — download post-processing: extraction, tags, cover art, trimming.
  Without it, downloads are saved as the raw stream.

All three tools are looked for in `tools\` next to the executable, next to the
executable itself, in `MONOLIST_TOOLS_DIR`, and then on PATH; bundled copies win.
On a Mac, yt-dlp and Deno are looked for first where the app unpacks and
updates them (above), FFmpeg next to the executable
(`Monolist.app/Contents/MacOS`), and PATH includes Homebrew's folders even when
the app is opened from Finder.

## Self-tests and diagnostics

The debug build keeps its console. Start it with `QT_FORCE_STDERR_LOGGING=1` to
see the log there, and with `MONOLIST_MPV_LOG=warn` (or `info`, `v`) to add
mpv's own messages.

Every file that starts playing writes one `stream:` line, built only from what
mpv reports once the sound has started: the codec and the decoder's rate,
channels and sample format; the bitrate (the file's average for a file that
holds sound alone; for one with a picture in it, what its container declares,
or else mpv's own measure); what the sound device was opened with; whether the
sound is resampled; and mpv's volume. For example
`stream: CmThpha4Hoo from Streaming · InnerTube: aac 44100 Hz stereo floatp,
130 kbps (file average) -> wasapi 48000 Hz stereo float, resampled 44100 ->
48000 Hz, volume 65%`. Now Playing and the player bar show its codec and
bitrate (`Player.streamQuality`), and the short form of the same line
(`Player.streamInfo`) under the pointer. A song that had to be rescued (a refused link, an
early end, a refused file) says what rescued it once its sound starts:
`playback: <id> rescued: its sound came from InnerTube, 301 ms after the
first failure`.

    monolist --play <videoId> [seconds] [--again] [--at <s>] [--spoil [n]] [--video [--switch-at <s>]]
             [--as "<title>" "<artist>" [length s]] [--spoil-saavn] [--saavn-on] [--saavn-late <ms>]
             [--saavn-off-at <s>]
                                                    resolve and play; --again replays from the cache,
                                                    --at jumps into the song; --spoil hands InnerTube's
                                                    first n links (1 by default) over spoiled, so mpv
                                                    refuses them: with one, InnerTube is asked afresh,
                                                    and --again then plays InnerTube's link; with two,
                                                    the track is rescued by its muxed stream (itag 18),
                                                    which the log names, and --again then starts from
                                                    that kept link, InnerTube having refused both; --video
                                                    plays it as a music video and
                                                    --switch-at asks for the picture after that long;
                                                    --as names the song, which JioSaavn is asked about;
                                                    --spoil-saavn hands its JioSaavn link over pointing at
                                                    a missing file, so the way back to YouTube shows;
                                                    --saavn-late holds JioSaavn's answer back, so YouTube
                                                    starts and the song moves to JioSaavn mid-song;
                                                    --saavn-off-at picks Standard sound quality that many
                                                    seconds in, which stops a move under way.
                                                    JioSaavn is asked only about a named song (--as, or
                                                    one in the library) or with --saavn-on, and never
                                                    with --spoil, so plain --play timings stay YouTube's
    monolist --saavn-test                           JioSaavn with no network: DES known answers and a link
                                                    made by another DES, HTTPS on JioSaavn's CDN only,
                                                    320 kbps and the 96 kbps floor, both details shapes,
                                                    entities, signatures, and the matcher on 68 invented
                                                    pairs (dubs, unknown asides, one-sided remixers) and
                                                    its choices between rows; and AudioAlign's lag on
                                                    invented sound shifted by known amounts
    monolist --saavn "<title>" "<artist>" [seconds] [--no-india-headers]
                                                    one real JioSaavn lookup: the row taken, its bitrate and
                                                    host (never the whole link), and every refusal's reason
    monolist --queue-test <videoId>... [--early]    load paused, Play, Next near the end, Previous twice,
                                                    Next while paused: the clock and what was recorded at
                                                    each step. One id that will not resolve checks the
                                                    quiet failure; --early presses Play while resolving
    monolist --queue-test <videoId> --recover <badId>
                                                    back from a failure: the broken id fails twice and
                                                    records nothing, the good one plays again and pauses,
                                                    and two quick Nexts while paused stay paused
    monolist --recovery-test                        how a song is kept playing, on the real mpv against a
                                                    stand-in server on this computer, with no network: a
                                                    refused link asked for afresh, a rescue link for one
                                                    play only, or kept an hour for a song InnerTube
                                                    refused twice (then no fresh InnerTube link for it),
                                                    a stream cut short at 60% (its reconnect
                                                    refused) picked up where it stopped, a row's own link
                                                    loaded again, a junk download streamed, a junk file
                                                    passed over, the three-in-a-row stop, and each switch
                                                    back (needs MONOLIST_DATA_DIR; about 90 s)
    monolist --session-test                         a launch opening on the queue, the song and the place the
                                                    last one left, on the real mpv against a stand-in server
                                                    on this computer (needs MONOLIST_DATA_DIR)
    monolist --loudness-test                        loudness levelling on the real mpv against the same
                                                    stand-in (needs MONOLIST_DATA_DIR)
    monolist --sound-test                           the sound effects with no libmpv and no sound card: the
                                                    chain's text against the copies proven inside libmpv,
                                                    the ramps' steps, the 16 curves, the clipping maths
                                                    against the filters' measured response, Studio clarity
                                                    (its bands and widths at each rate, beside the
                                                    equaliser, never clipping), what is kept and read back,
                                                    Slowed and Nightcore never on together (needs
                                                    MONOLIST_DATA_DIR; in the release gate)
    monolist --icon-test                            the app icon choice: both pictures, the setting, and
                                                    shortcuts pointed at the executable's icons, on
                                                    shortcuts made in the scratch folder (needs
                                                    MONOLIST_DATA_DIR; in the release gate)
    monolist --search-test                          a search with no cap: its next pages on a stand-in
                                                    YouTube Music on this computer (songs with the ones
                                                    shown dropped, the end, a failed page, card sections)
    monolist --search "q" [--filter f] --pages N    a real search and N more pages, counted as they land
    monolist --startup-test                         opening at sign-in: the Run value written, read back and
                                                    taken away, on a scratch key; Windows' own switch read;
                                                    a value naming a Monolist gone repaired; a sign-in start
                                                    leaving an open Monolist's window alone; files opened
                                                    with Monolist (a second start's, the system's open-file
                                                    events, the command line's) handed over whole and in order
                                                    (needs MONOLIST_DATA_DIR; in the release gate)
    monolist --sleep-test                           the sleep timer on a stand-in player: the countdown,
                                                    the fade and the volume put back, +10 minutes, the end
                                                    of the song (and the real player's stop after a song,
                                                    in --sound-live-test)
    monolist --import-test                          playlist import on stand-in sources: each link kind read,
                                                    the matching rule, pacing and rests, review, stop and
                                                    resume; no cookie or account header ever sent
                                                    (needs MONOLIST_DATA_DIR)
    monolist --tray-test                            closing to the system tray: the switch kept, the menu
                                                    and what it does (needs MONOLIST_DATA_DIR)
    monolist --sound-live-test                      the same effects on the real mpv with no sound device
                                                    (MONOLIST_MPV_AO=null): switched on and off mid-song, the
                                                    clock at 0.85x and 1.25x, every curve with the strongest
                                                    bass, reverb and Studio clarity (every filter answering),
                                                    Studio clarity's widths at 44.1 and 32 kHz, 8D added
                                                    mid-song, a chain mpv refuses or will not build (a song
                                                    from JioSaavn, and a picture with its sound), links that
                                                    will not open failing as with no effects on, a slow seek
                                                    through a stand-in server, the JioSaavn move carrying the
                                                    chain (needs MONOLIST_DATA_DIR; about a minute; timed,
                                                    so not in the release gate)
    monolist --download <videoId> [seconds]         one download through yt-dlp and FFmpeg, into the scratch
                                                    database and download folder (refuses without
                                                    MONOLIST_DATA_DIR; MONOLIST_DOWNLOAD_DIR may still
                                                    pick the folder)
    monolist --download-cleanup-test                what a failed or cancelled download deletes, on invented
                                                    files in a scratch folder: yt-dlp's partial and working
                                                    files, never a finished file, whatever the database
                                                    says; and where MONOLIST_DATA_DIR and
                                                    MONOLIST_DOWNLOAD_DIR send downloads
    monolist --saavn-download-test                  a download from JioSaavn's copy against a stand-in CDN on
                                                    this computer: name, tags and square cover as yt-dlp's,
                                                    a refused link handed to yt-dlp (an invented id, so
                                                    nothing is downloaded) with older files kept, no cover,
                                                    a cancel part-way, MP3, and no JioSaavn on Standard
                                                    (needs MONOLIST_DATA_DIR)
    monolist --search "<query>" [--filter <kind>]   one timed search, with suggestions; --filter songs,
                                                    videos, albums, artists or playlists picks the chip,
                                                    and the last three print their cards by section
    monolist --page <browse id> [--all]             an album or playlist page: the header, the first
                                                    hundred songs, then one more hundred as scrolling asks
                                                    for it, or with --all the whole rest as Play loads it,
                                                    timed, with how many rows are distinct songs
    monolist --shelf <browse id> [params]           a shelf's "show all" page: its title, each section's
                                                    cards, its songs, and one more part where it has more
    monolist --artist-page <channel id | name> [--mix shuffle|radio]
                                                    one artist page as the interface opens it (a name is
                                                    looked up first): the header, the top songs with each
                                                    credit's page, every shelf; --mix fetches that button's
                                                    songs too, and the open window plays them
    monolist --artist-links-test                    artist links with no network: credits from canned
                                                    answers, names split and kept across a restart, the
                                                    artist page's and artist search's parsers (needs
                                                    MONOLIST_DATA_DIR; its invented rows are removed)
    monolist --library-edit-test                    what the song and queue menus change, with no network:
                                                    moves in the queue (the current song, the radio heading
                                                    and the order shuffle off restores kept true) and in a
                                                    playlist (read back from the database), Remove from
                                                    library, Remove from history, Copy link (the clipboard
                                                    put back after); needs MONOLIST_DATA_DIR
    monolist --backup-test                          the library's backup file and its merge, with no network:
                                                    playlists' uids (the same again for the same rows, and in
                                                    the daily copy), tombstones and edits kept; a library
                                                    exported, emptied and brought back exactly; likes, saves,
                                                    playlists (songs as a set: added on two computers, taken
                                                    out on one), plays, turn-downs and History merged by every
                                                    rule, both ways round; the same file twice changing
                                                    nothing, History cleared once; times later than now left
                                                    out, never winning; a newer or a foreign file refused; no
                                                    download, path, setting, YouTube Music mirror or computer
                                                    name in the file; what the toast says; Export and Import
                                                    as Settings calls them (needs MONOLIST_DATA_DIR)
    monolist --drive-test                           the Google Drive backup against stand-ins on this computer
                                                    for Google and Drive, never Google itself: the sign-in's
                                                    page (client, loopback redirect, PKCE S256, state, three
                                                    scopes), the browser's return (wrong state ignored,
                                                    access_denied, the Drive box unticked refused), COPY LINK
                                                    for a browser that would not open, the code exchanged
                                                    with its verifier, tokens renewed on time and once on a
                                                    401, invalid_grant, a cancelled SIGN IN AGAIN; the first
                                                    file made in appDataFolder, another computer's merged
                                                    both ways and uploaded once, nothing uploaded twice,
                                                    plays waiting for the half-hourly sync, a clock five
                                                    hours off stopping the sync, a read cut off retried, a
                                                    damaged or newer file never overwritten, two copies made
                                                    one, backoff on 429/5xx/rate limit, the first sync's
                                                    toast kept through a failed upload; signing out deleting
                                                    only the backup's secret and settings, even in a copy
                                                    built without the client; no cookie sent, the token to
                                                    Drive alone, no redirect elsewhere followed, no secret
                                                    in the log (needs MONOLIST_DATA_DIR)
    monolist --update-test                          the in-app update on a stand-in for GitHub and scratch
                                                    copies, never GitHub or the real app: the feed read, the
                                                    file for each kind of copy, the project's release files
                                                    on GitHub only (and every redirect), checksums, a mismatch,
                                                    more than promised, a cut, CANCEL and a quit mid-download
                                                    leaving no .part, a launch's cleanup leaving a download
                                                    alone, CHECK/UPDATE/CANCEL/TRY AGAIN as About runs them (a
                                                    double-click's CANCEL let go), what the next launch says
                                                    and its wait for the helper; on a Mac, scratch bundles in
                                                    disk images got ready and refused (another app, not newer,
                                                    a broken seal, not the Developer ID), a copy put in place
                                                    by hand shown only a checked, quarantined image, and the
                                                    helper (--update-swap): its swap, second signature check,
                                                    way back (cannot move in, will not open, does not start)
                                                    and wait; on Windows, a portable copy's zip and swap (the
                                                    helper started inside its folder; an old folder with the
                                                    user's files kept; one holding the downloads left alone),
                                                    the way back, and the setup's arguments, never run once
                                                    changed (needs MONOLIST_DATA_DIR)
    monolist --set update.feed <url> --update-run   About's CHECK, UPDATE and RESTART with no window: this
                                                    copy downloads the update, hands over and quits, and is
                                                    replaced and opened again. The end-to-end test, against
                                                    a feed on this computer; it replaces the copy it runs
                                                    from, so only ever a scratch copy (needs
                                                    MONOLIST_DATA_DIR; MONOLIST_UPDATE_TEST_FAULT=move-new
                                                    makes the swap fail, to see the way back;
                                                    MONOLIST_TEST_VERSION=0.1.1 has one build play an older
                                                    one, scratch libraries only). scripts/update-e2e-windows.ps1
                                                    runs it on GitHub's Windows machine for copies installed
                                                    by a real setup and a portable one
    monolist --update-demo <state>                  Settings › Updates in a state (available, getit,
                                                    downloading, preparing, ready, manual, failed), nothing
                                                    fetched; with --settings-section updates for a shot
    monolist --lyrics "<query>"                     lyrics for the first three results, then one from the store
    monolist --lyrics-query-test                    the lyrics query and match rules with no network: the
                                                    18 fixed cases (C40), lines without credits never cut on
                                                    a guess, and the scorer ("Anti-Hero" is not "Hero")
    monolist --lyrics-flow-test                     how a lookup ends, against a stand-in LRCLIB and YouTube
                                                    Music: exact lookup, then search, the slow-exact hedge,
                                                    the 6 s deadline, provisional answers replaced once
                                                    LRCLIB answers, no "none" kept after a failure, old rows
                                                    read as final, the switch back; needs MONOLIST_DATA_DIR
    monolist --lyrics-race-test                     the lyrics race with scripted providers and no network:
                                                    order, the winner at once and losers called off,
                                                    patience, upgrades, timed and plain kept apart, error
                                                    is not "none", lazy providers, the serial switch, the
                                                    gate; then what the view shows of it; MONOLIST_DATA_DIR
    monolist --lyrics-prefetch-test                 the background lookups against a stand-in: two queue
                                                    rows kept with the lyrics closed, then opened from the
                                                    database, none for an unknown length, one lookup per
                                                    song, the setting, metered, no retry loop
    monolist --lyrics-pane "<query>" [--dwell <ms>] the search's first two songs played, the lyrics opened
                                                    <dwell> ms (3000) into the first and 1 s into the second;
                                                    one "pane-open" line each, timed from opening to lines
    monolist --library-test "<query>" [--videos]    a playlist, likes and a saved album from a real search;
                                                    --videos searches music videos, and says how many rows
                                                    read back from the database still know they are videos
    monolist --rec-download [--cancel-at <MB>]      the recommendation data downloaded as Settings does it,
                                                    each file kept, fetched or refused, then the shelves
                                                    built from it; --cancel-at stops part-way
    monolist --rec-remove                           Remove, as Settings does it, with the data in use
    monolist --secret-test                          the secret store: round trips, damaged files refused,
                                                    delete; exits 0 when every check passes
    monolist --lastfm-test                          Last.fm signing, the request body and every answer,
                                                    on invented keys and canned replies; no network
    monolist --listen-test                          when a listen counts for Last.fm, through the player
                                                    with the engine's part played by the test: 30 s, half,
                                                    4 min, seeks, pauses, buffering, repeat-one, a video
                                                    toggle, Play after a paused launch, a failed resolve
    monolist --scrobble-test                        the scrobble queue on canned replies: what is kept,
                                                    120 sent as 50/50/20, and each error's handling
    monolist --lastfm-connect-test                  connecting on canned replies: the browser page, polling,
                                                    window focus, I've approved it, the 10-minute limit,
                                                    the session kept encrypted, Disconnect
    monolist --scrobble-send-test [rows] [--expect-kept]
                                                    queued scrobbles sent over HTTP to MONOLIST_LASTFM_URL,
                                                    a stand-in on this computer (scripts/lastfm-mock.ps1)
    monolist --scrobble-kill-test                   two scrobbles queued, then it waits to be killed;
                                                    --diag afterwards shows they survived
    monolist --cookie-test                          the YouTube Music import on invented cookies: cookies.txt
                                                    (LF, CRLF, #HttpOnly_, spaces for tabs), a Cookie header,
                                                    cURL in bash and cmd quoting, duplicates across domains,
                                                    a missing LOGIN_INFO; the Cookie header for music, www
                                                    and s.youtube.com byte for byte; x-goog-authuser and the
                                                    visitor id from a copied request; the stored JSON
                                                    (always version 1, its session part one an older
                                                    build passes over; SI1's version 2 still read); the
                                                    cookies.txt written for yt-dlp (never
                                                    google.com), and read back as yt-dlp writes it (an
                                                    empty expiry for a session cookie); and the
                                                    SAPISIDHASH known answers
    monolist --ytm-session-test                     the YouTube Music session against a stand-in server on
                                                    this computer: signed-out requests byte for byte, each
                                                    host's own cookies, X-Goog-AuthUser, the account's
                                                    visitor id and a brand channel (onBehalfOfUser), the
                                                    check, rotation, 400/401/403, restart, sign-out, what
                                                    the row says in each state (Checking…, Signed in as the
                                                    name and handle, Could not reach, Session expired, Not
                                                    signed in, where to end it at Google), the offer to
                                                    delete the imported file, no value in the log
    monolist --home-account-test                    Home's feed as that account, against the same stand-in:
                                                    signed out byte for byte as before; the feed alone asked
                                                    again as the account once it is confirmed (logged_in=1),
                                                    new releases never; "Use my account for Home" off and on;
                                                    a sign-out and a 403 with the feed on its way; a launch
                                                    with a stored session; no value in the log
    monolist --account-play-test                    the account where a song needs it, and listens reported
                                                    to its history, against the same stand-in with this
                                                    program standing in for yt-dlp (--fake-yt-dlp): the
                                                    account asked only after a refusal it answers, never
                                                    ahead of time, switched off or past the hour's limit;
                                                    the cookies file (youtube.com's only, read back,
                                                    deleted, swept at start); no Cookie to mpv; a refused
                                                    signed-in link; sign-out; the listen report's /player,
                                                    GET, cookies and refusals; no value in the log
    monolist --visitor-test                         the one visitor id every InnerTube shares, against the
                                                    same stand-in: one fetch, a stored id used at once, the
                                                    30-day limit, LOGIN_REQUIRED renewed and asked once
                                                    more, the shared jar, Clear history, the switch back
    monolist --player-client-test                   /player's second client (VISIONOS 0.1) against the same
                                                    stand-in: asked only when 1.02's answer has no plain
                                                    stream, at once and as the same app, never after no
                                                    answer at all; no retry and 3 s of its own; the switch
    monolist --format-test                          which of /player's formats is played, against the same
                                                    stand-in: two real answers (URLs replaced) and invented
                                                    ones; Opus 774, 251, then AAC 141, 140, then the rest by
                                                    bitrate, DRC copies passed over, the video's own sound
                                                    before a dub, itag 18 from formats[] alone, the itag,
                                                    codec and kbps logged, and the switch back
    monolist --player-canary [videoId...]           each /player client alone on real YouTube: its answer,
                                                    itag and time, n= or pot= in the link, and whether the
                                                    link serves its first and second MiB (never the link
                                                    itself); fails when a client does not
    monolist --cancel-test [videoId] [rounds]       a skip while yt-dlp resolves: the cancel timed and this
                                                    thread watched every 5 ms, in turn as it is and with
                                                    ytdlp.cancel=wait, and each cancel's processes seen to end
    monolist --bounds-test [rounds] [--before]      how long a resolve may take, against a stand-in YouTube,
                                                    yt-dlp real or through a proxy that never answers: the
                                                    1.2 s hedge on its own connection, /player's 3 s, yt-dlp
                                                    at ~3 s with every /player 10 s late (real yt-dlp,
                                                    `rounds` times), the skip notice within 20 s with all
                                                    of it dark (real mpv), one yt-dlp at a time and the
                                                    switches; --before also times the same with every switch
                                                    back (needs MONOLIST_DATA_DIR)
    monolist --audio-devices [<videoId>]            the output menu: every device mpv lists, what the menu
                                                    offers (Auto, then the WASAPI or CoreAudio devices, then
                                                    a chosen one that is not connected), the kept choice and
                                                    the one in use; with a video id it plays, switches to
                                                    each device in turn with the clock read after each, and
                                                    goes back to the choice it found (needs
                                                    MONOLIST_DATA_DIR for that part)
    monolist --view <view> --scroll-test [--scroll-away]
                                                    that page scrolled as a person does (one notch, a quick
                                                    spin, touchpad streams, a fling, the wheel to the end, a
                                                    notch over a shelf): how far, how fast, the frame times
                                                    and the longest hold-up of the interface's thread, the
                                                    page's settings and make-up, and how many covers were
                                                    decoded where; the pointer sits over the page, or off
                                                    the window with --scroll-away
    monolist --diag                                 what the database holds, whether there is a Last.fm
                                                    key, the scrobbles waiting, and the YouTube Music session
                                                    (its state, size and cookie names, never a value)

Each quits by itself and reports on stderr. These open the window as it would
be, for a look at a state:

    monolist --view <view>                          home, search, downloads, library[:albums|:history],
                                                    page:<browse id>, artist:<channel id>,
                                                    artistname:<name>, shelf:<browse id>[|<params>],
                                                    playlist:<id>, playlist:liked
    monolist --query "<text>"                       search, with the text typed in
    monolist --open-queue  /  --now-playing         with the queue, or Now Playing, open
    monolist --effects-popup                        with the player bar's sound effects popup open
    monolist --settings-section <section>           Settings, scrolled to appearance, recommendations,
                                                    downloads, connections, effects, signin, ytmusic or backup
    monolist --ytm-demo <state>[+file]              the YouTube Music row as active, checking, unreachable,
                                                    rejected (a session that ended), notsignedin (an import
                                                    answered as signed out), unreadable (a stored copy that
                                                    would not open) or signedout (just signed out), with an
                                                    invented account and no cookies; +file adds the offer
                                                    to delete an imported file
    monolist --backup-demo <state>                  the Google Drive row as off, waiting, nobrowser,
                                                    finishing, synced, syncing, offline, slowed,
                                                    unreadable, newer, clock (a clock hours fast), expired,
                                                    denied, noscope, signedout, nostore, nobuild or
                                                    nobuildsignedin (a copy without the client that still
                                                    keeps an earlier copy's sign-in), with an invented
                                                    account and nothing stored, read or sent
    monolist --set <key> <value>                    write a setting first (lrclib_url, piped_instances,
                                                    invidious_instances; youtube.visitor launch|home
                                                    keeps the visitor id for one launch, as before;
                                                    youtube.player_client first|second asks /player as
                                                    VISIONOS 1.02 or 0.1 alone; youtube.format bitrate
                                                    plays the format with the highest bitrate, whatever
                                                    its codec, as before Opus came first; ytdlp.cancel
                                                    wait has a cancelled lookup wait for its processes,
                                                    as before; youtube.player_deadline off,
                                                    playback.resolve_deadline off and ytdlp.resolves
                                                    parallel undo the resolve's bounds, as before)

`MONOLIST_DATA_DIR` keeps the database somewhere else, so a test never touches
the real library; the scrobbling tests, `--ytm-session-test`,
`--home-account-test`, `--account-play-test`, `--download-cleanup-test` and `--library-edit-test` refuse to run without it,
since they empty the scrobble queue, replace the stored session, open the
downloads on that database or write playlists and likes into it. It moves downloads too, to `downloads` inside it, so a test never
writes to the real Music folder. `MONOLIST_DOWNLOAD_DIR` names the download
folder outright, and wins when both are set. It moves only the folder, not the
database, and a finished download is written into the library, which is why
`--download` refuses to run without `MONOLIST_DATA_DIR`.
`MONOLIST_REC_DATA_URL` fetches the recommendation data from another address, or a local folder (`file:///C:/dev/monolist-data/`), instead of
the pinned tag on GitHub. `MONOLIST_LASTFM_URL` sends the self-tests' Last.fm
calls, made with an invented key, to a stand-in on this computer, for
`--scrobble-send-test` against `scripts/lastfm-mock.ps1`. Only a loopback
address is taken, and never for the build's own key or a real session, which
only ever go to ws.audioscrobbler.com.

## Storage

    downloads         <Music>/Monolist/<artist> - <title> [<id>].<ext>
    database          <AppData>/monolist.db  (library, playlists, history, lyrics, settings)
    recommendations   <AppData>/recommendations/v1  (downloaded from Settings; Remove deletes it)
    secrets           <AppData>/secrets  (sign-in keys, encrypted with DPAPI for the Windows user)
    yt-dlp cookies    <LocalAppData>/yt-dlp-cookies  (one lookup's copy of the session, deleted after it)
    artwork           <Cache>/artwork  (256 MB cap)
    opened files      <AppData>/file-covers  (the covers of files opened with Monolist, one per file version)
    Drive backup      monolist-library.json in the Google account's hidden appDataFolder (signed in only)

On a Mac, `<Music>` is `~/Music`, `<AppData>` is
`~/Library/Application Support/Monolist/Monolist` and `<Cache>` is
`~/Library/Caches/Monolist/Monolist`.

Downloads live in the user's real Music folder, the convention Melody settled
on, so they survive reinstalls and other players can see them; with
`MONOLIST_DOWNLOAD_DIR` or `MONOLIST_DATA_DIR` set they go there instead (see
Self-tests and diagnostics). A download that fails or is cancelled takes only
what it wrote itself and yt-dlp's partial files with it: a finished file already
in the folder stays, even one the database has no row for.

## State

Builds and runs on Windows 11 (ARM64, through x64 emulation) with Qt 6.11.2 and
MinGW 13.1. Verified end to end: InnerTube search and suggestions, streaming
through yt-dlp, the link cache, downloads with tags and cover art, offline
playback, the queue and autoplay radio, Home, album and artist pages, artist
links, playlists, likes and saved albums, and synced and plain lyrics.

Known gaps:

* macOS: built and run on a Mac since 2026-10-24 (macOS 26 on Apple silicon,
  Homebrew's Qt 6.12, libmpv 0.41, FFmpeg 9): every offline self-test passes
  there, and playback, the title bar, the menu bar, the Keychain and opening
  at login were checked on it. Not yet used day to day there; the Dock's icon
  while closed, and a signed and notarised build for other Macs, are left.
* Linux builds and starts (checked on Ubuntu 24.04 with Qt 6.11.2), but has
  not been used day to day and has no packaging yet.
* The output menu's devices and the wheel and trackpad scrolling on Linux
  are untested. On the Mac both were checked (2026-10-24): the menu lists
  CoreAudio's devices (mpv also lists each under AVFoundation, which is left
  out), and `--scroll-test` scrolls Home at a frame a refresh at 120 Hz.

## About the Swift libraries

Kingfisher, SwiftyJSON, SwiftSoup, SwifterSwift, ColorThiefSwift and
SwiftUI-Introspect are Swift/iOS libraries. They cannot be linked into a
Qt/C++ application on Windows or Linux — there is no binding path, and
Introspect has no meaning outside SwiftUI. Each one's *job* is covered:

| Wanted | Used instead | Where |
| :--- | :--- | :--- |
| Kingfisher — async image load + cache | `QQuickAsyncImageProvider` + `QNetworkDiskCache` | `artworkcache.*` |
| ColorThiefSwift — dominant colour | Weighted RGB histogram over a downsampled image | `PaletteTool` |
| SwiftyJSON — JSON parsing | `QJsonDocument` (built into Qt) | throughout |
| SwiftSoup — HTML parsing | Not needed; InnerTube and yt-dlp removed the scraping | — |
| SwifterSwift — utility extensions | Qt's own containers and algorithms | — |
| SwiftUI-Introspect | Not applicable outside SwiftUI | — |

NewPipeExtractor is a **Java** library. Calling it from C++ means shipping a JVM
alongside the app on all three platforms, for metadata InnerTube and yt-dlp
already return. It is not wired in. If it is ever wanted, `MediaExtractor` is
the seam — it already isolates search behind signals, which is exactly what a
second extractor would slot into.

## Licence

The code in this repository is MIT-licensed — see [`LICENSE`](../LICENSE).

That covers what was written here, not what the app uses:

* **Archivo** in `fonts/` stays under the SIL Open Font License 1.1
  (`fonts/OFL.txt`), which lets it be bundled but not sold on its own.
* **Qt** is used under the LGPL 3, dynamically linked.
* **libmpv, yt-dlp, FFmpeg and Deno** are not in the repository. They are found
  at run time (see *Runtime dependencies*) and each keeps its own licence. A
  build that *ships* them is bound by those licences too — libmpv and FFmpeg
  builds are commonly GPL, which then applies to that distributed package.
* The **recommendation catalogue and graph** the app can be pointed at are not
  in the repository and are not covered by this licence; they carry their own
  (CC BY-NC).
* The **recommendation data** the app downloads from Settings lives in its own
  repository, [Monolist-data](https://github.com/droidboy08-hub/Monolist-data),
  under its own licences — CC BY-NC 4.0 for the catalogue, CC BY-NC-SA 3.0 for
  the graph — for non-commercial use only; see that repository for the credits.
