import QtQuick
import Monolist

// How the wheel scrolls a page or a list: declared inside the Flickable or
// ListView it scrolls (ScrollPage, the queue, the sidebar's playlists).
//
// Flickable's own wheel handling turns every wheel event into a new fling,
// with a floor of a quarter of its top speed whatever the size of the event,
// so how far a page went had little to do with how far the wheel turned: one
// notch went 144 px, five quick ones 424 (85 each), and a touchpad swipe worth
// eight notches, which arrives as a stream of small events, 161 — the page
// never went faster than about 600 px a second, however fast the hand. It
// also ignored the system's lines per notch. That is what felt slow
// (--scroll-test measures all of it).
//
// Here a page goes as far as the wheel turned. A notch is the system's lines
// per notch at 100/3 px a line, which is how far a browser scrolls a line on
// the same system — 100 px at Windows' default of three lines — and the page
// eases there over `Theme.quick`; notches that come while it is still moving
// add to where it is going, so a quick spin goes further, not less far. A
// delta finer than a notch (a touchpad, a free-spinning wheel) and a delta in
// pixels (a Mac's trackpad, which brings its own momentum) are followed at
// once, as the hand moves.
WheelHandler {
    id: wheel

    required property Flickable flickable
    // How far one line of wheel travel scrolls, in pixels.
    property real line: 100 / 3
    // The wheel is moving the page: from its first event until the page has
    // been still for a moment (ScrollPage keeps hover still meanwhile).
    readonly property bool scrolling: glide.running || still.running

    // Where a run of notches is taking the page, where the glide set out
    // from and how far into it it is, and the position last set: a move made
    // by anything else (a drag, the scroll bar, a new page starting at the
    // top) is noticed, and wins.
    property real destination: 0
    property real setOut: 0
    property real elapsed: 0
    property real placed: 0

    function top() {
        return flickable.originY - flickable.topMargin
    }
    function bottom() {
        return Math.max(top(), flickable.originY + flickable.contentHeight + flickable.bottomMargin
                               - flickable.height)
    }
    function bounded(y) {
        return Math.max(top(), Math.min(bottom(), y))
    }
    function place(y) {
        placed = bounded(y)
        flickable.contentY = placed
    }

    target: null

    onWheel: function(event) {
        if (!flickable.interactive)
            return
        still.restart()
        if (event.pixelDelta.y !== 0) {
            glide.stop()
            place(flickable.contentY - event.pixelDelta.y)
            return
        }
        const eighths = event.angleDelta.y
        if (eighths === 0)
            return
        const distance = -eighths / 120 * Application.styleHints.wheelScrollLines * line
        if (eighths % 120 !== 0) {
            glide.stop()
            place(flickable.contentY + distance)
            return
        }
        destination = bounded((glide.running ? destination : flickable.contentY) + distance)
        setOut = flickable.contentY
        elapsed = 0
        placed = flickable.contentY
        glide.start()
    }

    // Long enough to bridge the gaps in a stream of touchpad events.
    readonly property Timer still: Timer {
        interval: 150
    }

    // Eased as anything arriving is: fast at first, settling into place.
    readonly property FrameAnimation glide: FrameAnimation {
        onTriggered: {
            if (Math.abs(wheel.flickable.contentY - wheel.placed) > 0.5) {
                stop()
                return
            }
            wheel.elapsed += frameTime * 1000
            const t = Math.min(1, wheel.elapsed / Theme.quick)
            const eased = 1 - Math.pow(1 - t, 3)
            wheel.place(wheel.setOut + (wheel.destination - wheel.setOut) * eased)
            if (t >= 1)
                stop()
        }
    }
}
