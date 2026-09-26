import QtQuick
import Monolist
import Monolist.Backend
import "../components"

// A suggestion shelf's SHOW ALL: the shelf's own rows, then more from the
// same place — the same song, the same taste, the same artist's neighbours,
// the same country's list — as the reader nears the end, until that place
// has nothing more worth suggesting. Every row answers as it does on the
// shelf: pressed, it plays; its X turns it down; its dots are the song menu.
ScrollPage {
    id: root

    readonly property var info: Recs.more

    contentHeight: column.implicitHeight

    // A model of its own rather than Recs.moreRows itself, so a page of more
    // adds its rows to the end instead of making every row again.
    ListModel { id: songs }

    function entryOf(row) {
        return {
            title: row.title || "",
            artist: row.artist || "",
            lengthMs: row.lengthMs !== undefined ? row.lengthMs : -1,
            row: row.row !== undefined ? row.row : -1
        }
    }
    function reset() {
        songs.clear()
        const rows = Recs.moreRows
        for (let i = 0; i < rows.length; ++i)
            songs.append(entryOf(rows[i]))
    }
    function songAt(index) {
        const entry = songs.get(index)
        return { title: entry.title, artist: entry.artist, lengthMs: entry.lengthMs, row: entry.row }
    }
    function openMenu(index) {
        const song = songAt(index)
        Menus.openTrack({ title: song.title, artist: song.artist }, {
            suggestion: true,
            resolve: function(action, argument) {
                Recs.resolve(song, action + "|" + argument)
            }
        })
    }

    // As a long playlist does (PageView): the next part while the end is
    // still a screen away.
    function loadMoreIfNear() {
        if (visible && !Recs.moreLoading && !Recs.moreExhausted && contentY + height * 2 >= contentHeight)
            Recs.loadMore()
    }
    onContentYChanged: loadMoreIfNear()
    onContentHeightChanged: loadMoreIfNear()
    onVisibleChanged: loadMoreIfNear()

    Component.onCompleted: reset()

    Connections {
        target: Recs
        // Another shelf's list: from the top.
        function onMoreChanged() {
            root.reset()
            root.contentY = 0
        }
        function onMoreAppended(rows) {
            for (let i = 0; i < rows.length; ++i)
                songs.append(root.entryOf(rows[i]))
        }
        // Something on it turned down, or brought back.
        function onRowsEdited(shelf) {
            if (shelf === -1)
                root.reset()
        }
        function onMoreStateChanged() { root.loadMoreIfNear() }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        Column {
            width: parent.width
            spacing: Theme.space1

            Text {
                width: parent.width
                text: "SUGGESTED FOR YOU"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: Theme.neutral700
            }

            SectionHeader {
                width: parent.width
                number: "01"
                title: root.info.title !== undefined ? root.info.title : ""
                action: songs.count > 0 ? "PLAY ALL" : ""
                onActionTriggered: Recs.playAll(-1)
            }

            Text {
                visible: text.length > 0
                width: parent.width
                text: root.info.reason !== undefined ? root.info.reason : ""
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.neutral700
            }
        }

        Column {
            id: list
            width: parent.width

            Repeater {
                model: songs

                RecRow {
                    required property int index
                    required property var model

                    width: list.width
                    song: ({ title: model.title, artist: model.artist, lengthMs: model.lengthMs })
                    number: index + 1
                    onActivated: Recs.play(root.songAt(index))
                    onMenuRequested: root.openMenu(index)
                }
            }
        }

        // Returned to with Back or Forward after Search drew a new page
        // without this shelf on it.
        Text {
            visible: root.info.gone === true
            width: parent.width
            text: "This shelf is no longer on Search: the suggestions there have been drawn afresh since."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        Text {
            visible: Recs.moreLoading
            width: parent.width
            text: "Finding more…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        // The end says why it is the end, rather than simply stopping.
        Text {
            visible: Recs.moreExhausted && !Recs.moreLoading && songs.count > 0
            width: parent.width
            text: "That is everything close enough to suggest from here."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }
    }
}
