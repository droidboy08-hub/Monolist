#pragma once

class QQuickWindow;

// --scroll-test: how scrolling the page on screen behaves, in numbers rather
// than by feel. Open a page with --view, and once it has loaded this scrolls
// it the ways a person does — one notch of the wheel, a quick spin of it, a
// hard fling, the wheel all the way to the end, and a notch over a sideways
// shelf — and reports for each how far the page went, how long it took to get
// there, and how evenly the frames came: the time between frames on the
// render thread, and the longest the interface's own thread was held up. The
// settings that decide it (deceleration, top speed, the system's lines per
// notch) and what the page is made of are printed first. Quits when done.
void startScrollSelfTest(QQuickWindow *window);
