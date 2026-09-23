import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// Now Playing, full size. At the left the poster, printed for this song: its
// cover, in colour, on a field of the cover's own colour, and the title set
// large. At the right, on paper, the lyrics lit line by line, or what plays
// next. The player bar stays below, so the controls stay where they were.
Rectangle {
    id: root

    // "lyrics" or "queue"
    property string pane: "lyrics"
    signal closeRequested()

    readonly property var track: Player.currentTrack
    readonly property bool hasTrack: track.title !== undefined
    readonly property string artwork: track.artwork !== undefined ? track.artwork : ""
    readonly property bool wide: width >= 980
    // The picture is up once mpv has a frame to give, not when it was asked
    // for: until then the cover stays, and the switch shows it is working.
    readonly property bool videoShowing: Player.videoPlaying && videoSurface.showing

    // The cover's colour as the field, signal red until it is known.
    property color field: Theme.accent
    // Ink or paper, whichever reads better on the field; paper when it is
    // close, as on the red poster. (Not "onField": a name starting "on" and a
    // capital is taken for a signal handler, and read as nothing.)
    readonly property color posterInk: contrast(field, Theme.bg) * 1.1 >= contrast(field, Theme.text)
                                       ? Theme.bg : Theme.text

    color: Theme.bg

    // Relative luminance and contrast ratio, as WCAG defines them.
    function luminance(c) {
        function linear(v) { return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
        return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b)
    }
    function contrast(a, b) {
        var la = luminance(a), lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    // A field to print on: never so pale it fades into the paper, nor so dark
    // it reads as black.
    function fieldFrom(c) {
        var hue = c.hslHue < 0 ? 0 : c.hslHue
        return Qt.hsla(hue, Math.min(1, c.hslSaturation * 1.1), Math.max(0.24, Math.min(0.6, c.hslLightness)), 1)
    }

    onArtworkChanged: if (artwork.length > 0) CoverPalette.request(artwork)
    Component.onCompleted: if (artwork.length > 0) CoverPalette.request(artwork)

    Connections {
        target: CoverPalette
        function onColourReady(source, colour) {
            if (source === root.artwork)
                root.field = root.fieldFrom(colour)
        }
    }

    // Atmosphere, not information: nobody waits for the field to finish
    // changing, so it can take its time.
    Behavior on field {
        ColorAnimation { duration: Theme.slow; easing.type: Theme.enterCurve }
    }

    // Swallows clicks, so nothing underneath takes them.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    // — the poster —
    Rectangle {
        id: poster
        visible: root.wide
        width: visible ? Math.round(Math.min(root.width * 0.46, (root.height - Theme.titleBarHeight) * 0.86 + Theme.space8 * 2)) : 0
        height: parent.height
        color: root.field

        Column {
            id: posterContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.space8
            spacing: Theme.space6

            // The cover, or the same song moving. A video is 16:9 where a
            // cover is square, so the plate keeps its width and loses height
            // rather than showing the picture in black bars.
            Item {
                id: stage

                readonly property int edge: Math.max(120, Math.min(posterContent.width,
                    poster.height - Theme.titleBarHeight - info.implicitHeight - Theme.space8 * 2 - Theme.space6))

                width: edge
                height: root.videoShowing ? Math.round(edge / videoSurface.aspectRatio) : edge

                // Under the cover, so the still stays up until the first
                // frame arrives and the picture never appears as a black box.
                VideoSurface {
                    id: videoSurface
                    anchors.fill: parent
                    visible: Player.videoPlaying
                }

                Artwork {
                    id: cover
                    anchors.fill: parent
                    visible: !root.videoShowing
                    source: root.artwork
                    placeholder: ""
                    colour: true
                }

                // The switch between the still and the moving picture sits on
                // the thing it changes. Paper on the artwork, ink under the
                // pointer, like the poster's own button.
                Rectangle {
                    id: videoToggle

                    readonly property bool waiting: Player.videoWanted && !root.videoShowing

                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.space3
                    width: 40
                    height: 40
                    color: toggleHover.hovered ? Theme.text : Theme.bg
                    opacity: Player.videoAvailable ? 1 : 0.45

                    Behavior on color {
                        enabled: !toggleHover.hovered
                        ColorAnimation { duration: Theme.quick }
                    }

                    Icon {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        name: videoToggle.waiting ? "dots" : (root.videoShowing ? "image" : "video")
                        color: toggleHover.hovered ? Theme.bg : Theme.text
                    }

                    HoverHandler {
                        id: toggleHover
                        enabled: Player.videoAvailable
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: Player.videoAvailable
                        onTapped: Player.videoWanted = !Player.videoWanted
                    }

                    ToolTip.visible: toggleHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: !Player.videoAvailable ? "This song has no video"
                                : root.videoShowing ? "Show the cover"
                                : "Play the video"
                }
            }

            Column {
                id: info
                width: parent.width
                spacing: Theme.space2

                Text {
                    width: parent.width
                    text: root.hasTrack ? root.track.title : ""
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: poster.width >= 520 ? 48 : 36
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(poster.width >= 520 ? 48 : 36, -0.03)
                    lineHeight: 0.95
                    lineHeightMode: Text.ProportionalHeight
                    color: root.posterInk
                }
                Text {
                    width: parent.width
                    text: root.hasTrack ? root.track.artist : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 20
                    font.weight: Theme.weightMedium
                    color: root.posterInk
                }
                Text {
                    visible: text.length > 0
                    width: parent.width
                    text: root.hasTrack && root.track.album ? root.track.album.toUpperCase() : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.14)
                    color: root.posterInk
                    opacity: 0.8
                }
            }
        }
    }

    // — the strip along the top: the window's title bar here too —
    Item {
        id: strip
        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.titleBarHeight

        WindowDragArea {
            anchors.fill: parent
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space6 + Chrome.nativeButtonsInset
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space3

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "chevron-down"
                iconSize: 20
                iconColor: root.wide ? root.posterInk : Theme.text
                onClicked: root.closeRequested()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Close (Esc)"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "NOW PLAYING"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.18)
                color: root.wide ? root.posterInk : Theme.text
            }
        }

        // Under the paper side only; the poster runs to the top.
        Rectangle {
            anchors.bottom: parent.bottom
            x: poster.width
            width: parent.width - poster.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        WindowButtons {
            visible: !Chrome.nativeButtons
            anchors.top: parent.top
            anchors.right: parent.right
            height: parent.height - Theme.ruleWidth
        }
    }

    // — the paper side —
    Item {
        id: side
        anchors.left: poster.right
        anchors.right: parent.right
        anchors.top: strip.bottom
        anchors.bottom: parent.bottom

        // Narrow windows have no poster: the song goes above the lyrics.
        Row {
            id: compactHead
            visible: !root.wide
            x: Theme.space8
            y: Theme.space6
            width: parent.width - Theme.space8 * 2
            height: visible ? 64 : 0
            spacing: Theme.space4

            Artwork {
                width: 64
                height: 64
                source: root.artwork
                placeholder: ""
                colour: true
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 64 - Theme.space4
                spacing: 2

                Text {
                    width: parent.width
                    text: root.hasTrack ? root.track.title : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 22
                    font.weight: Theme.weightBlack
                    color: Theme.text
                }
                Text {
                    width: parent.width
                    text: root.hasTrack ? root.track.artist : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.neutral700
                }
            }
        }

        Item {
            id: paneHead
            anchors.top: compactHead.visible ? compactHead.bottom : parent.top
            anchors.topMargin: Theme.space6
            x: Theme.space8
            width: parent.width - Theme.space8 * 2
            height: 32

            // Negative spacing lets neighbouring segments share one 2px rule.
            Row {
                spacing: -Theme.ruleWidth

                ChoiceChip {
                    label: "LYRICS"
                    selected: root.pane === "lyrics"
                    onPicked: root.pane = "lyrics"
                }
                ChoiceChip {
                    label: "UP NEXT · " + Player.queue.upcomingCount
                    selected: root.pane === "queue"
                    onPicked: root.pane = "queue"
                }
            }

            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.pane === "lyrics" && Lyrics.source.length > 0
                text: (Lyrics.state === "plain" ? "NOT TIME-SYNCED · " : "") + "LYRICS: " + Lyrics.source.toUpperCase()
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.1)
                color: Theme.neutral600
            }
        }

        LyricsPane {
            visible: root.pane === "lyrics"
            anchors.top: paneHead.bottom
            anchors.topMargin: Theme.space6
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.leftMargin: Theme.space8 - gutter
            anchors.right: parent.right
            anchors.rightMargin: Theme.space6
        }

        QueuePanel {
            visible: root.pane === "queue"
            anchors.top: paneHead.bottom
            anchors.topMargin: Theme.space2
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.leftMargin: Theme.space2
            anchors.right: parent.right
            showHead: false
            showRule: false
            color: "transparent"
        }
    }
}
