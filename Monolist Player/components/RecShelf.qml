import QtQuick
import Monolist
import Monolist.Backend

// One shelf of suggestions: a heading, the reason it exists, and the songs.
// Its header offers what the shelf offers as a whole, as every shelf's does:
// PLAY ALL, and SHOW ALL for more from the same place.
Item {
    id: root

    property string title: ""
    property string reason: ""
    property var rows: []
    // Which of Recs.shelves this is: what PLAY ALL plays and SHOW ALL opens.
    property int shelfIndex: -1
    // Whether the shelf has somewhere to carry on from (Recs: `more`).
    property bool hasMore: true

    implicitHeight: body.implicitHeight
    // Every row turned down: nothing left to head.
    visible: rows.length > 0

    // The song menu, for a row that is still only a name: what needs the
    // song itself (Play next, Add to playlist, Copy link…) looks it up first,
    // as pressing the row does, and TrackMenu is handed the answer (Main).
    function openMenu(index) {
        const song = rows[index]
        if (!song)
            return
        Menus.openTrack({ title: song.title, artist: song.artist }, {
            suggestion: true,
            resolve: function(action, argument) {
                Recs.resolve(song, action + "|" + argument)
            }
        })
    }

    // A row turned down, or brought back by Undo, changes this shelf alone;
    // the page is not rebuilt around it.
    Connections {
        target: Recs
        function onRowsEdited(shelf) {
            if (shelf >= 0 && shelf === root.shelfIndex)
                root.rows = Recs.rowsOf(shelf)
        }
    }

    // A header link, set as SectionHeader and CardShelf set theirs.
    component HeaderLink: Text {
        id: link
        signal triggered()
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.12)
        color: linkHover.hovered ? Theme.accent700 : Theme.neutral700

        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: link.triggered() }
    }

    Column {
        id: body
        width: parent.width
        spacing: Theme.space3

        Item {
            width: parent.width
            height: heading.implicitHeight

            Text {
                id: heading
                width: Math.max(0, parent.width - links.width - Theme.space4)
                text: root.title
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 19
                font.weight: Theme.weightBlack
                color: Theme.text
            }

            Row {
                id: links
                anchors.right: parent.right
                anchors.baseline: heading.baseline
                baselineOffset: playAll.y + playAll.baselineOffset
                spacing: Theme.space4

                HeaderLink {
                    id: playAll
                    text: "PLAY ALL"
                    onTriggered: Recs.playAll(root.shelfIndex)
                }
                HeaderLink {
                    visible: root.hasMore
                    text: "SHOW ALL"
                    onTriggered: Nav.openSuggestions(root.shelfIndex)
                }
            }
        }

        Text {
            width: parent.width
            visible: root.reason.length > 0
            text: root.reason
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.neutral700
        }

        Column {
            id: list
            width: parent.width

            Repeater {
                model: root.rows

                RecRow {
                    required property var modelData
                    required property int index

                    width: list.width
                    song: modelData
                    number: index + 1
                    onActivated: Recs.play(modelData)
                    onMenuRequested: root.openMenu(index)
                }
            }
        }
    }
}
