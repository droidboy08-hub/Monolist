# Keeping the YouTube Music account safe

Research of 2026-09-29, behind `src/accountguard.*` and the owner's rule that
signing in must never put the user's Google account at risk, whatever way the
user signs in. Two studies fed it: what is known of accounts getting into
trouble with outside clients, and how InnerTubeX and the Meld app handle the
same account (GPL-3.0; studied for their behaviour only, no code taken).

Labels: [official] a project's or Google's own docs; [maint] a maintainer's
statement; [anecdotal] one user's report; [inference] reasoning, not sourced.

## What is known

- No cookie session is risk-free. yt-dlp's wiki: using an account "you run the
  risk of it being banned (temporarily or permanently)"; its maintainers say
  never to use an important Google account (yt-dlp wiki Extractors, 2025-06;
  issue #13964, 2025-08) [official, maint].
- What people actually reported in 2024-2026 was temporary: session rate
  limits of up to an hour, "Sign in to confirm you're not a bot" for hours to
  a day, account-level playback blocks for hours to two days. No verified case
  of a whole Google account suspended was found (yt-dlp #10085; Metrolist
  #4192, #4220; OuterTube #829; Pear Desktop #3257) [maint, anecdotal].
- What came before them: high volume ("used a lot", long playlists with no
  sleeps, bulk downloads, a looping sync), bursts (rapid song switching),
  identity churn (many new visitor ids: Koda PR #297 cut 8 requests and 3 new
  visitor ids per song to 1 and 0, and the bot checks stopped), and VPN or
  datacenter addresses [anecdotal, measured in Koda's case].
- Where YouTube throttles: about 300 videos an hour for a guest, about 2000
  (about 4000 requests) for an account, tied to the session rather than the
  address; yt-dlp then says "rate-limited by YouTube for up to an hour"
  (yt-dlp wiki, 2025-04; PR #12958) [official]. Its own preset sleeps 0.75 s
  between requests and 10-20 s between downloads.
- The same session in the browser and in the app at once gets it rotated out
  from under one of them (yt-dlp maintainer, #12009) [maint]. So the guide
  says to copy the sign-in from a private window and close it without using
  it again, and Monolist keeps the rotations it is sent (Set-Cookie) in its
  own encrypted copy.
- DBSC (device-bound sessions) is on by default in Chrome on Windows since
  2026-05; cookies exported from it may die quickly outside Chrome (Google
  Workspace Updates, 2026-05-28) [official, effect unverified]. Firefox is the
  suggested browser for the export.

## What InnerTubeX and Meld do (studied 2026-09-29)

- InnerTubeX (github.com/MetrolistGroup/innertubex, v0.7.3): cookie header
  only, no sign-in of its own. It retries 429 twice within about 1.5 s and
  ignores Retry-After; it has no pacing, no budget and no bot-page detection.
  Its v0.7.0 moved signed-in playback to "anonymous first".
- Meld (github.com/FrancescoGrazioso/Meld, on InnerTubeX v0.5.2): an embedded
  Google sign-in window (flagged as an unsafe browser by Google, Meld #68);
  the session kept in plain preferences; every browse and `next` call signed
  in by default; about 30-45 requests per Home load; the library synced both
  ways, pages fetched back to back (a 50 ms gap meant for database writes),
  every album and playlist fetched in full on each sync, a sync at launch, on
  reconnect and on pull-to-refresh with a 30-minute cooldown. No ban reports
  were found for it either, and nobody has measured whether its volume made
  the August 2026 wave of signed-in bot checks worse.

Monolist does the opposite of each of those.

## What Monolist does (AccountGuard and around it)

Every call that carries the account, whoever makes it (the session's check,
Home's feed, the library import, listen reports, yt-dlp's lookups with the
account's cookies), waits its turn with one guard:

| Rule | Value | Why |
| --- | --- | --- |
| In flight at once | 1 | No bursts; Meld's one-consumer queue is the one thing worth copying |
| Spacing | 1.5 s + 0-1 s random between starts | yt-dlp's own preset sleeps 0.75 s; twice that, never on a beat |
| Bucket | 6 close together, then one every 4 s | About 15 a minute sustained |
| Per hour / per day | 150 / 800 (every kind together) | A few per cent of the ~4000 an hour yt-dlp's wiki gives |
| Listen reports | 40 an hour, 400 a day | Conservative; no flags reported, but they are writes |
| yt-dlp with the account | 15 an hour, 60 a day, 20 s apart, counted as 4 calls | A lookup is several requests; the account is the last resort |
| 429, bot check, "unusual traffic", a 403, a redirect to /sorry or a sign-in page | Everything rests 1 h, then 3 h, then 24 h (each earlier rest remembered for three days, so a throttle right after a day's rest is another day), or longer if Retry-After says so; then the session is checked before anything else goes with it | "Up to an hour" limits; bot checks lasting about a day |
| Retries with the account | Network errors and 5xx only, once, 4-6 s later, through the guard; never a 429 or another 4xx; a list's part that failed is not asked for again as the reader scrolls | InnerTubeX's quick 429 retry is what not to do |
| Counts and rests | Kept in the settings table across restarts | A restart must not be a way round them |

Around it:

- 401 ends the session (its credentials refused). A 403 rests the account and
  has the session checked afterwards, rather than ending it: YouTube answers
  403 to an address it has stopped trusting too, and ending the session would
  send the user back to sign in again, which is more traffic, not less.
- The account is used only where it has to be: Home's feed, the library
  import, pages of the account's own lists, a song YouTube refuses signed out
  (after yt-dlp signed out has also refused it), and listen reports. Search,
  suggestions, radio, lyrics, the recommender and ordinary playback never
  carry it.
- Timers are jittered: the launch check 4-8 s in, the periodic check 6 h
  give or take 30 min, back-offs 75-125 %.
- The library import (`src/ytmimport.*`) reads only, one page at a time with
  3-6 s between pages and a longer pause every ten, caps the pages (50 of
  liked songs, 20 of playlists, one call of history), replaces what it holds
  only when a list was read to its end, never fetches every playlist's songs
  (one is read when it is opened), syncs on its own at most every 12 hours
  and never at the moment of a launch, and allows SYNC NOW at most every 15
  minutes.
- Nothing is ever written back to the account (likes, playlists, feedback);
  listen reports are the one write, behind their own switch.

Tested by `--account-guard-test` (the limits and rests, on a clock of the
test's), and by `--ytm-session-test`, `--home-account-test` and
`--account-play-test` and `--ytm-library-test` against a stand-in server.

## Still unknown (for the live test, docs/testing-your-youtube-account.md)

- How many requests one yt-dlp lookup with cookies makes, and whether yt-dlp
  retries a 429 itself.
- How a dead session looks on a browse (an error, or a quietly signed-out
  page), and whether YouTube Music answers 403 for anything but distrust.
- Page sizes and continuation shapes of VLLM, FEmusic_liked_playlists and
  FEmusic_history on WEB_REMIX today.
