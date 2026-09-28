#include "videosurface.h"
#include "mpvengine.h"

#include <QMutex>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QTimer>

// Built without libmpv (MONOLIST_NO_MPV) there is no player to draw from:
// the engine is never valid, so attach() makes no render context and every
// call into mpv below is never reached. The surfaces still exist, since the
// views are the same, and draw nothing.
#ifndef MONOLIST_NO_MPV
#include <mpv/client.h>
#include <mpv/render.h>
#else
struct mpv_render_context;
#endif

#include <utility>

namespace {

// The player, its one render context, the surface whose turn it is, and every
// surface there is, for handing over to. All of it belongs to the GUI thread.
MpvEngine *g_engine = nullptr;
mpv_render_context *g_render = nullptr;
VideoSurface *g_holder = nullptr;
QList<VideoSurface *> g_surfaces;

// With nothing showing the picture for this long, it stops being decoded. Not
// at once: one surface handing over to the next — Now Playing closing as the
// mini panel opens — can pass through a moment with neither.
constexpr int kUnwatchedAfterMs = 400;
QTimer *g_unwatched = nullptr;

// Rendering from the one context, by a surface on Qt's render thread or by
// the external holder on its own, one at a time.
QMutex g_renderLock;
// The external holder (attachExternal), read on mpv's thread as frames come.
QMutex g_externalLock;
std::function<void()> g_external;
bool g_externalHeld = false;   // GUI thread's own copy, for handing over

// Made once, on the first attach, and kept (see the class comment).
bool ensureRender(void (*onFrame)(void *))
{
#ifdef MONOLIST_NO_MPV
    Q_UNUSED(onFrame)
    return false;
#else
    if (g_render)
        return true;
    if (!g_engine || !g_engine->isValid())
        return false;
    int advanced = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW) },
        // Frames arrive as they are decoded rather than on a display clock,
        // which is what a Qt Quick item wants: it draws when told.
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&g_render, g_engine->handle(), params) < 0) {
        g_render = nullptr;
        return false;
    }
    mpv_render_context_set_update_callback(g_render, onFrame, nullptr);
    return true;
#endif
}

// Nobody showing the picture: after a moment, it stops being decoded.
void startUnwatched()
{
    if (!g_engine)
        return;
    if (!g_unwatched) {
        g_unwatched = new QTimer(g_engine);
        g_unwatched->setSingleShot(true);
        g_unwatched->setInterval(kUnwatchedAfterMs);
        QObject::connect(g_unwatched, &QTimer::timeout, g_engine, []() {
            if (!g_holder && !g_externalHeld && g_engine)
                g_engine->setVideoWatched(false);
        });
    }
    g_unwatched->start();
}

// QImage::Format_RGB32 is 0xffRRGGBB in a word, which on a little-endian
// machine is B, G, R, unused in memory — mpv's "bgr0".
const char *kFormat = "bgr0";

} // namespace

VideoSurface::VideoSurface(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    g_surfaces.append(this);
}

VideoSurface::~VideoSurface()
{
    m_dying = true;
    detach();
    g_surfaces.removeOne(this);
    // The last one gone: the context goes too, which must happen before the
    // player it renders for is destroyed.
    if (g_surfaces.isEmpty() && g_render) {
        QMutexLocker locker(&g_renderLock);
#ifndef MONOLIST_NO_MPV
        mpv_render_context_set_update_callback(g_render, nullptr, nullptr);
        mpv_render_context_free(g_render);
#endif
        g_render = nullptr;
    }
}

