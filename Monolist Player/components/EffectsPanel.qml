import QtQuick
import QtQuick.Shapes
import Monolist
import Monolist.Backend

// The sound effects as one block, the same in the player bar's popup and in
// Settings > Playback: four effects to switch on, the strengths of the ones
// that are on, and the equaliser.
//
// Every control writes to Sound at once and Sound hands it to the engine,
// which ramps to it, so nothing waits for an Apply. Each effect is a tile
// rather than a check box because each changes the whole sound at a stroke:
// off, a tile is paper in a 2px ink frame; on, it is printed in reverse, ink
// with paper type, carrying the red square that means "on", as the toast
// does. Slowed + reverb and Nightcore both set the speed, so turning one on
// turns the other off (Sound does that), and that tile goes back to paper.
//
// No animation: the strengths of an effect appear with it, by `visible`, as
// the JioSaavn switches do in Settings (DESIGN 2.7).
//
// Every control here takes its press for itself (`takesPress`, a tap's
// ReleaseWithinBounds): in the popup the page is underneath, and a plain tap
// lets the press go on to it, so choosing an effect also pressed whatever
// row or tab lay under the popup.
Column {
    id: root

    // In Settings: a line of explanation under each effect that is on, and
    // the equaliser's own. The popup keeps to the controls.
    property bool detailed: false
    // In the popup: the same height whatever is on. The popup stands on the
    // bar and grows upwards, so a row of strengths appearing would lift the
    // tiles away from under the pointer that had just clicked one; the room
    // for the strengths (three rows at most: Slowed and Nightcore are never
    // on together) and for the line on headroom is kept instead.
    property bool steady: false
    readonly property int strengthRoom: 3 * 32 + Theme.space2 + Theme.space3
    // The band sliders' tracks, and the room above them for the values.
    property int bandHeight: detailed ? 140 : 112
    readonly property int bandTrackTop: 20

    // The bands and the shelf as they stand, read again whenever the
    // effects change: Sound.responseCurve is a call, which a binding would
    // not know to repeat.
    property var curve: Sound.responseCurve(120)

    spacing: Theme.space4

    Connections {
        target: Sound
        function onEffectsChanged() { root.curve = Sound.responseCurve(120) }
    }

    // "0.85×"
    function speedText(speed) {
        return Number(speed).toFixed(2) + "×"
    }
    // How long a 3:00 song takes at this speed: "3:32".
    function lengthAt(speed) {
        const seconds = Math.round(180 / speed)
        const rest = seconds % 60
        return Math.floor(seconds / 60) + ":" + (rest < 10 ? "0" : "") + rest
    }
    // Moving a band or picking a curve is asking for the equaliser, so it
    // comes on, as it does in Apple's Music: a grey control that silently
    // did nothing would be worse. Flat alone does not, being no curve.
    function pickPreset(id) {
        Sound.applyPreset(id)
        if (!Sound.equaliser && id !== "flat")
            Sound.equaliser = true
    }
    function moveBand(band, db) {
        Sound.setBandGain(band, db)
        if (!Sound.equaliser)
            Sound.equaliser = true
    }
    // The curve through the sliders: 31 Hz under the first band's track,
    // 16 kHz under the last, in the sliders' own dB scale. What goes past
    // ±12 dB (a boost on top of High bass) runs along the edge.
    function curvePath(points, width) {
        const count = points.length
        if (count < 2 || width <= 0)
            return []
        const band = width / Sound.bandLabels.length
        const span = width - band
        const path = []
        for (let i = 0; i < count; ++i) {
            const db = Math.max(-12, Math.min(12, points[i]))
            path.push(Qt.point(band / 2 + span * i / (count - 1),
                               bandTrackTop + (12 - db) / 24 * bandHeight))
        }
        return path
    }

    // The small tracked capitals that name a part ("EFFECTS"), as Settings
    // sets them.
    component SmallCaps: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    component Note: Text {
        width: root.width
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // One effect, switched by a click anywhere on it.
    component EffectTile: Rectangle {
        id: tile

        property string name: ""
        property string hint: ""
        property bool checked: false
        signal toggled()

        implicitHeight: tileText.implicitHeight + Theme.space3 * 2
        color: checked ? Theme.text : (tileHover.hovered ? Theme.rowHover : "transparent")
        border.width: Theme.ruleWidth
        border.color: Theme.text

        Accessible.role: Accessible.CheckBox
        Accessible.name: name
        Accessible.description: hint
        Accessible.checkable: true
        Accessible.checked: checked

        // Switching is immediate, as choosing a chip is; the hover tint fades
        // out like every other.
        Behavior on color {
            enabled: !tile.checked && !tileHover.hovered
            ColorAnimation { duration: Theme.quick }
        }

        Column {
            id: tileText
            x: Theme.space3
            y: Theme.space3
            // Clear of the red square.
            width: parent.width - Theme.space3 * 2 - 8 - Theme.space2
            spacing: 3

            Text {
                width: parent.width
                text: tile.name
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(12, 0.1)
                color: tile.checked ? Theme.bg : Theme.text
            }
            Text {
                width: parent.width
                text: tile.hint
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: tile.checked ? Theme.bg : Theme.neutral700
                opacity: tile.checked ? 0.72 : 1
            }
        }

        Rectangle {
            visible: tile.checked
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: Theme.space3
            anchors.topMargin: Theme.space3 + 3
            width: 8
            height: 8
            color: Theme.accent
        }

        HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
        // Takes the press for itself, as PlateButton does: in the popup the
        // page is underneath, and a plain tap would press it too.
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: tile.toggled()
        }
    }

    // A strength: its name, then its chips sharing their rules.
    component Strengths: Row {
        id: strengths

        property string label: ""
        // [{ text, value }]
        property var choices: []
        property real current: 0
        signal chosen(real value)

        spacing: Theme.space3

        SmallCaps {
            width: 64
            anchors.verticalCenter: parent.verticalCenter
            text: strengths.label
        }

        Row {
            spacing: -Theme.ruleWidth

            Repeater {
                model: strengths.choices
                delegate: ChoiceChip {
                    required property var modelData
                    label: modelData.text
                    selected: Math.abs(modelData.value - strengths.current) < 0.001
                    takesPress: true
                    onPicked: strengths.chosen(modelData.value)
                }
            }
        }
    }

    // — the head —
    Item {
        width: root.width
        height: Math.max(effectsLabel.implicitHeight, turnOff.implicitHeight)

        SmallCaps {
            id: effectsLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "EFFECTS"
        }

        // Every effect off at once, the strengths and the curve kept for
        // next time. A tracked link, as a section header's.
        Text {
            id: turnOff
            visible: Sound.active
            anchors.right: parent.right
            anchors.baseline: effectsLabel.baseline
            text: "TURN ALL OFF"
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(12, 0.12)
            color: turnOffArea.containsMouse ? Theme.accent700 : Theme.neutral700

            // A MouseArea, as the toast's link has: it keeps the press, and a
            // little beyond the word so it is not a small target.
            MouseArea {
                id: turnOffArea
                anchors.fill: parent
                anchors.margins: -Theme.space1
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Sound.turnAllOff()
            }
        }
    }

    // mpv would not take the chain this session (MpvEngine::effectsFailed).
    Note {
        visible: Sound.failed
        text: "Sound effects would not start, so they are off until Monolist restarts. What is chosen here is "
              + "kept for then."
        color: Theme.accent700
    }

    // — the effects —
    // Neighbouring tiles share their rules, as chips do.
    Grid {
        id: tiles
        width: root.width
        columns: 2
        spacing: -Theme.ruleWidth

        readonly property real tileWidth: (width + Theme.ruleWidth) / 2
        readonly property real tileHeight: Math.max(slowedTile.implicitHeight, nightcoreTile.implicitHeight,
                                                    eightDTile.implicitHeight, bassTile.implicitHeight)

        EffectTile {
            id: slowedTile
            width: tiles.tileWidth
            height: tiles.tileHeight
            name: "SLOWED + REVERB"
            hint: "Slower and lower, in a large hall"
            checked: Sound.slowedReverb
            onToggled: Sound.slowedReverb = !Sound.slowedReverb
        }
        EffectTile {
            id: nightcoreTile
            width: tiles.tileWidth
            height: tiles.tileHeight
            name: "NIGHTCORE"
            hint: "Faster and higher, like a record played fast"
            checked: Sound.nightcore
            onToggled: Sound.nightcore = !Sound.nightcore
        }
        EffectTile {
            id: eightDTile
            width: tiles.tileWidth
            height: tiles.tileHeight
            name: "8D AUDIO"
            hint: "Carried round your head. Best with headphones"
            checked: Sound.eightD
            onToggled: Sound.eightD = !Sound.eightD
        }
        EffectTile {
            id: bassTile
            width: tiles.tileWidth
            height: tiles.tileHeight
            name: "HIGH BASS"
            hint: "Lifts the kick and the bass, not the voices"
            checked: Sound.highBass
            onToggled: Sound.highBass = !Sound.highBass
        }
    }

    // — the strengths of what is on —
    Column {
        readonly property bool any: Sound.slowedReverb || Sound.nightcore || Sound.highBass

        width: root.width
        height: root.steady ? Math.max(implicitHeight, root.strengthRoom) : implicitHeight
        spacing: Theme.space3
        visible: root.steady || any || (root.detailed && Sound.eightD)

        Note {
            visible: root.steady && !parent.any
            text: "Slowed + reverb, Nightcore and High bass each come in a few strengths, chosen here once "
                  + "they are on."
        }

        Column {
            visible: Sound.slowedReverb
            width: parent.width
            spacing: Theme.space2

            Strengths {
                label: "SPEED"
                choices: Sound.slowedSpeeds.map(speed => ({ text: root.speedText(speed), value: speed }))
                current: Sound.slowedSpeed
                onChosen: function(value) { Sound.slowedSpeed = value }
            }
            Strengths {
                label: "REVERB"
                choices: Sound.reverbLevels.map((name, level) => ({ text: name, value: level }))
                current: Sound.reverbLevel
                onChosen: function(value) { Sound.reverbLevel = value }
            }
            Note {
                visible: root.detailed
                text: Sound.slowedSpeed >= 0.995
                      ? "At 1× every song keeps its own speed and pitch: only the large hall is added."
                      : "Every song slower and lower, like a record played slow, in a large hall. Times and lyrics "
                        + "follow the song: a 3:00 song still shows 3:00 and takes about "
                        + root.lengthAt(Sound.slowedSpeed) + "."
            }
        }

        Column {
            visible: Sound.nightcore
            width: parent.width
            spacing: Theme.space2

            Strengths {
                label: "SPEED"
                choices: Sound.nightcoreSpeeds.map(speed => ({ text: root.speedText(speed), value: speed }))
                current: Sound.nightcoreSpeed
                onChosen: function(value) { Sound.nightcoreSpeed = value }
            }
            Note {
                visible: root.detailed
                text: "Every song faster and higher, like a record played fast. Times and lyrics follow the "
                      + "song: a 3:00 song still shows 3:00 and takes about " + root.lengthAt(Sound.nightcoreSpeed) + "."
            }
        }

        Note {
            visible: root.detailed && Sound.eightD
            text: "The song circles your head once every ten seconds, in a light room of its own. Through "
                  + "speakers it only drifts from side to side."
        }

        Column {
            visible: Sound.highBass
            width: parent.width
            spacing: Theme.space2

            Strengths {
                label: "BASS"
                choices: Sound.highBassSteps.map(db => ({ text: "+" + db + " dB", value: db }))
                current: Sound.highBassDb
                onChosen: function(value) { Sound.highBassDb = value }
            }
            Note {
                visible: root.detailed
                text: "Lifts what is under 100 Hz, the kick and the bass, not the voices. The rest is turned "
                      + "down a little so nothing clips."
            }
        }
    }

    // — the equaliser —
    Rectangle {
        width: root.width
        height: 1
        color: Theme.hairline
    }

    ToggleRow {
        width: root.width
        label: "Equaliser"
        hint: root.detailed ? "Ten bands, up to 12 dB up or down each. Picking a curve or moving a band turns it on."
                            : ""
        checked: Sound.equaliser
        takesPress: true
        onToggled: Sound.equaliser = !Sound.equaliser
    }

    // The curves, the one in use inverted; "Custom" once a band has been
    // moved off every one of them. Grey while the equaliser is off, and
    // still there to be picked.
    //
    // Custom keeps its place in the line while it is not shown (opacity, not
    // `visible`): where it would start a line of its own, its coming and
    // going moved the bands below by a line in the middle of a drag, and the
    // band then read the hand against the moved track (+7 dB for a 1.5 dB
    // move). Its handlers are off with it (`enabled`).
    Flow {
        width: root.width
        spacing: -Theme.ruleWidth
        opacity: Sound.equaliser ? 1 : 0.45

        Repeater {
            model: Sound.presets
            delegate: ChoiceChip {
                required property var modelData
                label: modelData.name
                selected: Sound.preset === modelData.id
                takesPress: true
                onPicked: root.pickPreset(modelData.id)
            }
        }
        ChoiceChip {
            readonly property bool shown: Sound.preset === "custom"
            opacity: shown ? 1 : 0
            enabled: shown
            Accessible.ignored: !shown
            label: "Custom"
            selected: true
            takesPress: true
        }
    }

    // The ten bands, and behind them the curve they make together, with
    // High bass's shelf while it is on: the bands add up, so the curve can
    // stand higher than any one slider.
    Item {
        id: bands
        width: root.width
        height: bandRow.implicitHeight

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                strokeColor: Sound.equaliser ? Theme.text : Theme.neutral500
                strokeWidth: Theme.ruleWidth
                fillColor: "transparent"
                capStyle: ShapePath.FlatCap
                joinStyle: ShapePath.RoundJoin

                PathPolyline { path: root.curvePath(root.curve, bands.width) }
            }
        }

        Row {
            id: bandRow

            Repeater {
                model: Sound.bandLabels
                delegate: BandSlider {
                    required property int index
                    required property string modelData
                    width: bands.width / Sound.bandLabels.length
                    label: modelData
                    value: Sound.bandGains[index]
                    dimmed: !Sound.equaliser
                    trackHeight: root.bandHeight
                    trackTop: root.bandTrackTop
                    onMoved: function(db) { root.moveBand(index, db) }
                }
            }
        }
    }

    // How far the sound is turned down for the boosts. Kept as an empty line
    // in the popup while nothing is (`steady`).
    Note {
        readonly property bool shown: Sound.active && !Sound.failed && Sound.headroomDb >= 0.05
        readonly property bool boost: Sound.highBass
                                      || (Sound.equaliser && Math.max.apply(null, Sound.bandGains) > 0)
        visible: shown || root.steady
        text: shown ? "Turned down " + Sound.headroomDb.toFixed(1) + " dB so the "
                      + (boost ? "boost does" : "effects do") + " not clip; a limiter catches what is left."
                    : ""
    }
}
