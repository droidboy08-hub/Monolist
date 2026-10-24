#include "menubar.h"

#include "macos/macwindow.h"
#include "playbackcontroller.h"

#include <QAction>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QTranslator>
#include <QVariant>
#include <QWindow>

namespace {

// Qt names the application menu's items itself, from its own strings,
// whatever the actions say: Settings… came out as "Preferences...", the name
// macOS gave it before 13. Those strings are translatable, and this is the
// translation.
class MenuNames : public QTranslator
{
public:
    using QTranslator::QTranslator;
    QString translate(const char *context, const char *source, const char *, int) const override
    {
        if (qstrcmp(context, "MAC_APPLICATION_MENU") == 0 && qstrcmp(source, "Preferences...") == 0)
            return QStringLiteral("Settings…");
        return QString();
    }
    bool isEmpty() const override { return false; }
};

} // namespace

MacMenuBar::MacMenuBar(PlaybackController *player, QObject *parent)
    : QObject(parent), m_player(player)
{
    QCoreApplication::installTranslator(new MenuNames(this));

    // With no parent, the menu bar at the top of the screen, for every
    // window of the app (the mini player's too).
    m_bar = new QMenuBar(nullptr);

    // The application menu: Qt moves these into it by their roles, and macOS
    // gives it Services, Hide and Quit of its own.
    QMenu *app = m_bar->addMenu(QStringLiteral("Monolist"));
    QAction *about = app->addAction(QStringLiteral("About Monolist"));
    about->setMenuRole(QAction::AboutRole);
    connect(about, &QAction::triggered, this, [this]() { call("openSettingsAt", QStringLiteral("about")); });
    QAction *settings = app->addAction(QStringLiteral("Settings…"));
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence(QKeySequence::Preferences));
    connect(settings, &QAction::triggered, this, [this]() { call("openSettingsAt", QString()); });

    QMenu *controls = m_bar->addMenu(QStringLiteral("Controls"));
    m_playPause = controls->addAction(QStringLiteral("Play"));
    connect(m_playPause, &QAction::triggered, m_player, &PlaybackController::togglePlay);
    const auto namePlayPause = [this]() {
        m_playPause->setText(m_player->playing() ? QStringLiteral("Pause") : QStringLiteral("Play"));
        m_playPause->setEnabled(!m_player->currentTrack().isEmpty());
    };
    connect(m_player, &PlaybackController::playingChanged, this, namePlayPause);
    connect(m_player, &PlaybackController::currentTrackChanged, this, namePlayPause);
    namePlayPause();
    // Ctrl is the Command key in Qt's names on a Mac.
    QAction *next = controls->addAction(QStringLiteral("Next"));
    next->setShortcut(QKeySequence(QStringLiteral("Ctrl+Right")));
    connect(next, &QAction::triggered, m_player, &PlaybackController::next);
    QAction *previous = controls->addAction(QStringLiteral("Previous"));
    previous->setShortcut(QKeySequence(QStringLiteral("Ctrl+Left")));
    connect(previous, &QAction::triggered, m_player, &PlaybackController::previous);
    controls->addSeparator();
    m_mini = controls->addAction(QStringLiteral("Mini Player"));
    m_mini->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")));
    connect(m_mini, &QAction::triggered, this, [this]() {
        if (m_window)
            call(m_window->property("miniMode").toBool() ? "leaveMini" : "enterMini");
    });

    QMenu *windowMenu = m_bar->addMenu(QStringLiteral("Window"));
    QAction *minimise = windowMenu->addAction(QStringLiteral("Minimize"));
    minimise->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    connect(minimise, &QAction::triggered, this, [this]() {
        // Whichever is in front: the mini player is a window of its own.
        if (QWindow *front = QGuiApplication::focusWindow())
            front->showMinimized();
        else if (m_window)
            m_window->showMinimized();
    });
    QAction *zoom = windowMenu->addAction(QStringLiteral("Zoom"));
    connect(zoom, &QAction::triggered, this, [this]() { MacWindow::zoom(m_window); });
    windowMenu->addSeparator();
    QAction *front = windowMenu->addAction(QStringLiteral("Bring All to Front"));
    connect(front, &QAction::triggered, this, []() { MacWindow::bringAllToFront(); });
    // AppKit's own Window menu from here: it lists the open windows below.
    MacWindow::useAsWindowsMenu(windowMenu);
}

MacMenuBar::~MacMenuBar()
{
    delete m_bar;
}

void MacMenuBar::attach(QWindow *window)
{
    m_window = window;
}

void MacMenuBar::call(const char *function, const QVariant &argument)
{
    if (!m_window)
        return;
    if (argument.isValid())
        QMetaObject::invokeMethod(m_window, function, Q_ARG(QVariant, argument));
    else
        QMetaObject::invokeMethod(m_window, function);
}
