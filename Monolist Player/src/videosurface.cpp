#include "videosurface.h"
#include "mpvengine.h"

#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

// Built without libmpv (MONOLIST_NO_MPV) there is never a picture: the item
// exists, so the views that place it still load, but it draws nothing and
// never reports one as showing.
#ifndef MONOLIST_NO_MPV
#include <mpv/client.h>
#include <mpv/render.h>
#endif

namespace {

// The player, and which surface holds its render context: mpv allows one.
MpvEngine *g_engine = nullptr;
VideoSurface *g_holder = nullptr;

#ifndef MONOLIST_NO_MPV
// QImage::Format_RGB32 is 0xffRRGGBB in a word, which on a little-endian
// machine is B, G, R, unused in memory — mpv's "bgr0".
const char *kFormat = "bgr0";
#endif

} // namespace

VideoSurface::VideoSurface(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

VideoSurface::~VideoSurface()
{
    detach();
}

void VideoSurface::setEngine(MpvEngine *engine)
{
    g_engine = engine;
}

void VideoSurface::itemChange(ItemChange change, const ItemChangeData &value)
{
    // Shown: take the picture. Hidden or gone: give it back, so mpv stops
    // rendering frames nobody looks at.
    if (change == ItemVisibleHasChanged || change == ItemSceneChange) {
        if (isVisible() && window())
            attach();
        else
            detach();
    }
    QQuickItem::itemChange(change, value);
}

void VideoSurface::releaseResources()
{
    detach();
}

// Called by mpv when a frame is ready, from its own thread.
void VideoSurface::onFrame(void *surface)
{
    auto *self = static_cast<VideoSurface *>(surface);
    QMetaObject::invokeMethod(self, &QQuickItem::update, Qt::QueuedConnection);
}

void VideoSurface::attach()
{
    if (m_render || !g_engine || !g_engine->isValid())
        return;
    if (g_holder && g_holder != this)
        g_holder->detach();

#ifndef MONOLIST_NO_MPV
    int advanced = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW) },
        // Frames arrive as they are decoded rather than on a display clock,
        // which is what a Qt Quick item wants: it draws when told to.
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (mpv_render_context_create(&m_render, g_engine->handle(), params) < 0) {
        m_render = nullptr;
        return;
    }
    g_holder = this;
    mpv_render_context_set_update_callback(m_render, &VideoSurface::onFrame, this);
    connect(g_engine, &MpvEngine::videoSizeChanged, this, &VideoSurface::setVideoSize);
    // What is already playing, in case it started before this was shown.
    setVideoSize(g_engine->videoSize());
    update();
#endif
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

void VideoSurface::detach()
{
    if (!m_render)
        return;
    if (g_engine)
        disconnect(g_engine, &MpvEngine::videoSizeChanged, this, &VideoSurface::setVideoSize);
#ifndef MONOLIST_NO_MPV
    mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
    mpv_render_context_free(m_render);
#endif
    m_render = nullptr;
    if (g_holder == this)
        g_holder = nullptr;
    m_frame = QImage();
    if (m_showing) {
        m_showing = false;
        Q_EMIT showingChanged();
    }
}

QSGNode *VideoSurface::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    const QQuickWindow *view = window();
    if (!m_render || !view || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }

    // In device pixels, so the picture is rendered at the size it is shown.
    const qreal ratio = view->effectiveDevicePixelRatio();
    const QSize size(qMax(1, qRound(width() * ratio)), qMax(1, qRound(height() * ratio)));
    if (m_frame.size() != size)
        m_frame = QImage(size, QImage::Format_RGB32);

#ifndef MONOLIST_NO_MPV
    int sizes[2] = { size.width(), size.height() };
    size_t stride = size_t(m_frame.bytesPerLine());
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_SW_SIZE, sizes },
        { MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(kFormat) },
        { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, m_frame.bits() },
        { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    mpv_render_context_render(m_render, params);
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
