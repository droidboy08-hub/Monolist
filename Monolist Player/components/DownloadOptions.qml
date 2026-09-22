import QtQuick
import Monolist
import Monolist.Backend

// How downloads are saved: the format, and whether non-music parts are cut.
// Shown on the Downloads page and in Settings; both write the same options.
Column {
    id: root

    visible: Downloads.available
    spacing: Theme.space3

    Text {
        text: "FORMAT"
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    // Negative spacing lets neighbouring segments share one 2px rule.
    Row {
        spacing: -Theme.ruleWidth
        enabled: Downloads.canConvert
        opacity: enabled ? 1 : 0.4

        ChoiceChip {
            label: "ORIGINAL · BEST"
            selected: Downloads.format === "original"
            onPicked: Downloads.format = "original"
        }
        ChoiceChip {
            label: "M4A"
            selected: Downloads.format === "m4a"
            onPicked: Downloads.format = "m4a"
        }
        ChoiceChip {
            label: "MP3"
            selected: Downloads.format === "mp3"
            onPicked: Downloads.format = "mp3"
        }
    }

    Text {
        width: parent.width
        text: Downloads.format === "m4a"
              ? "AAC in an .m4a file, about 128 kbps. Plays on Apple devices and older players."
              : Downloads.format === "mp3"
                ? "Re-encoded to MP3 at the highest VBR setting. Plays anywhere, at a small cost in quality."
                : "The stream exactly as YouTube publishes it, usually Opus at 130–160 kbps. Nothing is re-encoded."
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 12
        color: Theme.neutral700
    }

    ToggleRow {
        width: parent.width
        enabled: Downloads.canConvert
        opacity: enabled ? 1 : 0.4
        label: "Cut non-music parts"
        hint: "Removes intros, outros and skits that SponsorBlock users have marked in music videos."
        checked: Downloads.skipNonMusic
        onToggled: Downloads.skipNonMusic = !Downloads.skipNonMusic
    }
}
