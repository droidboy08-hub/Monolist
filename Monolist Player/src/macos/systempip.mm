#include "systempip.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "videosurface.h"

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QQuickWindow>
#include <QSize>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

namespace {

// The system's window is small: a picture larger than this on its longer side
// is drawn smaller, and the system scales it the rest of the way on the GPU.
constexpr int kLongestSide = 960;
// How long the picture and the system may take to be ready for the window,
// polled this often: the picture may still be on its way from the network.
constexpr int kStartPollMs = 50;
constexpr int kStartTries = 200;

// A cover without the black bars some thumbnails carry above and below the
// picture (YouTube's 4:3 ones of a 16:9 video): rows that are all but black
// are cut, up to a quarter of the height from each side.
QImage withoutBars(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    const auto dark = [&image](int y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        const int step = std::max(1, image.width() / 64);
        for (int x = 0; x < image.width(); x += step) {
            if (qRed(row[x]) + qGreen(row[x]) + qBlue(row[x]) > 3 * 24)
                return false;
        }
        return true;
    };
    const int limit = image.height() / 4;
    int top = 0;
    while (top < limit && dark(top))
        ++top;
    int bottom = 0;
    while (bottom < limit && dark(image.height() - 1 - bottom))
        ++bottom;
    if (top == 0 && bottom == 0)
        return image;
    return image.copy(0, top, image.width(), image.height() - top - bottom);
}

} // namespace

API_AVAILABLE(macos(12.0))
@interface MonolistPipDelegate : NSObject <AVPictureInPictureControllerDelegate,
                                           AVPictureInPictureSampleBufferPlaybackDelegate>
@property (nonatomic, assign) SystemPip::Private *owner;
@end

struct SystemPip::Private
{
    SystemPip *q = nullptr;
    QNetworkAccessManager *network = nullptr;

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
    // The video's frames go to the window while a video plays; otherwise its
    // cover does, with a line saying why (showCover).
    std::atomic<bool> showVideo { false };
    std::atomic<int> videoWidth { -1 };
    std::atomic<int> videoHeight { -1 };
    std::atomic<bool> playing { false };
    // Pictures handed to the layer, video or cover.
    std::atomic<int> enqueued { 0 };
    // For --pip-at: what came in and what went out.
    std::atomic<int> framesAsked { 0 };
    std::atomic<int> framesDrawn { 0 };
    std::atomic<int> coversShown { 0 };
    std::atomic<int> rendersFailed { 0 };
    std::atomic<int> lastBrightness { -1 };
    std::atomic<qint64> renderMicros { 0 };
    // The drawing queue's own.
    CVPixelBufferPoolRef pool = nullptr;
    QSize poolSize;
    CMVideoFormatDescriptionRef format = nullptr;

    // The GUI thread's own.
    QTimer startPoll;
    int startTries = 0;
    // Pictures handed over when start() was called: the window opens only
    // after one more has reached the layer. Asked to open on an empty layer,
    // AVKit lays its window out from a picture of no size, and shows nothing
    // or crashes.
    int enqueuedAtStart = 0;
    // While the system's window is up: the layer kept the picture's size.
    QTimer fitPoll;
    QPointer<QNetworkReply> coverReply;
    QString coverSource;
    QImage cover;

    ~Private()
    {
        if (pool)
            CVPixelBufferPoolRelease(pool);
        if (format)
            CFRelease(format);
    }

    // The size pictures are drawn at: the video's shape, or 16:9 with none,
    // at most kLongestSide on its longer side.
    QSize pictureSize() const
    {
        const int width = videoWidth.load();
        const int height = videoHeight.load();
        if (width <= 0 || height <= 0)
            return QSize(kLongestSide, kLongestSide * 9 / 16);
        const double scale = std::min(1.0, double(kLongestSide) / double(std::max(width, height)));
        return QSize(std::max(2, int(std::lround(width * scale)) & ~1),
                     std::max(2, int(std::lround(height * scale)) & ~1));
    }

    // The system's window mirrors this layer at the layer's own size, so it
    // is made as large as the picture it shows — and kept just outside the
    // app's window, where it is never seen there. At two points square, the
    // system's window showed a dot in its corner.
    void fitLayer()
    {
        const QSize size = pictureSize();
        const CGFloat w = kLongestSide;
        const CGFloat h = std::round(kLongestSide * double(size.height()) / double(size.width()));
        const NSRect wanted = NSMakeRect(-(w + 64), 0, w, h);
        if (NSEqualRects(host.frame, wanted) && CGRectEqualToRect(layer.frame, host.bounds))
            return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        host.frame = wanted;
        layer.frame = host.bounds;
        [CATransaction commit];
    }

