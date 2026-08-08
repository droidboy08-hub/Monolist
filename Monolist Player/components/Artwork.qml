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

    Image {
        id: image
        anchors.fill: parent
        source: root.source
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
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
