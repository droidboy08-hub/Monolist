import QtQuick
import Monolist

// The cover of a collection: its photograph, or a mosaic of four once a
// playlist has four different ones, or a plate when there is nothing to show.
// Photographs print black and white unless `colour` is set, as everywhere.
//
//   plate "liked"   the signal-red plate with a heart, for Liked songs
//   plate "new"     an outlined plate with a plus, for making a playlist
//   plate "import"  an outlined plate with an arrow in, for importing one
Rectangle {
    id: root

    property var artworks: []
    property bool colour: false
    property string plate: ""

    readonly property int count: artworks ? artworks.length : 0
    readonly property bool photographic: plate.length === 0 && count > 0
    readonly property bool outlined: plate === "new" || plate === "import"

    color: plate === "liked" ? Theme.accent
         : outlined ? "transparent"
         : Theme.neutral300
    border.width: outlined ? Theme.ruleWidth : 0
    border.color: Theme.text
    clip: true

    // The pictures are asked for once the cover is first in view, and kept
    // from then on. Not bound to `visible` itself, which reads false while
    // any parent is hidden: a page left dropped every picture on it, and
    // they were all fetched and decoded again when it came back.
    property bool seen: false
    onVisibleChanged: if (visible) seen = true
    Component.onCompleted: if (visible) seen = true

    Artwork {
        anchors.fill: parent
        visible: root.photographic && root.count < 4
        source: root.seen && root.photographic && root.count < 4 ? root.artworks[0] : ""
        placeholder: ""
        colour: root.colour
    }

    Grid {
        anchors.fill: parent
        visible: root.photographic && root.count >= 4
        columns: 2

        Repeater {
            model: root.seen && root.photographic && root.count >= 4 ? 4 : 0

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
            : root.plate === "import" ? "import"
            : "list-music"
        width: Math.round(root.width * (root.plate === "liked" ? 0.36 : 0.28))
        height: width
        thickness: root.outlined ? 1.5 : 2
        color: root.plate === "liked" ? Theme.accentForeground
             : root.outlined ? Theme.text
             : Theme.neutral600
    }
}
