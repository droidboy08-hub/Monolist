import QtQuick
import Monolist

// Cards in rows that fill the width, as the library lays out its albums: a
// search for albums, artists or playlists, and a shelf's "show all". The
// cards are ShelfCards, so an album's or a playlist's plays from its corner.
Flow {
    id: root

    property var items: []
    property string origin: ""
    signal cardActivated(var card)

    readonly property int columns: Math.max(1, Math.min(6, Math.floor((width + Theme.space6) / (188 + Theme.space6))))
    readonly property int cardWidth: Math.floor((width - (columns - 1) * Theme.space6) / columns)

    spacing: Theme.space6

    Repeater {
        model: root.items

        delegate: ShelfCard {
            required property var modelData

            width: root.cardWidth
            card: modelData
            origin: root.origin
            onActivated: function(card) { root.cardActivated(card) }
        }
    }
}
