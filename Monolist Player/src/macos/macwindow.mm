#include "macwindow.h"

#include <QWindow>

#import <AppKit/AppKit.h>

namespace {

// The window's NSWindow, or nil while it has no native window. winId() is its
// content view; handle() first, because winId() would create one.
NSWindow *nativeWindow(QWindow *window)
{
    if (!window || !window->handle())
        return nil;
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    return view.window;
}

} // namespace

namespace MacWindow {

void hideTitle(QWindow *window)
{
    if (NSWindow *native = nativeWindow(window))
        native.titleVisibility = NSWindowTitleHidden;
}

void titleBarDoubleClicked(QWindow *window)
{
    NSWindow *native = nativeWindow(window);
    if (!native)
        return;

    // "Maximize" (the default, which is zoom), "Minimize" or "None"; newer
    // versions of macOS add "Fill", which zooming is the nearest thing to.
    NSString *action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"None"])
        return;
    if ([action isEqualToString:@"Minimize"])
        [native performMiniaturize:nil];
    else
        [native performZoom:nil];
}

} // namespace MacWindow
