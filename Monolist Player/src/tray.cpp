#include "tray.h"

#include "library.h"
#include "playbackcontroller.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

namespace {

const char kCloseToTrayKey[] = "window.close_to_tray";
const char kToldKey[] = "window.tray_told";

} // namespace

Tray::Tray(Library *library, PlaybackController *player, QObject *parent)
    : QObject(parent), m_library(library), m_player(player)
{
#ifndef Q_OS_MACOS
    m_available = QSystemTrayIcon::isSystemTrayAvailable();
#endif
    if (m_library)
        m_closeToTray = m_library->settingValue(QString::fromLatin1(kCloseToTrayKey), QStringLiteral("1"))
                        != QLatin1String("0");

    // Quit, or Windows signing out or shutting down: from here on a window
    // closing closes. Windows' ending is heard as it happens (Qt's
    // aboutToQuit, from WM_ENDSESSION), not as it is asked about: a sign-out
    // another app cancels (commitDataRequest, WM_QUERYENDSESSION, and no end
    // after it) leaves the close button going to the tray, as before. Nothing
    // here keeps Windows waiting: it closes no windows of its own to end a
    // session, and a hidden one holds nothing up.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        m_ending = true;
        Q_EMIT changed();
    });

    if (!m_available)
        return;

    m_menu = new QMenu();
    m_song = m_menu->addAction(QString());
    m_song->setEnabled(false);
    m_menu->addSeparator();
    m_play = m_menu->addAction(QStringLiteral("Play"));
    QAction *next = m_menu->addAction(QStringLiteral("Next"));
    QAction *previous = m_menu->addAction(QStringLiteral("Previous"));
    m_menu->addSeparator();
    QAction *open = m_menu->addAction(QStringLiteral("Open Monolist"));
    QAction *mini = m_menu->addAction(QStringLiteral("Mini player"));
    m_menu->addSeparator();
    QAction *quit = m_menu->addAction(QStringLiteral("Quit Monolist"));
    if (m_player) {
        connect(m_play, &QAction::triggered, m_player, &PlaybackController::togglePlay);
        connect(next, &QAction::triggered, m_player, &PlaybackController::next);
        connect(previous, &QAction::triggered, m_player, &PlaybackController::previous);
        connect(m_player, &PlaybackController::playingChanged, this, &Tray::refresh);
        connect(m_player, &PlaybackController::currentTrackChanged, this, &Tray::refresh);
    }
    connect(open, &QAction::triggered, this, &Tray::openRequested);
    connect(mini, &QAction::triggered, this, &Tray::miniRequested);
    connect(quit, &QAction::triggered, this, []() {
        QCoreApplication::quit();
    });

    m_icon = new QSystemTrayIcon(this);
    m_icon->setContextMenu(m_menu);
    connect(m_icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            Q_EMIT showRequested();
    });
    // The note said once; a click on it brings the window too.
    connect(m_icon, &QSystemTrayIcon::messageClicked, this, &Tray::showRequested);

    refresh();
    showIcon(m_closeToTray);
}

Tray::~Tray()
{
    delete m_menu;
}

void Tray::setCloseToTray(bool on)
{
    if (on == m_closeToTray)
        return;
    m_closeToTray = on;
    if (m_library)
        m_library->setSetting(QString::fromLatin1(kCloseToTrayKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    showIcon(on);
    Q_EMIT changed();
}

QString Tray::toolTip() const
{
    const QVariantMap track = m_player ? m_player->currentTrack() : QVariantMap();
    const QString title = track.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        return QStringLiteral("Monolist");
    const QString artist = track.value(QStringLiteral("artist")).toString();
    return QStringLiteral("Monolist\n") + title + (artist.isEmpty() ? QString() : QStringLiteral(" · ") + artist);
}

// The song, play or pause, and the picture: whatever the app is showing
// now (the icon follows Settings' app icon, AppIcon).
void Tray::refresh()
{
    if (!m_icon)
        return;
    const QVariantMap track = m_player ? m_player->currentTrack() : QVariantMap();
    const QString title = track.value(QStringLiteral("title")).toString();
    const QString artist = track.value(QStringLiteral("artist")).toString();
    m_song->setText(title.isEmpty() ? QStringLiteral("Nothing playing")
                                    : title + (artist.isEmpty() ? QString() : QStringLiteral(" — ") + artist));
    m_play->setText(m_player && m_player->playing() ? QStringLiteral("Pause") : QStringLiteral("Play"));
    m_play->setEnabled(!title.isEmpty());
    m_icon->setIcon(QGuiApplication::windowIcon());
    m_icon->setToolTip(toolTip());
}

void Tray::showIcon(bool shown)
{
    if (!m_icon)
        return;
    if (shown) {
        refresh();
        m_icon->show();
    } else {
        m_icon->hide();
    }
}

void Tray::hidden()
{
    if (!m_icon || !m_library || !m_closeToTray)
        return;
    refresh();
    if (m_library->settingValue(QString::fromLatin1(kToldKey)) == QLatin1String("1"))
        return;
    m_library->setSetting(QString::fromLatin1(kToldKey), QStringLiteral("1"));
    m_icon->showMessage(QStringLiteral("Monolist is still playing"),
                        QStringLiteral("Closing the window keeps the music going. Click Monolist's icon here to "
                                       "bring it back, or right-click it to quit."),
                        QGuiApplication::windowIcon(), 8000);
}
