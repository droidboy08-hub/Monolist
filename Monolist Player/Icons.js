.pragma library

// Lucide (ISC) glyph outlines, transcribed as SVG path data so they can be
// stroked in any color by Qt Quick Shapes. viewBox is 24x24 for every icon.
var data = {
    "home":            [{ d: "M3 9 12 2 21 9 V20 a2 2 0 0 1 -2 2 H5 a2 2 0 0 1 -2 -2 Z" },
                        { d: "M9 22 V12 h6 v10" }],
    "search":          [{ d: "M19 11 A8 8 0 1 1 3 11 A8 8 0 1 1 19 11" },
                        { d: "M21 21 L16.65 16.65" }],
    "library":         [{ d: "M16 6 L20 20" }, { d: "M12 6 V20" }, { d: "M8 8 V20" }, { d: "M4 4 V20" }],
    "plus":            [{ d: "M5 12 H19" }, { d: "M12 5 V19" }],
    "arrow-left":      [{ d: "M12 19 L5 12 L12 5" }, { d: "M19 12 H5" }],
    "arrow-right":     [{ d: "M5 12 H19" }, { d: "M12 5 L19 12 L12 19" }],
    "play":            [{ d: "M6 3 L20 12 L6 21 Z", fill: true }],
    "pause":           [{ d: "M6 4 H10 V20 H6 Z", fill: true }, { d: "M14 4 H18 V20 H14 Z", fill: true }],
    "skip-back":       [{ d: "M19 20 L9 12 L19 4 Z", fill: true }, { d: "M5 19 V5" }],
    "skip-forward":    [{ d: "M5 4 L15 12 L5 20 Z", fill: true }, { d: "M19 5 V19" }],
    "shuffle":         [{ d: "M2 18 h1.4 c1.3 0 2.5 -0.6 3.3 -1.7 l6.1 -8.6 c0.7 -1.1 2 -1.7 3.3 -1.7 H22" },
                        { d: "M18 2 L22 6 L18 10" },
                        { d: "M2 6 h1.9 c1.5 0 2.9 0.9 3.6 2.2" },
                        { d: "M22 18 h-5.9 c-1.3 0 -2.6 -0.7 -3.3 -1.8 l-0.5 -0.8" },
                        { d: "M18 14 L22 18 L18 22" }],
    "repeat":          [{ d: "M17 2 L21 6 L17 10" },
                        { d: "M3 11 V10 a4 4 0 0 1 4 -4 H21" },
                        { d: "M7 22 L3 18 L7 14" },
                        { d: "M21 13 V14 a4 4 0 0 1 -4 4 H3" }],
    "repeat-1":        [{ d: "M17 2 L21 6 L17 10" },
                        { d: "M3 11 V10 a4 4 0 0 1 4 -4 H21" },
                        { d: "M7 22 L3 18 L7 14" },
                        { d: "M21 13 V14 a4 4 0 0 1 -4 4 H3" },
                        { d: "M11 10 h1 v4" }],
    "heart":           [{ d: "M19 14 c1.49 -1.46 3 -3.21 3 -5.5 A5.5 5.5 0 0 0 16.5 3 c-1.76 0 -3 0.5 -4.5 2 -1.5 -1.5 -2.74 -2 -4.5 -2 A5.5 5.5 0 0 0 2 8.5 c0 2.3 1.5 4.05 3 5.5 l7 7 Z" }],
    "clock":           [{ d: "M22 12 A10 10 0 1 1 2 12 A10 10 0 1 1 22 12" },
                        { d: "M12 6 V12 L16 14" }],
    "list-music":      [{ d: "M21 15 V6" },
                        { d: "M21 15 A2.5 2.5 0 1 1 16 15 A2.5 2.5 0 1 1 21 15" },
                        { d: "M12 12 H3" }, { d: "M16 6 H3" }, { d: "M12 18 H3" }],
    "monitor-speaker": [{ d: "M5.5 20 H8" },
                        { d: "M14 4 h6 a2 2 0 0 1 2 2 v12 a2 2 0 0 1 -2 2 h-6 a2 2 0 0 1 -2 -2 V6 a2 2 0 0 1 2 -2 Z" },
                        { d: "M8 6 H4 a2 2 0 0 0 -2 2 v6 a2 2 0 0 0 2 2 h4" },
                        { d: "M18 15 A1 1 0 1 1 16 15 A1 1 0 1 1 18 15" },
                        { d: "M17 9 h0.01" }],
    "volume-2":        [{ d: "M11 5 L6.5 8.5 H3 a1 1 0 0 0 -1 1 v5 a1 1 0 0 0 1 1 h3.5 L11 19 Z" },
                        { d: "M16 9 a5 5 0 0 1 0 6" },
                        { d: "M19.4 5.6 a9 9 0 0 1 0 12.8" }],
    "download":        [{ d: "M21 15 v4 a2 2 0 0 1 -2 2 H5 a2 2 0 0 1 -2 -2 v-4" },
                        { d: "M7 10 L12 15 L17 10" },
                        { d: "M12 15 V3" }],
    "menu":            [{ d: "M4 6 H20" }, { d: "M4 12 H20" }, { d: "M4 18 H20" }],
    "x":               [{ d: "M18 6 L6 18" }, { d: "M6 6 L18 18" }],
    "music":           [{ d: "M9 18 V5 l12 -2 v13" },
                        { d: "M9 18 A3 3 0 1 1 3 18 A3 3 0 1 1 9 18" },
                        { d: "M21 16 A3 3 0 1 1 15 16 A3 3 0 1 1 21 16" }],
    "check":           [{ d: "M20 6 L9 17 L4 12" }],
    "check-circle":    [{ d: "M22 12 A10 10 0 1 1 2 12 A10 10 0 1 1 22 12" },
                        { d: "M9 12 L11 14 L15 10" }],
    "rotate-ccw":      [{ d: "M3 12 a9 9 0 1 0 9 -9 a9.75 9.75 0 0 0 -6.74 2.74 L3 8" },
                        { d: "M3 3 v5 h5" }],
    "folder":          [{ d: "M20 20 a2 2 0 0 0 2 -2 V8 a2 2 0 0 0 -2 -2 h-7.9 a2 2 0 0 1 -1.69 -0.9 L9.6 3.9 A2 2 0 0 0 7.93 3 H4 a2 2 0 0 0 -2 2 v13 a2 2 0 0 0 2 2 Z" }],
    "trash":           [{ d: "M3 6 H21" },
                        { d: "M19 6 V20 c0 1 -1 2 -2 2 H7 c-1 0 -2 -1 -2 -2 V6" },
                        { d: "M8 6 V4 c0 -1 1 -2 2 -2 h4 c1 0 2 1 2 2 v2" },
                        { d: "M10 11 v6" },
                        { d: "M14 11 v6" }],
    "minimize":        [{ d: "M5 12 H19" }],
    "maximize":        [{ d: "M5 5 H19 V19 H5 Z" }],
    "restore":         [{ d: "M8 8 H19 V19 H8 Z" }, { d: "M5 16 V5 H16" }],
    "dots":            [{ d: "M6 12 A1 1 0 1 1 4 12 A1 1 0 1 1 6 12" },
                        { d: "M13 12 A1 1 0 1 1 11 12 A1 1 0 1 1 13 12" },
                        { d: "M20 12 A1 1 0 1 1 18 12 A1 1 0 1 1 20 12" }]
};

function subpath(name, i) {
    var paths = data[name];
    return (paths && paths[i]) ? paths[i].d : "";
}

function isFilled(name, i) {
    var paths = data[name];
    return !!(paths && paths[i] && paths[i].fill);
}
