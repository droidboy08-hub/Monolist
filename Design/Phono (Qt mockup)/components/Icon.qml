import QtQuick
import QtQuick.Shapes
import Phono
import "../Icons.js" as Glyphs

// Stroked vector glyph. Colors come from the binding, not from a bitmap,
// so icons stay crisp at any DPI and tint with the design tokens.
Item {
    id: root

    property string name: ""
    property color color: Theme.text
    property real thickness: 2

    implicitWidth: 18
    implicitHeight: 18

    Item {
        width: 24
        height: 24
        anchors.centerIn: parent
        scale: Math.min(root.width, root.height) / 24

        Shape {
            anchors.fill: parent
            antialiasing: true
            layer.enabled: true
            layer.samples: 4

            ShapePath {
                strokeColor: Glyphs.isFilled(root.name, 0) ? "transparent" : root.color
                fillColor: Glyphs.isFilled(root.name, 0) ? root.color : "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath(root.name, 0) }
            }
            ShapePath {
                strokeColor: Glyphs.isFilled(root.name, 1) ? "transparent" : root.color
                fillColor: Glyphs.isFilled(root.name, 1) ? root.color : "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath(root.name, 1) }
            }
            ShapePath {
                strokeColor: Glyphs.isFilled(root.name, 2) ? "transparent" : root.color
                fillColor: Glyphs.isFilled(root.name, 2) ? root.color : "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath(root.name, 2) }
            }
            ShapePath {
                strokeColor: Glyphs.isFilled(root.name, 3) ? "transparent" : root.color
                fillColor: Glyphs.isFilled(root.name, 3) ? root.color : "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath(root.name, 3) }
            }
            ShapePath {
                strokeColor: Glyphs.isFilled(root.name, 4) ? "transparent" : root.color
                fillColor: Glyphs.isFilled(root.name, 4) ? root.color : "transparent"
                strokeWidth: root.thickness
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: Glyphs.subpath(root.name, 4) }
            }
        }
    }
}
