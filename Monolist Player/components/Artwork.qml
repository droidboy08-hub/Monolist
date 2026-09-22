import QtQuick
import QtQuick.Effects
import Monolist

// Cover slot. Photographs print black and white in this system, so any
// supplied artwork is desaturated; with no source it stays a flat plate.
// `colour` lets a cover bloom into colour: under the pointer, or when it is
// the cover a whole page is about.
Rectangle {
    id: root

    property string source: ""
    property string placeholder: "Album art"
    property bool colour: false

    color: Theme.neutral300
    clip: true

    // Routed through the "artwork" image provider rather than loading the URL
    // directly: that gives an off-thread decode, a 256 MB disk cache that
    // survives restarts, and downsampling to the displayed size instead of
    // holding a 1280px thumbnail in memory for a 52px slot.
    Image {
        id: image
        anchors.fill: parent
        // Percent-encoded: the provider id goes through URL parsing, which
        // collapses the "//" in "https://" and hands the provider a broken
        // address. The provider decodes it back.
        source: root.source.length > 0
                ? "image://artwork/" + encodeURIComponent(root.source)
                : ""
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
        saturation: root.colour ? 0.0 : -1.0
        visible: image.status === Image.Ready

        Behavior on saturation {
            NumberAnimation { duration: 240; easing.type: Easing.OutCubic }
        }
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
