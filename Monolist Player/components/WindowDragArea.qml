import QtQuick
import Monolist.Backend

// Makes the area it fills act as the window's title bar: drag to move (the
// native move, so edge snapping still works), double-click to maximise or
// restore (on a Mac: whatever System Settings says a double click does),
// right-click for the window menu. Put it under the bar's buttons and fields,
// which take their own clicks first.
Item {
    id: root

    // Read here, on an Item: the attached property is only offered to Items,
    // and a handler asking for it gets null.
    readonly property var hostWindow: Window.window

    function toggleMaximized() {
        if (!hostWindow)
            return
        if (hostWindow.visibility === Window.Maximized)
            hostWindow.showNormal()
        else
            hostWindow.showMaximized()
    }

    DragHandler {
        target: null
        onActiveChanged: {
            // Past the drag threshold the window goes to the system's own
            // move loop, which follows the pointer until it is released.
            if (active && root.hostWindow)
                root.hostWindow.startSystemMove()
        }
    }

    TapHandler {
        onDoubleTapped: {
            if (!Chrome.titleBarDoubleClicked())
                root.toggleMaximized()
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: Chrome.showSystemMenu()
    }
}
