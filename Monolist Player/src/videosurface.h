#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class MpvEngine;
class QQuickWindow;

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
// mpv allows one render context per player, and freeing it while a picture
// plays takes the picture away for good: mpv drops the video track. So the
// context is made once and kept for as long as any surface exists, and the
// surfaces — Now Playing's, the mini panel's, full screen's — take turns at
// being where its frames go. A surface that is shown takes them; one that is
// hidden hands them to another still on screen. When none is, the engine is
// told nobody is looking and stops decoding the picture, and the first
// surface shown again starts it where the file has got to.
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
    // Shown, in a window that is not minimised: somewhere the picture can be seen.
    bool canShow() const;
    void reconsider();   // attach or detach, as canShow says
    void attach();       // become where the frames go
    void detach();       // stop, handing them to another surface on screen
    void release();      // forget the picture, handing over to nobody
    static void onFrame(void *);

    QImage m_frame;
    bool m_showing = false;
    bool m_dying = false;   // in the destructor: hand over, but draw nothing more
    qreal m_aspect = 16.0 / 9.0;
    // The window whose minimising hides this surface along with everything.
    QPointer<QQuickWindow> m_window;
};
