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
    readonly property color accentForeground: "#f3f2f2"

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

    // — motion —
    // Why these, and what may move at all, is argued in DESIGN.md. In short:
    // a pointer's own feedback is instant or the app feels slow; 120 ms is a
    // colour changing in place; 220 ms is something appearing where it
    // already is; 320 ms is something arriving from elsewhere; past 400 ms
    // the user is waiting, so it is kept for long distances (the lyrics
    // scrolling) and for atmosphere (the poster's colour field).
    readonly property int instant: 0
    readonly property int quick: 120
    readonly property int normal: 220
    readonly property int page: 320
    readonly property int slow: 520
    // Leaving should not hold attention, so it goes at four fifths the time.
    readonly property int leaving: 260

    // Entering decelerates into place, leaving accelerates away, and what
    // moves while staying is eased at both ends. Nothing overshoots: paper
    // does not bounce.
    readonly property int enterCurve: Easing.OutCubic
    readonly property int exitCurve: Easing.InCubic
    readonly property int moveCurve: Easing.InOutCubic

    // — layout —
    readonly property int sidebarWidth: 296
    readonly property int playerBarHeight: 84
    // The top strip — the sidebar's brand and the top bar — is the title bar.
    readonly property int titleBarHeight: 64
    readonly property int captionButtonWidth: 46
    readonly property int sidebarBreakpoint: 900
    readonly property int wideBreakpoint: 1180

    function tracking(px, em) { return px * em }
}
