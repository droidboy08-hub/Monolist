import QtQuick
import Monolist
import Monolist.Backend

// An artist line in which each name is a link to that artist's page.
//
// Plain type at rest, like any other line of the page: a list whose every
// credit was underlined or coloured would be a page of links rather than a
// list of songs. The name under the pointer takes a 1px rule in the line's
// own colour, which is how a printed page marks a reference; it arrives at
// once and fades over `quick` (DESIGN 2.6). A joint credit links each name
// to its own page ("Bruno Mars" and "Lady Gaga" separately), and the words
// between them are not links.
//
// Deliberately one plain Text rather than rich text with anchors: it elides
// like every other line, costs what a Text costs in a list of hundreds, and
// the pieces are found under the pointer by measuring them.
Text {
    id: root

    // The line as written, and the track's own pieces of it (its `credits`
    // role) where the list kept them. Without them the names are found by
    // Artists, from what YouTube Music has linked before.
    property string artist: ""
    property var credits: undefined
    // Anything after the names that is not one ("  —  Album"), set alike.
    property string suffix: ""
    // Off where the line must stay plain.
    property bool linksEnabled: true
    // "artist", or "page" for a line that is an album's title: then `pageId`
    // is what it opens, and without one it is plain.
    property string opens: "artist"
    property string pageId: ""

    // [{ text, id, link }]
    readonly property var pieces: artist.length === 0 ? []
                                : opens === "page" ? [{ text: artist, id: pageId, link: pageId.length > 0 }]
                                : Artists.credits(artist, credits)
    // The name under the pointer, or -1.
    readonly property int hoveredPiece: linksEnabled && pointer.hovered ? pieceAt(pointer.point.position.x) : -1

    // Where each piece starts, measured in this line's own font. Summed
    // piece by piece, which differs from the laid-out line only by the
    // kerning across a boundary — a fraction of a pixel.
    function offsets() {
        var edges = [0]
        for (var i = 0; i < pieces.length; ++i)
            edges.push(edges[i] + metrics.advanceWidth(pieces[i].text))
        return edges
    }

    function pieceAt(x) {
        if (x < 0 || x > width)
            return -1
        var edges = offsets()
        for (var i = 0; i < pieces.length; ++i) {
            if (x >= edges[i] && x < edges[i + 1])
                return pieces[i].link ? i : -1
        }
        return -1
    }

    function open(index) {
        var piece = pieces[index]
        if (!piece || !piece.link)
            return
        if (opens === "page")
            Nav.openPage(piece.id)
        else
            Nav.openArtist(piece.text, piece.id)
    }

    text: artist + suffix
    elide: Text.ElideRight
    maximumLineCount: 1

    FontMetrics {
        id: metrics
        font: root.font
    }

    // The rule under the name the pointer is on. Kept where it was while it
    // fades, so it leaves from the name it was under.
    Rectangle {
        id: rule
        property real from: 0
        property real to: 0
        x: from
        width: Math.max(0, Math.min(to, root.width) - from)
        y: root.baselineOffset + 2
        height: 1
        color: root.color
        opacity: root.hoveredPiece >= 0 ? 1 : 0

        Behavior on opacity {
            enabled: root.hoveredPiece < 0
            NumberAnimation { duration: Theme.quick }
        }
    }

    onHoveredPieceChanged: {
        if (hoveredPiece < 0)
            return
        var edges = offsets()
        rule.from = edges[hoveredPiece]
        rule.to = edges[hoveredPiece + 1]
    }

    // Passive: the row under this line keeps its own hover, and its pointer.
    HoverHandler {
        id: pointer
        enabled: root.linksEnabled
    }
    // The hand, over a name only; anywhere else the pointer is whatever the
    // row under the line makes it.
    HoverHandler {
        enabled: root.hoveredPiece >= 0
        cursorShape: Qt.PointingHandCursor
    }

    // A press on a name is taken here, so the row under it does not also
    // play; anywhere else it is let through to the row.
    MouseArea {
        anchors.fill: parent
        enabled: root.linksEnabled
        acceptedButtons: Qt.LeftButton
        onPressed: function(mouse) { mouse.accepted = root.pieceAt(mouse.x) >= 0 }
        onClicked: function(mouse) { root.open(root.pieceAt(mouse.x)) }
    }
}
