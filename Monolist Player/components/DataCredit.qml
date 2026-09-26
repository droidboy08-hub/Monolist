import QtQuick
import Monolist

// The credit the recommendation data's licences ask for: who made each part,
// under which licence, and that it is for non-commercial use only. Each name
// links to its source and each licence to its text; the first words link to
// the repository the data is downloaded from, which carries the full notices.
// Shown under Recommendations and again in About.
Text {
    id: credit

    wrapMode: Text.WordWrap
    textFormat: Text.StyledText
    font.family: Theme.fontFamily
    font.pixelSize: 12
    color: Theme.neutral700
    linkColor: Theme.text
    text: "<a href=\"https://github.com/droidboy08-hub/Monolist-data\">Recommendation data</a>: "
          + "<a href=\"https://huggingface.co/datasets/GD-Studio/embeat_45m_spotify_tracks\">Embeat 45M</a> by GD-Studio "
          + "(<a href=\"https://creativecommons.org/licenses/by-nc/4.0/\">CC BY-NC 4.0</a>); "
          + "<a href=\"https://musicbrainz.org\">MusicBrainz</a> and "
          + "<a href=\"https://listenbrainz.org\">ListenBrainz</a> by the "
          + "<a href=\"https://metabrainz.org\">MetaBrainz Foundation</a> "
          + "(<a href=\"https://creativecommons.org/licenses/by-nc-sa/3.0/\">CC BY-NC-SA 3.0</a> / "
          + "<a href=\"https://creativecommons.org/publicdomain/zero/1.0/\">CC0</a>). "
          + "Non-commercial use only."

    onLinkActivated: function(link) { Qt.openUrlExternally(link) }

    HoverHandler {
        cursorShape: credit.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
}
