# Testing the YouTube Music account, live

Everything the account does in Monolist has so far been tested only against a
stand-in server (`--ytm-session-test`, `--home-account-test`,
`--account-play-test`, `--ytm-library-test`, `--account-guard-test`). This is
the check with a real account, step by step. It closes ROADMAP C02, C03, C04,
C07 and C08.

**Use a spare Google account**, one you can afford to lose. yt-dlp's own
maintainers warn that an account used from outside YouTube's apps can be
limited or banned, and Monolist's limits make that unlikely, not impossible
(`docs/research/account-safety.md`). Never your Gmail account.

## Before you start

1. Build the current `main` and run it from a terminal, so its log is there
   to read:

   ```powershell
   $env:QT_FORCE_STDERR_LOGGING = '1'
   C:\dev\monolist-build\debug\monolist.exe 2>&1 | Select-String 'ytmusic:|ytmlibrary:|account:|catalog:|resolver:'
   ```

   No line of that log ever holds a cookie value or a SAPISID hash; names and
   counts only. If you ever see one, stop and tell Claude.
2. Have a text file open to note, for each step, what you saw and the time.

## 1. Sign in (C02)

1. Open a **private window** in Firefox (suggested: Chrome on Windows may bind
   the session to the device, and an export from it can stop working within
   minutes).
2. Go to music.youtube.com and sign in with the spare account. Your picture
   shows at the top right.
3. In Monolist: Settings → Connections → YouTube Music → IMPORT SIGN-IN, and
   follow the steps there (a cookies.txt from an extension, or the Cookie
   header / Copy as cURL from the developer tools).
4. **Close the private window without signing out and without using it
   again.** Using the same session in the browser rotates it out from under
   Monolist.
5. Expect: the row says *Checking…*, then within a few seconds *Signed in as
   <name> · @<handle>*. The log says `YouTube Music confirms the session
   (logged_in=1)`.
6. If Monolist offers to delete the exported file, say DELETE THE FILE.
7. Quit Monolist and start it again. Expect: *Checking…* for a few seconds,
   then *Signed in as …* again (the session survives a restart).

## 2. Home (C03)

1. Open Home. Expect: a red line *FOR <NAME> · FROM YOUR YOUTUBE MUSIC* under
   the poster, then YouTube Music's own shelves in its order (Quick picks,
   Listen again, Mixed for you, …) with their own small headings, then New
   releases, then Recently played.
2. The log says `Home's feed is the account's, from the check's own answer
   (nothing asked again)` after the launch check: one call, not two.
3. Play a song from Quick picks. Open a mix from *Mixed for you*: it opens
   (the log shows it was asked with the account) and plays.
4. Settings → Connections → turn *Use my account for Home* off. Home goes back
   to the signed-out feed at once, and your own suggestions (*Suggested for
   you*) show at the top if the recommendation data is installed and you have
   played a few songs here. Turn it on again.

## 3. Your library (C04)

1. A minute or two after the sign-in is confirmed, the log says `reading the
   account's library (on its own)` and, a little later, `read N liked songs,
   M playlists and K songs of history in X calls`. The pages come at least
   three seconds apart: that is on purpose.
2. Compare with music.youtube.com (in any browser, **not** the private window
   you exported from):
   - *Liked on YouTube Music* (sidebar, and Your Library) has as many songs as
     Library → Liked music there, in the same order (up to 5,000).
   - Your Library → Playlists lists the account's playlists (Liked music
     itself not among them), private ones included. Open a **private**
     playlist: it opens and shows its songs.
   - Your Library → History → ON YOUTUBE MUSIC shows the latest of the
     account's history (one page of it).
3. Settings → Connections → SYNC NOW. It reads again; pressed again at once,
   it says when it works again (a quarter of an hour: to go easy on the
   account).
4. Like a song on music.youtube.com, then SYNC NOW after the quarter of an
   hour: it shows up. Nothing you do in Monolist changes anything on YouTube
   Music: check that a song liked *in Monolist* does **not** appear in the
   account's likes.

## 4. A song YouTube refuses signed out (C07)

1. Search for a song or music video you know is age-restricted, and play it.
2. Expect: the log says `refused signed out (…); asking yt-dlp signed out,
   then with the account`, then `resolved with the account`. It plays; the
   stream line says *YouTube · signed in*. It takes a few seconds longer than
   usual.
3. Note how long it took to start. The account is asked at most 15 times an
   hour (60 a day), and not twice within 20 seconds.

## 5. A listen in the account's history (C08)

1. Play an ordinary song for more than half its length (or four minutes).
2. The log says `… reported to the account's YouTube history`.
3. On music.youtube.com → History, the song is there (it can take a minute).
4. Settings → Connections → turn *Send my listens to YouTube history* off,
   play another song past half: it is not reported, and not in the history.
   Turn it on again if you want it.

## 6. If YouTube asks Monolist to slow down

You should not see this. If you do, the row says *YouTube asked Monolist to
slow down, so nothing is asked with your account until HH:MM*, and everything
plays signed out meanwhile. Note the time and the log lines around it
(`account: YouTube asked to slow down (…)`), and tell Claude: that is exactly
the evidence the limits are tuned with. Do not sign in again to get round it.

## 7. Sign out

1. Settings → Connections → SIGN OUT.
2. Expect, at once: the row says the session was deleted from Monolist and
   how to end it at Google; *Liked on YouTube Music*, the account's playlists
   and its history are gone from Monolist; Home is the signed-out one; a
   private playlist page that was open says you are no longer signed in.
3. At Google: Google Account → Security → Your devices → the Firefox private
   session → Sign out.

## What to send back

The notes from each step, the times, and the log lines named above. Record
the jar's size and the cookie names only if asked; never a value.
