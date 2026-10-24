#pragma once

#include <QImage>
#include <QRect>

class QMenu;
class QWindow;

// The AppKit side of WindowChrome on macOS. Plain C++ here, so that
// windowchrome.cpp needs no Objective-C; the work is in macwindow.mm.
namespace MacWindow {

// With the content extended under a transparent title bar, AppKit still
// centres the window's title over it — right where the top bar keeps its
// breadcrumb and search field. This hides the text. The title itself stays,
// for the Window menu, Mission Control and the Dock.
//
// Belongs to the native window, which Qt makes again after the window has
// been closed and shown, so it has to be applied again each time.
void hideTitle(QWindow *window);

// A double click on the title bar, doing what System Settings › Desktop & Dock
// says it should: zoom (the default), minimise, or nothing at all.
void titleBarDoubleClicked(QWindow *window);

// The traffic lights moved into the design's top strip: the close button's
// left edge `left` points from the window's edge, the three centred in the
// strip's `height`. AppKit lays them out again (a resize, the window becoming
// key, leaving full screen) and they are put back each time; in full screen,
// where the title bar is AppKit's own, they are left to it. Applied to the
// native window, so again whenever Qt makes a new one.
void placeTrafficLights(QWindow *window, int left, int height);

// The green button's zoom, for the Window menu: the window to its best size
// and back, as AppKit decides it.
void zoom(QWindow *window);

// The menu AppKit keeps as the app's Window menu, listing the open windows
// and adding Bring All to Front.
void useAsWindowsMenu(QMenu *menu);

// The app's windows in front of every other app's, as the Window menu's
// Bring All to Front does.
void bringAllToFront();

// The Dock's picture of the app while it runs; a null image gives it back
// the bundle's own.
void setDockIcon(const QImage &image);

// Where the traffic lights are, in the window's own coordinates (points from
// its top left), all three together; empty while it has no native window.
// Qt's picture of the window (--shot) has the design under them but not
// them, as the system draws them over it.
QRect trafficLights(QWindow *window);

} // namespace MacWindow
