pragma Singleton
import QtQuick

// Modernist tokens, transcribed 1:1 from the design system stylesheet.
QtObject {
    // — color —
    readonly property color bg: "#f3f2f2"
    readonly property color surface: "#eae9e9"
    readonly property color text: "#201e1d"
    readonly property color accent: "#ec3013"
    readonly property color divider: "#66201e1d"      // #201e1d at 40%
    readonly property color hairline: "#33201e1d"     // 1px table rules

    readonly property color neutral300: "#d7d3d3"
    readonly property color neutral500: "#9b9797"
    readonly property color neutral600: "#7d7979"
    readonly property color neutral700: "#605d5d"

    readonly property color accent600: "#dd2b0f"
    readonly property color accent700: "#ae1800"
    readonly property color onAccent: "#f3f2f2"

    readonly property color ghostHover: "#1aec3013"   // accent 10%
    readonly property color ghostActive: "#2eec3013"  // accent 18%
    readonly property color rowHover: "#0a201e1d"     // text 4%

    // — type —
    readonly property string fontFamily: "Archivo"
    readonly property int weightRegular: Font.Normal
    readonly property int weightMedium: Font.DemiBold
    readonly property int weightBlack: Font.ExtraBold

    // — spacing —
    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space6: 24
    readonly property int space8: 32

    readonly property int radius: 0
    readonly property int ruleWidth: 2

    // — layout —
    readonly property int sidebarWidth: 296
    readonly property int playerBarHeight: 84
    readonly property int sidebarBreakpoint: 900
    readonly property int wideBreakpoint: 1180

    function tracking(px, em) { return px * em }
}
