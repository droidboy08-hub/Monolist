import QtQuick
import QtQuick.Effects
import Monolist

// Cover slot. Photographs print black and white in this system, so any
// supplied artwork is desaturated; with no source it stays a flat plate.
// `colour` lets a cover bloom into colour: under the pointer, or when it is
// the cover a whole page is about. A `radius` rounds the picture's corners
// as well as the plate's (a list row's small cover, TrackCover).
Rectangle {
    id: root

    property string source: ""
    property string placeholder: "Album art"
    property bool colour: false

    color: Theme.neutral300
    clip: true

    // A picture is shown (rather than the plate, waiting or with none).
    readonly property bool ready: image.status === Image.Ready

    // How many pixels this plate really covers on this screen.
    readonly property int wanted: Math.ceil(Math.max(width, height) * Screen.devicePixelRatio)

    // YouTube Music serves a cover at any size, its address saying which
    // ("=w544-h544"), so each plate asks for what it draws and no more: a
    // 40px row does not fetch a 1200px picture, and a full-size cover is not
    // a 544px one stretched. Buckets, so the same cover is not fetched at a
    // dozen sizes that differ by a pixel.
    readonly property string address: {
        if (source.length === 0 || wanted <= 0)
            return ""
        if (source.indexOf("googleusercontent.com") < 0)
            return source
        const buckets = [120, 240, 360, 544, 800, 1200, 1600]
        let size = buckets[buckets.length - 1]
        for (let i = 0; i < buckets.length; ++i) {
            if (wanted <= buckets[i]) {
                size = buckets[i]
                break
            }
        }
        return source.replace(/=w\d+-h\d+/, "=w" + size + "-h" + size)
    }

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
        source: root.address.length > 0
                ? "image://artwork/" + encodeURIComponent(root.address)
                : ""
        // In device pixels, so a cover is decoded at the size it is drawn.
        sourceSize.width: Math.max(1, root.wanted)
        sourceSize.height: Math.max(1, root.wanted)
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        visible: false
    }

    // The rounded corners, as a mask: clipping cuts square. A list cover's
    // (6px) is the one Main draws for all of them; any other radius, or
    // before that one is made, its own.
    readonly property bool sharedCorners: root.radius === 6 && root.width === 40 && root.height === 40
                                          && Nav.coverMask !== null
    Item {
        id: corners
        anchors.fill: parent
        visible: false
        layer.enabled: root.radius > 0 && !root.sharedCorners
        layer.smooth: true

        Rectangle {
            anchors.fill: parent
            radius: root.radius
            antialiasing: true
        }
    }

    MultiEffect {
        anchors.fill: parent
        source: image
        saturation: root.colour ? 0.0 : -1.0
        visible: image.status === Image.Ready
        maskEnabled: root.radius > 0
        maskSource: root.sharedCorners ? Nav.coverMask : corners
        // Soft at the curve, so the corners are not stepped.
        maskThresholdMin: 0.5
        maskSpreadAtMin: 1.0

        // Colour arriving means "this is the subject", so it is worth seeing
        // happen rather than switching.
        Behavior on saturation {
            NumberAnimation { duration: Theme.normal; easing.type: Theme.enterCurve }
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
