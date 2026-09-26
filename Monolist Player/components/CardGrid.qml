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

    // More cards at the end (a "show all" loading its next part): added to
    // the ones here, which stay as they are, covers and all. A new `items`
    // starts the grid again.
    function append(more) {
        for (let i = 0; i < more.length; ++i)
            cards.append({ card: more[i] })
    }

    function reset() {
        cards.clear()
        append(root.items || [])
    }

    onItemsChanged: reset()
    Component.onCompleted: if (cards.count === 0) reset()

    spacing: Theme.space6

    // A model of its own rather than `items` itself, so appending does not
    // make every card again.
    ListModel { id: cards }

    Repeater {
        model: cards

        delegate: ShelfCard {
            required property var model

            width: root.cardWidth
            card: model.card
            origin: root.origin
            onActivated: function(card) { root.cardActivated(card) }
        }
    }
}
