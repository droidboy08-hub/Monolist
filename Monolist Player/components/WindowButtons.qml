import QtQuick
import QtQuick.Controls.Basic
import Monolist

// Minimise, maximise and close, drawn in the system's style: square, ink on
// paper, the full height of the bar, so that a flick to the top-right corner
// still closes a maximised window. Close turns signal red under the pointer —
// Windows' own convention, and this design's accent.
Row {
    id: root

    readonly property bool maximized: Window.window !== null
                                      && Window.window.visibility === Window.Maximized

    component CaptionButton: Button {
        id: button

        property string glyph: ""
        property bool danger: false

        width: Theme.captionButtonWidth
        height: root.height
        padding: 0
        hoverEnabled: true
        focusPolicy: Qt.NoFocus

        background: Rectangle {
            color: button.danger ? (button.down ? Theme.accent700 : button.hovered ? Theme.accent : "transparent")
                                 : (button.down ? Theme.neutral300 : button.hovered ? Theme.surface : "transparent")
        }

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                width: 13
                height: 13
                thickness: 1.8
                name: button.glyph
                color: button.danger && (button.hovered || button.down) ? Theme.accentForeground : Theme.text
            }
        }
    }

    CaptionButton {
        glyph: "minimize"
        onClicked: Window.window.showMinimized()
    }
    CaptionButton {
        glyph: root.maximized ? "restore" : "maximize"
        onClicked: root.maximized ? Window.window.showNormal() : Window.window.showMaximized()
    }
    CaptionButton {
        glyph: "x"
        danger: true
        onClicked: Window.window.close()
    }
}
