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
// sign-in (Startup's "mini") — and its corner button, or Ctrl+Shift+M again,
// goes back.
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

    // A window of its own, not one the full window owns (as one declared in
    // it would be): Windows gives an owned window no taskbar button and no
    // place in Alt+Tab, and its owner is hidden while this one is in use, so
    // once covered it could only be found again from the tray.
    transientParent: null

    width: 320
    height: 448
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    title: "Monolist"
    color: Theme.bg
    // Minimised from its taskbar button, as any window is.
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowMinimizeButtonHint
           | (onTop ? Qt.WindowStaysOnTopHint : 0)

    // Closed by Windows rather than by its own X — Alt+F4, the taskbar's or
    // Alt+Tab's close — it does what the X does (Main.qml closeMini: the
    // tray, or quit), never only hiding with Monolist running on unseen.
    // Ending (Quit, or Windows signing out), it closes.
    onClosing: function(close) {
        if (Tray.ending)
            return
        close.accepted = false
        mini.closeRequested()
    }

    function setOnTop(on) {
        onTop = on
        Library.setSetting("mini.on_top", on ? "1" : "0")
    }

    // The app's answers while it is the window in use (Main.qml say): a
    // song that will not play, above all. The full window's own toast is
    // hidden with that window.
    function notice(message, actionLabel, onAction, sticky) {
        miniToast.show(message, actionLabel, onAction, sticky)
    }

    // Where it was left, if that is still on one of the screens; else the
    // corner of its screen, clear of the taskbar.
    function place() {
        const sx = parseInt(Library.settingValue("mini.x", ""))
        const sy = parseInt(Library.settingValue("mini.y", ""))
        if (!isNaN(sx) && !isNaN(sy) && mini.onAScreen(sx, sy)) {
            mini.x = sx
            mini.y = sy
            return
        }
        // Its own screen's corner (the desktop's sizes span every screen),
        // above where a taskbar stands.
        mini.x = Screen.virtualX + Screen.width - mini.width - Theme.space6
        mini.y = Screen.virtualY + Screen.height - mini.height - Theme.space6 - 48
    }

    // A place for it with half of it across, and the top of its cover (what
    // moves it) on, some one screen. Each screen by itself: those left of or
    // above the main one stand at negative places, and the desktop's sizes
    // are the span of them all, not where any one ends — a place on a screen
    // since unplugged is on none.
    function onAScreen(px, py) {
        const screens = Application.screens
        for (let i = 0; i < screens.length; ++i) {
            const s = screens[i]
            if (px > s.virtualX - mini.width / 2 && px < s.virtualX + s.width - 80
                    && py >= s.virtualY && py < s.virtualY + s.height - 80)
                return true
        }
        return false
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

        // Not while the effects are over it: a press there is the panel's,
        // and one that strays must not carry the window off with it.
        DragHandler {
            target: null
            enabled: !mini.effectsOpen
            onActiveChanged: if (active) mini.startSystemMove()
        }

        // Put away while the effects are over them, so that none answers the
        // pointer through the panel.
        Row {
            visible: !mini.effectsOpen
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
                tip: "Open Monolist (Ctrl+Shift+M)"
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

            // The cover beneath answers nothing while it is open: its plates
            // are put away and its drag is off, and the panel keeps taps and
            // the pointer's hover to itself.
            TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }
            HoverHandler { blocking: true }

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

                // mpv would not take the chain this session, as the effects
                // panel says it: the tiles still keep what is chosen, for then.
                Text {
                    visible: Sound.failed
                    width: parent.width
                    text: "Effects would not start; they are off until Monolist restarts."
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    lineHeight: 1.2
                    color: Theme.paper
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

        // Red while any effect is on, and its tip as on the player bar:
        // what is on, or that they would not start.
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
            ToolTip.text: mini.effectsOpen ? "Hide the effects"
                          : Sound.failed ? "Effects would not start; off until Monolist restarts"
                          : Sound.active ? "Effects: " + Sound.summary : "Effects"
        }
    }

    // — the app's answers, over the foot of the cover (notice) —
    Toast {
        id: miniToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: cover.bottom
        anchors.bottomMargin: Theme.space3
        maximumWidth: mini.width - Theme.space4 * 2
        // Over the cover and the effects, under the window's frame.
        z: 9
    }

    Shortcut { sequence: "Space"; onActivated: Player.togglePlay() }
    Shortcut { sequence: "Escape"; enabled: mini.effectsOpen; onActivated: mini.effectsOpen = false }
    // The keys that made the window small make it whole again (Main.qml's
    // own Shortcut goes the other way).
    Shortcut { sequence: "Ctrl+Shift+M"; onActivated: mini.fullRequested() }

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