void VideoSurface::setEngine(MpvEngine *engine)
{
    g_engine = engine;
    // Made now rather than when a picture is first shown: mpv's video output
    // needs it to open, and a picture asked for with nothing on screen yet —
    // turned on from the player bar, or taken by picture in picture — would
    // otherwise have its track dropped ("No render context set").
    ensureRender(&VideoSurface::onFrame);
    if (!engine)
        return;
    // A mid-song upgrade replaces the player (MpvEngine::startUpgrade). Its
    // sound alone, never a picture, so there is nothing on screen to lose:
    // the context is freed on the old player, which must outlive it, and
    // made again on the new one, where the next picture will need it.
    QObject::connect(engine, &MpvEngine::handleAboutToChange, engine, []() {
        if (!g_render)
            return;
        QMutexLocker locker(&g_renderLock);
#ifndef MONOLIST_NO_MPV
        mpv_render_context_set_update_callback(g_render, nullptr, nullptr);
        mpv_render_context_free(g_render);
#endif
        g_render = nullptr;
    });
    QObject::connect(engine, &MpvEngine::handleChanged, engine, []() {
        ensureRender(&VideoSurface::onFrame);
    });
}

void VideoSurface::itemChange(ItemChange change, const ItemChangeData &value)
{
    // A minimised window hides the picture as surely as a hidden item does.
    if (change == ItemSceneChange) {
        if (m_window)
            disconnect(m_window.data(), &QWindow::visibilityChanged, this, &VideoSurface::reconsider);
        m_window = value.window;
        if (m_window)
            connect(m_window.data(), &QWindow::visibilityChanged, this, &VideoSurface::reconsider);
    }
    // Shown: take the picture. Hidden or gone: give it up, so mpv stops
    // rendering frames nobody looks at.
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        reconsider();
    QQuickItem::itemChange(change, value);
}

void VideoSurface::releaseResources()
{
    detach();
}

bool VideoSurface::canShow() const
{
    const QQuickWindow *view = window();
    return isVisible() && view && view->isVisible()
           && view->visibility() != QWindow::Minimized
           && view->visibility() != QWindow::Hidden;
}

void VideoSurface::reconsider()
{
    if (canShow())
        attach();
    else
        detach();
}

// Called by mpv when a frame is ready, from its own thread. Which surface
// draws it is looked up on the GUI thread, where that changes.
void VideoSurface::onFrame(void *)
{
    {
        QMutexLocker locker(&g_externalLock);
        if (g_external) {
            g_external();
            return;
        }
    }
    if (!g_engine)
        return;
    QMetaObject::invokeMethod(g_engine, []() {
        if (g_holder)
            g_holder->update();
    }, Qt::QueuedConnection);
}

void VideoSurface::attach()
{
    if (g_holder == this || g_externalHeld || !g_engine || !g_engine->isValid())
        return;
#ifdef MONOLIST_NO_MPV
    return;
#else
    if (!ensureRender(&VideoSurface::onFrame))
        return;

    if (VideoSurface *previous = std::exchange(g_holder, this))
        previous->release();
    if (g_unwatched)
        g_unwatched->stop();
    connect(g_engine, &MpvEngine::videoSizeChanged, this, &VideoSurface::setVideoSize,
            Qt::UniqueConnection);
    // What is already playing, in case it started before this was shown.
    setVideoSize(g_engine->videoSize());
    g_engine->setVideoWatched(true);
    update();
#endif
}

void VideoSurface::detach()
{
    if (g_holder != this)
        return;
    g_holder = nullptr;
    release();

    // Another surface still on screen carries on with the picture.
    for (VideoSurface *other : std::as_const(g_surfaces)) {
        if (other != this && other->canShow()) {
            other->attach();
            return;
        }
    }

    startUnwatched();
}

void VideoSurface::attachExternal(std::function<void()> onFrame)
{
    if (!ensureRender(&VideoSurface::onFrame))
        return;
    if (VideoSurface *previous = std::exchange(g_holder, nullptr))
        previous->release();
    {
        QMutexLocker locker(&g_externalLock);
        g_external = std::move(onFrame);
    }
    g_externalHeld = true;
    if (g_unwatched)
        g_unwatched->stop();
    if (g_engine)
        g_engine->setVideoWatched(true);
}

