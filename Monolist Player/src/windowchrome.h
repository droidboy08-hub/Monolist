#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QWindow;

// The window's own title bar.
//
// The system title bar goes; the design draws its own in the top strip, with
// window buttons in the system's style. What the platform does well is kept:
//
//   Windows  The native frame stays and only its caption is removed
//            (WM_NCCALCSIZE), so snapping, Win+arrow keys, the drop shadow,
//            the minimise and restore animations and the resize edges all
//            behave as in any other window. Corners are square, as every
//            other corner in the design is.
//   macOS    The content extends under the title bar and the traffic lights
//            stay, so no buttons are drawn and the brand leaves them room.
//   Linux    The frame goes entirely; the window moves and resizes through
//            the compositor (startSystemMove / startSystemResize), so snapping
//            works there too, and QML draws the resize edges.
//
// Moving, maximising and closing are done from QML with the Window API.
class WindowChrome : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    // True where the platform frame is gone entirely and QML must provide the
    // resize edges itself.
    Q_PROPERTY(bool drawsResizeEdges READ drawsResizeEdges CONSTANT)
    // True where the system keeps its own window buttons (macOS).
    Q_PROPERTY(bool nativeButtons READ nativeButtons CONSTANT)
    // Room the native buttons need at the left of the title bar.
    Q_PROPERTY(int nativeButtonsInset READ nativeButtonsInset CONSTANT)
    // Which corner the window buttons belong in, and which buttons, in the
    // order they are drawn from left to right ("minimize", "maximize",
    // "close"). Windows: the right, all three. Linux: wherever the desktop's
    // own button layout puts close (GNOME's button-layout, KDE's kwinrc), and
    // only the buttons it lists there. macOS: the system's, so on the left.
    Q_PROPERTY(bool buttonsOnLeft READ buttonsOnLeft CONSTANT)
    Q_PROPERTY(QStringList windowButtons READ windowButtons CONSTANT)
    // Round buttons, centred in the bar, as GTK and Breeze draw them; square
    // caption buttons the full height of the bar, as Windows draws them.
    Q_PROPERTY(bool roundButtons READ roundButtons CONSTANT)
public:
    explicit WindowChrome(QObject *parent = nullptr);
    ~WindowChrome() override;

    // Takes over the window's decorations. Call once, before it is shown.
    void attach(QWindow *window);

    bool drawsResizeEdges() const;
    bool nativeButtons() const;
    int nativeButtonsInset() const;
    bool buttonsOnLeft() const;
    QStringList windowButtons() const;
    bool roundButtons() const;

    // The window menu (restore, move, size, minimise, maximise, close) at the
    // pointer, as a right click on a system title bar shows it. Windows only.
    Q_INVOKABLE void showSystemMenu();

    // The pointer's shape over the window now (a Qt::CursorShape), as Qt Quick
    // last set it from what is under it: a page moving under the wheel keeps
    // it while hover is held still (ScrollPage).
    Q_INVOKABLE int cursorShape() const;

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    // Read once, the first time it is asked for: the desktop's layout is not
    // followed live, as it is changed about as often as the desktop itself.
    void readButtonLayout() const;

    QPointer<QWindow> m_window;
    mutable bool m_layoutRead = false;
    mutable bool m_buttonsOnLeft = false;
    mutable QStringList m_windowButtons;
};
