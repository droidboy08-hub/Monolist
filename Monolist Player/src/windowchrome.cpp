#include "windowchrome.h"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QWindow>

#ifdef Q_OS_MACOS
#include "macos/macwindow.h"

// Qt::ExpandedClientAreaHint and Qt::NoTitleBarBackgroundHint, which put the
// content under a transparent title bar, arrived in Qt 6.9.
#  if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#    define MONOLIST_MAC_EXPANDED_TITLE_BAR 1
#  endif
#endif

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

namespace {

// GTK's notation for where the window buttons go: "appmenu:minimize,maximize,
// close" puts what is left of the colon at the left of the title bar and the
// rest at the right. Everything below is turned into it.
QString gtkLayout()
{
    QProcess gsettings;
    gsettings.start(QStringLiteral("gsettings"),
                    { QStringLiteral("get"), QStringLiteral("org.gnome.desktop.wm.preferences"),
                      QStringLiteral("button-layout") });
    // Not there at all (no GNOME), or not answering: the default, rather than
    // a window that waits to open.
    if (!gsettings.waitForFinished(500)) {
        gsettings.kill();
        gsettings.waitForFinished(100);
        return {};
    }
    if (gsettings.exitStatus() != QProcess::NormalExit || gsettings.exitCode() != 0)
        return {};
    QString layout = QString::fromUtf8(gsettings.readAllStandardOutput()).trimmed();
    if (layout.size() >= 2 && layout.startsWith(u'\'') && layout.endsWith(u'\''))
        layout = layout.mid(1, layout.size() - 2);
    return layout;
}

// KWin keeps its buttons as letters, one string per side: X close,
// I minimise, A maximise, the rest (menu, pin, help) nothing drawn here.
QString kdeLayout()
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                         + QStringLiteral("/kwinrc");
    QSettings kwin(path, QSettings::IniFormat);
    kwin.beginGroup(QStringLiteral("org.kde.kdecoration2"));
    const auto names = [](const QString &letters) {
        QStringList out;
        for (const QChar c : letters) {
            if (c == u'X')
                out << QStringLiteral("close");
            else if (c == u'I')
                out << QStringLiteral("minimize");
            else if (c == u'A')
                out << QStringLiteral("maximize");
        }
        return out.join(u',');
    };
    return names(kwin.value(QStringLiteral("ButtonsOnLeft"), QStringLiteral("MS")).toString())
           + u':'
           + names(kwin.value(QStringLiteral("ButtonsOnRight"), QStringLiteral("HIAX")).toString());
}

QStringList buttonsIn(const QString &side)
{
    QStringList out;
    for (QString name : side.split(u',', Qt::SkipEmptyParts)) {
        name = name.trimmed();
        if ((name == QLatin1String("minimize") || name == QLatin1String("maximize")
             || name == QLatin1String("close")) && !out.contains(name))
            out << name;
    }
    return out;
}

} // namespace
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

namespace {

// Windows 11 attributes, spelt out so older SDK headers are enough.
constexpr DWORD kCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kBorderColour = 34;       // DWMWA_BORDER_COLOR
constexpr DWORD kRoundAsUsual = 0;        // DWMWCP_DEFAULT

// The window's native handle, or none when it has none. handle() comes first
// because winId() creates a native window that is not there: at shutdown, when
// the native window is already gone, that meant a failed attempt to create it
// again for every message that passed through the filter.
HWND handleOf(QWindow *window)
{
    return window && window->handle() ? reinterpret_cast<HWND>(window->winId()) : nullptr;
}

// How deep, in physical pixels, the invisible resize edge reaches into the
// window, where the frame used to be.
int resizeEdge(QWindow *window)
{
    return qMax(4, qRound(6 * (window ? window->devicePixelRatio() : 1.0)));
}

} // namespace
#endif

WindowChrome::WindowChrome(QObject *parent)
    : QObject(parent)
{
}

