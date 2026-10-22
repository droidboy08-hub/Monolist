# Monolist — the design, and why

This is the argument behind the interface. Every rule here has a reason; where
a rule is broken, the reason for breaking it is written down too. If a change
cannot be justified against this document, either the change is wrong or the
document is — and then this is what gets edited first.

## 1. What the interface is

**A printed page, not a pane of glass.** Paper (`#f3f2f2`), ink (`#201e1d`), one
signal red (`#ec3013`), Archivo, 2px rules, square corners, no gradients, no
shadows, no blur.

*Why.* Hierarchy in this design comes from type, weight and white space, the way
it does in print. Decoration — glows, gradients, translucency — would carry the
hierarchy instead, and then every screen needs more decoration than the last to
stay legible. A printed page also survives being dense: this app shows long
lists of small text, which is exactly what editorial layout is good at.

**Three exceptions, the owner's (2026-09-30).** A song's cover at the head of
a list row has its corners softened (6px, TrackCover): small and many, the
covers read as pictures set into the list rather than as more of its rules.
**Dark** prints the same page in reverse: paper `#141312`, ink `#ebe9e8`, the
greys stepped the other way, red type lighter so it still reads as red; the red
itself, and anything printed over a picture or on a red field, stays as it is
(`Theme.ink`, `Theme.paper`). **Ambient colour** washes the paper with a little
of the playing song's cover colour (11% light, 16% dark) and glows it, as a
gradient, at the head of the page; a grey cover gives none, and Settings >
Appearance turns it off. Both are atmosphere, not hierarchy: nothing reads only
by them, and the page is legible without either.

**And two more, the owner's (2026-10-21).** The player bar's cover and its red
play square have the list covers' 6px corners (the Modern bar's; Classic keeps
its square ones). And Now Playing is a stage
rather than a page (its "Ambient stage", see 3): one field of the cover's own
colour over the whole sheet, and on it the cover with 10px corners standing
off the field on a soft shadow, the one shadow in the app. The shadow is drawn
by a shader from the shape alone (RectangularShadow), so it costs nothing
while the song plays.

**One accent, used sparingly.** Red marks what is *active* (the song playing,
the open page) and what is *primary* (Play, and nothing else on the same
screen). The line being sung is the exception: on Now Playing's field it is
the one line in full ink, the rest in tints, with no red mark beside it.

*Why.* An accent that appears everywhere stops meaning anything. Because red
means "here", it can be small — a 10px square, a heart, a 2px rule — and still
be found instantly.

**Photographs are black and white, except where a page is about one.** Covers in
lists, shelves and cards print grey; the cover of an album page, a playlist page
or Now Playing is in colour, and a card's cover turns to colour under the
pointer. Settings > Appearance's **Monochrome covers**, the owner's
(2026-10-02), turns this off: every picture in colour, everywhere. Nothing
reads only by colour (a playing row has its bars, a card under the pointer
its play button), so the page still works with it off.

*Why.* Album art is the loudest thing in a music app; left alone it fights the
type on every screen. Desaturating it makes colour mean "this is the subject" —
which is also why the pointer bringing a card to colour reads as "this one".

An artist's page keeps its portrait black and white. The page is about a
person and a shelf of records, not about one photograph, and a coloured
portrait above a row of grey covers would claim that the picture, rather than
the music, is the subject. The records on it come to colour under the pointer,
as cards do everywhere.

**The window belongs to the system; everything inside belongs to the design.**
The app draws its own title bar, but keeps Windows' corner radius, snapping,
shadow, window menu and resize edges.

*Why.* A window is an operating-system object, and people use system habits on
it — snap it, drag it, double-click it. Breaking those to be consistent with our
own square corners would trade a real skill for a cosmetic detail.

## 2. Motion

**Motion explains; it does not decorate.** Every animation in the app answers
one of three questions: *where did this come from*, *what just changed*, or
*where am I now*. An animation that answers none of them is removed.

*Why.* Motion is the most expensive thing in an interface: it spends the user's
time. A 300ms flourish, met twenty times a session, is a minute a week of
watching the app instead of using it — and it is the first thing to stutter on
a slow machine.

### 2.1 Durations

