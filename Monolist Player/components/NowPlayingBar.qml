import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The player bar, as the owner chose it (2026-10-21, "Quiet"; DESIGN 3): the
// song at the left (its cover, title, artist and heart), the transport in the
// middle with the progress line under it, and at the right the sound's badge
// and the few controls used while listening, every glyph the same size and
// weight. What is used now and then (the mini player, the sleep timer, the
// video, a download, the song's own actions) is behind "…" at the end, and a
// sleep timer or a video that is on comes back out beside it while it is.
Rectangle {
    id: root

    // In Now Playing this is whether its UP NEXT pane is showing.
    property bool queueOpen: false
    property bool nowPlayingOpen: false
    // Whether Now Playing is open on its lyrics (Main).
    property bool lyricsOpen: false
    signal queueToggled()
    signal nowPlayingToggled()
    // Now Playing on its lyrics, or away again if that is what shows.
    signal lyricsToggled()
    // The effects popup's link to Settings > Playback's EFFECTS block.
    signal effectsSettingsRequested()
    // The mini player, in place of the window (Main.qml).
    signal miniPlayerRequested()

    color: Theme.bg
    implicitHeight: Theme.playerBarHeight

    // — measures —
    // As the mockup set them: every glyph 20 px in a 36 px square, two
    // pixels between the right side's squares, the cover 56. The transport
    // wants 300 px at least (its five buttons, 248, over a line long enough
    // to aim at) and gives anything past 620 to the sides, where it makes the
    // title longer rather than the line.
    readonly property int edgeLeft: 16
    readonly property int edgeRight: 20
    readonly property int square: 36
    readonly property int step: 2
    readonly property int coverSide: 56
    readonly property int songGap: 14
    readonly property int transportMin: 300
    readonly property int transportMax: 620
    // The left side with the title at its shortest worth showing, and as
    // wide as the mockup drew it.
    readonly property int titledMin: coverSide + songGap + 120 + songGap + square
    readonly property int titledWant: 360
    // The badge's type stands 6 px inside its frame, as the mockup has it.
    readonly property int badgePad: 6
    // Held for the badge whether or not it is showing, so the badge arriving
    // a moment after the sound starts moves nothing (and "OPUS 160" is the
    // longest it usually is).
    readonly property int badgeSlot: Math.ceil(badgeMetrics.advanceWidth) + 2 * badgePad
                                     + 2 * Theme.ruleWidth + Theme.space2

    TextMetrics {
        id: badgeMetrics
        font: badgeText.font
        text: "OPUS 160"
    }

    // — folding —
    // As the window narrows the bar gives things up in one order, worked out
    // from these measures alone (never from what it is drawing, which would
    // go round in a loop):
    //   1  the volume slider folds into its glyph, with a popup;
    //   2  the lyrics and the output go into "…";
    //   3  the badge goes;
    //   4  the title and the artist go, the cover (and the heart, while
    //      there is room) staying;
    //   5  the transport's buttons close up and Now Playing's button goes
    //      (the cover opens it, as it always has);
    //   6  the moon and the video, out beside "…", go back into it.
    // Between steps the title takes the slack, shorter as the window is, so
    // the transport stays at the bar's middle as long as anything allows.
    // The sleep timer's moon and the video's glyph count while they are out.
    readonly property int cues: (SleepTimer.active ? 1 : 0) + (Player.videoWanted ? 1 : 0)

    function rightAt(level) {
        let buttons = 4                         // queue, sound, volume, "…"
        if (level < 5)
            buttons += 1                        // Now Playing
        if (level < 2)
            buttons += 2                        // lyrics, output
        if (level < 6)
            buttons += cues
        let w = buttons * square + (buttons - 1) * step
        if (level < 1)
            w += step + 88 + Theme.space2       // the volume slider
        if (level < 3)
            w += badgeSlot + step
        return w
    }
    function gapAt(level) { return level >= 5 ? Theme.space4 : Theme.space6 }
    // The least the transport can have: a line worth aiming at under its
    // buttons while the title is there; then only the buttons' own width
    // (248, the line under them as wide); then the buttons closed up.
    function transportAt(level) { return level <= 3 ? transportMin : level === 4 ? 248 : 200 }
    function fits(level) {
        const room = width - edgeLeft - edgeRight - 2 * gapAt(level)
        const right = rightAt(level)
        if (level <= 3)
            return room - 2 * Math.max(right, titledMin) >= transportAt(level)
        return room - coverSide - right >= transportAt(level)
    }
    readonly property int fold: {
        for (let level = 0; level < 6; ++level) {
            if (fits(level))
                return level
        }
        return 6
    }
    // A popup stands over the button that opened it, and a fold moves the
    // buttons: whatever is open goes, as a menu would.
    onFoldChanged: {
        outputMenu.close()
        volumePopup.close()
        effectsPopup.close()
        sleepPopup.close()
    }

    readonly property bool showTitle: fold <= 3
    readonly property bool lyricsFolded: fold >= 2
    readonly property bool outputFolded: fold >= 2
    readonly property int gap: gapAt(fold)
    readonly property real room: width - edgeLeft - edgeRight - 2 * gap
    readonly property real rightNeed: rightAt(fold)
    readonly property real leftNeed: showTitle ? titledMin : coverSide
    // Equal sides put the transport at the bar's middle; when they cannot
    // be (a narrow window, the title gone), each side takes what it needs
    // and the transport the rest.
    readonly property real sideMin: Math.max(rightNeed, leftNeed)
    readonly property bool centred: room - 2 * sideMin >= transportAt(fold)
    readonly property real side: Math.max(sideMin,
                                          Math.min(showTitle ? titledWant : coverSide, (room - transportMin) / 2),
                                          (room - transportMax) / 2)
    readonly property real leftWidth: centred ? side : leftNeed
    readonly property real rightWidth: centred ? side : rightNeed
    // The heart stays beside the cover after the title has gone, while the
    // left side has room for both.
    readonly property bool showHeart: showTitle || leftWidth >= coverSide + songGap + square

    // — volume —
    // Muting is a volume of nothing. The level it had is kept here so unmuting
    // brings it back; a restart finds the volume as it was left, muted or not.
    property real volumeBeforeMute: 0.65
    readonly property bool muted: Player.volume <= 0
    readonly property string volumeIcon: muted ? "volume-x"
                                       : Player.volume < 0.5 ? "volume-1" : "volume-2"

    function toggleMute() {
        if (muted) {
            Player.setVolume(volumeBeforeMute > 0 ? volumeBeforeMute : 0.65)
        } else {
            volumeBeforeMute = Player.volume
            Player.setVolume(0)
        }
    }

    // — output —
    // Where the sound goes, by the name the system gives it. Auto follows the
    // system's default device as that changes; a device picked from the menu
    // is kept, and while it is unplugged Auto stands in for it.
    readonly property string outputName: {
        const devices = Player.audioDevices
        for (let i = 0; i < devices.length; ++i) {
            if (devices[i].name === Player.audioDevice)
                return devices[i].name === "auto" ? "Auto" : devices[i].description
        }
        return "Auto"
    }

    // Stands on the bar's top rule, pulled up out of the bar as the volume
    // popup is, its right edge under the button that opened it; and appears
    // without motion, as every menu does.
    function openOutputs(anchor) {
        outputMenu.x = Math.round(anchor.mapToItem(root, anchor.width, 0).x) - outputMenu.width
        outputMenu.open()
    }

    // — sound effects —
    // Slowed + reverb, Nightcore, 8D, High bass, Studio clarity and the
    // equaliser (Sound).
    // They are kept from one launch to the next, so the glyph is red while
    // any of them is on: a slowed song the next morning is explained, and
    // one click from being undone. Not red once the effects would not start
    // this session, since then nothing is changing the sound.
    readonly property bool effectsOn: Sound.active && !Sound.failed
    readonly property string effectsTip: Sound.failed ? "Effects would not start; off until Monolist restarts"
                                       : Sound.active ? "Effects: " + Sound.summary : "Effects"

    // The glyph never leaves the bar, so this is where the popup opens; a
    // fold, which moves the glyph, closes it.
    function openEffects() {
        outputMenu.close()
        volumePopup.close()
        effectsPopup.open()
    }
    // For --effects-popup.
    function showEffects() {
        openEffects()
    }
    // As far up as the window goes, short of the popups' margin.
    readonly property real roomAbove: Window.height - height + Theme.ruleWidth - Theme.space2

    // — the video —
    // The picture lives in Now Playing: turned on from here, it opens there
    // rather than playing for nobody.
    function toggleVideo() {
        Player.videoWanted = !Player.videoWanted
        if (Player.videoWanted && !root.nowPlayingOpen)
            root.nowPlayingToggled()
    }

    // — the song —
    // The song playing has the song menu, as it has anywhere else: from a
    // right click on the cover or the title, and with the bar's own entries
    // at its head from "…".
    readonly property bool hasSong: Player.currentTrack.title !== undefined
    function openMenu() {
        if (hasSong)
            Menus.openTrack(Player.currentTrack, { playing: true })
    }
    function openMore() {
        Menus.openTrack(hasSong ? Player.currentTrack : ({}), { playing: true, bar: root, anchor: moreButton })
    }

    // The song's download, which had a button of its own on the bar and is
    // now one of "…"'s entries: a rule along the foot of "…" says how far it
    // has got while it runs.
    readonly property string songDownload: Player.currentSourceId.length > 0 && Downloads.revision >= 0
                                           ? Downloads.stateFor(Player.currentSourceId) : ""
    readonly property real songDownloadProgress: songDownload === "processing" ? 1
                                               : songDownload === "downloading" && Downloads.progressRevision >= 0
                                                 ? Downloads.progressFor(Player.currentSourceId) : 0
    readonly property bool songDownloading: songDownload === "queued" || songDownload === "downloading"
                                            || songDownload === "processing"

    // Built like the volume popup: paper in a 2px ink frame, standing on the
    // bar's top rule with its right edge under the button, and no motion.
    // The whole block is in it, the same as Settings has, so nothing needs
    // a second visit; Settings adds only the longer explanations. It scrolls
    // in a window too short for it.
    Popup {
        id: effectsPopup

        // Where the button is on the bar, read as it opens: it does not
        // move while open (a fold closes it).
        property real anchorY: 0

        parent: effectsButton
        width: Math.min(592, root.width - Theme.space2 * 2)
        height: Math.min(implicitHeight, root.roomAbove)
        x: effectsButton.width - width
        // Kept standing on the rule if it grows (a Custom chip that starts a
        // line; the panel keeps its height otherwise, `steady`).
        y: -anchorY - height + Theme.ruleWidth
        margins: Theme.space2
        padding: Theme.space4
        // A press on the button is its toggle, so it does not count as a
        // press outside; the button closes it itself.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        focus: true
        onAboutToShow: anchorY = effectsButton.mapToItem(root, 0, 0).y

        background: Rectangle {
            color: Theme.bg
            border.width: Theme.ruleWidth
            border.color: Theme.text

            // A press on the popup's own paper, between its controls, and
            // the wheel, stop here rather than reaching the page behind; and
            // the pointer over it is the popup's, not a row's beneath it.
            // (The controls take their presses for themselves: EffectsPanel.)
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onWheel: function(wheel) { wheel.accepted = true }
            }
            HoverHandler { blocking: true }
        }

        contentItem: Flickable {
            id: effectsFlick
            implicitHeight: effectsContent.implicitHeight
            contentWidth: width
            contentHeight: effectsContent.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height

            ScrollBar.vertical: MonoScrollBar {}

            Column {
                id: effectsContent
                // Clear of the scroll bar while there is one. Narrower only
                // ever means taller, so this cannot take the bar away again.
                width: effectsFlick.width - (effectsFlick.interactive ? Theme.space4 : 0)
                spacing: Theme.space3

                EffectsPanel {
                    width: parent.width
                    steady: true
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.hairline
                }

                // Drawn like a name link: ink, with a 1px rule under the
                // pointer, arriving at once and fading over `quick`.
                Text {
                    id: settingsLink
                    text: "All sound settings"
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.text

                    Rectangle {
                        y: settingsLink.baselineOffset + 2
                        width: parent.width
                        height: 1
                        color: settingsLink.color
                        opacity: settingsLinkArea.containsMouse ? 1 : 0

                        Behavior on opacity {
                            enabled: !settingsLinkArea.containsMouse
                            NumberAnimation { duration: Theme.quick }
                        }
                    }

                    // A MouseArea, as the toast's link has: it keeps the
                    // press from the page under the popup.
                    MouseArea {
                        id: settingsLinkArea
                        anchors.fill: parent
                        anchors.margins: -Theme.space1
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            effectsPopup.close()
                            root.effectsSettingsRequested()
                        }
                    }
                }
            }
        }
    }

    // The same menu as "…"'s Output entry, standing on the bar.
    OutputMenu {
        id: outputMenu
        y: Theme.ruleWidth - height
    }

    // A press on the bar's own paper, between its controls, and the wheel,
    // stop at the bar. It stands on the full-screen picture, where a
    // double-click on bare bar left full screen. First, so every control
    // above it still takes its own press.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    Rectangle {
        width: parent.width
        height: Theme.ruleWidth
        color: Theme.text
    }

    // — the song —
    Item {
        id: nowPlaying
        x: root.edgeLeft
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: Theme.ruleWidth / 2
        width: root.leftWidth
        height: root.coverSide

        // The cover and the title open Now Playing, and a right click on
        // them opens the song's menu. A MouseArea, so the press stays here
        // (the bar stands on the full-screen picture); under the artist's
        // names, which take a press on a name for themselves (ArtistLine).
        MouseArea {
            width: root.showTitle ? titleColumn.x + titleColumn.width : root.coverSide
            height: parent.height
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            onClicked: function(mouse) {
                if (mouse.button === Qt.RightButton)
                    root.openMenu()
                else
                    root.nowPlayingToggled()
            }
        }

        // Its corners softened as a list cover's are (6px), through the mask
        // Main draws once for it (Nav.barCoverMask), not a layer of its own.
        Artwork {
            id: barArt
            width: root.coverSide
            height: root.coverSide
            radius: 6
            placeholder: ""
            source: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""

            // No cover, or nothing playing: a note on the plate, as a
            // song row has.
            Icon {
                visible: !barArt.ready
                anchors.centerIn: parent
                width: 20
                height: 20
                thickness: 1.75
                name: "music"
                color: Theme.neutral600
            }
        }

        Column {
            id: titleColumn
            visible: root.showTitle
            x: root.coverSide + root.songGap
            anchors.verticalCenter: parent.verticalCenter
            // Up to the heart, which ends the side.
            width: Math.max(0, parent.width - x - root.songGap - root.square)
            spacing: 2

            Text {
                width: parent.width
                text: Player.currentTrack.title !== undefined ? Player.currentTrack.title : ""
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Theme.weightBlack
                color: Theme.text
            }
            // Doubles as the status line: while a source is resolving, or
            // when the audio engine is missing entirely, that matters more
            // than the artist and there is nowhere else it would be seen.
            // Otherwise the artist, whose names open their pages; a click
            // anywhere else on the line opens Now Playing, as before.
            Item {
                readonly property bool showStatus: !Player.engineAvailable || Player.resolving
                                                   || Player.statusError

                width: parent.width
                height: statusLine.implicitHeight

                Text {
                    id: statusLine
                    visible: parent.showStatus
                    width: parent.width
                    text: Player.statusText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Player.engineAvailable && !Player.statusError ? Theme.neutral700
                                                                        : Theme.accent
                }

                ArtistLine {
                    visible: !parent.showStatus
                    width: parent.width
                    artist: Player.currentTrack.artist !== undefined ? Player.currentTrack.artist : ""
                    credits: Player.currentTrack.credits
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }
            }
        }

        LikeButton {
            visible: root.showHeart
            // Nothing loaded, or a file with no video id: there is
            // nothing a like could be kept against. Dimmed rather than
            // hidden, so it is already in its place when a song arrives.
            enabled: Player.currentSourceId.length > 0
            x: root.showTitle ? parent.width - width : root.coverSide + root.songGap
            anchors.verticalCenter: parent.verticalCenter
            liked: Player.favourite
            side: root.square
            iconSize: 20
            iconThickness: 1.75
            onClicked: Player.toggleFavourite()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: Player.favourite ? "Remove from Liked songs" : "Add to Liked songs"
        }
    }

    // — transport —
    // Its buttons, and under them the progress line with the times either
    // side of it.
    Item {
        id: transport
        // On the window's middle while the sides are equal: the edges are
        // not (16 and 20, as the mockup has them), and the play button is
        // where the eye looks for the middle.
        x: root.centred ? Math.round((root.width - width) / 2) : root.edgeLeft + root.leftWidth + root.gap
        width: root.centred ? root.room - 2 * root.side
                            : root.room - root.leftWidth - root.rightWidth
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: Theme.ruleWidth / 2
        height: buttons.height + 6 + timeline.height

        Row {
            id: buttons
            anchors.horizontalCenter: parent.horizontalCenter
            // Closed up only in the narrowest window, where the line under
            // them would otherwise be wider than the room for it.
            spacing: root.fold >= 5 ? Theme.space1 : Theme.space4

            BarButton {
                iconName: "shuffle"
                iconColor: Player.shuffle ? Theme.accent : Theme.neutral700
                tip: Player.shuffle ? "Shuffle is on" : "Shuffle"
                onClicked: Player.setShuffle(!Player.shuffle)
            }
            BarButton {
                iconName: "skip-back"
                tip: "Previous"
                onClicked: Player.previous()
            }

            // The one red thing on the bar, and the one control that is a
            // shape rather than a glyph: it is what the bar is for. Its
            // corners softened (6px) like the cover's beside it.
            Rectangle {
                width: 40
                height: 40
                radius: 6
                antialiasing: true
                anchors.verticalCenter: parent.verticalCenter
                color: playArea.containsMouse || playArea.pressed ? Theme.accent600 : Theme.accent

                Accessible.role: Accessible.Button
                Accessible.name: Player.playing ? "Pause" : "Play"

                Behavior on color {
                    enabled: !playArea.containsMouse
                    ColorAnimation { duration: Theme.quick }
                }

                // At 18 it is drawn as large as the mockup's 20: Lucide's
                // play and pause fill more of their square than the stroked
                // glyphs do.
                Icon {
                    anchors.centerIn: parent
                    name: Player.playing ? "pause" : "play"
                    width: 18
                    height: 18
                    color: Theme.accentForeground
                }

                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Player.togglePlay()
                }
            }

            BarButton {
                iconName: "skip-forward"
                tip: "Next"
                onClicked: Player.next()
            }
            BarButton {
                iconName: Player.repeatMode === 2 ? "repeat-1" : "repeat"
                iconColor: Player.repeatMode === 0 ? Theme.neutral700 : Theme.accent
                tip: Player.repeatMode === 0 ? "Repeat" : Player.repeatMode === 2 ? "Repeating this song" : "Repeating"
                onClicked: Player.cycleRepeat()
            }
        }

        // The times are set in equal-width figures and each given the room
        // of the longer, so the line between them never shifts as they count.
        TextMetrics {
            id: timeMetrics
            font: elapsed.font
            text: Player.durationText.length >= Player.positionText.length
                  ? Player.durationText.replace(/[0-9]/g, "0") : Player.positionText.replace(/[0-9]/g, "0")
        }

        Item {
            id: timeline
            anchors.top: buttons.bottom
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, root.transportMax)
            height: elapsed.implicitHeight

            Text {
                id: elapsed
                width: Math.ceil(timeMetrics.advanceWidth)
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                text: Player.positionText
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.features: ({ "tnum": 1 })
                color: Theme.neutral700
            }

            ProgressSlider {
                anchors.left: elapsed.right
                anchors.right: total.left
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                height: 3
                value: Player.progress
                onMoved: function(v) { Player.seekFraction(v) }
            }

            Text {
                id: total
                width: elapsed.width
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: Player.durationText
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.features: ({ "tnum": 1 })
                color: Theme.neutral700
            }
        }
    }

    // — the right side —
    Row {
        id: rightControls
        anchors.right: parent.right
        anchors.rightMargin: root.edgeRight
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: Theme.ruleWidth / 2
        spacing: root.step

        // What is playing, as mpv reports it: the codec and the bitrate,
        // "OPUS 160", or "AAC 320" once JioSaavn has it. A note, not a
        // control: grey type in a grey frame, and the whole line from Now
        // Playing under the pointer. Nothing until both are known.
        Item {
            visible: root.fold < 3 && Player.streamQuality.length > 0
            width: badge.width + Theme.space2
            height: root.square
            anchors.verticalCenter: parent.verticalCenter

            Rectangle {
                id: badge
                anchors.verticalCenter: parent.verticalCenter
                width: badgeText.implicitWidth + 2 * root.badgePad + 2 * Theme.ruleWidth
                height: badgeText.implicitHeight + root.badgePad + 2 * Theme.ruleWidth
                color: "transparent"
                border.width: Theme.ruleWidth
                border.color: Theme.neutral300

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: Player.streamQuality
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Theme.weightMedium
                    font.letterSpacing: Theme.tracking(10, 0.08)
                    color: Theme.neutral700
                }

                HoverHandler { id: badgeHover }
                ToolTip.visible: badgeHover.hovered
                ToolTip.delay: 600
                ToolTip.text: Player.streamInfo
            }
        }

        // The lyrics: Now Playing, opened on them; red while it shows them.
        BarButton {
            visible: !root.lyricsFolded
            iconName: "mic"
            iconColor: root.lyricsOpen ? Theme.accent : Theme.neutral700
            tip: root.lyricsOpen ? "Hide the lyrics" : "Lyrics"
            onClicked: root.lyricsToggled()
        }
        BarButton {
            iconName: "list-music"
            iconColor: root.queueOpen ? Theme.accent : Theme.neutral700
            tip: root.nowPlayingOpen ? (root.queueOpen ? "Show lyrics" : "Up next")
                                     : (root.queueOpen ? "Hide queue" : "Queue")
            onClicked: root.queueToggled()
        }
        // The sound effects, red while any is on. Never folded away, so the
        // colour shows in a window of any width.
        BarButton {
            id: effectsButton
            iconName: "sliders-vertical"
            iconColor: root.effectsOn ? Theme.accent : Theme.neutral700
            tip: root.effectsTip
            tipHeld: effectsPopup.visible
            onClicked: effectsPopup.visible ? effectsPopup.close() : root.openEffects()
        }
        BarButton {
            id: outputButton
            visible: !root.outputFolded
            iconName: "speaker"
            tip: "Output: " + root.outputName
            tipHeld: outputMenu.visible
            onClicked: root.openOutputs(outputButton)
        }

        // With the slider beside it, the glyph mutes; folded, it opens the
        // slider and the mute in a popup. Red while muted either way.
        // Grey while the popup is open, like any glyph the pointer has left:
        // ink means the pointer is on it (DESIGN 2.6a), and the framed popup
        // standing on it already shows what is open.
        BarButton {
            id: volumeButton
            iconName: root.volumeIcon
            iconColor: root.muted ? Theme.accent : Theme.neutral700
            tip: root.fold < 1 ? (root.muted ? "Unmute" : "Mute")
                               : (root.muted ? "Volume (muted)" : "Volume")
            tipHeld: volumePopup.visible
            onClicked: {
                if (root.fold < 1)
                    root.toggleMute()
                else
                    volumePopup.visible ? volumePopup.close() : volumePopup.open()
            }

            // It stands on the bar's top rule, as if pulled up out of the bar,
            // and appears without motion: it answers a click, as a menu does.
            Popup {
                id: volumePopup
                x: volumeButton.width - width
                margins: Theme.space2
                topPadding: Theme.space2
                bottomPadding: Theme.space2
                leftPadding: Theme.space2
                rightPadding: Theme.space4
                // A press on the button is its toggle, so it does not count as
                // a press outside; the button closes it itself.
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                focus: true
                onAboutToShow: y = -volumeButton.mapToItem(root, 0, 0).y - height + Theme.ruleWidth

                background: Rectangle {
                    color: Theme.bg
                    border.width: Theme.ruleWidth
                    border.color: Theme.text

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.AllButtons
                        onWheel: function(wheel) { wheel.accepted = true }
                    }
                    HoverHandler { blocking: true }
                }

                contentItem: Row {
                    spacing: Theme.space2

                    BarButton {
                        iconName: root.volumeIcon
                        iconColor: root.muted ? Theme.accent : Theme.neutral700
                        tip: root.muted ? "Unmute" : "Mute"
                        onClicked: root.toggleMute()
                    }

                    ProgressSlider {
                        width: 120
                        height: 3
                        anchors.verticalCenter: parent.verticalCenter
                        value: Player.volume
                        fillColor: Theme.text
                        onMoved: function(v) { Player.setVolume(v) }
                    }
                }
            }
        }
        Item {
            visible: root.fold < 1
            width: 88 + Theme.space2
            height: root.square
            anchors.verticalCenter: parent.verticalCenter

            ProgressSlider {
                width: 88
                height: 3
                anchors.verticalCenter: parent.verticalCenter
                value: Player.volume
                fillColor: Theme.text
                onMoved: function(v) { Player.setVolume(v) }
            }
        }

        // Now Playing. Open, it is the way back down: the same chevron as the
        // close button beside the window buttons, in red.
        BarButton {
            visible: root.fold < 5
            iconName: root.nowPlayingOpen ? "chevron-down" : "maximize-2"
            iconColor: root.nowPlayingOpen ? Theme.accent : Theme.neutral700
            tip: root.nowPlayingOpen ? "Close Now Playing" : "Now Playing"
            onClicked: root.nowPlayingToggled()
        }

        // The video, out beside "…" while it is on: red, three dots while it
        // loads, and a click turns it off. Off, it is one of "…"'s entries.
        BarButton {
            readonly property bool waiting: Player.videoWanted && !Player.videoPlaying
            visible: Player.videoWanted && root.fold < 6
            iconName: waiting ? "dots" : "video"
            iconColor: Theme.accent
            tip: waiting ? "Loading the video. Click to stop it" : "Stop the video"
            onClicked: root.toggleVideo()
        }

        // The sleep timer, out beside "…" while it runs: a red moon, its
        // countdown, +10 minutes and Turn off in a popup built like the
        // volume one's. Started from "…", which has its choices too.
        BarButton {
            id: sleepButton
            visible: SleepTimer.active && root.fold < 6
            iconName: "moon"
            iconColor: Theme.accent
            tip: SleepTimer.endOfSong ? "Sleep timer: at the end of this song"
                                      : "Sleep timer: " + SleepTimer.remainingText + " left"
            tipHeld: sleepPopup.visible
            onClicked: sleepPopup.visible ? sleepPopup.close() : sleepPopup.open()
            onVisibleChanged: if (!visible) sleepPopup.close()

            Popup {
                id: sleepPopup
                // Its own width: the lines take theirs from it.
                width: 232
                x: sleepButton.width - width
                padding: 0
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                focus: true
                // Standing on the bar's top rule, and kept there as lines come
                // and go (+10 minutes and Turn off, once it runs).
                function stand() { y = -sleepButton.mapToItem(root, 0, 0).y - height + Theme.ruleWidth }
                onAboutToShow: stand()
                onHeightChanged: if (visible) stand()

                background: Rectangle {
                    color: Theme.bg
                    border.width: Theme.ruleWidth
                    border.color: Theme.text

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.AllButtons
                        onWheel: function(wheel) { wheel.accepted = true }
                    }
                    HoverHandler { blocking: true }
                }

                contentItem: Column {
                    topPadding: Theme.space3
                    bottomPadding: Theme.space2

                    // The heading, and the time left beside it while it runs.
                    Item {
                        width: parent.width
                        height: sleepHeading.implicitHeight + Theme.space2

                        Text {
                            id: sleepHeading
                            x: Theme.space4
                            text: "SLEEP TIMER"
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            font.letterSpacing: Theme.tracking(11, 0.14)
                            color: Theme.neutral700
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.space4
                            anchors.baseline: sleepHeading.baseline
                            visible: SleepTimer.active
                            text: SleepTimer.fading ? "FADING OUT" : SleepTimer.remainingText.toUpperCase()
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: Font.Bold
                            font.letterSpacing: Theme.tracking(11, 0.08)
                            color: Theme.accent700
                        }
                    }

                    // The same choices as "…"'s Sleep timer (SleepMenu) and
                    // the tray's.
                    Repeater {
                        model: [
                            { label: "15 minutes", minutes: 15 },
                            { label: "30 minutes", minutes: 30 },
                            { label: "45 minutes", minutes: 45 },
                            { label: "1 hour", minutes: 60 },
                            { label: "End of this song", minutes: 0 }
                        ]
                        delegate: SleepChoice {
                            required property var modelData
                            label: modelData.label
                            onChosen: {
                                if (modelData.minutes > 0)
                                    SleepTimer.start(modelData.minutes)
                                else
                                    SleepTimer.startEndOfSong()
                                sleepPopup.close()
                            }
                        }
                    }

                    Rectangle {
                        visible: SleepTimer.active
                        x: Theme.space4
                        width: parent.width - Theme.space4 * 2
                        height: 1
                        color: Theme.hairline
                    }
                    SleepChoice {
                        visible: SleepTimer.active
                        label: "+10 minutes"
                        onChosen: SleepTimer.extend(10)
                    }
                    SleepChoice {
                        visible: SleepTimer.active
                        label: "Turn off"
                        onChosen: {
                            SleepTimer.cancel()
                            sleepPopup.close()
                        }
                    }
                }
            }
        }

        // "…": the mini player, the sleep timer, the video, and in a narrow
        // window the lyrics and the output, then the song's own menu
        // (TrackMenu, with the bar's entries at its head). Never dimmed: the
        // bar's entries are there with nothing playing.
        BarButton {
            id: moreButton
            iconName: "dots"
            tip: root.songDownload === "downloading"
                 ? "More · Downloading " + Math.round(root.songDownloadProgress * 100) + "%"
                 : root.songDownload === "queued" ? "More · Download queued"
                 : root.songDownload === "processing" ? "More · Finishing the download"
                 : "More"
            onClicked: root.openMore()

            // The download's rule, as a download button draws it: 2 px along
            // the foot, red as far as it has got.
            Rectangle {
                visible: root.songDownloading
                x: Theme.space2
                width: parent.width - Theme.space2 * 2
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 3
                height: 2
                color: Theme.neutral300

                Rectangle {
                    width: Math.round(parent.width * Math.max(0, Math.min(1, root.songDownloadProgress)))
                    height: parent.height
                    color: Theme.accent
                }
            }
        }
    }

    // One of the bar's glyph buttons: 20 px in a 36 px square, at the
    // weight of every other glyph on the bar, grey at rest. IconButton draws
    // its glyph across the room its padding leaves, so the padding is what
    // makes the glyph 20.
    component BarButton: IconButton {
        // What it does, for the pointer's tool tip and for a screen reader.
        property string tip: ""
        // No tool tip while the popup this button opened is up.
        property bool tipHeld: false

        side: 36
        iconSize: 20
        padding: (side - iconSize) / 2
        iconThickness: 1.75
        iconColor: Theme.neutral700
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined

        Accessible.name: tip
        ToolTip.visible: hovered && !tipHeld && tip.length > 0
        ToolTip.delay: 600
        ToolTip.text: tip
    }

    // A line of the sleep timer's popup: a choice, taken at a click.
    component SleepChoice: Rectangle {
        id: choice

        property string label: ""
        signal chosen()

        width: parent ? parent.width : 0
        height: 36
        color: choiceHover.hovered ? Theme.rowHover : "transparent"

        Accessible.role: Accessible.MenuItem
        Accessible.name: label

        Text {
            x: Theme.space4
            anchors.verticalCenter: parent.verticalCenter
            text: choice.label
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.text
        }
        HoverHandler { id: choiceHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: choice.chosen()
        }
    }
}
