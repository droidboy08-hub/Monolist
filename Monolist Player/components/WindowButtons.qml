import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// Minimise, maximise and close, drawn in the system's style and in the order
// the system puts them (Chrome.windowButtons).
//
// Windows: square, ink on paper, the full height of the bar, so that a flick
// to the top-right corner still closes a maximised window. Close turns signal
// red under the pointer — Windows' own convention, and this design's accent.
//
// Linux: round, centred in the bar, as GTK and Breeze draw them, a tint of
// the ink behind each glyph. Set `ink` where the bar is not paper (the Now
// Playing poster), and the buttons are drawn in that instead.
Row {
    id: root

    property color ink: Theme.text

    readonly property bool maximized: Window.window !== null
                                      && Window.window.visibility === Window.Maximized
    readonly property bool round: Chrome.roundButtons
    readonly property int roundSide: 24

    spacing: round ? 10 : 0
    leftPadding: round ? Theme.space3 : 0
    rightPadding: round ? Theme.space3 : 0

    component CaptionButton: Button {
        id: button

        property string glyph: ""
        property bool danger: false

        width: root.round ? root.roundSide : Theme.captionButtonWidth
        height: root.round ? root.roundSide : root.height
        y: root.round ? Math.round((root.height - height) / 2) : 0
        padding: 0
        hoverEnabled: true
        focusPolicy: Qt.NoFocus

        background: Rectangle {
            radius: root.round ? width / 2 : 0
            color: root.round
                   ? Qt.rgba(root.ink.r, root.ink.g, root.ink.b,
                             button.down ? 0.28 : button.hovered ? 0.18 : 0.1)
                   : button.danger ? (button.down ? (Theme.dark ? Theme.accent600 : Theme.accent700)
                                                  : button.hovered ? Theme.accent : "transparent")
                                   : (button.down ? Theme.neutral300 : button.hovered ? Theme.surface : "transparent")
        }

        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                width: root.round ? 10 : 13
                height: root.round ? 10 : 13
                thickness: root.round ? 2.6 : 1.8
                name: button.glyph
                color: !root.round && button.danger && (button.hovered || button.down)
                       ? Theme.accentForeground : root.ink
            }
        }
    }

    Repeater {
        model: Chrome.windowButtons

        CaptionButton {
            required property string modelData

            glyph: modelData === "minimize" ? "minimize"
                 : modelData === "maximize" ? (root.maximized ? "restore" : "maximize")
                 : "x"
            danger: modelData === "close"
            onClicked: {
                if (modelData === "minimize")
                    Window.window.showMinimized()
                else if (modelData === "maximize")
                    root.maximized ? Window.window.showNormal() : Window.window.showMaximized()
                else
                    Window.window.close()
            }
        }
    }
}
