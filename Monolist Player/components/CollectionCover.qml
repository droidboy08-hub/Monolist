import QtQuick
import Monolist

// The cover of a collection: its photograph, or a mosaic of four once a
// playlist has four different ones, or a plate when there is nothing to show.
// Photographs print black and white unless `colour` is set, as everywhere.
//
//   plate "liked"   the signal-red plate with a heart, for Liked songs
//   plate "new"     an outlined plate with a plus, for making a playlist
Rectangle {
    id: root

    property var artworks: []
    property bool colour: false
    property string plate: ""

    readonly property int count: artworks ? artworks.length : 0
    readonly property bool photographic: plate.length === 0 && count > 0

    color: plate === "liked" ? Theme.accent
         : plate === "new" ? "transparent"
         : Theme.neutral300
    border.width: plate === "new" ? Theme.ruleWidth : 0
    border.color: Theme.text
    clip: true

    Artwork {
        anchors.fill: parent
        visible: root.photographic && root.count < 4
        source: visible ? root.artworks[0] : ""
        placeholder: ""
        colour: root.colour
    }

    Grid {
        anchors.fill: parent
        visible: root.photographic && root.count >= 4
        columns: 2

        Repeater {
            model: parent.visible ? 4 : 0

            delegate: Artwork {
                required property int index
                width: root.width / 2
                height: root.height / 2
                source: root.artworks[index]
                placeholder: ""
                colour: root.colour
            }
        }
    }

    Icon {
        anchors.centerIn: parent
        visible: !root.photographic
        name: root.plate === "liked" ? "heart-filled"
            : root.plate === "new" ? "plus"
            : "list-music"
        width: Math.round(root.width * (root.plate === "liked" ? 0.36 : 0.28))
        height: width
        thickness: root.plate === "new" ? 1.5 : 2
        color: root.plate === "liked" ? Theme.accentForeground
             : root.plate === "new" ? Theme.text
             : Theme.neutral600
    }
}