WindowChrome::~WindowChrome()
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

bool WindowChrome::drawsResizeEdges() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return false;
#else
    return true;
#endif
}

bool WindowChrome::nativeButtons() const
{
#ifdef Q_OS_MACOS
    return true;
#else
    return false;
#endif
}

int WindowChrome::nativeButtonsInset() const
{
    // The traffic lights and their margin, where they sit over the content.
    // Under a system title bar (Qt before 6.9) they are above it instead.
#ifdef MONOLIST_MAC_EXPANDED_TITLE_BAR
    return 78;
#else
    return 0;
#endif
}

bool WindowChrome::buttonsOnLeft() const
{
    readButtonLayout();
    return m_buttonsOnLeft;
}

QStringList WindowChrome::windowButtons() const
{
    readButtonLayout();
    return m_windowButtons;
}

bool WindowChrome::roundButtons() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return false;
#else
    return true;
#endif
}

void WindowChrome::readButtonLayout() const
{
    if (m_layoutRead)
        return;
    m_layoutRead = true;

    const QStringList all = { QStringLiteral("minimize"), QStringLiteral("maximize"),
                              QStringLiteral("close") };
    m_buttonsOnLeft = false;
    m_windowButtons = all;

#if defined(Q_OS_MACOS)
    // The traffic lights, drawn by the system at the left.
    m_buttonsOnLeft = true;
    m_windowButtons.clear();
#elif !defined(Q_OS_WIN)
    const bool kde = qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("KDE"),
                                                                           Qt::CaseInsensitive);
    const QString layout = kde ? kdeLayout() : gtkLayout();
    const int colon = layout.indexOf(u':');
    if (colon < 0)
        return;                           // nothing read: the right, all three

    const QStringList left = buttonsIn(layout.left(colon));
    const QStringList right = buttonsIn(layout.mid(colon + 1));
    if (left.isEmpty() && right.isEmpty())
        return;                           // no buttons at all would leave no way to close

    // The buttons go together, in the corner that has close: that is the
    // corner people reach for. One the desktop put in the other corner joins
    // them on the inner side.
    m_buttonsOnLeft = left.contains(QLatin1String("close"))
                      || (!right.contains(QLatin1String("close")) && right.isEmpty());
    // Left to right, either way: on the left the other corner's come after
    // the group, on the right before it.
    m_windowButtons = left + right;
    m_windowButtons.removeDuplicates();
#endif
}

int WindowChrome::cursorShape() const
{
#if QT_CONFIG(cursor)
    if (m_window)
        return int(m_window->cursor().shape());
#endif
    return int(Qt::ArrowCursor);
}

void WindowChrome::attach(QWindow *window)
{
    m_window = window;
    if (!window)
        return;

#if defined(Q_OS_WIN)
    window->create();                     // the native window, still unshown
    const HWND hwnd = handleOf(window);

    // A one-pixel extension of the frame into the client area keeps the DWM
    // drawing the drop shadow once the caption is gone.
    const MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // The window's corners are the system's to round — every other corner in
    // the design is square, but the window is the system's object, not the
    // design's. On Windows 11 the border is a hairline in the ink colour
    // instead of the accent-tinted default. Both are ignored where unsupported.
    const DWORD corners = kRoundAsUsual;
    DwmSetWindowAttribute(hwnd, kCornerPreference, &corners, sizeof corners);
    const COLORREF ink = RGB(0x20, 0x1e, 0x1d);
    DwmSetWindowAttribute(hwnd, kBorderColour, &ink, sizeof ink);

    QCoreApplication::instance()->installNativeEventFilter(this);
    // Have Windows ask for the new frame size (WM_NCCALCSIZE) straight away.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#elif defined(MONOLIST_MAC_EXPANDED_TITLE_BAR)
    window->setFlags(window->flags() | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
    // The native window is made again whenever the window is shown after
    // being closed (which on a Mac leaves the app, and the music, running),
    // so the hidden title is applied each time one is made.
    window->installEventFilter(this);
    window->create();
    MacWindow::hideTitle(window);
#elif defined(Q_OS_MACOS)
    // An older Qt: the system title bar stays, above the design's own.
#else
    window->setFlags(window->flags() | Qt::FramelessWindowHint);
#endif
}

void WindowChrome::showSystemMenu()
{
#ifdef Q_OS_WIN
    const HWND hwnd = handleOf(m_window);
    if (!hwnd)
        return;
    const HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu)
        return;

    const bool maximized = IsZoomed(hwnd);
    const auto enable = [menu](UINT command, bool on) {
        EnableMenuItem(menu, command, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    };
    enable(SC_RESTORE, maximized);
    enable(SC_MOVE, !maximized);
    enable(SC_SIZE, !maximized);
    enable(SC_MINIMIZE, true);
    enable(SC_MAXIMIZE, !maximized);
    enable(SC_CLOSE, true);

    POINT cursor;
    GetCursorPos(&cursor);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, hwnd, nullptr);
    if (command)
        PostMessageW(hwnd, WM_SYSCOMMAND, command, 0);
#endif
}

