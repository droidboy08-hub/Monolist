#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <memory>

class PlaybackController;
class MpvEngine;

// The system's own picture in picture, where there is one (macOS 12 and
// later): the video in a small window of the system's, floating over every
// app, with its own play and pause and a button back to Now Playing.
//
// It is lighter than the app's own small panel (MiniVideo): the frames are
// drawn on a thread of their own straight into a layer the system shows,
// instead of into the app's window, which the panel had redrawn in full for
// every frame. Where there is none — Windows, Linux — `supported` is false
// and the app's own panel is used.
//
// Plain C++ here, so main.cpp needs no Objective-C; AVKit is in
// macos/systempip.mm.
class SystemPip : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    // The system's window is up, or on its way.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
public:
    SystemPip(PlaybackController *player, MpvEngine *engine, QObject *parent = nullptr);
    ~SystemPip() override;

    bool supported() const;
    bool active() const { return m_active; }

    // Takes the picture into the system's window. `failed` if it cannot.
    Q_INVOKABLE void start();
    // Puts the system's window away; the picture goes back to the app.
    Q_INVOKABLE void stop();
    // What it has drawn and what the system says, for --pip-test.
    QString diagnostics() const;
    QString windowTree() const;

    struct Private;

Q_SIGNALS:
    void activeChanged();
    // The system's window closed from its own close button.
    void stopped();
    // Its button back to the app: Now Playing, where the picture is large.
    void restoreRequested();
    void failed(const QString &reason);

private:
    friend struct Private;
    void setActive(bool active);

    PlaybackController *m_player;
    MpvEngine *m_engine;
    bool m_active = false;
    std::unique_ptr<Private> d;
};