void VideoSurface::detachExternal()
{
    if (!g_externalHeld)
        return;
    {
        QMutexLocker locker(&g_externalLock);
        g_external = nullptr;
    }
    g_externalHeld = false;
    for (VideoSurface *surface : std::as_const(g_surfaces)) {
        if (surface->canShow()) {
            surface->attach();
            return;
        }
    }
    startUnwatched();
}

bool VideoSurface::renderExternal(void *pixels, const QSize &size, qsizetype stride)
{
    QMutexLocker locker(&g_renderLock);
    if (!g_render || !pixels || size.isEmpty())
        return false;
#ifdef MONOLIST_NO_MPV
    Q_UNUSED(stride)
    return false;
#else
    int sizes[2] = { size.width(), size.height() };
    size_t pitch = size_t(stride);
    int block = 0;   // see updatePaintNode
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, sizes },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(kFormat) },
        { MPV_RENDER_PARAM_SW_STRIDE, &pitch },
        { MPV_RENDER_PARAM_SW_POINTER, pixels },
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    return mpv_render_context_render(g_render, params) >= 0;
#endif
}

void VideoSurface::release()
{
    if (g_engine)
        disconnect(g_engine, &MpvEngine::videoSizeChanged, this, &VideoSurface::setVideoSize);
    m_frame = QImage();
    if (m_dying)
        return;
    if (m_showing) {
        m_showing = false;
        Q_EMIT showingChanged();
    }
    update();   // so the last frame drawn here goes as well
}

// What mpv is playing has a picture of this size, or none at all.
void VideoSurface::setVideoSize(const QSize &size)
{
    const bool showing = !size.isEmpty();
    if (showing) {
        const qreal aspect = qreal(size.width()) / qreal(size.height());
        if (!qFuzzyCompare(aspect, m_aspect)) {
            m_aspect = aspect;
            Q_EMIT aspectRatioChanged();
        }
    }
    if (showing != m_showing) {
        m_showing = showing;
        Q_EMIT showingChanged();
    }
    update();
}

QSGNode *VideoSurface::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    const QQuickWindow *view = window();
    if (g_holder != this || !g_render || !view || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }

    // In device pixels, so the picture is rendered at the size it is shown —
    // but never larger than the video itself. mpv draws in software, here on
    // the GUI thread, and every frame is uploaded afresh: full screen at the
    // item's size would scale a 640×360 stream up to several million pixels a
    // frame on the processor. At most the video's own size instead, the shape
    // of the item kept so mpv fits the picture into it the same way, and the
    // texture node stretches it the rest of the way on the graphics card.
    const qreal ratio = view->effectiveDevicePixelRatio();
    QSizeF target(width() * ratio, height() * ratio);
    const QSize video = g_engine ? g_engine->videoSize() : QSize();
    if (!video.isEmpty()) {
        const qreal scale = qMin<qreal>(1.0, qMax(video.width() / target.width(),
                                                  video.height() / target.height()));
        target *= scale;
    }
    const QSize size(qMax(1, qRound(target.width())), qMax(1, qRound(target.height())));
    if (m_frame.size() != size)
        m_frame = QImage(size, QImage::Format_RGB32);

#ifndef MONOLIST_NO_MPV
    int sizes[2] = { size.width(), size.height() };
    size_t stride = size_t(m_frame.bytesPerLine());
    int block = 0;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, sizes },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(kFormat) },
        { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, m_frame.bits() },
        // Drawn now, not held until the frame's moment comes: this runs while
        // Qt's GUI thread waits for it, and mpv's default wait — up to a frame
        // — froze the whole window for that long at every frame.
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    {
        QMutexLocker locker(&g_renderLock);
        mpv_render_context_render(g_render, params);
    }
#endif

    auto *node = static_cast<QSGSimpleTextureNode *>(old);
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }
    node->setTexture(view->createTextureFromImage(m_frame, QQuickWindow::TextureIsOpaque));
    node->setRect(boundingRect());
    return node;
}
