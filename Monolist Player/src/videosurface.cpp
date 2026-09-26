#include "videosurface.h"
#include "mpvengine.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QTimer>

#include <mpv/client.h>
#include <mpv/render.h>

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
        mpv_render_context_set_update_callback(g_render, nullptr, nullptr);
        mpv_render_context_free(g_render);
        g_render = nullptr;
    }
}

void VideoSurface::setEngine(MpvEngine *engine)
{
    g_engine = engine;
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
    if (!g_engine)
        return;
    QMetaObject::invokeMethod(g_engine, []() {
        if (g_holder)
            g_holder->update();
    }, Qt::QueuedConnection);
}

void VideoSurface::attach()
{
    if (g_holder == this || !g_engine || !g_engine->isValid())
        return;

    if (!g_render) {
        int advanced = 1;
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW) },
            // Frames arrive as they are decoded rather than on a display
            // clock, which is what a Qt Quick item wants: it draws when told.
            { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
            { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        if (mpv_render_context_create(&g_render, g_engine->handle(), params) < 0) {
            g_render = nullptr;
            return;
        }
        mpv_render_context_set_update_callback(g_render, &VideoSurface::onFrame, nullptr);
    }

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

    if (!g_engine)
        return;
    if (!g_unwatched) {
        g_unwatched = new QTimer(g_engine);
        g_unwatched->setSingleShot(true);
        g_unwatched->setInterval(kUnwatchedAfterMs);
        QObject::connect(g_unwatched, &QTimer::timeout, g_engine, []() {
            if (!g_holder && g_engine)
                g_engine->setVideoWatched(false);
        });
    }
    g_unwatched->start();
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

    // In device pixels, so the picture is rendered at the size it is shown.
    const qreal ratio = view->effectiveDevicePixelRatio();
    const QSize size(qMax(1, qRound(width() * ratio)), qMax(1, qRound(height() * ratio)));
    if (m_frame.size() != size)
        m_frame = QImage(size, QImage::Format_RGB32);

    int sizes[2] = { size.width(), size.height() };
    size_t stride = size_t(m_frame.bytesPerLine());
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, sizes },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(kFormat) },
        { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, m_frame.bits() },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    mpv_render_context_render(g_render, params);

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
