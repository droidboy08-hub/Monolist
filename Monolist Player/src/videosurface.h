#pragma once

#include <QImage>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

struct mpv_render_context;
class MpvEngine;

// Draws what mpv is playing, inside the Qt Quick scene.
//
// mpv renders the frame into memory (its "software" render API) and the item
// hands that to the scene graph. The picture is therefore part of the scene
// like any other item: menus, the player bar and the window's own corners draw
// over it, it can be clipped and animated, and it needs no particular graphics
// backend — Direct3D on Windows, Metal on macOS, whatever Qt chose.
//
// The other way round would be to give mpv a window of its own, which renders
// on the GPU but always sits on top of everything: no controls over the video,
// no fullscreen overlay. That trade is why this one renders through Qt.
//
// mpv allows one render context per player, so one surface shows the picture at
// a time: a surface takes the context when it is shown and gives it back when
// it is hidden.
class VideoSurface : public QQuickItem
{
    Q_OBJECT
    // Declared to the module rather than registered at run time, so the QML
    // tooling knows what it is and can check the views that use it.
    QML_ELEMENT
    // A frame has arrived and the picture is worth showing.
    Q_PROPERTY(bool showing READ showing NOTIFY showingChanged)
    // The picture's shape, for laying it out; 16:9 until the first frame.
    Q_PROPERTY(qreal aspectRatio READ aspectRatio NOTIFY aspectRatioChanged)
public:
    explicit VideoSurface(QQuickItem *parent = nullptr);
    ~VideoSurface() override;

    // The player whose picture the surfaces draw. Set once, at startup.
    static void setEngine(MpvEngine *engine);

    bool showing() const { return m_showing; }
    qreal aspectRatio() const { return m_aspect; }

Q_SIGNALS:
    void showingChanged();
    void aspectRatioChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void releaseResources() override;

private Q_SLOTS:
    void setVideoSize(const QSize &size);

private:
    void attach();     // take the render context
    void detach();     // give it back
    static void onFrame(void *surface);

    mpv_render_context *m_render = nullptr;
    QImage m_frame;
    bool m_showing = false;
    qreal m_aspect = 16.0 / 9.0;
};