    bool ensureObjects();
    // The system window's play and pause.
    void setPlaying(bool wanted)
    {
        if (q->m_player && q->m_player->playing() != wanted)
            q->m_player->togglePlay();
    }
    void frameReady();                           // mpv's thread
    void drawFrame();                            // the queue
    CVPixelBufferRef makeBuffer(const QSize &size);   // the queue
    void enqueue(CVPixelBufferRef buffer);       // the queue; releases it
    void videoStateChanged();
    void trackChanged();
    void fetchCover(const QString &source);
    QString coverCaption() const;
    void showCover();
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

        // The system opens its window from a layer that is in a window, and
        // animates out of it. Sized by fitLayer before the system's window
        // opens, since that window is laid out from it.
        layer = [AVSampleBufferDisplayLayer layer];
        layer.videoGravity = AVLayerVideoGravityResizeAspect;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        host = [[NSView alloc] initWithFrame:NSMakeRect(-1024, 0, 16, 9)];
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
    ++framesAsked;
    if (!running.load() || !showVideo.load() || framePending.exchange(true))
        return;
    dispatch_async(queue, ^{ drawFrame(); });
}

CVPixelBufferRef SystemPip::Private::makeBuffer(const QSize &size)
{
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
            return nullptr;
        }
        poolSize = size;
    }
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) != kCVReturnSuccess)
        return nullptr;
    return buffer;
}

void SystemPip::Private::drawFrame()
{
    framePending.store(false);
    // No video yet (none, or its first frame still on the way): the cover
    // stays, rather than the empty picture mpv would draw.
    if (!running.load() || !showVideo.load() || videoWidth.load() <= 0 || videoHeight.load() <= 0)
        return;

    const QSize size = pictureSize();
    CVPixelBufferRef buffer = makeBuffer(size);
    if (!buffer)
        return;
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    const auto began = std::chrono::steady_clock::now();
    const bool drawn = VideoSurface::renderExternal(base, size, qsizetype(stride));
    renderMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - began).count();
    if (drawn) {
        // mpv leaves the fourth byte of each pixel unset; the system reads it
        // as alpha, so it is made opaque.
        for (int row = 0; row < size.height(); ++row) {
            auto *pixel = reinterpret_cast<uint32_t *>(base + row * stride);
            for (int column = 0; column < size.width(); ++column)
                pixel[column] |= 0xff000000u;
        }
        // A rough brightness of the middle row, 0-255, so a black picture
        // shows up in the self-test's numbers.
        const auto *middle = base + (size.height() / 2) * stride;
        long sum = 0;
        for (int column = 0; column < size.width(); ++column)
            sum += middle[column * 4 + 1];
        lastBrightness.store(int(sum / std::max(1, size.width())));
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    // Drawn after the cover took over: that stays.
    if (!drawn || !showVideo.load()) {
        if (!drawn)
            ++rendersFailed;
        CVPixelBufferRelease(buffer);
        return;
    }
    ++framesDrawn;
    enqueue(buffer);
}

void SystemPip::Private::enqueue(CVPixelBufferRef buffer)
{
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
    ++enqueued;
}

// The video started, stopped, or is on its way: frames or the cover.
void SystemPip::Private::videoStateChanged()
{
    const bool video = q->m_player && q->m_player->videoPlaying();
    showVideo.store(video);
    if (video)
        frameReady();   // a paused video sends no frames: draw the one there is
    else
        showCover();    // and again as its line changes: loading, or none
}

void SystemPip::Private::trackChanged()
{
    fetchCover(q->m_player ? q->m_player->currentTrack().value(QStringLiteral("artwork")).toString()
                           : QString());
    showCover();
}

