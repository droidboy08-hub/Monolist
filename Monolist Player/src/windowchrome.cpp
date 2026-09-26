#include "windowchrome.h"

#include <QCoreApplication>
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
        // which runs the native move loop, snapping included.
        if (IsZoomed(msg->hwnd)) {
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