bool WindowChrome::titleBarDoubleClicked()
{
#ifdef Q_OS_MACOS
    MacWindow::titleBarDoubleClicked(m_window);
    return true;
#else
    return false;
#endif
}

bool WindowChrome::eventFilter(QObject *watched, QEvent *event)
{
#ifdef MONOLIST_MAC_EXPANDED_TITLE_BAR
    if (watched == m_window && event->type() == QEvent::Show)
        MacWindow::hideTitle(m_window);
#endif
    return QObject::eventFilter(watched, event);
}

bool WindowChrome::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" || !m_window)
        return false;
    MSG *msg = static_cast<MSG *>(message);
    const HWND hwnd = handleOf(m_window);
    if (!hwnd || msg->hwnd != hwnd)
        return false;

    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (msg->wParam != TRUE)
            return false;
        // No caption and no visible frame: the client area is the whole
        // window. A maximised window, though, is placed with its frame hanging
        // off the monitor, so keep its client area to the work area, or its
        // edges — and the window buttons — would be cut off.
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (IsZoomed(msg->hwnd)) {
            MONITORINFO monitor;
            monitor.cbSize = sizeof monitor;
            if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                params->rgrc[0] = monitor.rcWork;
        }
        *result = 0;
        return true;
    }

    case WM_NCHITTEST: {
        // The resize edges, now inside the window. Everything else is client
        // area: the title bar moves the window from QML (startSystemMove),
        // which runs the native move loop, snapping included. A maximised or
        // full-screen window has no edges to drag: in full screen the video
        // runs to the screen's edge, and a resize cursor there would be a lie.
        if (IsZoomed(msg->hwnd) || m_window->visibility() == QWindow::FullScreen) {
            *result = HTCLIENT;
            return true;
        }
        RECT window;
        GetWindowRect(msg->hwnd, &window);
        const int x = GET_X_LPARAM(msg->lParam);
        const int y = GET_Y_LPARAM(msg->lParam);
        const int edge = resizeEdge(m_window);
        const bool left = x < window.left + edge;
        const bool right = x >= window.right - edge;
        const bool top = y < window.top + edge;
        const bool bottom = y >= window.bottom - edge;

        if (top && left)
            *result = HTTOPLEFT;
        else if (top && right)
            *result = HTTOPRIGHT;
        else if (bottom && left)
            *result = HTBOTTOMLEFT;
        else if (bottom && right)
            *result = HTBOTTOMRIGHT;
        else if (left)
            *result = HTLEFT;
        else if (right)
            *result = HTRIGHT;
        else if (top)
            *result = HTTOP;
        else if (bottom)
            *result = HTBOTTOM;
        else
            *result = HTCLIENT;
        return true;
    }

    default:
        break;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return false;
}
