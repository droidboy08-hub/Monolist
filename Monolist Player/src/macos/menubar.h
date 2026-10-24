#pragma once

#include <QObject>
#include <QPointer>
#include <QVariant>

class PlaybackController;
class QAction;
class QMenuBar;
class QWindow;

// The Mac's menu bar, which every Mac app has and Windows' window does not:
//
//   Monolist   About Monolist, Settings… (⌘,), and what macOS adds itself
//              (Services, Hide, Quit)
//   Controls   Play or Pause, Next (⌘→), Previous (⌘←), Mini Player (⇧⌘M)
//   Window     Minimize (⌘M), Zoom, Bring All to Front, and the windows, as
//              AppKit lists them
//
// The keys here are the same the window answers to elsewhere (Main.qml's
// Shortcuts), which give them up on a Mac to the menu: one key bound twice
// is ambiguous to Qt, and neither would fire. The menu's work in the window
// is done by calling Main.qml's own functions, so the two cannot drift
// apart. Space stays the window's alone: as a menu key it would be taken from
// the search field too.
class MacMenuBar : public QObject
{
    Q_OBJECT
public:
    MacMenuBar(PlaybackController *player, QObject *parent = nullptr);
    ~MacMenuBar() override;

    // The app's window, once QML has made it: its functions are what the
    // menu's items call.
    void attach(QWindow *window);

private:
    void call(const char *function, const QVariant &argument = QVariant());

    PlaybackController *m_player;
    QPointer<QWindow> m_window;
    QMenuBar *m_bar = nullptr;
    QAction *m_playPause = nullptr;
    QAction *m_mini = nullptr;
};
