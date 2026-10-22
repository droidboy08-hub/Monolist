import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import Monolist
import Monolist.Backend
import "../components"

// Now Playing, full size: the owner's "Ambient stage" (2026-10-21). One calm
// field of the playing cover's own colour over the whole sheet, deep in dark
// and pale in light, everything on it printed in tints of that colour. At the
// left the cover, its corners softened and standing off the field on a soft
// shadow, the song's name under it and two small tags (the sound, and what
// plays next). At the right the lyrics set large, or what plays next. The
// player bar stays below, so the controls stay where they were.
Rectangle {
    id: root

    // "lyrics" or "queue"
    property string pane: "lyrics"
    // Whether the video's place is here. Main gives the picture one place at
    // a time: full screen takes it from here, and the picture-in-picture
    // button sends it to the small panel above the player bar. Main also
    // holds it back while this view slides in or out.
    property bool videoHere: true
    signal closeRequested()
    signal fullscreenRequested()
    signal pipRequested()

    readonly property var track: Player.currentTrack
    readonly property bool hasTrack: track.title !== undefined
    readonly property string artwork: track.artwork !== undefined ? track.artwork : ""
    // Wide enough for the cover beside the lyrics; narrower, the song heads
    // the page and the lyrics take its whole width.
    readonly property bool wide: width >= 1000
    // The picture is up once mpv has a frame to give, not when it was asked
    // for: until then the cover stays, and the switch shows it is working.
    readonly property bool videoShowing: Player.videoPlaying && videoSurface.showing
    readonly property bool videoWaiting: Player.videoWanted && !videoShowing
    readonly property string switchTip: videoWaiting ? "Loading the video"
                                      : Player.videoWanted ? "Show the cover (V)"
                                      : "Play the video (V)"

    // The sound as mpv reports it, "OPUS · 133 KBPS" (Player.streamQuality,
    // the player bar's badge), once both the codec and the rate are known.
    readonly property string quality: hasTrack && !Player.resolving && Player.streamQuality.length > 0
                                      ? Player.streamQuality.replace(/ (\d+)$/, " · $1 KBPS") : ""
    // The sound effects that are on ("SLOWED 0.85× + REVERB · HIGH BASS +6
    // DB"): they are what the song sounds like, so they are named beside it.
    readonly property string effects: hasTrack && Sound.active && !Sound.failed
                                      ? Sound.summary.toUpperCase() : ""
    // What plays next, or null. Read through the counts, so the tag follows
    // a reorder, an addition or a song ending.
    readonly property var nextTrack: {
        const upcoming = Player.queue.upcomingCount
        const at = Player.queue.currentIndex + 1
        return upcoming > 0 && at < Player.queue.count ? Player.queue.get(at) : null
    }
    readonly property bool showUpNext: hasTrack && nextTrack !== null

    // — the colour field —
    // The cover's colour as CoverPalette measured it; kept from the last song
    // until the next one's is known, transparent for a song with no cover.
    property color coverColour: "transparent"
    function measure() {
        if (artwork.length > 0)
            CoverPalette.request(artwork)
        else
            coverColour = "transparent"
    }
    onArtworkChanged: measure()
    Component.onCompleted: measure()
    Connections {
        target: CoverPalette
        function onColourReady(source, colour) {
            if (source === root.artwork)
                root.coverColour = colour
        }
    }

    // The cover's hue, calm: its strength held to a third, deep in dark
    // (#18222d for a blue cover) and pale in light (#dde6ef). A grey cover,
    // or none, gives a grey field.
    function fieldFrom(c, dark) {
        const known = c.a > 0
        const hue = known && c.hslHue >= 0 ? c.hslHue : 0
        let strength = known ? Math.min(0.34, c.hslSaturation * 0.85) : 0
        if (strength < 0.06)
            strength = 0
        return Qt.hsla(hue, strength, dark ? 0.135 : 0.9, 1)
    }
    property color field: fieldFrom(coverColour, Theme.dark)
    // Atmosphere, not information: nobody waits for it, so it takes its time.
    Behavior on field {
        ColorAnimation { duration: Theme.slow; easing.type: Theme.enterCurve }
    }
    color: field

    // Relative luminance and contrast ratio, as WCAG defines them.
    function luminance(c) {
        function linear(v) { return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
        return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b)
    }
    function contrast(a, b) {
        const la = luminance(a), lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    // A tint of the field's own hue, at `share` of its strength, standing
    // `ratio` off it in contrast: lighter on a deep field, darker on a pale
    // one. Found by halving the lightness, so it holds on any cover's colour
    // rather than only on the blue the mockup was drawn with.
    function tone(f, ratio, share) {
        const hue = f.hslHue < 0 ? 0 : f.hslHue
        const strength = f.hslSaturation * share
        const lighter = Theme.dark
        let lo = lighter ? f.hslLightness : 0
        let hi = lighter ? 1 : f.hslLightness
        for (let i = 0; i < 14; ++i) {
            const mid = (lo + hi) / 2
            const enough = contrast(f, Qt.hsla(hue, strength, mid, 1)) >= ratio
            if (lighter === enough)
                hi = mid
            else
                lo = mid
        }
        return Qt.hsla(hue, strength, lighter ? hi : lo, 1)
    }
    // The mockup's inks, as contrasts against its field: the line being sung
    // and the titles in printed paper or ink; the artist and the notes
    // ("soft"); what is still to be sung ("next", readable); what has been
    // sung ("past", faded); the rules.
    readonly property color ink: Theme.dark ? Theme.paper : Theme.ink
    readonly property color soft: tone(field, Theme.dark ? 7.5 : 5.6, 0.55)
    readonly property color next: tone(field, Theme.dark ? 5.2 : 4.8, 0.55)
    readonly property color past: tone(field, Theme.dark ? 3.0 : 2.3, 0.5)
    readonly property color line: tone(field, Theme.dark ? 1.8 : 1.6, 0.7)
    // The pointer's tint on a control printed on the field.
    function tint(amount) { return Qt.rgba(ink.r, ink.g, ink.b, amount) }
    // The cover's shadow: black in dark; in light a deep shade of the field's
    // own hue, lighter, so the pale page is not smudged grey.
    readonly property color shadowFar: Theme.dark ? Qt.rgba(0, 0, 0, 0.5)
                                                  : Qt.hsla(field.hslHue < 0 ? 0 : field.hslHue,
                                                            Math.min(0.45, field.hslSaturation * 1.25), 0.19, 0.28)
    readonly property color shadowNear: Theme.dark ? Qt.rgba(0, 0, 0, 0.35)
                                                   : Qt.hsla(field.hslHue < 0 ? 0 : field.hslHue,
                                                             Math.min(0.45, field.hslSaturation * 1.25), 0.19, 0.18)

    // — measures —
    // As the mockup has them at 1440: 120 at either side and between the
    // cover and the lyrics, the cover 420 and 56 below the strip; less as the
    // window is smaller.
    readonly property int edge: Math.round(Math.max(Theme.space8, Math.min(120, width * 0.083)))
    readonly property int contentTop: Theme.titleBarHeight
                                      + Math.round(Math.max(Theme.space6, Math.min(56, (height - 600) / 4)))
    readonly property int columnWidth: Math.round(Math.min(420, (width - 3 * edge) * 0.45))
    readonly property int gap: 20
    // The cover is as large as the column, or as the height leaves room for
    // over the song's name and tags.
    readonly property int coverRoom: height - contentTop - gap - info.height
                                     - (tags.visible ? gap + tags.height : 0) - Theme.space6
    readonly property int coverSide: Math.max(120, Math.min(columnWidth, coverRoom))
    // Narrow: the song heads the page, a smaller cover beside its name.
    readonly property int narrowEdge: width >= 700 ? Theme.space8 : Theme.space6
    readonly property int headTop: Theme.titleBarHeight + Theme.space4
    readonly property int headCover: width >= 700 ? 96 : 72
    readonly property int pictureRadius: wide || videoShowing ? 10 : 8

    // Swallows clicks, so nothing underneath takes them. Only where nothing
    // of this view's own is under the pointer: a plain TapHandler over it
    // passes the press on below this, to the handlers under the view, so
    // Main also turns off what is under it while it is open.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }
    // Nothing under the view is hovered through it: no hand, tooltip or row
    // lit on the hidden page.
    HoverHandler { blocking: true }

    // — the picture: the cover, or the same song moving —
    // In its place in either layout. The video takes the cover's place at its
    // own 16:9, the column's width, with the same corners and shadow; the
    // cover stays up under it until the first frame arrives, so the picture
    // never appears as a black box.
    Item {
        id: picture

        readonly property real aspect: videoSurface.aspectRatio > 0 ? videoSurface.aspectRatio : 16 / 9
        // Wide: the column's width, or less where the height is short.
        readonly property int wideVideoWidth: Math.max(160, Math.min(root.columnWidth,
                                                                     Math.round(root.coverRoom * aspect)))
        // Narrow: the page's width under the song's name, no taller than half
        // of what is left, so the lyrics keep room below.
        readonly property real narrowTop: tags.visible ? tags.y + tags.height + root.gap : info.y + info.height + root.gap
        readonly property int narrowVideoWidth: Math.max(160, Math.min(root.width - 2 * root.narrowEdge,
                                                                       Math.round((root.height - narrowTop) * 0.5 * aspect)))

        x: root.wide ? root.edge : root.narrowEdge
        y: root.wide ? root.contentTop : root.videoShowing ? narrowTop : root.headTop
        width: root.videoShowing ? (root.wide ? wideVideoWidth : narrowVideoWidth)
                                 : (root.wide ? root.coverSide : root.headCover)
        height: root.videoShowing ? Math.round(width / aspect) : width

        // Two shadows, as the mockup's two: a wide soft one well below, a
        // close one under the edge. Drawn by a shader from the shape alone,
        // so nothing is rendered again while the song plays.
        RectangularShadow {
            anchors.fill: parent
            radius: root.pictureRadius
            offset.y: root.wide ? 28 : 12
            blur: root.wide ? 56 : 28
            color: root.shadowFar
        }
        RectangularShadow {
            anchors.fill: parent
            radius: root.pictureRadius
            offset.y: root.wide ? 6 : 3
            blur: root.wide ? 16 : 8
            color: root.shadowNear
        }

        // The video's corners, as the cover's: a mask, since clipping cuts
        // square. Drawn once for the picture's size, and only while a video
        // plays; the frames then go through it as they arrive.
        Item {
            id: pictureCorners
            anchors.fill: parent
            visible: false
            layer.enabled: Player.videoPlaying
            layer.smooth: true

            Rectangle {
                anchors.fill: parent
                radius: root.pictureRadius
                antialiasing: true
            }
        }

        // Under the cover, so the still stays up until the first frame. Drawn
        // into a layer, for the mask, only while the picture shows.
        Item {
            anchors.fill: parent
            layer.enabled: root.videoShowing
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: pictureCorners
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }

            // mpv's frames have no alpha (bgr0: the fourth byte is 0), which
            // a picture drawn straight to the window never shows. Copied into
            // the layer as they are, that 0 reads as transparent, and the
            // mask's blend then adds the picture onto the field behind it,
            // washed out on a pale one. So the frames are blended onto black
            // here instead (a hair under full opacity is what makes Qt blend
            // rather than copy), which leaves the layer opaque.
            Rectangle {
                anchors.fill: parent
                visible: root.videoShowing
                color: "black"
            }
            VideoSurface {
                id: videoSurface
                anchors.fill: parent
                visible: root.videoHere && Player.videoPlaying
                opacity: 0.99
            }
        }

        Artwork {
            id: cover
            anchors.fill: parent
            visible: !root.videoShowing
            source: root.artwork
            placeholder: ""
            colour: true
            radius: root.pictureRadius
            color: root.line
        }
        // No cover, or nothing playing: a note on the plate.
        Icon {
            visible: !root.videoShowing && !cover.ready
            anchors.centerIn: parent
            width: Math.round(parent.width * 0.16)
            height: width
            thickness: 1.75
            name: "music"
            color: root.past
        }

        // A double-click on the moving picture fills the screen with it.
        // Takes the press: a plain tap here was what let a click on the
        // cover play the song in the hidden row beneath.
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onDoubleTapped: if (root.videoShowing) root.fullscreenRequested()
        }

        // Picture in picture and full screen sit on the picture, once it
        // plays. The switch between it and the cover is in the strip.
        Row {
            visible: root.videoShowing
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.space3
            spacing: Theme.space1

            PlateButton {
                iconName: "pip"
                tip: "Picture in picture"
                onClicked: root.pipRequested()
            }
            PlateButton {
                iconName: "fullscreen"
                tip: "Full screen (F)"
                onClicked: root.fullscreenRequested()
            }
        }
    }

    // — the song's name —
    // Under the cover when wide; beside the small cover, or over the moving
    // picture, when narrow.
    Column {
        id: info

        readonly property real narrowX: root.videoShowing ? root.narrowEdge
                                                          : root.narrowEdge + root.headCover + root.gap
        // Narrow, beside the cover, the name and the tags stand centred on it.
        readonly property real block: height + (tags.visible ? Theme.space3 + tags.height : 0)

        x: root.wide ? root.edge : narrowX
        y: root.wide ? picture.y + picture.height + root.gap
                     : root.videoShowing ? root.headTop
                     : root.headTop + Math.max(0, Math.round((root.headCover - block) / 2))
        width: root.wide ? root.columnWidth : root.width - narrowX - root.narrowEdge
        spacing: Theme.space1

        Text {
            width: parent.width
            text: root.hasTrack ? root.track.title : ""
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: root.wide ? 30 : 24
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(root.wide ? 30 : 24, -0.01)
            lineHeight: 1.05
            lineHeightMode: Text.ProportionalHeight
            color: root.ink
        }
        // Each name opens its page, and closes this view to show it.
        ArtistLine {
            width: parent.width
            artist: root.hasTrack ? root.track.artist : ""
            credits: root.hasTrack ? root.track.credits : undefined
            font.family: Theme.fontFamily
            font.pixelSize: root.wide ? 17 : 15
            color: root.soft
        }
    }

    // — two small tags: the sound, and what plays next —
    Flow {
        id: tags

        // From what the tags show, not from their own `visible`, which is
        // false while this is hidden and would keep it hidden.
        visible: root.quality.length > 0 || root.effects.length > 0 || root.showUpNext
        x: info.x
        y: info.y + info.height + (root.wide ? root.gap : Theme.space3)
        width: info.width
        spacing: Theme.space2

        Rectangle {
            id: qualityTag
            visible: root.quality.length > 0
            width: qualityText.implicitWidth + 20 + 2 * Theme.ruleWidth
            height: 30
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: root.line

            Text {
                id: qualityText
                anchors.centerIn: parent
                text: root.quality
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: root.soft
            }
            // The whole line under the pointer, as on the player bar's badge.
            HoverHandler { id: qualityHover }
            ToolTip.visible: qualityHover.hovered && Player.streamInfo.length > 0
            ToolTip.delay: 600
            ToolTip.text: Player.streamInfo
        }

        Rectangle {
            id: effectsTag
            visible: root.effects.length > 0
            width: Math.min(effectsText.implicitWidth + 20 + 2 * Theme.ruleWidth, tags.width)
            height: 30
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: root.line

            Text {
                id: effectsText
                anchors.verticalCenter: parent.verticalCenter
                x: 10 + Theme.ruleWidth
                width: parent.width - 20 - 2 * Theme.ruleWidth
                text: root.effects
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: root.soft
            }
        }

        // What plays next; a click shows the whole queue at the right.
        Rectangle {
            id: upNextTag
            visible: root.showUpNext
            width: Math.min(Theme.ruleWidth * 2 + 10 + upNextLabel.implicitWidth + Theme.space2
                            + upNextSong.implicitWidth + Theme.space2 + 16 + Theme.space2, tags.width)
            height: 30
            color: upNextArea.pressed ? root.tint(0.16) : upNextArea.containsMouse ? root.tint(0.1) : "transparent"
            border.width: Theme.ruleWidth
            border.color: root.line

            Accessible.role: Accessible.Button
            Accessible.name: "Up next: " + upNextSong.text

            Behavior on color {
                enabled: !upNextArea.containsMouse
                ColorAnimation { duration: Theme.quick }
            }

            Text {
                id: upNextLabel
                x: Theme.ruleWidth + 10
                anchors.verticalCenter: parent.verticalCenter
                text: "UP NEXT"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Theme.weightBlack
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.accent700
            }
            Text {
                id: upNextSong
                anchors.left: upNextLabel.right
                anchors.leftMargin: Theme.space2
                anchors.right: upNextChevron.left
                anchors.rightMargin: Theme.space2
                anchors.verticalCenter: parent.verticalCenter
                text: root.nextTrack ? [root.nextTrack.title, root.nextTrack.artist]
                                       .filter(part => part && part.length > 0).join(" · ") : ""
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: root.ink
            }
            Icon {
                id: upNextChevron
                anchors.right: parent.right
                anchors.rightMargin: Theme.ruleWidth + Theme.space2
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                thickness: 1.75
                name: "chevron-right"
                color: root.ink
            }

            MouseArea {
                id: upNextArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.pane = "queue"
            }
        }
    }

    // — the right side: the lyrics, or what plays next —
    Item {
        id: side

        readonly property real narrowTop: Math.max(picture.y + picture.height,
                                                   tags.visible ? tags.y + tags.height : info.y + info.height)
                                          + Theme.space6
        x: root.wide ? root.edge + root.columnWidth + root.edge : root.narrowEdge
        y: root.wide ? root.contentTop : narrowTop
        width: root.wide ? root.width - x - root.edge : root.width - 2 * root.narrowEdge
        height: root.height - y

        // Where the lyrics come from, and whether they keep time.
        Text {
            id: paneHead
            width: parent.width
            height: 16
            visible: root.pane === "lyrics"
            text: Lyrics.source.length === 0 || (Lyrics.state !== "synced" && Lyrics.state !== "plain") ? ""
                  : (Lyrics.state === "plain" ? "NOT TIME-SYNCED · " : "SYNCED · ") + Lyrics.source.toUpperCase()
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.1)
            color: root.soft
        }

        LyricsPane {
            visible: root.pane === "lyrics"
            anchors.top: paneHead.bottom
            anchors.topMargin: Theme.space2
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            marked: false
            readingLine: 0.28
            paper: root.field
            ink: root.ink
            pastInk: root.past
            nextInk: root.next
            hoverInk: root.soft
        }

        // The queue's own rows, printed in the field's tints. Its captions
        // and covers stand a margin in from its edge, so it starts that much
        // to the left and above, and they line up with the lyrics'.
        QueuePanel {
            visible: root.pane === "queue"
            x: -Theme.space6
            y: -Theme.space6
            width: parent.width + Theme.space6 + Theme.space4
            height: parent.height - y
            showHead: false
            showRule: false
            color: "transparent"
            ink: root.ink
            soft: root.soft
            rule: root.line
            hoverTint: root.tint(Theme.dark ? 0.06 : 0.05)
            plate: root.field
        }
    }

    // — the strip along the top: the window's title bar here too —
    Item {
        id: strip

        // Where the window's own buttons are drawn here: at the right on
        // Windows, at the left on a desktop that keeps them there; none on
        // macOS, whose traffic lights are the system's.
        readonly property bool buttonsAtRight: !Chrome.nativeButtons && !Chrome.buttonsOnLeft
        readonly property bool buttonsAtLeft: !Chrome.nativeButtons && Chrome.buttonsOnLeft

        width: parent.width
        height: Theme.titleBarHeight

        // Its empty parts drag the window, as the top strip does everywhere.
        WindowDragArea {
            anchors.fill: parent
        }

        // — at the left: close (after the window's buttons, where the system
        // keeps them there) —
        Row {
            id: leftCluster
            anchors.left: parent.left
            anchors.leftMargin: Chrome.nativeButtons ? Chrome.nativeButtonsInset
                              : strip.buttonsAtLeft ? 0 : Theme.space3
            height: parent.height
            spacing: Theme.space2

            WindowButtons {
                visible: strip.buttonsAtLeft
                height: parent.height
                rightPadding: 0
                ink: root.soft
                overField: true
            }

            // Down, back to the bar it came from. A word and its key, so it
            // is not read as one of the window's own buttons.
            Item {
                id: closeButton
                anchors.verticalCenter: parent.verticalCenter
                width: closeRow.implicitWidth + Theme.space2 * 2
                height: 40

                Accessible.role: Accessible.Button
                Accessible.name: "Close Now Playing"

                // The pointer's tint: in at once, out over a moment.
                Rectangle {
                    anchors.fill: parent
                    color: root.tint(closeArea.pressed ? 0.16 : 0.1)
                    opacity: closeArea.containsMouse || closeArea.pressed ? 1 : 0
                    Behavior on opacity {
                        enabled: !closeArea.containsMouse
                        NumberAnimation { duration: Theme.quick }
                    }
                }

                Row {
                    id: closeRow
                    anchors.centerIn: parent
                    spacing: Theme.space2

                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        thickness: 2.4
                        name: "chevron-down"
                        color: root.ink
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "CLOSE"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        font.weight: Theme.weightBlack
                        font.letterSpacing: Theme.tracking(12, 0.12)
                        color: root.ink
                    }
                    // The key that does the same, printed on the button
                    // rather than hidden in a tooltip; given up in the
                    // narrowest windows, where the strip has no room for it.
                    Rectangle {
                        visible: root.width >= 560
                        anchors.verticalCenter: parent.verticalCenter
                        width: escLabel.implicitWidth + 8
                        height: escLabel.implicitHeight + 2
                        color: "transparent"
                        border.width: 1.5
                        border.color: root.ink

                        Text {
                            id: escLabel
                            anchors.centerIn: parent
                            text: "ESC"
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            font.weight: Theme.weightBlack
                            font.letterSpacing: Theme.tracking(9, 0.06)
                            color: root.ink
                        }
                    }
                }

                // Takes the press, so a double-click on it is not also one on
                // the title bar beneath, which would maximise the window.
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closeRequested()
                }

                ToolTip.visible: closeArea.containsMouse
                ToolTip.delay: 600
                ToolTip.text: "Close Now Playing (Esc)"
            }
        }

        // — at the right: the pane switch, the video, the window's buttons —
        Row {
            id: rightCluster
            anchors.right: parent.right
            anchors.rightMargin: strip.buttonsAtRight ? 0 : Theme.space3
            height: parent.height
            spacing: Theme.space2

            // LYRICS | UP NEXT: one frame in the field's rule, the side shown
            // printed in reverse.
            Rectangle {
                id: paneSwitch
                anchors.verticalCenter: parent.verticalCenter
                width: paneRow.width + 2 * Theme.ruleWidth
                height: 30 + 2 * Theme.ruleWidth
                color: "transparent"
                border.width: Theme.ruleWidth
                border.color: root.line

                Row {
                    id: paneRow
                    x: Theme.ruleWidth
                    y: Theme.ruleWidth

                    Repeater {
                        model: [{ label: "LYRICS", pane: "lyrics" }, { label: "UP NEXT", pane: "queue" }]

                        Rectangle {
                            id: choice

                            required property var modelData
                            readonly property bool selected: root.pane === modelData.pane

                            width: choiceLabel.implicitWidth + 28
                            height: 30
                            color: selected ? root.ink : choiceHover.hovered ? root.tint(0.1) : "transparent"

                            Accessible.role: Accessible.RadioButton
                            Accessible.name: modelData.label
                            Accessible.checked: selected

                            // Choosing is immediate; the hover tint fades out.
                            Behavior on color {
                                enabled: !choice.selected && !choiceHover.hovered
                                ColorAnimation { duration: Theme.quick }
                            }

                            Text {
                                id: choiceLabel
                                anchors.centerIn: parent
                                text: choice.modelData.label
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Theme.weightBlack
                                font.letterSpacing: Theme.tracking(11, 0.1)
                                color: choice.selected ? root.field : root.ink
                            }

                            HoverHandler {
                                id: choiceHover
                                cursorShape: choice.selected ? Qt.ArrowCursor : Qt.PointingHandCursor
                            }
                            // Takes the press: a plain tap also counted toward
                            // a double-click on the title bar under it, which
                            // maximised the window.
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: root.pane = choice.modelData.pane
                            }
                        }
                    }
                }
            }

            // The video, for a song that has one: grey at rest, ink under the
            // pointer, red while the picture is on, dots while it loads.
            IconButton {
                visible: Player.videoAvailable
                anchors.verticalCenter: parent.verticalCenter
                side: 40
                iconSize: 20
                iconThickness: 1.75
                iconName: root.videoWaiting ? "dots" : "video"
                iconColor: Player.videoWanted ? Theme.accent : root.soft
                hoverColor: Player.videoWanted ? Theme.accentGlyphHover : root.ink
                onClicked: Player.videoWanted = !Player.videoWanted
                Accessible.name: root.switchTip
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: root.switchTip
            }

            // Sets the window's buttons apart from the view's own.
            Rectangle {
                visible: strip.buttonsAtRight
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.ruleWidth
                height: 20
                color: root.line
            }

            WindowButtons {
                visible: strip.buttonsAtRight
                height: parent.height
                leftPadding: 0
                ink: root.soft
                overField: true
            }
        }
    }
}