| Token | Time | For |
| :--- | :--- | :--- |
| `Theme.instant` | 0 ms | A pointer's own feedback: hover tints, pressed states |
| `Theme.quick` | 120 ms | A colour or opacity changing in place: a like, a selected chip, a row leaving hover; and a page gliding the length of a turn of the wheel |
| `Theme.normal` | 220 ms | Something appearing or disappearing where it already is: a toast, a control fading in |
| `Theme.page` | 320 ms | Something arriving from elsewhere: Now Playing, the queue panel |
| `Theme.slow` | 520 ms | Long distances and atmosphere: the lyrics scrolling, Now Playing's colour field |

*Why these.* Under about 100ms a change reads as instantaneous — which is what a
pointer's own feedback must be, or the app feels laggy. Between 200 and 320ms
the eye can follow a movement without waiting for it; that is the working range
for anything entering or leaving. Past 400ms the user is waiting, so it is
reserved for a long scroll (where the distance justifies the time) and for the
colour field (which nobody is waiting on).

*The wheel.* A notch of the wheel moves a page as far as a browser on the same
system would (the system's lines per notch, at 100/3 px a line), easing there
over `quick`, and notches that come while it moves add to where it is going; a
touchpad's fine steps and a trackpad's pixels are followed as they come
(`SmoothWheel`). The page is the pointer's own feedback here, so it gets the
shortest duration that still shows it travelled rather than jumped. Qt's own
wheel handling made each event a new fling with a speed floor, so a quick spin
went less far per notch than a slow one and a touchpad swipe was held to about
600 px a second, which read as slow.

### 2.2 Easing

| Kind | Curve | Why |
| :--- | :--- | :--- |
| Entering | `OutCubic` | It starts fast, as if already on its way, and settles. The user sees the destination early |
| Leaving | `InCubic`, at 0.8× the duration | Leaving should not hold attention; it accelerates away |
| Moving in place | `InOutCubic` | Both ends are visible, so both ends are eased |
| Colour | `OutCubic`, quick | Colour has no momentum; the curve only softens the start |

**Never overshoot, never bounce.** No `OutBack`, no springs.

*Why.* Overshoot implies mass and elasticity — a physical object on a spring.
This interface is printed: paper does not bounce, and a bouncing panel would be
the one place in the app that claims to be a thing rather than a page.

### 2.3 What may move

Allowed: **position**, **opacity**, **colour**, **scroll offset**, and a
panel's **width** when it pushes content aside.

Not allowed: **scale**, **rotation**, **blur**, **skew**, **perspective**.

*Why.* Scaling and rotating imply an object in space with a size of its own; in
a printed layout an element has one size, decided by the grid. (This is also why
a liked heart changes colour instead of popping: the pop is a scale.) Blur and
perspective belong to glass interfaces, which this is not.

### 2.4 Direction means something

* **Now Playing rises from the bottom**, because it *is* the player bar
  enlarged — it comes from where the bar is, and returns there.
* **The queue arrives from the right** and pushes the content aside rather than
  covering it: it is the next thing in reading order, and what it is next to
  matters.
* **Menus do not travel.** They appear where the pointer is, without animation.
  A menu is a response to a click, not a place you go; sliding it in delays the
  answer and moves the target the user is already aiming at.
* **Views do not slide.** Changing page replaces the content and fades the new
  one in over `Theme.quick`.

*Why no page slides.* A slide implies the pages are laid out side by side, which
would have to be true of every pair — Home to Settings, an album to a playlist —
and it is not. It would also cost `Theme.page` on every single navigation, which
is the difference between an app that feels immediate and one that feels
animated.

### 2.5 Lists never stagger

Rows appear together, not one after another.

*Why.* A staggered list withholds information for the sake of looking alive: the
tenth row arrives a third of a second after the first, and the list cannot be
read or clicked until it settles. In a music player the list *is* the content.

### 2.6 Hover in, fade out

Hover feedback appears instantly and leaves over `Theme.quick`.

*Why.* Instant is what makes an interface feel responsive. The fade on leaving
is not decoration: dragging the pointer down a list would otherwise flicker
black-white-black at every row boundary.

### 2.6a An icon answers in the glyph, never in a plate behind it

No icon button draws a shape on hover. The glyph itself carries all three
things it can say:

| | Colour | Means |
| :--- | :--- | :--- |
| Rest | `neutral700` | there if you want it |
| Hover, press | `text` | the pointer is on it |
| On | `accent` | the thing it controls is on |

A glyph that is already `accent` — a navigation arrow, the plus, a shuffle that
is on — cannot go to ink without losing what its colour says, so it brightens
to `accent600` instead. The heart is the one that can do better still: it
fills, part-way under the pointer and completely once liked, which is a preview
of the click rather than a report that the pointer arrived.

*Why.* The plate was a filled square in 10% accent. It put a pale red box
around every arrow and dot in the app — three at once inside a hovered track
row — and directly behind the heart it turned the one red mark people aim at
into a smudge. A box is also a second shape arguing with the glyph inside it,
in an interface whose whole claim is that shapes mean something. Ink for "you
are here" costs nothing, never collides with the accent, and leaves red to go
on meaning *on*.

This is why the transport arrows sit at grey rather than ink: ink had to be
freed up to mean the pointer. The bar reads quieter for it, and the one red
thing in it is the play button.

### 2.7 Motion is cheap or it is not there

Animate `opacity`, `x`/`y`, `color`, `contentY`. Never animate the width or
height of items inside a long list, and never animate anything that re-lays out
a whole view.

*Why.* Opacity and position are composited; width and height re-run layout for
every child, every frame. On a modest machine — or a virtual one — that is the
difference between 60fps and a slideshow, and it will always be the animation
that gets blamed for the app being slow.

**What moves for as long as a song plays steps; it does not animate.** The
playing bars step fifteen times a second on whole pixels, and the progress
fill grows a whole pixel at a time. *Why.* An animation draws the whole window
again at every refresh of the screen (180 times a second on a fast one) for as
long as it runs; a song plays for minutes, and the mini player stays on screen
over everything else. A step that lands where the thing already is draws
nothing at all.

## 3. Where things are, and why

* **The title bar is the top strip**, shared by the logo block and the top bar:
  one 64px band across the window. Its empty parts drag the window.
  *Why.* A custom title bar that only spans part of the width leaves a dead
  strip; making the whole band draggable means the window can always be grabbed
  where people reach for it.
* **Search sits at the top right, beside the window buttons.**
  *Why.* It is used from every page, so it lives in the chrome rather than in
  any page; the top-right corner is the one place the sidebar never covers.
* **Navigation is the sidebar; the library lives under it.**
  *Why.* Places (Home, Search, Library, Downloads, Settings) are fixed and few;
  playlists are many and change, so they scroll under a rule that separates the
  fixed from the personal.
* **The player bar spans the full width, below everything.**
  *Why.* It belongs to the app, not to a page: it keeps playing while views come
  and go, and its position is the one thing that must never move.
* **The player bar is quiet: the song, the transport, and a row of glyphs.**
  The owner's choice of three (2026-10-21, "A · Quiet"), after calling the old
  bar "cluttered, with uneven looking icons" beside LastWave's "lean and
  clean" one. At the left the cover (56px), the title (15px, extrabold) over
  the artist (13px, grey), and the heart at the end of the side; a click on the
  cover or the title opens Now Playing, a right click the song's menu. The
  cover's corners are softened to 6px, as a list cover's are, through a mask
  drawn once for it (Nav.barCoverMask) rather than a layer of its own. In the
  middle shuffle, previous, the red play square (40px, 6px corners), next and
  repeat, and
  under them a 3px progress line, red as far as the song has got, with the
  times either side in 11px equal-width figures. At the right a badge with the
  codec and bitrate mpv reports ("OPUS 160", "AAC 320" from JioSaavn; the whole
  stream line under the pointer; nothing until both are known), then lyrics
  (Now Playing on them), queue, sound, output, the volume glyph and an 88px
  slider, Now Playing, and "…". Every glyph is 20px in a 36px square at one
  stroke (1.75 on Lucide's grid of 24), grey at rest, ink under the pointer,
  red when on (2.6a). "…" is the song's menu headed by what left the bar: the
  mini player (Ctrl+Shift+M), the sleep timer's choices, the video for a song
  that has one, then the song's own entries, its download among them. What is
  on comes back out beside "…" for as long as it is: a red moon while the sleep
  timer runs, which opens its countdown, +10 minutes and Turn off; the red video
  glyph while the picture is on, which turns it off; and a download running
  draws its 2px rule along the foot of "…".
  *Why.* A bar is looked at all day and used for a few things: play, skip, the
  volume, the queue, the lyrics. The old one also held, all the time, what is
  wanted now and then, at four glyph sizes and two weights, so the eye sorted it
  again at every glance. One size and one weight make the right side a single
  row of marks, and red stays the play square and whatever is on. Hiding the
  rest costs nothing that matters: whatever is running shows itself on the bar,
  and nothing that was one click away is more than two. Two departures from the
  mockup, both for this document's rules: previous and next are grey at rest,
  not ink, since ink means the pointer (2.6a); and the sound keeps its vertical
  sliders, since the mockup's horizontal ones beside the queue's three lines
  read as the same glyph twice (and the mini player has the vertical ones).
  *Folding.* As the window narrows the bar gives things up in one order: the
  volume slider folds into its glyph, with a popup; the lyrics and the output
  go into "…"; the badge goes; the title and the artist go, the cover and,
  while there is room, the heart staying; the transport closes up and Now
  Playing's button goes (the cover opens it); last, the moon and the video go
  back into "…". Between the steps the title takes the slack, and the transport
  keeps the bar's middle for as long as the two sides can be equal (down to
  760px). The badge's room is held before it arrives, so a song starting moves
  nothing. The sound glyph never folds, so its red shows at any width. At 1400px
  everything is there; at 1120 the slider has folded; at 900 and 880 the
  lyrics, the output and the badge have gone and the title is shorter; at 760
  the cover and the heart are left.
* **The bar before it stays, as a choice.** Settings > Appearance > Player bar,
  above the app icon: two small pictures, Classic and Modern (the owner's,
  2026-10-22). Modern is the quiet bar above, and the default; Classic is the
  bar as it was in 0.1.152, square play button and all, with Studio clarity in
  its red sliders like every other effect. Only the chosen bar is built, and
  choosing the other replaces it at once.
  *Why.* The owner wanted the new bar offered, not imposed; a picture says
  which is which without a line of explanation.
* **Now Playing keeps the player bar visible, and slides out from behind it.**
  *Why.* Opening the big view should not take away the controls the user already
  knows; it adds the cover, the lyrics and the queue, and changes nothing else.
  Because the view is that bar enlarged, it emerges from behind the bar and
  tucks back behind it when closed — the bar is drawn on top throughout. Sliding
  *over* the bar and off the bottom would read as a page leaving, which is the
  wrong story: nothing left, the player is simply small again.
* **Now Playing is an ambient stage.** The owner's choice (2026-10-21, "A ·
  Ambient stage"). One calm field over the whole sheet, the playing cover's
  own colour (CoverPalette) held to a third of its strength: deep in dark
  (#18222d for a blue cover), pale in light (#dde6ef); a grey cover gives a
  grey field. Everything on it is printed in tints of the same hue, each found
  by its contrast against the field rather than written down, so a yellow or a
  red cover reads as well as the mockup's blue: the titles and the line being
  sung in printed paper (dark) or ink (light), the artist and the notes
  ("soft", 7.5:1 dark, 5.6:1 light), the lines still to come ("next", 5.2 and
  4.8), the lines sung ("past", 3 and 2.3), the rules (1.8 and 1.6). At the
  left the cover, 420px at most (less where the height or the width is
  short), with 10px corners and a soft shadow; under it the title (30px,
  extrabold), the artist (17px, soft, each name a link), then two small tags:
  the sound as mpv reports it ("OPUS · 133 KBPS", the whole stream line under
  the pointer) and UP NEXT with the next song, which shows the queue. A third
  tag names the sound effects while any is on. At the right the lyrics at
  36px, headed by where they come from ("SYNCED · LRCLIB"), or the queue in
  the field's tints. No NOW PLAYING label, no album line and no stream line:
  the badge says what the line said. A window under 1000px puts the song at
  the head of the page, a 96px cover beside its name and tags, and the lyrics
  under it at the full width.
  *Why.* The poster split the screen into a coloured half and a paper half,
  and the lyrics, the thing read for minutes, sat on the plain one. One field
  makes the whole sheet the song's, and the type, not a panel, carries the
  hierarchy on it. Tints of the field's own hue, rather than the page's warm
  greys, keep the field one colour.
* **Now Playing closes at the top left.** A **⌄ CLOSE** with its key (Esc)
  printed on it, in the field's ink, after the window's buttons where the
  system keeps them at the left, and after the traffic lights on macOS. Under
  the pointer it takes a tint of the ink. The player bar's Now Playing button
  turns to the same chevron, in red, while the view is open. In the narrowest
  windows the key is given up for room.
  *Why.* The stage's strip is the owner's design: the close the old screen
  had, where the page begins, and the right end kept for what changes the page
  (LYRICS | UP NEXT, the video) and the window's own buttons.
* **The video is a glyph in the strip, for a song that has one.** Beside
  LYRICS | UP NEXT: grey at rest, ink under the pointer, red while the picture
  is on, dots while it loads (the cover stays up until the first frame). A
  song without a video shows no glyph. `V` does the same.
  *Why.* The COVER | VIDEO switch spent a labelled pair on a choice most songs
  do not have; the stage keeps its strip to a few marks, and a red glyph says
  "on" as the player bar's does.
* **Window buttons follow the desktop.** Square caption buttons the height of
  the bar on Windows; round ones, centred, on Linux, in the order and on the
  side the desktop lists them. With them on the left, the sidebar's brand holds
  them ahead of the name, or the top bar when the sidebar is folded away.
* **The stack, bottom to top:** the page, the queue panel, Now Playing, the
  player bar, the narrow-window sidebar with its dimmed page, then the toast.
  *Why.* Each layer covers the one below only for as long as it is being used,
  and the two that are always true — the player bar, and the app's answer to
  what you just did — are never covered.
* **The video plays where the cover is.**
  *Why.* The video is the same song moving: it belongs in the place the still
  occupies, not in a panel of its own. The picture takes the cover column's
  width at its own 16:9 shape, with the cover's corners and shadow, rather
  than sitting in black bars; the title and the tags close up under it. The
  cover stays up until the first frame arrives, so the place is never a black
  box, and the strip's glyph shows three dots while the picture is being
  fetched. What stays on the picture is picture in picture and full screen,
  once it plays: those act on the picture itself.
* **Video is never fetched until it is asked for.**
  *Why.* It costs many times the bandwidth of audio and most listening does not
  want it. Asking is one click, and the song carries on from the same second.
* **The picture is always in exactly one place: full screen, Now Playing, or a
  small panel above the player bar.** Closing Now Playing moves it to the
  panel, at the right, framed by a 2px ink rule; a click on the panel opens Now
  Playing, and its close button turns the picture off while the song plays on
  as sound. The player bar has the same switch, Video in its "…" (and, while
  the picture is on, a red video glyph beside "…" that turns it off), so the
  picture can be asked for, and put away, from anywhere. In a narrow window the picture plays under the song's
  name, full width and no taller than half of what is left.
  *Why.* A picture nobody can see should not be decoded, and one that vanished
  whenever Now Playing closed would make the switch mean "only in Now
  Playing". So there is no hidden state: wherever the video is on, it is on
  screen, and when nothing can show it (a minimised window) mpv stops decoding
  it until something can.
* **Full screen is ink.** F, or a double-click on the picture, fills the screen;
  the strip along the top and the player bar fade once the pointer has been
  still a moment and come back when it moves. Esc leaves.
  *Why.* It is the one place the page is not paper. A moving picture on paper
  reads as a photograph pasted on, and the margins beside it as bars; on ink
  the margins are the frame, and the picture is the page.
* **A card that opens a page can also be played where it stands.** Under the
  pointer an album's or a playlist's cover takes a play plate in its corner;
  the card still opens the page, the plate plays it. A song's card has no
  plate, because pressing the card already plays it.
  *Why.* "Play this album" is the commonest wish on a shelf, and opening the
  page to press Play is a page load and a second click for it. The mark sits
  on the picture, so it is a plate (paper square, ink glyph) rather than a
  bare glyph, for the reason full screen on the video is (2.6a is about glyphs on
  paper); and it is there only under the pointer, so a shelf at rest is still
  covers and titles.
* **A shelf says what it offers as a whole in its header**, as a section
  does: SHOW ALL where YouTube Music has a page of everything the shelf shows
  a few of, PLAY ALL where the shelf is songs. The same small tracked link,
  at the header's end, beside the paging arrows.
  *Why.* One place and one look for "this whole list", so it is found where
  it was found last time, and it never competes with the page's own red Play.
* **Lyrics sit at the right, the cover at the left.**
  *Why.* Lyrics are read left-to-right and change constantly; they need the
  wider side, clear of the picture, and they need to start at a consistent left
  edge.
* **The sound effects are a glyph on the player bar, red while any is on.**
  Slowed + reverb, Nightcore, 8D audio, High bass, Studio clarity and the
  equaliser are kept from one launch to the next, so the sliders glyph beside
  the output button turns red whenever the sound is being changed, and opens
  them all in a popup built like the volume popup's: paper in a 2px ink frame,
  standing on the bar, no motion. Settings > Playback has the same block
  (EFFECTS, not SOUND, which is the quality above it), with a line of
  explanation under each effect that needs one, and a tag beside Now Playing's
  sound badge names the effects that are on. The glyph stays on the bar at any width; it
  is the one of the right side's glyphs that never folds.
  *Why.* An effect kept from yesterday — a song slowed the next morning — has
  to explain itself where the hand already is, and be one click from undone; a
  window snapped to half the screen still shows the red. The five effects are
  tiles rather than check boxes because each changes the whole sound at a
  stroke: off, a tile is paper in an ink frame; on, it is printed in reverse
  with the red square, as the toast carries it. Two rows of two, and Studio
  clarity across the width under them, so that the fifth does not stand alone
  in half a row as if one beside it were missing. Their strengths are chips, the
  equaliser's bands are ink bars standing on a 0 dB tick, never red — a lifted
  band is a setting, not something that is on — with the curve they make
  together drawn behind them (High bass's shelf and Studio clarity's curve in
  it while they are on), since the bands add up and the curve can stand
  higher than any one of them. The popup keeps one height whatever is on: it
  grows upwards from the bar, so a row of strengths appearing would lift the
  tiles away from under the pointer that had just clicked one.
* **The mini player is the window made small, never a second one beside it.**
  The owner's choice of three (2026-10-08, "B · Card"): 320 by 448, the cover
  in colour across the top — it is the subject, as an album page's cover is —
  then a 4px progress line, the title, the artist and the times, and under them
  like, previous, the red play square, next and the effects glyph. Three ink
  plates on the cover keep it over other windows (the pin, red while it does),
  go back to the full window, and close it. It opens in place of the window,
  from the bar's "…", the tray, Ctrl+Shift+M or at sign-in, and Ctrl+Shift+M takes
  it back; any close, its X or Windows', does what the window's does (the
  tray, or quit). It has its own taskbar button, and the app's answers (a song
  that will not play) come in a toast of its own over the foot of the cover.
  The effects open over the cover, printed in reverse: paper on ink, the
  plates put away beneath them.
  *Why.* Two windows showing the same song would ask which one is the app. A
  window that goes small keeps one answer, and the cover can be the whole top
  of it because, at that size, nothing else competes with it.
* **The sleep timer is started from the bar's "…", and is a red moon beside
  it while it runs.** "…"'s Sleep timer is a plain list (15, 30, 45 minutes,
  1 hour, the end of this song), its row saying the time left once it runs,
  with +10 minutes and Turn off; the moon opens the same list in a popup with
  the time left beside its heading. The tray has the same list, for a night
  with the window closed. When the time is up the music fades over ten seconds
  and pauses, and the volume is put back.
  *Why.* It is set once a night, so it waits in "…"; but a timer running is a
  promise the music will stop, which has to be seen and undone where it was
  made, so for as long as it runs it is on the bar.
