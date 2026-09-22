import QtQuick
import Monolist.Backend

// Makes the area it fills act as the window's title bar: drag to move (the
// native move, so edge snapping still works), double-click to maximise or
// restore, right-click for the window menu. Put it under the bar's buttons
// and fields, which take their own clicks first.
Item {
    function toggleMaximized() {
        var window = Window.window
        if (window.visibility === Window.Maximized)
            window.showNormal()
        else
            window.showMaximized()
    }

    DragHandler {
        target: null
        onActiveChanged: {
            if (active)
                Window.window.startSystemMove()
        }
    }

    TapHandler {
        onDoubleTapped: parent.toggleMaximized()
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: Chrome.showSystemMenu()
    }
}
