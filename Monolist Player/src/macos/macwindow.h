#pragma once

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

} // namespace MacWindow
