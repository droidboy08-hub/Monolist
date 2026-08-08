import QtQuick
import QtQuick.Effects
import Monolist

// Cover slot. Photographs print black and white in this system, so any
// supplied artwork is desaturated; with no source it stays a flat plate.
Rectangle {
    id: root

    property string source: ""
    property string placeholder: "Album art"

    color: Theme.neutral300
    clip: true

    // Routed through the "artwork" image provider rather than loading the URL
    // directly: that gives an off-thread decode, a 256 MB disk cache that
    // survives restarts, and downsampling to the displayed size instead of
    // holding a 1280px thumbnail in memory for a 52px slot.
    Image {
        id: image
        anchors.fill: parent
        source: root.source.length > 0 ? "image://artwork/" + root.source : ""
        sourceSize.width: Math.max(1, Math.ceil(root.width))
        sourceSize.height: Math.max(1, Math.ceil(root.height))
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        visible: false
    }

    MultiEffect {
        anchors.fill: parent
        source: image
        saturation: -1.0
        visible: image.status === Image.Ready
    }

    Text {
        anchors.centerIn: parent
        visible: image.status !== Image.Ready
        text: root.placeholder
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Theme.weightMedium
        color: Theme.neutral600
    }
}
