import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    // As the window narrows the bar gives things up in one order: the volume
    // slider folds into a button, and the output button goes into its popup
    // with it; then the progress line moves under the buttons, then the title
    // goes, and the effects button goes into the volume popup with the
    // output's. The video, Now Playing and queue buttons never go: nothing
    // else opens any of them.
    readonly property bool showVolume: width >= 1120
    readonly property bool showMeta: width >= 760
    // Under the buttons once one line would leave the progress line too short
    // to aim at. Without the title it stays under, so narrowing the window
    // never puts it back on the line it has just left.
    readonly property bool stacked: !showMeta || transport.width < 440
    // In Now Playing this is whether its UP NEXT pane is showing.
    property bool queueOpen: false
    property bool nowPlayingOpen: false
    signal queueToggled()
    signal nowPlayingToggled()

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

    // The button it stands over moves when the slider folds or unfolds.
    onShowVolumeChanged: outputMenu.close()

    // — sound effects —
    // Slowed + reverb, Nightcore, 8D, High bass and the equaliser (Sound).
    // They are kept from one launch to the next, so the glyph is red while
    // any of them is on: a slowed song the next morning is explained, and
    // one click from being undone. Not red once the effects would not start
    // this session, since then nothing is changing the sound.
    readonly property bool effectsOn: Sound.active && !Sound.failed
    readonly property string effectsTip: Sound.failed ? "Effects would not start; off until Monolist restarts"
                                       : Sound.active ? "Effects: " + Sound.summary : "Effects"
    // The popup's link to Settings > Playback's EFFECTS block.
    signal effectsSettingsRequested()
    // The mini player, in place of the window (Main.qml).
    signal miniPlayerRequested()

    // From the glyph on the bar, or, once that has gone into the volume
    // popup, from the volume button, as the output menu is.
    function openEffects(anchor) {
        outputMenu.close()
        volumePopup.close()
        effectsPopup.anchor = anchor
        effectsPopup.open()
    }
    // Wherever the glyph is now (--effects-popup).
    function showEffects() {
        openEffects(root.showMeta ? effectsButton : volumeButton)
    }
    // As far up as the window goes, short of the popups' margin.
    readonly property real roomAbove: Window.height - height + Theme.ruleWidth - Theme.space2

    // Built like the volume popup: paper in a 2px ink frame, standing on the
    // bar's top rule with its right edge under the button, and no motion.
    // The whole block is in it, the same as Settings has, so nothing needs
    // a second visit; Settings adds only the longer explanations. It scrolls
    // in a window too short for it.
    Popup {
        id: effectsPopup

        property Item anchor: effectsButton
        // Where the button is on the bar, read as it opens: it does not
        // move while open (a breakpoint crossed closes it).
        property real anchorY: 0

        parent: anchor
        width: Math.min(592, root.width - Theme.space2 * 2)
        height: Math.min(implicitHeight, root.roomAbove)
        x: anchor.width - width
        // Kept standing on the rule if it grows (a Custom chip that starts a
        // line; the panel keeps its height otherwise, `steady`).
        y: -anchorY - height + Theme.ruleWidth
        margins: Theme.space2
        padding: Theme.space4
        // A press on the button is its toggle, so it does not count as a
        // press outside; the button closes it itself.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        focus: true
        onAboutToShow: anchorY = anchor.mapToItem(root, 0, 0).y

        background: Rectangle {
            color: Theme.bg
            border.width: Theme.ruleWidth
            border.color: Theme.text

            // A press on the popup's own paper, between its controls, and
            // the wheel, stop here rather than reaching the page behind. (The
            // controls take their presses for themselves: EffectsPanel.)
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onWheel: function(wheel) { wheel.accepted = true }
            }
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

    MonoMenu {
        id: outputMenu
        width: 320
        y: Theme.ruleWidth - height

        Instantiator {
            model: Player.audioDevices
            delegate: MonoMenuItem {
                required property var modelData
                // Never hidden, so the height need not follow `visible`,
                // which the menu changes as it takes a replaced row out.
                implicitHeight: 34
                text: modelData.missing ? modelData.description + " · not connected"
                                        : modelData.description
                enabled: !modelData.missing
                current: modelData.name === Player.audioDevice
                onTriggered: Player.setAudioDevice(modelData.name)
            }
            onObjectAdded: function(index, object) { outputMenu.insertItem(index, object) }
            onObjectRemoved: function(index, object) { outputMenu.removeItem(object) }
        }
    }

    color: Theme.bg
    implicitHeight: Theme.playerBarHeight

    Rectangle {
        width: parent.width
        height: Theme.ruleWidth
        color: Theme.text
    }

    // The song playing has the song menu, as it has anywhere else: from the
    // dots beside the title, or a right click on the cover or the title.
    readonly property bool hasSong: Player.currentTrack.title !== undefined
    function openMenu() {
        if (hasSong)
            Menus.openTrack(Player.currentTrack, { playing: true })
    }

    // — now playing —
    Item {
        id: nowPlaying
        anchors.left: parent.left
        anchors.leftMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        width: root.showMeta ? 320 : 52
        height: 52

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: root.openMenu()
        }

        Row {
            anchors.fill: parent
            spacing: Theme.space3

            // The cover and the title open Now Playing.
            Artwork {
                id: barArt
                width: 52
                height: 52
                placeholder: ""
                source: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""

                // No cover, or nothing playing: a note on the plate, as a
                // song row has.
                Icon {
                    visible: !barArt.ready
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    name: "music"
                    color: Theme.neutral600
                }

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nowPlayingToggled() }
            }

            Column {
                visible: root.showMeta
                anchors.verticalCenter: parent.verticalCenter
                // artwork, then the heart, the download control and the dots
                // beside the text
                width: parent.width - 52 - 30 * 3 - Theme.space3 * 4
                spacing: 1

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nowPlayingToggled() }

                Text {
                    width: parent.width
                    text: Player.currentTrack.title !== undefined ? Player.currentTrack.title : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
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
                        font.pixelSize: 12
                        color: Player.engineAvailable && !Player.statusError ? Theme.neutral700
                                                                            : Theme.accent
                    }

                    ArtistLine {
                        visible: !parent.showStatus
                        width: parent.width
                        artist: Player.currentTrack.artist !== undefined ? Player.currentTrack.artist : ""
                        credits: Player.currentTrack.credits
                        suffix: Player.currentTrack.album ? " — " + Player.currentTrack.album : ""
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                }
            }

            LikeButton {
                visible: root.showMeta
                // Nothing loaded, or a file with no video id: there is
                // nothing a like could be kept against. Dimmed rather than
                // hidden, so it is already in its place when a song arrives.
                enabled: Player.currentSourceId.length > 0
                anchors.verticalCenter: parent.verticalCenter
                liked: Player.favourite
                side: 30
                iconSize: 15
                onClicked: Player.toggleFavourite()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: Player.favourite ? "Remove from Liked songs" : "Add to Liked songs"
            }

            DownloadButton {
                readonly property string sourceId: Player.currentTrack.sourceId !== undefined
                                                   ? Player.currentTrack.sourceId : ""
                visible: root.showMeta && sourceId.length > 0 && Downloads.available
                anchors.verticalCenter: parent.verticalCenter
                side: 30
                iconSize: 15
                videoId: sourceId
                title: Player.currentTrack.title !== undefined ? Player.currentTrack.title : ""
                artist: Player.currentTrack.artist !== undefined ? Player.currentTrack.artist : ""
                artwork: Player.currentTrack.artwork !== undefined ? Player.currentTrack.artwork : ""
                durationMs: Player.duration
                isVideo: Player.currentTrack.isVideo === true
                album: Player.currentTrack.album !== undefined ? Player.currentTrack.album : ""
            }

            // Dimmed with nothing loaded, like the heart, so it is in its
            // place when a song arrives.
            IconButton {
                visible: root.showMeta
                enabled: root.hasSong
                opacity: enabled ? 1 : 0.4
                anchors.verticalCenter: parent.verticalCenter
                side: 30
                iconName: "dots"
                iconSize: 15
                iconColor: Theme.neutral700
                onClicked: root.openMenu()
            }
        }
    }

    // — transport —
    Item {
        id: transport
        anchors.left: nowPlaying.right
        anchors.leftMargin: Theme.space6
        anchors.right: rightControls.left
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        height: root.stacked ? buttons.height + Theme.space1 + timeline.height : buttons.height

        Row {
            id: buttons
            x: root.stacked ? Math.round((parent.width - width) / 2) : 0
            spacing: Theme.space2

            IconButton {
                iconName: "shuffle"
                iconColor: Player.shuffle ? Theme.accent : Theme.neutral700
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.setShuffle(!Player.shuffle)
            }
            IconButton {
                iconName: "skip-back"
                iconColor: Theme.neutral700
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.previous()
            }

            Rectangle {
                width: 44
                height: 44
                color: playHover.hovered ? Theme.accent600 : Theme.accent
                anchors.verticalCenter: parent.verticalCenter

                Behavior on color {
                    enabled: !playHover.hovered
                    ColorAnimation { duration: Theme.quick }
                }

                Icon {
                    anchors.centerIn: parent
                    name: Player.playing ? "pause" : "play"
                    width: 18
                    height: 18
                    color: Theme.accentForeground
                }

                HoverHandler { id: playHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: Player.togglePlay() }
            }

            IconButton {
                iconName: "skip-forward"
                iconColor: Theme.neutral700
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.next()
            }
            IconButton {
                iconName: Player.repeatMode === 2 ? "repeat-1" : "repeat"
                iconColor: Player.repeatMode === 0 ? Theme.neutral700 : Theme.accent
                iconSize: 15
                anchors.verticalCenter: parent.verticalCenter
                onClicked: Player.cycleRepeat()
            }
        }

        // The times and the progress line: beside the buttons, or under them
        // across the whole width when beside would leave the line too short.
        Item {
            id: timeline
            x: root.stacked ? 0 : buttons.width + Theme.space4
            y: root.stacked ? buttons.height + Theme.space1 : Math.round((parent.height - height) / 2)
            width: parent.width - x
            height: elapsed.implicitHeight

            Text {
                id: elapsed
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: Player.positionText
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }

            ProgressSlider {
                anchors.left: elapsed.right
                anchors.right: total.left
                anchors.leftMargin: Theme.space4
                anchors.rightMargin: Theme.space4
                anchors.verticalCenter: parent.verticalCenter
                value: Player.progress
                onMoved: function(v) { Player.seekFraction(v) }
            }

            Text {
                id: total
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: Player.durationText
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }
        }
    }

    // — output —
    Row {
        id: rightControls
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space2

        // The picture, from anywhere: it plays in Now Playing, which opens
        // for it if it is closed. Red while it is on, three
        // dots while it loads, greyed for a song that has none, so it is
        // there to be found before it is needed. Never folded away: nothing
        // else turns the picture on while Now Playing is closed.
        IconButton {
            readonly property bool waiting: Player.videoWanted && !Player.videoPlaying
            enabled: Player.videoAvailable
            opacity: enabled ? 1 : 0.4
            iconName: waiting ? "dots" : "video"
            iconColor: Player.videoWanted ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: {
                Player.videoWanted = !Player.videoWanted
                // The picture lives in Now Playing: turned on from here, it
                // opens there rather than playing for nobody.
                if (Player.videoWanted && !root.nowPlayingOpen)
                    root.nowPlayingToggled()
            }
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: Player.videoWanted ? "Stop the video" : "Play the video"
        }
        IconButton {
            // Open, it is the way back down: the same chevron as the close
            // button beside the window buttons.
            iconName: root.nowPlayingOpen ? "chevron-down" : "maximize-2"
            iconColor: root.nowPlayingOpen ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.nowPlayingToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.nowPlayingOpen ? "Close Now Playing" : "Now Playing and lyrics"
        }
        IconButton {
            iconName: "list-music"
            iconColor: root.queueOpen ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.queueToggled()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.nowPlayingOpen ? (root.queueOpen ? "Show lyrics" : "Up next")
                                              : (root.queueOpen ? "Hide queue" : "Queue")
        }
        // The mini player: the song in a small window of its own, in place
        // of this one. On the bar while the title is.
        IconButton {
            visible: root.showMeta
            iconName: "mini-player"
            iconColor: Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.miniPlayerRequested()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "Mini player (Ctrl+Shift+M)"
        }
        // The sound effects, red while any is on. On the bar for as long as
        // the title is, so the colour shows in a window snapped to half the
        // screen; narrower, it goes into the volume popup.
        IconButton {
            id: effectsButton
            visible: root.showMeta
            iconName: "sliders-vertical"
            iconColor: root.effectsOn ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: effectsPopup.visible ? effectsPopup.close() : root.openEffects(effectsButton)
            onVisibleChanged: if (!visible && effectsPopup.anchor === effectsButton) effectsPopup.close()
            ToolTip.visible: hovered && !effectsPopup.visible
            ToolTip.delay: 600
            ToolTip.text: root.effectsTip
        }
        IconButton {
            id: outputButton
            visible: root.showVolume
            iconName: "monitor-speaker"
            iconColor: Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.openOutputs(outputButton)
            ToolTip.visible: hovered && !outputMenu.visible
            ToolTip.delay: 600
            ToolTip.text: "Output: " + root.outputName
        }

        IconButton {
            visible: root.showVolume
            iconName: root.volumeIcon
            iconColor: root.muted ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            onClicked: root.toggleMute()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.muted ? "Unmute" : "Mute"
        }

        ProgressSlider {
            visible: root.showVolume
            width: 90
            anchors.verticalCenter: parent.verticalCenter
            value: Player.volume
            fillColor: Theme.text
            onMoved: function(v) { Player.setVolume(v) }
        }

        // Too narrow for the slider: one button, and the slider and mute in a
        // popup above it. Red while muted, so that still shows when folded.
        // Grey while the popup is open, like any glyph the pointer has left:
        // ink means the pointer is on it (DESIGN 2.6a), and the framed popup
        // standing on it already shows what is open.
        IconButton {
            id: volumeButton
            visible: !root.showVolume
            iconName: root.volumeIcon
            iconColor: root.muted ? Theme.accent : Theme.neutral700
            iconSize: 15
            anchors.verticalCenter: parent.verticalCenter
            // The effects popup stands here too once its glyph is folded in:
            // then this button puts that away first.
            onClicked: effectsPopup.visible ? effectsPopup.close()
                                            : volumePopup.visible ? volumePopup.close() : volumePopup.open()
            onVisibleChanged: {
                if (visible)
                    return
                volumePopup.close()
                if (effectsPopup.anchor === volumeButton)
                    effectsPopup.close()
            }
            ToolTip.visible: hovered && !volumePopup.visible
            ToolTip.delay: 600
            ToolTip.text: root.muted ? "Volume (muted)" : "Volume"

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
                }

                contentItem: Row {
                    spacing: Theme.space2

                    // The effects glyph, once the title has gone from the
                    // bar: red while any is on. Its popup opens where this
                    // one was, from the volume button, as the output menu
                    // does.
                    IconButton {
                        visible: !root.showMeta
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "sliders-vertical"
                        iconColor: root.effectsOn ? Theme.accent : Theme.neutral700
                        iconSize: 15
                        onClicked: root.openEffects(volumeButton)
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: root.effectsTip
                    }

                    // The output menu opens where this popup was, from the
                    // button that is still on the bar: this one goes with
                    // the popup it stands in.
                    IconButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "monitor-speaker"
                        iconColor: Theme.neutral700
                        iconSize: 15
                        onClicked: {
                            volumePopup.close()
                            root.openOutputs(volumeButton)
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: "Output: " + root.outputName
                    }

                    IconButton {
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: root.volumeIcon
                        iconColor: root.muted ? Theme.accent : Theme.neutral700
                        iconSize: 15
                        onClicked: root.toggleMute()
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: root.muted ? "Unmute" : "Mute"
                    }

                    ProgressSlider {
                        width: 120
                        anchors.verticalCenter: parent.verticalCenter
                        value: Player.volume
                        fillColor: Theme.text
                        onMoved: function(v) { Player.setVolume(v) }
                    }
                }
            }
        }
    }
}
