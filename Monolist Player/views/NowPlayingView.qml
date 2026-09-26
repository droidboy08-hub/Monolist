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

    // Closing sits with the window buttons, on whichever side the system
    // keeps them: the hand is already there to deal with the window, and it
    // should not have to cross the screen to put the player away. A plate
    // with a word on it, not a bare glyph, so it is not read as one of the
    // window's own buttons.
    component CloseButton: Button {
        id: closeButton

        // The ink it is printed in: the palette's on paper, the poster's own
        // over the colour field.
        property color ink: Theme.text
        property bool overField: false
        // The full height of the bar, like Windows' caption buttons beside it.
        property bool tall: false

        height: tall ? parent.height : 36
        leftPadding: Theme.space3 + 2
        rightPadding: Theme.space4
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        Accessible.name: "Close Now Playing"

        background: Rectangle {
            color: closeButton.overField
                   ? Qt.rgba(closeButton.ink.r, closeButton.ink.g, closeButton.ink.b,
                             closeButton.down ? 0.34 : closeButton.hovered ? 0.26 : 0.16)
                   : (closeButton.down ? Theme.neutral500 : closeButton.hovered ? Theme.neutral300 : Theme.surface)
        }

        contentItem: Row {
            spacing: Theme.space2 + 2

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                thickness: 2.4
                name: "chevron-down"
                color: closeButton.ink
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "CLOSE"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.14)
                color: closeButton.ink
            }
            // The key that does the same, printed on the button rather than
            // hidden in a tooltip.
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: escLabel.implicitWidth + 10
                height: escLabel.implicitHeight + 4
                color: "transparent"
                border.width: 1.5
                border.color: Qt.rgba(closeButton.ink.r, closeButton.ink.g, closeButton.ink.b, 0.7)

                Text {
                    id: escLabel
                    anchors.centerIn: parent
                    text: "ESC"
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(10, 0.08)
                    color: Qt.rgba(closeButton.ink.r, closeButton.ink.g, closeButton.ink.b, 0.8)
                }
            }
        }

        HoverHandler { cursorShape: Qt.PointingHandCursor }

        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: "Close Now Playing (Esc)"
    }

    // What sets the close button apart from the window's own: a 2px rule the
    // height of the bar beside square buttons, a short tinted one beside
    // round ones.
    component ClusterRule: Item {
        property color ink: Theme.text

        width: Theme.ruleWidth
        height: parent.height

        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: Chrome.roundButtons ? 20 : parent.height
            color: Chrome.roundButtons ? Qt.rgba(parent.ink.r, parent.ink.g, parent.ink.b, 0.25)
                                       : Theme.divider
        }
    }

    // One side of the cover / video switch: ChoiceChip's switch, printed in
    // the poster's ink and with a glyph, over the colour field.
    component PictureChoice: Rectangle {
        id: choice

        property string label: ""
        property string iconName: ""
        property bool selected: false
        property bool available: true
        property string hint: ""
        property color ink: Theme.text
        property color field: Theme.accent
        signal picked()

        implicitWidth: choiceRow.implicitWidth + Theme.space3 * 2
        implicitHeight: 32
        opacity: available ? 1 : 0.45
        color: selected ? ink : (choiceHover.hovered && available ? Qt.rgba(ink.r, ink.g, ink.b, 0.14)
                                                                  : "transparent")
        border.width: Theme.ruleWidth
        border.color: ink

        Accessible.role: Accessible.RadioButton
        Accessible.name: label
        Accessible.checked: selected

        Behavior on color {
            enabled: !choice.selected && !choiceHover.hovered
            ColorAnimation { duration: Theme.quick }
        }

        Row {
            id: choiceRow
            anchors.centerIn: parent
            spacing: Theme.space2

            Icon {
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                thickness: 2.2
                name: choice.iconName
                color: choice.selected ? choice.field : choice.ink
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: choice.label
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.14)
                color: choice.selected ? choice.field : choice.ink
            }
        }

        // Still under the pointer when there is nothing to switch to, so the
        // tooltip can say why.
        HoverHandler {
            id: choiceHover
            cursorShape: choice.available && !choice.selected ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
        TapHandler {
            enabled: choice.available && !choice.selected
            onTapped: choice.picked()
        }

        ToolTip.visible: choiceHover.hovered && hint.length > 0
        ToolTip.delay: 400
        ToolTip.text: hint
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
            // cover is square, so it takes the poster's full width where the
            // height allows, rather than the cover's plate with black bars.
            Item {
                id: stage

                // Room above the song's name, below the strip.
                readonly property int room: poster.height - Theme.titleBarHeight - info.implicitHeight
                                            - Theme.space8 * 2 - Theme.space6
                readonly property int edge: Math.max(120, Math.min(posterContent.width, room))
                readonly property real aspect: videoSurface.aspectRatio > 0 ? videoSurface.aspectRatio : 16 / 9
                readonly property int videoWidth: Math.max(120, Math.min(posterContent.width,
                                                                         Math.round(room * aspect)))

                width: root.videoShowing ? videoWidth : edge
                height: root.videoShowing ? Math.round(videoWidth / aspect) : edge

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

        // macOS keeps its traffic lights at the left, and some Linux
        // desktops their buttons: closing goes there with them.
        readonly property bool closeAtLeft: Chrome.nativeButtons || Chrome.buttonsOnLeft
        // Over the poster things are drawn in the poster's own ink: grey and
        // ink from the palette would be arbitrary there, because what is
        // behind them is whatever colour the cover is.
        readonly property color leftInk: root.wide ? root.posterInk : Theme.text

        anchors.left: parent.left
        anchors.right: parent.right
        height: Theme.titleBarHeight

        WindowDragArea {
            anchors.fill: parent
        }

        // Under the paper side only; the poster runs to the top.
        Rectangle {
            anchors.bottom: parent.bottom
            x: poster.width
            width: parent.width - poster.width
            height: Theme.ruleWidth
            color: Theme.divider
        }

        // — at the left: the window buttons and close, where the system
        // keeps its buttons there, and the page's name —
        Row {
            id: leftCluster
            anchors.left: parent.left
            anchors.leftMargin: Chrome.nativeButtons ? Chrome.nativeButtonsInset
                              : Chrome.buttonsOnLeft ? 0 : Theme.space6
            anchors.top: parent.top
            height: parent.height - Theme.ruleWidth
            spacing: Theme.space3

            WindowButtons {
                visible: Chrome.buttonsOnLeft && !Chrome.nativeButtons
                height: parent.height
                rightPadding: 0
                ink: strip.leftInk
            }
            ClusterRule {
                visible: Chrome.buttonsOnLeft && !Chrome.nativeButtons
                ink: strip.leftInk
            }
            CloseButton {
                visible: strip.closeAtLeft
                anchors.verticalCenter: parent.verticalCenter
                ink: strip.leftInk
                overField: root.wide
                onClicked: root.closeRequested()
            }
        }

        // Given up before it would run into the picture switch.
        Text {
            anchors.left: leftCluster.right
            anchors.leftMargin: strip.closeAtLeft ? Theme.space3 : 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -Theme.ruleWidth / 2
            visible: !root.wide || x + implicitWidth <= pictureSwitch.x - Theme.space4
            text: "NOW PLAYING"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.18)
            color: strip.leftInk
        }

        // — the switch between the still and the moving picture —
        // In the strip at the poster's right edge, directly above the picture
        // it changes, and always there with both sides, so it never moves:
        // a song without a video dims its VIDEO side and says so.
        Row {
            id: pictureSwitch

            readonly property bool waiting: Player.videoWanted && !root.videoShowing

            visible: root.wide
            x: poster.width - Theme.space8 - width
            anchors.verticalCenter: parent.verticalCenter
            spacing: -Theme.ruleWidth

            PictureChoice {
                label: "COVER"
                iconName: "image"
                ink: root.posterInk
                field: root.field
                selected: !Player.videoWanted || !Player.videoAvailable
                hint: selected ? "" : "Show the cover (V)"
                onPicked: Player.videoWanted = false
            }
            PictureChoice {
                label: "VIDEO"
                iconName: pictureSwitch.waiting ? "dots" : "video"
                ink: root.posterInk
                field: root.field
                available: Player.videoAvailable
                selected: Player.videoWanted && Player.videoAvailable
                hint: !Player.videoAvailable ? "This song has no video"
                    : pictureSwitch.waiting ? "Loading the video"
                    : selected ? "" : "Play the video (V)"
                onPicked: Player.videoWanted = true
            }
        }

        // — at the right: close, then the window buttons —
        Row {
            visible: !strip.closeAtLeft
            anchors.top: parent.top
            anchors.right: parent.right
            height: parent.height - Theme.ruleWidth
            spacing: Chrome.roundButtons ? Theme.space3 : 0

            CloseButton {
                anchors.verticalCenter: parent.verticalCenter
                tall: !Chrome.roundButtons
                onClicked: root.closeRequested()
            }
            ClusterRule {}
            WindowButtons {
                height: parent.height
                leftPadding: 0
            }
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
