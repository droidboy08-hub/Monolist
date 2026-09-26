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

**One accent, used sparingly.** Red marks what is *active* (the song playing,
the open page, the line being sung) and what is *primary* (Play, and nothing
else on the same screen).

*Why.* An accent that appears everywhere stops meaning anything. Because red
means "here", it can be small — a 10px square, a heart, a 2px rule — and still
be found instantly.

**Photographs are black and white, except where a page is about one.** Covers in
lists, shelves and cards print grey; the cover of an album page, a playlist page
or Now Playing is in colour, and a card's cover turns to colour under the
pointer.

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
| `Theme.slow` | 520 ms | Long distances and atmosphere: the lyrics scrolling, the poster's colour field |

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
* **Now Playing keeps the player bar visible, and slides out from behind it.**
  *Why.* Opening the big view should not take away the controls the user already
  knows; it adds the cover, the lyrics and the queue, and changes nothing else.
  Because the view is that bar enlarged, it emerges from behind the bar and
  tucks back behind it when closed — the bar is drawn on top throughout. Sliding
  *over* the bar and off the bottom would read as a page leaving, which is the
  wrong story: nothing left, the player is simply small again.
* **Now Playing closes beside the window buttons, on whichever side they are.**
  A **⌄ CLOSE** plate, with its key (Esc) printed on it, sits next to minimise:
  on Windows at the right, the full height of the bar like the caption buttons
  it joins; on Linux wherever the desktop's own button layout puts close
  (GNOME's `button-layout`, KDE's `kwinrc`), left or right; on macOS after the
  traffic lights. The player bar's Now Playing button turns to the same
  chevron, in red, while the view is open.
  *Why.* The close used to be a small chevron at the top left while Windows
  keeps its buttons at the top right, so putting the player away meant
  crossing the whole window. The corner with the window buttons is where the
  hand already goes to deal with the window. It is a plate with a word, split
  off by a rule, so it is not mistaken for one of the window's own buttons —
  the red X is one button further on.
* **The picture switch is COVER | VIDEO, above the picture it changes.** In the
  strip at the poster's right edge, printed in the poster's ink. Both sides are
  always there, so it never moves: a song without a video dims VIDEO and says
  why under the pointer, and while a video loads VIDEO is already chosen and
  shows dots, with the cover up until the first frame. `V` does the same. A
  video takes the poster's full width, where a cover keeps its square.
  *Why.* The old switch was a bare 40px glyph on the cover's corner; a glyph
  that changes between "video" and "image" reads as a status, not a choice.
  Two labelled sides say what there is to choose and which one is on.
* **Window buttons follow the desktop.** Square caption buttons the height of
  the bar on Windows; round ones, centred, on Linux, in the order and on the
  side the desktop lists them. With them on the left, the sidebar's brand holds
  them ahead of the name, or the top bar when the sidebar is folded away.
* **The stack, bottom to top:** the page, the queue panel, Now Playing, the
  player bar, the narrow-window sidebar with its dimmed page, then the toast.
  *Why.* Each layer covers the one below only for as long as it is being used,
  and the two that are always true — the player bar, and the app's answer to
  what you just did — are never covered.
* **The video plays where the cover is, and the switch is right above it.**
  *Why.* The video is the same song moving: it belongs in the place the still
  occupies, not in a panel of its own, and the control that swaps them belongs
  next to the thing it swaps (COVER | VIDEO, in the strip; see above). The
  picture takes the poster's width at its own 16:9 shape rather than sitting
  in black bars — bars would be the only black in a paper interface. Its
  bottom edge stays put, so the title below it does not move. The cover stays
  up until the first frame arrives, so the panel is never a black box, and the
  switch shows three dots while the picture is being fetched. Songs with no
  video (YouTube Music's own audio tracks are a still image) show the switch
  greyed rather than hidden, so the feature is findable. What stays on the
  picture is full screen, once it plays: that one acts on the picture itself.
* **Video is never fetched until it is asked for.**
  *Why.* It costs many times the bandwidth of audio and most listening does not
  want it. Asking is one click, and the song carries on from the same second.
* **The picture is always in exactly one place: full screen, Now Playing, or a
  small panel above the player bar.** Closing Now Playing moves it to the
  panel, at the right, framed by a 2px ink rule; a click on the panel opens Now
  Playing, and its close button turns the picture off while the song plays on
  as sound. The player bar has the same switch, so the picture can be asked for
  from anywhere. In a narrow window the switch ends the song's line and the
  picture plays under it, full width and no taller than half the page.
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
  *Why.* Lyrics are read left-to-right and change constantly; they need the side
  that is not interrupted by the cover's colour field, and they need to start at
  a consistent left edge.

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
