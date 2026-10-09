import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Monolist
import Monolist.Backend

// The mini player (the owner's choice of three, "B · Card", 2026-10-08): a
// window of its own, 320 by 448, the cover in colour on top and the song
// and its controls under it. It stands over other windows unless its pin is
// turned off, and moves by its cover. Main.qml opens it in place of the full
// window (enterMini) — from the player bar, the tray, Ctrl+Shift+M, or at
// sign-in (Startup's "mini") — and its corner button goes back.
//
// Everything on it is the player's own state (Player, Sound): it is another
// view of the same song, never a second player.
Window {
    id: mini

    // Back to the full window; and the window's own close (Main.qml decides:
    // the tray, or quit).
    signal fullRequested()
    signal closeRequested()
    // Effects' "All sound settings": the full window, on Settings' effects.
    signal soundSettingsRequested()

    readonly property bool hasSong: (Player.currentTrack.title || "").length > 0
    property bool onTop: Library.settingValue("mini.on_top", "1") !== "0"
    property bool effectsOpen: false

    width: 320
    height: 448
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    title: "Monolist"
    color: Theme.bg
    flags: Qt.Window | Qt.FramelessWindowHint | (onTop ? Qt.WindowStaysOnTopHint : 0)

    function setOnTop(on) {
        onTop = on
        Library.setSetting("mini.on_top", on ? "1" : "0")
    }

    // Where it was left, if that is still on the desktop; else the corner of
    // its screen, clear of the taskbar.
    function place() {
        const sx = parseInt(Library.settingValue("mini.x", ""))
        const sy = parseInt(Library.settingValue("mini.y", ""))
        if (!isNaN(sx) && !isNaN(sy) && sx > -mini.width / 2 && sy >= 0
                && sx < Screen.desktopAvailableWidth - 80 && sy < Screen.desktopAvailableHeight - 80) {
            mini.x = sx
            mini.y = sy
            return
        }
        // Its own screen's corner (the desktop's sizes span every screen),
        // above where a taskbar stands.
        mini.x = Screen.virtualX + Screen.width - mini.width - Theme.space6
        mini.y = Screen.virtualY + Screen.height - mini.height - Theme.space6 - 48
    }

    // Kept once it has stopped moving, not at every step of a drag.
    onXChanged: savePlace.restart()
    onYChanged: savePlace.restart()
    Timer {
        id: savePlace
        interval: 500
        onTriggered: if (mini.visible) {
            Library.setSetting("mini.x", String(mini.x))
            Library.setSetting("mini.y", String(mini.y))
        }
    }

    onVisibleChanged: if (!visible) effectsOpen = false

    // The window's frame: a 2px rule, drawn over everything, as the design
    // draws its edges.
    Rectangle {
        anchors.fill: parent
        z: 10
        color: "transparent"
        border.width: Theme.ruleWidth
        border.color: Theme.text
    }

    // — the cover, which also moves the window —
    Item {
        id: cover
        x: Theme.ruleWidth
        y: Theme.ruleWidth
        width: parent.width - Theme.ruleWidth * 2
        height: width

        Artwork {
            anchors.fill: parent
            source: Player.currentTrack.artwork || ""
            placeholder: ""
            colour: true
        }

        Text {
            visible: !mini.hasSong
            anchors.centerIn: parent
            text: "Nothing playing"
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral600
        }

        DragHandler {
            target: null
            onActiveChanged: if (active) mini.startSystemMove()
        }

        Row {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: Theme.space2
            spacing: Theme.space1

            CoverButton {
                iconName: "pin"
                iconColor: mini.onTop ? Theme.red : Theme.paper
                tip: mini.onTop ? "Stop keeping it over other windows" : "Keep it over other windows"
                onClicked: mini.setOnTop(!mini.onTop)
            }
            CoverButton {
                iconName: "maximize-2"
                tip: "Open Monolist"
                onClicked: mini.fullRequested()
            }
            CoverButton {
                iconName: "x"
                tip: "Close"
                onClicked: mini.closeRequested()
            }
        }

        // — the effects, over the cover —
        Rectangle {
            visible: mini.effectsOpen
            anchors.fill: parent
            color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, 0.94)

            // The cover beneath keeps its drag and its taps to itself.
            TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }

            Column {
                x: Theme.space4
                y: Theme.space4
                width: parent.width - Theme.space4 * 2
                spacing: Theme.space3

                Item {
                    width: parent.width
                    height: effectsHeading.implicitHeight

                    Text {
                        id: effectsHeading
                        text: "EFFECTS"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        font.letterSpacing: Theme.tracking(11, 0.14)
                        color: Theme.inkGrey
                    }
                    Text {
                        anchors.right: parent.right
                        text: "DONE"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        font.letterSpacing: Theme.tracking(11, 0.14)
                        color: doneHover.hovered ? Theme.paper : Theme.inkGrey
                        HoverHandler { id: doneHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: mini.effectsOpen = false }
                    }
                }

                Grid {
                    width: parent.width
                    columns: 2
                    spacing: -Theme.ruleWidth

                    EffectSwitch { name: "SLOWED + REVERB"; checked: Sound.slowedReverb; onToggled: Sound.slowedReverb = !Sound.slowedReverb }
                    EffectSwitch { name: "NIGHTCORE"; checked: Sound.nightcore; onToggled: Sound.nightcore = !Sound.nightcore }
                    EffectSwitch { name: "8D AUDIO"; checked: Sound.eightD; onToggled: Sound.eightD = !Sound.eightD }
                    EffectSwitch { name: "HIGH BASS"; checked: Sound.highBass; onToggled: Sound.highBass = !Sound.highBass }
                }

                EffectSwitch {
                    width: parent.width
                    name: "EQUALISER"
                    checked: Sound.equaliser
                    onToggled: Sound.equaliser = !Sound.equaliser
                }

                Text {
                    text: "All sound settings"
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.paper
                    font.underline: settingsHover.hovered
                    HoverHandler { id: settingsHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: mini.soundSettingsRequested() }
                }
            }
        }
    }

    // — the song's progress: a click or a drag seeks —
    ProgressSlider {
        id: progress
        anchors.top: cover.bottom
        x: Theme.ruleWidth
        width: parent.width - Theme.ruleWidth * 2
        height: 4
        value: Player.progress
        interactive: mini.hasSong
        onMoved: function(v) { Player.seekFraction(v) }
    }

    // — the song —
    Item {
        id: info
        anchors.top: progress.bottom
        anchors.topMargin: Theme.space3
        x: Theme.space4
        width: parent.width - Theme.space4 * 2
        height: titleText.implicitHeight + artistText.implicitHeight + 3

        Text {
            id: titleText
            anchors.left: parent.left
            anchors.right: times.left
            anchors.rightMargin: Theme.space3
            text: mini.hasSong ? Player.currentTrack.title : "Monolist"
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 17
            font.weight: Theme.weightBlack
            color: Theme.text
        }
        Text {
            id: artistText
            anchors.top: titleText.bottom
            anchors.topMargin: 3
            anchors.left: parent.left
            anchors.right: parent.right
            text: Player.currentTrack.artist || ""
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }
        Text {
            id: times
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 4
            visible: mini.hasSong
            text: Player.positionText + " / " + Player.durationText
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Theme.weightMedium
            font.letterSpacing: Theme.tracking(11, 0.04)
            color: Theme.neutral700
        }
    }

    // — the controls —
    Item {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space3 + Theme.ruleWidth
        x: Theme.space3
        width: parent.width - Theme.space3 * 2
        height: 48

        LikeButton {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            enabled: Player.currentSourceId.length > 0
            liked: Player.favourite
            side: 36
            iconSize: 17
            onClicked: Player.toggleFavourite()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: Player.favourite ? "Remove from Liked songs" : "Add to Liked songs"
        }

        Row {
            anchors.centerIn: parent
            spacing: Theme.space2

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                side: 40
                iconName: "skip-back"
                iconColor: Theme.text
                iconSize: 17
                onClicked: Player.previous()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Previous"
            }
            Rectangle {
                width: 48
                height: 48
                color: playHover.hovered ? Theme.accent600 : Theme.accent

                Accessible.role: Accessible.Button
                Accessible.name: Player.playing ? "Pause" : "Play"

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
                anchors.verticalCenter: parent.verticalCenter
                side: 40
                iconName: "skip-forward"
                iconColor: Theme.text
                iconSize: 17
                onClicked: Player.next()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "Next"
            }
        }

        // Red while any effect is on, as on the player bar.
        IconButton {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            side: 36
            iconName: "sliders-vertical"
            iconColor: Sound.active && !Sound.failed ? Theme.accent : Theme.neutral700
            iconSize: 17
            onClicked: mini.effectsOpen = !mini.effectsOpen
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: mini.effectsOpen ? "Hide the effects" : "Effects"
        }
    }

    Shortcut { sequence: "Space"; onActivated: Player.togglePlay() }
    Shortcut { sequence: "Escape"; enabled: mini.effectsOpen; onActivated: mini.effectsOpen = false }

    // A button on the cover: a glyph on a plate of ink, so it reads on any
    // picture.
    component CoverButton: Rectangle {
        id: plate

        property string iconName: ""
        property color iconColor: Theme.paper
        property string tip: ""
        signal clicked()

        width: 32
        height: 32
        color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, plateHover.hovered ? 0.92 : 0.72)

        Accessible.role: Accessible.Button
        Accessible.name: tip

        Icon {
            anchors.centerIn: parent
            name: plate.iconName
            width: 14
            height: 14
            color: plate.iconColor
        }
        HoverHandler { id: plateHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: plate.clicked()
        }
        ToolTip.visible: plateHover.hovered
        ToolTip.delay: 600
        ToolTip.text: tip
    }

    // An effect on the ink: paper type, filled paper when it is on, with the
    // red square every switched-on effect has.
    component EffectSwitch: Rectangle {
        id: effect

        property string name: ""
        property bool checked: false
        signal toggled()

        width: (parent ? parent.width + Theme.ruleWidth : 0) / 2
        height: 44
        color: checked ? Theme.paper : (effectHover.hovered ? Qt.rgba(1, 1, 1, 0.08) : "transparent")
        border.width: Theme.ruleWidth
        border.color: Theme.paper

        Accessible.role: Accessible.CheckBox
        Accessible.name: name
        Accessible.checkable: true
        Accessible.checked: checked

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space3
            anchors.right: dot.left
            anchors.rightMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            text: effect.name
            // Two lines where a name needs them ("SLOWED + / REVERB").
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            lineHeight: 0.95
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.1)
            color: effect.checked ? Theme.ink : Theme.paper
        }
        Rectangle {
            id: dot
            visible: effect.checked
            anchors.right: parent.right
            anchors.rightMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            color: Theme.red
        }
        HoverHandler { id: effectHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: effect.toggled()
        }
    }
}