* **Importing a playlist is a sheet over the window, and the import goes on
  without it.** It is offered beside the sidebar's plus, on Your Library's
  shelf of playlists, at the rail's foot, and above Search's results when
  what was typed is a playlist's link. Paper in a 2px ink frame over a dimmed
  page, as the sign-in's caution is, it goes from the link (or a list of
  songs, or a CSV file), to what that holds, shown before anything is made
  with the source's own caveats in red type as the caution has its own, to
  the songs being found, live, the one being looked for in red. Closing it
  stops nothing: the import's name and how far it has got stand under the
  sidebar's PLAYLISTS heading (a mark on the rail), on a red rule while it
  asks YouTube Music, and a toast says when it is done, with REVIEW. What it
  was not sure of, and what it could not find, waits on the playlist's own
  page, above its songs.
  *Why.* An import takes minutes, at the pace the owner set so that neither
  the account nor this computer's address draws YouTube's attention; a page
  of its own would keep the user on it for nothing, and a sheet that had to
  stay open would hold the whole window. The playlist's page is where the
  songs arrive, so it is where the ones that did not are settled. Its pause
  is the user's alone: while it rests on a refusal, or waits on the hour's
  searches, it says until when, and offers no way to hurry it.
* **Backup and sync is a section of Settings of its own, after Connections.**
  Two rows: Google Drive, a service row like Last.fm's and YouTube Music's
  (SIGN IN WITH GOOGLE; CANCEL, with the steps open, while the browser has
  it; SYNC NOW and SIGN OUT while signed in; SIGN IN AGAIN when the sign-in
  has ended), and A backup file, with EXPORT… and IMPORT… side by side on one
  shared rule. Signed in, one bold line says who and when: "Signed in as
  name@gmail.com · Synced 2 minutes ago". What stopped a sync goes under it,
  grey while waiting will get past it (no network, a busy Drive: with the time
  it tries again), red only when the user is the one who can end it (a backup
  it will not overwrite, and then what would end it; a full Drive; this
  computer's clock hours off). A build with no Google client, or a computer
  with nowhere safe to keep the sign-in, says so on the row and opens the
  steps instead (no dead button), and the file still works; one that still
  keeps an earlier copy's sign-in offers SIGN OUT, in grey. A browser that
  would not open gets COPY LINK beside the steps, never a link that opens the
  same way. The answers to Export, Import and SYNC NOW are toasts that stay
  six seconds, with OK, since they name what was removed (History's rows
  among it) as well as what was added, and call a playlist that only changed
  changed, not new; the first sync's after signing in stays until OK, since
  it can come while the window is still behind the browser. A sync in the
  background says nothing. The privacy policy is a small link in the row's
  fine print, after how to delete the backup from Drive while there is one.
  *Why.* It is the user's own library, not an account the app uses, so it is
  not among the Connections; and it is useful with no account at all, so the
  file stands beside the Drive row, not under it. A sync is a promise kept in
  the background: at rest the row shows its last success, and it stays quiet
  until something needs a person.
* **Closing keeps the music: the window goes to the tray.** As Spotify does it,
  unless Settings turns it off; the icon by the clock brings it back, and its
  menu has the song, play, next, previous, the window, the mini player and
  Quit. The first time, a note says where it went.

## 4. Type and rules

* Archivo, three weights: regular for body, semibold for emphasis, extrabold for
  headings and titles.
* Headings carry negative tracking (`Theme.tracking(size, -0.02)`), small caps
  labels carry positive tracking (`+0.08` to `+0.18`).
  *Why.* Large type looks loose at default spacing and small capitals look
  cramped; the tracking is calibrated to Archivo's metrics, which is why the
  font is bundled rather than assumed.
* A 2px rule separates regions; a 1px hairline separates rows within a region.
  *Why.* Two weights are enough to say "different thing" and "same thing, next
  one", and the difference is visible at a glance without colour.
* Names are links without looking like links. An artist's name, anywhere it is
  printed, opens that artist's page, and an album's title in a track table its
  album; at rest they are the same type as the line around them, and the one
  under the pointer takes a 1px rule in its own colour, arriving at once and
  fading over `quick`. In a joint credit each name is its own link.
  *Why.* Nearly every row carries a name, so a page of coloured or underlined
  names would be a page of links rather than a list of songs, and the red would
  stop meaning "here". A rule under a word is how print marks a reference; it
  appears only where the pointer asks the question.

## 5. Copy

Sentence case in the interface, capitals only for the small tracking labels
("ALBUMS & SINGLES · JP"). No exclamation marks. Messages say what happened and
what the user can do: "YouTube Music has no Afghanistan — back to India", not
"Error 400".

*Why.* The interface is written, not shouted; and an error that names the cause
and the remedy is the difference between a bug report and a shrug.
