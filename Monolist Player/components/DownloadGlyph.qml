import QtQuick
import QtQuick.Shapes
import Monolist
import "../Icons.js" as Glyphs

// The download glyph, drawn in three pieces so the arrow can fall through the
// tray: the arrow slides down and fades out under the lip, reappears above,
// and settles back; the tray takes the weight as it lands, then it all goes
// round again for as long as the pointer stays.
//
// The pieces are the same three subpaths Icon would draw as one Shape, still
// read from Icons.js, and they move inside the same 24-unit grid — so the fall
// scales with the icon instead of being a fixed number of screen pixels.
//
// Motion is position and opacity only. The original of this animation also
// pulsed the tray to 1.05; at the 15px this is drawn at, that is three
// quarters of a pixel, and scale is the one thing the rest of the app never
// does (DESIGN 2.3), so the tray takes the weight by dipping instead.
Item {
    id: root

    property color color: Theme.text
    property real thickness: 2
    // Runs the fall on a loop. Off, everything eases back to where it started.
    property bool falling: false

    implicitWidth: 18
    implicitHeight: 18

    Item {
        width: 24
        height: 24
        anchors.centerIn: parent
        scale: Math.min(root.width, root.height) / 24

        Shape {
            id: tray
            width: 24
            height: 24
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: root.color
                fillColor: "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath("download", 0) }
            }
        }

        Shape {
            id: head
            width: 24
            height: 24
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: root.color
                fillColor: "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath("download", 1) }
            }
        }

        Shape {
            id: stem
            width: 24
            height: 24
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: root.color
                fillColor: "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath("download", 2) }
            }
        }
    }

    // One cycle. The stem leads and the head follows a tenth of a second
    // behind, which is what stops it reading as one rigid arrow sliding; the
    // tray answers once they have both landed.
    SequentialAnimation {
        id: cycle
        loops: Animation.Infinite

        ParallelAnimation {
            SequentialAnimation {
                ParallelAnimation {
                    NumberAnimation { target: stem; property: "y"; to: 8; duration: 300; easing.type: Theme.moveCurve }
                    NumberAnimation { target: stem; property: "opacity"; to: 0; duration: 300 }
                }
                PauseAnimation { duration: 100 }
                PropertyAction { target: stem; property: "y"; value: -8 }
                PauseAnimation { duration: 100 }
                ParallelAnimation {
                    NumberAnimation { target: stem; property: "y"; to: 0; duration: 500; easing.type: Theme.moveCurve }
                    NumberAnimation { target: stem; property: "opacity"; to: 1; duration: 500 }
                }
            }

            SequentialAnimation {
                ParallelAnimation {
                    NumberAnimation { target: head; property: "y"; to: 8; duration: 400; easing.type: Theme.moveCurve }
                    NumberAnimation { target: head; property: "opacity"; to: 0; duration: 400 }
                }
                PauseAnimation { duration: 100 }
                PropertyAction { target: head; property: "y"; value: -8 }
                PauseAnimation { duration: 100 }
                ParallelAnimation {
                    NumberAnimation { target: head; property: "y"; to: 0; duration: 400; easing.type: Theme.moveCurve }
                    NumberAnimation { target: head; property: "opacity"; to: 1; duration: 400 }
                }
            }
        }

        SequentialAnimation {
            NumberAnimation { target: tray; property: "y"; to: 2; duration: 150; easing.type: Theme.enterCurve }
            NumberAnimation { target: tray; property: "y"; to: 0; duration: 150; easing.type: Theme.exitCurve }
        }

        PauseAnimation { duration: 200 }
    }

    // Leaving is never abrupt: wherever the pieces are when the pointer goes,
    // they come home over `page`.
    ParallelAnimation {
        id: settle

        NumberAnimation {
            targets: [tray, head, stem]
            properties: "y"
            to: 0
            duration: Theme.page
            easing.type: Theme.moveCurve
        }
        NumberAnimation {
            targets: [head, stem]
            properties: "opacity"
            to: 1
            duration: Theme.page
        }
    }

    onFallingChanged: {
        if (root.falling) {
            settle.stop()
            cycle.restart()
        } else {
            cycle.stop()
            settle.restart()
        }
    }
}
