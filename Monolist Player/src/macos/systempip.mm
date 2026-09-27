#include "systempip.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "videosurface.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSize>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

namespace {

// The system's window is small: a picture larger than this on its longer side
// is drawn smaller, and the system scales it the rest of the way on the GPU.
constexpr int kLongestSide = 960;
// How long the system may take to say the window can open, polled this often.
constexpr int kStartPollMs = 50;
constexpr int kStartTries = 60;

} // namespace

API_AVAILABLE(macos(12.0))
@interface MonolistPipDelegate : NSObject <AVPictureInPictureControllerDelegate,
                                           AVPictureInPictureSampleBufferPlaybackDelegate>
@property (nonatomic, assign) SystemPip::Private *owner;
@end

struct SystemPip::Private
{
    SystemPip *q = nullptr;

    // Made on the first start and kept: a layer in the window for the system
    // to take the picture from, and the controller that opens its window.
    NSView *host = nil;
    AVSampleBufferDisplayLayer *layer = nil;
    id controller = nil;   // AVPictureInPictureController
    id delegate = nil;     // MonolistPipDelegate

    // Frames are drawn here, one at a time, never on the GUI thread.
    dispatch_queue_t queue = nil;
    std::atomic<bool> running { false };
    std::atomic<bool> framePending { false };
    std::atomic<int> videoWidth { 0 };
    std::atomic<int> videoHeight { 0 };
    std::atomic<bool> playing { false };
    // The drawing queue's own.
    CVPixelBufferPoolRef pool = nullptr;
    QSize poolSize;
    CMVideoFormatDescriptionRef format = nullptr;

    QTimer startPoll;
    int startTries = 0;

    ~Private()
    {
        if (pool)
            CVPixelBufferPoolRelease(pool);
        if (format)
            CFRelease(format);
    }

    bool ensureObjects();
    // The system window's play and pause.
    void setPlaying(bool wanted)
    {
        if (q->m_player && q->m_player->playing() != wanted)
            q->m_player->togglePlay();
    }
    void frameReady();
    void drawFrame();
    // The picture back to the app, the system's window gone or going.
    void finish();
};

bool SystemPip::Private::ensureObjects()
{
    if (controller)
        return true;
    if (@available(macOS 12.0, *)) {
        QQuickWindow *window = nullptr;
        for (QWindow *candidate : QGuiApplication::topLevelWindows()) {
            if (auto *quick = qobject_cast<QQuickWindow *>(candidate); quick && quick->isVisible()) {
                window = quick;
                break;
            }
        }
        if (!window)
            return false;
        NSView *view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
        if (!view)
            return false;

        // The system opens its window from a layer that is on screen, and
        // animates out of it: two points in the window's corner, under the
        // title bar, where nothing is seen of it.
        layer = [AVSampleBufferDisplayLayer layer];
        layer.videoGravity = AVLayerVideoGravityResizeAspect;
        host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 2, 2)];
        host.wantsLayer = YES;
        layer.frame = host.bounds;
        [host.layer addSublayer:layer];
        [view addSubview:host];

        MonolistPipDelegate *owner = [MonolistPipDelegate new];
        owner.owner = this;
        delegate = owner;
        auto *source = [[AVPictureInPictureControllerContentSource alloc]
            initWithSampleBufferDisplayLayer:layer playbackDelegate:owner];
        AVPictureInPictureController *made = [[AVPictureInPictureController alloc]
            initWithContentSource:source];
        made.delegate = owner;
        controller = made;
        return true;
    }
    return false;
}

// On mpv's thread: a frame is ready. Drawn on the queue, at most one waiting.
void SystemPip::Private::frameReady()
{
    if (!running.load() || framePending.exchange(true))
        return;
    dispatch_async(queue, ^{ drawFrame(); });
}