// As MacMediaSession fetches it: through the artwork cache, so a cover the
// app has already shown is not downloaded again.
void SystemPip::Private::fetchCover(const QString &source)
{
    if (source == coverSource)
        return;
    coverSource = source;
    cover = QImage();
    if (coverReply) {
        coverReply->abort();
        coverReply = nullptr;
    }
    if (source.isEmpty())
        return;

    const QUrl url(source);
    if (!url.isValid() || url.isLocalFile() || url.scheme().isEmpty()) {
        const QImage image(url.isLocalFile() ? url.toLocalFile() : source);
        if (!image.isNull())
            cover = withoutBars(image);
        return;
    }
    if (!network)
        return;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist/0.1"));
    request.setTransferTimeout(10000);
    QNetworkReply *reply = network->get(request);
    coverReply = reply;
    QObject::connect(reply, &QNetworkReply::finished, q, [this, reply, source]() {
        reply->deleteLater();
        if (coverReply == reply)
            coverReply = nullptr;
        // A cover for a song that is no longer playing is not wanted.
        if (reply->error() != QNetworkReply::NoError || source != coverSource)
            return;
        const QImage image = QImage::fromData(reply->readAll());
        if (image.isNull())
            return;
        cover = withoutBars(image);
        showCover();
    });
}

QString SystemPip::Private::coverCaption() const
{
    const PlaybackController *player = q->m_player;
    if (!player || player->currentTrack().isEmpty())
        return QString();
    if (player->videoWanted())
        return QStringLiteral("Loading the video…");
    if (!player->videoAvailable())
        return QStringLiteral("No video for this song");
    return QStringLiteral("This video would not play");
}

// The song's cover, with a line under it, while there is no video to show:
// the window stays up from one song to the next, and the next song's video
// takes over from it as soon as it plays.
void SystemPip::Private::showCover()
{
    if (!running.load() || showVideo.load())
        return;

    const QSize size = pictureSize();
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor(12, 12, 12));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QString caption = coverCaption();
    const int side = int(std::lround(size.height() * (caption.isEmpty() ? 0.8 : 0.64)));
    const int top = caption.isEmpty() ? (size.height() - side) / 2 : int(std::lround(size.height() * 0.09));
    const QRect square((size.width() - side) / 2, top, side, side);
    if (!cover.isNull()) {
        // Filled and cut square: a video's thumbnail is 16:9, with the cover
        // in its middle.
        const QImage fitted = cover.scaled(square.size(), Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation);
        const QRect middle((fitted.width() - side) / 2, (fitted.height() - side) / 2, side, side);
        painter.drawImage(square, fitted, middle);
    } else {
        painter.fillRect(square, QColor(38, 38, 38));
    }
    if (!caption.isEmpty()) {
        QFont font(QStringLiteral("Archivo"));
        font.setPixelSize(std::max(12, size.height() / 16));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 215));
        const int textTop = square.bottom() + int(std::lround(size.height() * 0.05));
        painter.drawText(QRect(0, textTop, size.width(), size.height() - textTop),
                         Qt::AlignHCenter | Qt::AlignTop, caption);
    }
    painter.end();
    ++coversShown;

    dispatch_async(queue, ^{
        if (!running.load() || showVideo.load())
            return;
        CVPixelBufferRef buffer = makeBuffer(image.size());
        if (!buffer)
            return;
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        const size_t row = std::min(stride, size_t(image.bytesPerLine()));
        for (int y = 0; y < image.height(); ++y)
            std::memcpy(base + y * stride, image.constScanLine(y), row);
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        enqueue(buffer);
    });
}

void SystemPip::Private::finish()
{
    startPoll.stop();
    fitPoll.stop();
    if (!running.exchange(false))
        return;
    VideoSurface::detachExternal();
    q->setActive(false);
}

@implementation MonolistPipDelegate

- (void)pictureInPictureControllerDidStartPictureInPicture:(AVPictureInPictureController *)controller
{
    Q_UNUSED(controller)
    qInfo("pip: the system's window is open");
    if (_owner)
        _owner->fitLayer();
}

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
    qWarning("pip: the system's window would not open: %s",
             qPrintable(QString::fromNSString(error.localizedDescription)));
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
    qInfo("pip: render size %dx%d", newRenderSize.width, newRenderSize.height);
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

