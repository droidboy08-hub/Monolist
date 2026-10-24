#include "macwindow.h"

#include <QGuiApplication>
#include <QMenu>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

// Puts one window's traffic lights where the design wants them, and back
// there whenever AppKit moves them. Kept on the NSWindow (an associated
// object), so it lives and goes with the native window.
@interface MonolistTrafficLights : NSObject
@property (nonatomic, weak) NSWindow *window;
@property (nonatomic) CGFloat left;
@property (nonatomic) CGFloat height;
@property (nonatomic) BOOL placing;
- (void)place;
@end

@implementation MonolistTrafficLights

- (void)place
{
    NSWindow *window = self.window;
    if (!window || self.placing || (window.styleMask & NSWindowStyleMaskFullScreen))
        return;
    NSButton *close = [window standardWindowButton:NSWindowCloseButton];
    NSButton *minimise = [window standardWindowButton:NSWindowMiniaturizeButton];
    NSButton *zoom = [window standardWindowButton:NSWindowZoomButton];
    // The buttons' own view, then the title bar's container, which clips
    // them to the system title bar's height unless it is made the strip's.
    NSView *container = close.superview.superview;
    if (!close || !minimise || !zoom || !container)
        return;
    self.placing = YES;

    const CGFloat buttonHeight = NSHeight(close.frame);
    const CGFloat step = NSMinX(minimise.frame) - NSMinX(close.frame);
    NSRect frame = container.frame;
    if (NSHeight(frame) != self.height || NSMaxY(frame) != NSHeight(container.superview.bounds)) {
        frame.size.height = self.height;
        frame.origin.y = NSHeight(container.superview.bounds) - self.height;
        container.frame = frame;
    }
    const CGFloat y = std::round((self.height - buttonHeight) / 2.0);
    NSArray<NSButton *> *buttons = @[ close, minimise, zoom ];
    for (NSUInteger i = 0; i < buttons.count; ++i) {
        // The button's own superview may not fill the container: placed in
        // the container's coordinates, then converted.
        const NSPoint inContainer = NSMakePoint(self.left + i * step, NSHeight(frame) - y - buttonHeight);
        const NSPoint origin = [buttons[i].superview convertPoint:inContainer fromView:container];
        if (!NSEqualPoints(buttons[i].frame.origin, origin))
            [buttons[i] setFrameOrigin:origin];
    }
    self.placing = NO;
}

- (void)somethingMoved:(NSNotification *)note
{
    (void)note;
    // After AppKit's own layout pass, which is what moved them.
    dispatch_async(dispatch_get_main_queue(), ^{ [self place]; });
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

@end

namespace {

// AppKit's windows only on Qt's Cocoa platform: elsewhere (the offscreen
// one, which self-tests run on) a window's id is not an NSView.
bool onCocoa()
{
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

// The window's NSWindow, or nil while it has no native window. winId() is its
// content view; handle() first, because winId() would create one.
NSWindow *nativeWindow(QWindow *window)
{
    if (!window || !window->handle() || !onCocoa())
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

void placeTrafficLights(QWindow *window, int left, int height)
{
    NSWindow *native = nativeWindow(window);
    if (!native)
        return;
    static char key;
    MonolistTrafficLights *lights = objc_getAssociatedObject(native, &key);
    if (!lights) {
        lights = [MonolistTrafficLights new];
        lights.window = native;
        objc_setAssociatedObject(native, &key, lights, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

        NSNotificationCenter *centre = NSNotificationCenter.defaultCenter;
        for (NSNotificationName name : { NSWindowDidResizeNotification, NSWindowDidExitFullScreenNotification,
                                         NSWindowDidBecomeKeyNotification, NSWindowDidResignKeyNotification,
                                         NSWindowDidChangeBackingPropertiesNotification }) {
            [centre addObserver:lights selector:@selector(somethingMoved:) name:name object:native];
        }
        // The title bar laid out again for a reason of its own.
        NSView *container = [native standardWindowButton:NSWindowCloseButton].superview.superview;
        if (container) {
            container.postsFrameChangedNotifications = YES;
            [centre addObserver:lights selector:@selector(somethingMoved:)
                           name:NSViewFrameDidChangeNotification object:container];
        }
    }
    lights.left = left;
    lights.height = height;
    [lights place];
}

QRect trafficLights(QWindow *window)
{
    NSWindow *native = nativeWindow(window);
    if (!native)
        return {};
    NSRect all = NSZeroRect;
    for (NSWindowButton kind : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton }) {
        NSButton *button = [native standardWindowButton:kind];
        if (!button || button.hidden)
            continue;
        const NSRect inWindow = [button convertRect:button.bounds toView:nil];
        all = NSIsEmptyRect(all) ? inWindow : NSUnionRect(all, inWindow);
    }
    if (NSIsEmptyRect(all))
        return {};
    // AppKit counts up from the bottom of the window's frame; Qt down from
    // the top of its content, which here is the whole frame.
    const CGFloat height = native.contentView.frame.size.height;
    return QRect(qRound(all.origin.x), qRound(height - NSMaxY(all)), qRound(all.size.width),
                 qRound(all.size.height));
}

void setDockIcon(const QImage &image)
{
    if (!onCocoa())
        return;
    if (image.isNull()) {
        NSApp.applicationIconImage = nil;
        return;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nullptr pixelsWide:rgba.width() pixelsHigh:rgba.height()
                   bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:rgba.width() * 4 bitsPerPixel:32];
    for (int y = 0; y < rgba.height(); ++y)
        memcpy(rep.bitmapData + y * rep.bytesPerRow, rgba.constScanLine(y), size_t(rgba.width()) * 4);
    // Drawn at half its pixels: a 512 px picture is the Dock's largest
    // icon, 256 points, on a Retina screen.
    NSImage *picture = [[NSImage alloc] initWithSize:NSMakeSize(rgba.width() / 2.0, rgba.height() / 2.0)];
    [picture addRepresentation:rep];
    NSApp.applicationIconImage = picture;
}

void zoom(QWindow *window)
{
    if (NSWindow *native = nativeWindow(window))
        [native performZoom:nil];
}

void useAsWindowsMenu(QMenu *menu)
{
    if (menu && onCocoa())
        NSApp.windowsMenu = menu->toNSMenu();
}

void bringAllToFront()
{
    if (onCocoa())
        [NSApp arrangeInFront:nil];
}

} // namespace MacWindow
