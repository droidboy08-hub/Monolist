import QtQuick
import QtQuick.Shapes
import Monolist
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

        // The glyphs are drawn on Lucide's 24x24 grid and scaled to the
        // requested size. With CurveRenderer this stays a geometry transform,
        // so the curves are still evaluated at final device resolution.
        scale: Math.min(root.width, root.height) / 24

        Shape {
            anchors.fill: parent

            // CurveRenderer evaluates curve coverage per fragment, which keeps
            // the outlines sharp at fractional scales and fractional DPI.
            //
            // The previous `layer.enabled: true` + `layer.samples: 4` did the
            // opposite of what it looked like: it rasterised each glyph into a
            // 24x24 offscreen texture, which `scale` then resampled. Every icon
            // was effectively a small bitmap being stretched, so the vectors
            // bought nothing. Requires Qt 6.6+.
            preferredRendererType: Shape.CurveRenderer

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
