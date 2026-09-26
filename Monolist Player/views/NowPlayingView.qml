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
    // Whether the video's place is here. Main gives the picture one place at
    // a time: full screen takes it from here, and closing this view sends it
    // to the mini panel above the player bar.
    property bool videoHere: true
    signal closeRequested()
    signal fullscreenRequested()

    readonly property var track: Player.currentTrack
    readonly property bool hasTrack: track.title !== undefined
    readonly property string artwork: track.artwork !== undefined ? track.artwork : ""
    readonly property bool wide: width >= 980
    // The picture is up once mpv has a frame to give, not when it was asked
    // for: until then the cover stays, and the switch shows it is working.
    readonly property bool videoShowing: Player.videoPlaying
                                         && (wide ? videoSurface.showing : narrowSurface.showing)
    readonly property bool videoWaiting: Player.videoWanted && !videoShowing
    // What the switch says, wherever it is.
    readonly property string switchTip: !Player.videoAvailable ? "This song has no video"
                                      : videoShowing ? "Show the cover"
                                      : "Play the video"

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
                    visible: root.videoHere && Player.videoPlaying
                }

                Artwork {
                    id: cover
                    anchors.fill: parent
                    visible: !root.videoShowing
                    source: root.artwork
                    placeholder: ""
                    colour: true
                }

                // A double-click on the moving picture fills the screen with it.
                TapHandler {
                    onDoubleTapped: if (root.videoShowing) root.fullscreenRequested()
                }

                // The switches sit on the thing they change: the one between
                // the still and the moving picture, and, once the picture
                // plays, full screen beside it.
                Row {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.space3
                    spacing: Theme.space1

                    PlateButton {
                        visible: root.videoShowing
                        iconName: "fullscreen"
                        tip: "Full screen (F)"
                        onClicked: root.fullscreenRequested()
                    }
                    PlateButton {
                        id: videoToggle
                        available: Player.videoAvailable
                        iconName: root.videoWaiting ? "dots" : (root.videoShowing ? "image" : "video")
                        tip: root.switchTip
                        onClicked: Player.videoWanted = !Player.videoWanted
                    }
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
                // Each name opens its page, and closes this view to show it.
                ArtistLine {
                    width: parent.width
                    artist: root.hasTrack ? root.track.artist : ""
                    credits: root.hasTrack ? root.track.credits : undefined
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
                // Over the poster the ladder is drawn in the poster's own ink:
                // quiet until the pointer is on it, then full strength. Grey
                // and ink from the palette would be arbitrary here, because
                // what is behind them is whatever colour the cover is.
                iconColor: root.wide ? Qt.rgba(root.posterInk.r, root.posterInk.g,
                                               root.posterInk.b, 0.65)
                                     : Theme.neutral700
                hoverColor: root.wide ? root.posterInk : Theme.text
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

        // Narrow windows have no poster: the song goes above the lyrics, with
        // the video switch at the end of its line, and the picture, while it
        // plays, below it.
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
                width: parent.width - 64 - narrowToggle.width - Theme.space4 * 2
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
                ArtistLine {
                    width: parent.width
                    artist: root.hasTrack ? root.track.artist : ""
                    credits: root.hasTrack ? root.track.credits : undefined
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    color: Theme.neutral700
                }
            }

            // On paper, so a bare glyph like the rest of the page's: grey at
            // rest, red while the picture is on, greyed out for a song with
            // none rather than hidden.
            IconButton {
                id: narrowToggle
                anchors.verticalCenter: parent.verticalCenter
                side: 40
                iconSize: 18
                enabled: Player.videoAvailable
                hoverEnabled: enabled
                opacity: enabled ? 1 : 0.45
                iconName: root.videoWaiting ? "dots" : (root.videoShowing ? "image" : "video")
                iconColor: Player.videoWanted ? Theme.accent : Theme.neutral700
                onClicked: Player.videoWanted = !Player.videoWanted
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: root.switchTip
            }
        }

        // The picture in a narrow window: the full width of the page, at its
        // own shape, and no taller than half the page, so the lyrics keep
        // room below. Laid out as soon as the video starts loading, with the
        // cover in it until the first frame, so the picture never arrives as
        // a black box and nothing below jumps twice.
        Item {
            id: narrowStage
            visible: !root.wide && Player.videoPlaying
            anchors.top: compactHead.bottom
            anchors.topMargin: Theme.space6
            x: Theme.space8
            readonly property real room: Math.max(120, (side.height - compactHead.height) * 0.5)
            width: Math.min(parent.width - Theme.space8 * 2, room * narrowSurface.aspectRatio)
            height: visible ? Math.round(width / narrowSurface.aspectRatio) : 0
            clip: true

            VideoSurface {
                id: narrowSurface
                anchors.fill: parent
                visible: root.videoHere && Player.videoPlaying
            }

            Artwork {
                anchors.fill: parent
                visible: !root.videoShowing
                source: root.artwork
                placeholder: ""
                colour: true
            }

            TapHandler {
                onDoubleTapped: if (root.videoShowing) root.fullscreenRequested()
            }

            PlateButton {
                visible: root.videoShowing
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space3
                iconName: "fullscreen"
                tip: "Full screen (F)"
                onClicked: root.fullscreenRequested()
            }
        }

        Item {
            id: paneHead
            anchors.top: narrowStage.visible ? narrowStage.bottom
                       : compactHead.visible ? compactHead.bottom : parent.top
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