void SystemPip::Private::drawFrame()
{
    framePending.store(false);
    if (!running.load())
        return;
    const int width = videoWidth.load();
    const int height = videoHeight.load();
    if (width <= 0 || height <= 0)
        return;

    const double scale = std::min(1.0, double(kLongestSide) / double(std::max(width, height)));
    const QSize size(std::max(2, int(std::lround(width * scale)) & ~1),
                     std::max(2, int(std::lround(height * scale)) & ~1));
    if (!pool || poolSize != size) {
        if (pool)
            CVPixelBufferPoolRelease(pool);
        pool = nullptr;
        NSDictionary *attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey: @(size.width()),
            (id)kCVPixelBufferHeightKey: @(size.height()),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        };
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool)
            != kCVReturnSuccess) {
            pool = nullptr;
            return;
        }
        poolSize = size;
    }

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) != kCVReturnSuccess)
        return;
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    const bool drawn = VideoSurface::renderExternal(base, size, qsizetype(stride));
    if (drawn) {
        // mpv leaves the fourth byte of each pixel unset; the system reads it
        // as alpha, so it is made opaque.
        for (int row = 0; row < size.height(); ++row) {
            auto *pixel = reinterpret_cast<uint32_t *>(base + row * stride);
            for (int column = 0; column < size.width(); ++column)
                pixel[column] |= 0xff000000u;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    if (!drawn) {
        CVPixelBufferRelease(buffer);
        return;
    }

    if (!format || !CMVideoFormatDescriptionMatchesImageBuffer(format, buffer)) {
        if (format)
            CFRelease(format);
        format = nullptr;
        if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer, &format) != noErr) {
            format = nullptr;
            CVPixelBufferRelease(buffer);
            return;
        }
    }

    // Shown the moment it arrives: the timing is mpv's, which has already
    // kept the picture in step with the sound.
    CMSampleTimingInfo timing = { kCMTimeInvalid, CMClockGetTime(CMClockGetHostTimeClock()), kCMTimeInvalid };
    CMSampleBufferRef sample = nullptr;
    const OSStatus made = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, buffer, format,
                                                                   &timing, &sample);
    CVPixelBufferRelease(buffer);
    if (made != noErr || !sample)
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto *first = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(first, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    }
    if (@available(macOS 14.0, *)) {
        AVSampleBufferVideoRenderer *renderer = layer.sampleBufferRenderer;
        if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed)
            [renderer flush];
        [renderer enqueueSampleBuffer:sample];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (layer.status == AVQueuedSampleBufferRenderingStatusFailed)
            [layer flush];
        [layer enqueueSampleBuffer:sample];
#pragma clang diagnostic pop
    }
    CFRelease(sample);
}

void SystemPip::Private::finish()
{
    startPoll.stop();
    if (!running.exchange(false))
        return;
    VideoSurface::detachExternal();
    q->setActive(false);
}

@implementation MonolistPipDelegate

- (void)pictureInPictureController:(AVPictureInPictureController *)controller
    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler:(void (^)(BOOL))completionHandler
{
    Q_UNUSED(controller)
    if (_owner)
        Q_EMIT _owner->q->restoreRequested();
    completionHandler(YES);
}

- (void)pictureInPictureControllerDidStopPictureInPicture:(AVPictureInPictureController *)controller
{
    Q_UNUSED(controller)
    if (!_owner || !_owner->running.load())
        return;   // stop() already took the picture back
    _owner->finish();
    Q_EMIT _owner->q->stopped();
}

- (void)pictureInPictureController:(AVPictureInPictureController *)controller
    failedToStartPictureInPictureWithError:(NSError *)error
{
    Q_UNUSED(controller)
    if (!_owner)
        return;
    _owner->finish();
    Q_EMIT _owner->q->failed(QString::fromNSString(error.localizedDescription));
}

// — its play and pause —

- (void)pictureInPictureController:(AVPictureInPictureController *)controller setPlaying:(BOOL)playing
{
    Q_UNUSED(controller)
    if (_owner)
        _owner->setPlaying(playing);
}

