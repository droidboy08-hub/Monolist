#pragma once

#include <QObject>
#include <QPointer>

class Library;
class PlaybackController;
class QAction;
class QMenu;
class QSystemTrayIcon;

// Closing the window to the system tray, the music playing on (the owner's,
// 2026-10-08), as Spotify does it: an icon by the clock while Monolist runs,
// a click on it bringing the window back, a right-click for the song, play,
// next, previous, the window, the mini player and Quit. Settings has the
// switch ("Keep playing in the system tray", kept as "window.close_to_tray",
// on unless turned off); with it off, closing the window quits as before.
//
// The first time the window goes to the tray, a note from the icon says
// where it went, once ("window.tray_told").
//
// Windows signing out or shutting down is never kept waiting by a window
// that will not close: `ending` is set as the session ends (and on Quit),
// and a window then closes as before. Only the end itself sets it, never the
// question before it, so a sign-out that another app cancels leaves the
// close button going to the tray.
//
// Not on the Mac, whose apps go on with their windows closed anyway: there
// `available` is false and Settings shows nothing.
//
// Exposed to QML as the "Tray" singleton.
class Tray : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY changed)
    Q_PROPERTY(bool ending READ ending NOTIFY changed)

public:
    Tray(Library *library, PlaybackController *player, QObject *parent = nullptr);
    ~Tray() override;

    bool available() const { return m_available; }
    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool on);
    bool ending() const { return m_ending; }

    // The window went to the tray: the first time, a note says so.
    Q_INVOKABLE void hidden();

    // For the self-test: the menu as built, and its text as it stands.
    QMenu *menu() const { return m_menu; }
    QString toolTip() const;

public Q_SLOTS:
    // The menu's song and play/pause, the tooltip, and the picture (the
    // app icon chosen in Settings, AppIcon).
    void refresh();

Q_SIGNALS:
    void changed();
    // A click on the icon: whichever window is in use (the full one or the
    // mini player), back.
    void showRequested();
    // The menu's Open Monolist: the full window, whichever was in use.
    void openRequested();
    // The menu's Mini player.
    void miniRequested();

private:
    void showIcon(bool shown);

    Library *m_library = nullptr;
    PlaybackController *m_player = nullptr;
    bool m_available = false;
    bool m_closeToTray = true;
    bool m_ending = false;
    QSystemTrayIcon *m_icon = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_song = nullptr;
    QAction *m_play = nullptr;
};
