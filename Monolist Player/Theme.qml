pragma Singleton
import QtQuick

// Modernist tokens, transcribed 1:1 from the design system stylesheet, with
// a dark printing of each (ink and paper swapped, the red kept) and the
// playing song's colour washed through the paper (ambient).
QtObject {
    id: theme

    // — appearance —
    // "light", "dark", or "system" for Windows' own choice (Settings >
    // Personalisation > Colours > app mode). Set from the settings at launch
    // and by Settings > Appearance (Main.qml keeps it).
    property string mode: "system"
    readonly property bool dark: mode === "dark"
                                 || (mode === "system" && Application.styleHints.colorScheme === Qt.ColorScheme.Dark)

    // The playing song's colour, from its cover (Main.qml, eased there), or
    // transparent: nothing playing, a cover still being measured, or ambient
    // colour turned off. The paper and the surfaces take a little of it.
    property color ambient: "transparent"
    // Settings > Appearance's "Ambient colour", kept in the settings.
    property bool ambientEnabled: true
    // Settings > Appearance's "Monochrome covers", kept in the settings: on,
    // pictures print black and white until they are the subject (Artwork's
    // `colour`); off, every picture is in colour throughout.
    property bool monochrome: true
    readonly property real ambientShare: ambient.a * (dark ? 0.16 : 0.11)
    function ambientOver(base, share) {
        return share > 0 ? Qt.tint(base, Qt.rgba(ambient.r, ambient.g, ambient.b, share)) : base
    }

    // Ink and paper as printed, whatever the appearance: for marks over a
    // picture, which is the same picture in either.
    readonly property color ink: "#201e1d"
    readonly property color paper: "#f3f2f2"
    // Grey type on ink, and the poster red, as printed in either.
    readonly property color inkGrey: "#9b9797"
    readonly property color red: "#ec3013"

    // — color —
    readonly property color bg: ambientOver(dark ? "#141312" : "#f3f2f2", ambientShare)
    readonly property color surface: ambientOver(dark ? "#1e1c1b" : "#eae9e9", ambientShare * 1.3)
    readonly property color text: dark ? "#ebe9e8" : "#201e1d"
    readonly property color accent: dark ? "#f0401f" : "#ec3013"
    readonly property color divider: dark ? "#66ebe9e8" : "#66201e1d"     // text at 40%
    readonly property color hairline: dark ? "#29ebe9e8" : "#33201e1d"    // 1px table rules

    // Grey steps away from the paper: 300 a plate, 700 the secondary type.
    readonly property color neutral300: dark ? "#302d2c" : "#d7d3d3"
    readonly property color neutral500: dark ? "#6e6a68" : "#9b9797"
    readonly property color neutral600: dark ? "#8c8785" : "#7d7979"
    readonly property color neutral700: dark ? "#aca7a5" : "#605d5d"

    // 600 a red fill under the pointer; 700 red type, darker on paper and
    // lighter on ink, so it reads as red on either.
    readonly property color accent600: dark ? "#d42a0e" : "#dd2b0f"
    readonly property color accent700: dark ? "#ff6e52" : "#ae1800"
    readonly property color accentForeground: "#f3f2f2"
    // A red glyph under the pointer: it brightens, which on paper is the
    // deeper red and on ink the lighter one.
    readonly property color accentGlyphHover: dark ? "#ff6e52" : "#dd2b0f"

    readonly property color ghostHover: "#1aec3013"   // accent 10%
    readonly property color ghostActive: "#2eec3013"  // accent 18%
    readonly property color rowHover: dark ? "#0febe9e8" : "#0a201e1d"     // text 4-6%

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