// No scrubber: the song's place is kept by the app, and a live range shows
// play and pause alone.
- (CMTimeRange)pictureInPictureControllerTimeRangeForPlayback:(AVPictureInPictureController *)controller
{
    Q_UNUSED(controller)
    return CMTimeRangeMake(kCMTimeNegativeInfinity, kCMTimePositiveInfinity);
}

- (BOOL)pictureInPictureControllerIsPlaybackPaused:(AVPictureInPictureController *)controller
{
    Q_UNUSED(controller)
    return _owner ? !_owner->playing.load() : YES;
}

- (void)pictureInPictureController:(AVPictureInPictureController *)controller
         didTransitionToRenderSize:(CMVideoDimensions)newRenderSize
{
    Q_UNUSED(controller)
    Q_UNUSED(newRenderSize)
}

- (void)pictureInPictureController:(AVPictureInPictureController *)controller
                    skipByInterval:(CMTime)skipInterval
                 completionHandler:(void (^)(void))completionHandler
{
    Q_UNUSED(controller)
    Q_UNUSED(skipInterval)
    completionHandler();
}

@end

SystemPip::SystemPip(PlaybackController *player, MpvEngine *engine, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_engine(engine)
    , d(std::make_unique<Private>())
{
    d->q = this;
    d->queue = dispatch_queue_create("monolist.pip.frames", DISPATCH_QUEUE_SERIAL);
    d->startPoll.setInterval(kStartPollMs);

    if (m_engine) {
        const QSize size = m_engine->videoSize();
        d->videoWidth.store(size.width());
        d->videoHeight.store(size.height());
        connect(m_engine, &MpvEngine::videoSizeChanged, this, [this](const QSize &size) {
            d->videoWidth.store(size.width());
            d->videoHeight.store(size.height());
        });
    }
    if (m_player) {
        d->playing.store(m_player->playing());
        connect(m_player, &PlaybackController::playingChanged, this, [this]() {
            d->playing.store(m_player->playing());
            if (@available(macOS 12.0, *)) {
                if (d->controller)
                    [(AVPictureInPictureController *)d->controller invalidatePlaybackState];
            }
        });
    }

    // Open the system's window once it says it can: the layer needs a frame
    // and a moment on screen first.
    connect(&d->startPoll, &QTimer::timeout, this, [this]() {
        if (@available(macOS 12.0, *)) {
            auto *controller = (AVPictureInPictureController *)d->controller;
            if (controller.isPictureInPicturePossible) {
                d->startPoll.stop();
                [controller startPictureInPicture];
                return;
            }
        }
        if (++d->startTries >= kStartTries) {
            d->finish();
            Q_EMIT failed(QStringLiteral("Picture in picture could not start"));
        }
    });
}

SystemPip::~SystemPip()
{
    d->finish();
    if (d->host)
        [d->host removeFromSuperview];
}

bool SystemPip::supported() const
{
    if (@available(macOS 12.0, *))
        return [AVPictureInPictureController isPictureInPictureSupported];
    return false;
}

void SystemPip::start()
{
    if (m_active)
        return;
    if (!supported() || !d->ensureObjects()) {
        Q_EMIT failed(QStringLiteral("Picture in picture is not available"));
        return;
    }
    Private *p = d.get();
    d->running.store(true);
    VideoSurface::attachExternal([p]() { p->frameReady(); });
    // A paused song sends no frames: draw the one there is.
    d->frameReady();
    setActive(true);
    d->startTries = 0;
    d->startPoll.start();
}

void SystemPip::stop()
{
    if (!m_active)
        return;
    if (@available(macOS 12.0, *)) {
        auto *controller = (AVPictureInPictureController *)d->controller;
        // Taken back first, so the delegate's stop finds nothing to do.
        d->finish();
        if (controller.isPictureInPictureActive)
            [controller stopPictureInPicture];
        return;
    }
    d->finish();
}

void SystemPip::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
}