SystemPip::SystemPip(PlaybackController *player, MpvEngine *engine, QNetworkAccessManager *network,
                     QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_engine(engine)
    , d(std::make_unique<Private>())
{
    d->q = this;
    d->network = network;
    d->queue = dispatch_queue_create("monolist.pip.frames", DISPATCH_QUEUE_SERIAL);
    d->startPoll.setInterval(kStartPollMs);
    d->fitPoll.setInterval(200);
    connect(&d->fitPoll, &QTimer::timeout, this, [this]() { d->fitLayer(); });

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
        connect(m_player, &PlaybackController::videoChanged, this, [this]() {
            if (d->running.load())
                d->videoStateChanged();
        });
        connect(m_player, &PlaybackController::currentTrackChanged, this, [this]() {
            if (d->running.load())
                d->trackChanged();
        });
    }

    // Open the system's window once it says it can: the layer needs a picture
    // and a moment in a window first.
    connect(&d->startPoll, &QTimer::timeout, this, [this]() {
        if (@available(macOS 12.0, *)) {
            auto *controller = (AVPictureInPictureController *)d->controller;
            const bool pictured = d->enqueued.load() > d->enqueuedAtStart;
            if (pictured && controller.isPictureInPicturePossible) {
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
    d->enqueuedAtStart = d->enqueued.load();
    d->showVideo.store(m_player && m_player->videoPlaying());
    d->fitLayer();
    d->running.store(true);
    VideoSurface::attachExternal([p]() { p->frameReady(); });
    d->fetchCover(m_player ? m_player->currentTrack().value(QStringLiteral("artwork")).toString()
                           : QString());
    d->videoStateChanged();
    setActive(true);
    d->startTries = 0;
    d->startPoll.start();
    d->fitPoll.start();
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

static void describeViews(NSView *view, int depth, QStringList &out)
{
    if (depth > 8 || out.size() > 40)
        return;
    const NSRect frame = view.frame;
    QString line = QString(depth * 2, u' ')
                   + QString::fromNSString(NSStringFromClass([view class]))
                   + QStringLiteral(" %1,%2 %3x%4").arg(frame.origin.x).arg(frame.origin.y)
                         .arg(frame.size.width).arg(frame.size.height);
    for (CALayer *sub in view.layer.sublayers) {
        line += QStringLiteral(" [%1 %2x%3]").arg(QString::fromNSString(NSStringFromClass([sub class])))
                    .arg(sub.frame.size.width).arg(sub.frame.size.height);
    }
    out << line;
    for (NSView *child in view.subviews)
        describeViews(child, depth + 1, out);
}

QString SystemPip::windowTree() const
{
    QStringList out;
    for (NSWindow *window in NSApp.windows) {
        out << QStringLiteral("window %1 %2x%3 visible %4")
                   .arg(QString::fromNSString(NSStringFromClass([window class])))
                   .arg(window.frame.size.width).arg(window.frame.size.height).arg(window.isVisible);
        if (![NSStringFromClass([window class]) hasPrefix:@"QNS"] && window.contentView)
            describeViews(window.contentView, 1, out);
    }
    return out.join(u'\n');
}

QString SystemPip::diagnostics() const
{
    QString state = QStringLiteral("none");
    QString layerState = QStringLiteral("none");
    if (@available(macOS 12.0, *)) {
        if (auto *controller = (AVPictureInPictureController *)d->controller) {
            state = QStringLiteral("possible=%1 active=%2")
                        .arg(controller.isPictureInPicturePossible ? 1 : 0)
                        .arg(controller.isPictureInPictureActive ? 1 : 0);
        }
    }
    if (d->layer) {
        if (@available(macOS 14.0, *)) {
            AVSampleBufferVideoRenderer *renderer = d->layer.sampleBufferRenderer;
            layerState = QStringLiteral("status=%1 error=%2")
                             .arg(int(renderer.status))
                             .arg(QString::fromNSString(renderer.error.localizedDescription ?: @"-"));
        }
        const CGRect frame = d->layer.frame;
        layerState += QStringLiteral(" frame %1x%2").arg(frame.size.width).arg(frame.size.height);
    }
    const int drawnCount = d->framesDrawn.load();
    return QStringLiteral("%1, video %2x%3, frames asked %4 drawn %5 failed %6, covers %7, brightness %8, "
                          "%9, layer %10, render %11 ms each")
        .arg(d->showVideo.load() ? QStringLiteral("showing video") : QStringLiteral("showing cover"))
        .arg(d->videoWidth.load()).arg(d->videoHeight.load())
        .arg(d->framesAsked.load()).arg(drawnCount).arg(d->rendersFailed.load())
        .arg(d->coversShown.load()).arg(d->lastBrightness.load())
        .arg(state, layerState)
        .arg(drawnCount > 0 ? double(d->renderMicros.load()) / drawnCount / 1000.0 : 0.0, 0, 'f', 1);
}

void SystemPip::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
}
