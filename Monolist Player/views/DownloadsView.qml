import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// The offline set, in three parts: how files are saved, what is in flight, and
// what is on this device.
Flickable {
    id: root

    contentWidth: width
    contentHeight: column.height
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: MonoScrollBar {}

    // A square check box with a label and a line of explanation.
    component Toggle: Item {
        id: toggle

        property string label: ""
        property string hint: ""
        property bool checked: false
        signal toggled()

        implicitHeight: Math.max(18, toggleText.implicitHeight)

        Rectangle {
            id: box
            y: 1
            width: 18
            height: 18
            color: toggle.checked ? Theme.accent : "transparent"
            border.width: Theme.ruleWidth
            border.color: toggle.checked ? Theme.accent : Theme.text

            Icon {
                anchors.centerIn: parent
                width: 12
                height: 12
                name: "check"
                thickness: 3
                visible: toggle.checked
                color: Theme.accentForeground
            }
        }

        Column {
            id: toggleText
            anchors.left: box.right
            anchors.leftMargin: Theme.space3
            anchors.right: parent.right
            spacing: 2

            Text {
                width: parent.width
                text: toggle.label
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 14
                color: Theme.text
            }
            Text {
                visible: text.length > 0
                width: parent.width
                text: toggle.hint
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }
        }

        HoverHandler { cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: toggle.toggled() }
    }

    component Caption: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        // — 01 downloads —
        SectionHeader {
            width: parent.width
            number: "01"
            title: "Downloads"
            action: "OPEN FOLDER →"
            onActionTriggered: Downloads.openDownloadFolder()
        }

        Column {
            width: parent.width
            spacing: Theme.space1

            Text {
                width: parent.width
                text: Downloads.library.count + (Downloads.library.count === 1 ? " track" : " tracks")
                      + " on this device · " + Downloads.library.totalSizeText
                      + (Downloads.activeCount > 0 ? " · " + Downloads.activeCount + " downloading" : "")
                      + (Downloads.queuedCount > 0 ? " · " + Downloads.queuedCount + " queued" : "")
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.letterSpacing: Theme.tracking(13, 0.02)
                color: Theme.text
            }

            Text {
                width: parent.width
                text: Downloads.downloadDirectory
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }
        }

        Text {
            visible: !Downloads.available
            width: parent.width
            text: "Downloading needs yt-dlp. Run scripts\\setup-windows.ps1, or place yt-dlp next to the app."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        Text {
            visible: Downloads.available && !Downloads.canConvert
            width: parent.width
            text: "FFmpeg was not found, so files are saved exactly as they arrive: no cover art, no tags, no format choice."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        // — how files are saved —
        Column {
            visible: Downloads.available
            width: parent.width
            spacing: Theme.space3

            Caption { text: "FORMAT" }

            // Negative spacing lets neighbouring segments share one 2px rule.
            Row {
                spacing: -Theme.ruleWidth
                enabled: Downloads.canConvert
                opacity: enabled ? 1 : 0.4

                ChoiceChip {
                    label: "ORIGINAL · BEST"
                    selected: Downloads.format === "original"
                    onPicked: Downloads.format = "original"
                }
                ChoiceChip {
                    label: "M4A"
                    selected: Downloads.format === "m4a"
                    onPicked: Downloads.format = "m4a"
                }
                ChoiceChip {
                    label: "MP3"
                    selected: Downloads.format === "mp3"
                    onPicked: Downloads.format = "mp3"
                }
            }

            Text {
                width: parent.width
                text: Downloads.format === "m4a"
                      ? "AAC in an .m4a file, about 128 kbps. Plays on Apple devices and older players."
                      : Downloads.format === "mp3"
                        ? "Re-encoded to MP3 at the highest VBR setting. Plays anywhere, at a small cost in quality."
                        : "The stream exactly as YouTube publishes it, usually Opus at 130–160 kbps. Nothing is re-encoded."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }

            Toggle {
                width: parent.width
                enabled: Downloads.canConvert
                opacity: enabled ? 1 : 0.4
                label: "Cut non-music parts"
                hint: "Removes intros, outros and skits that SponsorBlock users have marked in music videos."
                checked: Downloads.skipNonMusic
                onToggled: Downloads.skipNonMusic = !Downloads.skipNonMusic
            }
        }

        // — 02 in progress —
        HRule {
            visible: Downloads.queue.count > 0
            width: parent.width
        }

        SectionHeader {
            visible: Downloads.queue.count > 0
            width: parent.width
            number: "02"
            title: "In progress"
        }

        Column {
            visible: Downloads.queue.count > 0
            width: parent.width

            Repeater {
                model: Downloads.queue

                delegate: Item {
                    id: job

                    required property string videoId
                    required property string title
                    required property string artist
                    required property string artwork
                    required property string phase
                    required property real progress
                    required property string detail

                    width: parent ? parent.width : 0
                    height: 64

                    Artwork {
                        id: jobArt
                        width: 44
                        height: 44
                        anchors.verticalCenter: parent.verticalCenter
                        placeholder: ""
                        source: job.artwork
                    }

                    Column {
                        anchors.left: jobArt.right
                        anchors.leftMargin: Theme.space4
                        anchors.right: jobActions.left
                        anchors.rightMargin: Theme.space4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4

                        Text {
                            width: parent.width
                            text: job.title
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: Theme.weightMedium
                            color: Theme.text
                        }

                        Text {
                            width: parent.width
                            text: (job.artist.length > 0 ? job.artist + " · " : "") + job.detail
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: job.phase === "failed" ? Theme.accent700 : Theme.neutral700
                        }

                        ProgressSlider {
                            visible: job.phase === "downloading" || job.phase === "processing"
                            width: parent.width
                            height: 2
                            interactive: false
                            value: job.progress
                        }
                    }

                    Row {
                        id: jobActions
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space1

                        IconButton {
                            visible: job.phase === "failed"
                            iconName: "rotate-ccw"
                            iconColor: Theme.accent700
                            iconSize: 15
                            onClicked: Downloads.retry(job.videoId)
                        }
                        IconButton {
                            iconName: "x"
                            iconColor: Theme.text
                            iconSize: 14
                            onClicked: Downloads.cancel(job.videoId)
                        }
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.hairline
                    }
                }
            }
        }

        // — on this device —
        HRule { width: parent.width }

        SectionHeader {
            width: parent.width
            number: Downloads.queue.count > 0 ? "03" : "02"
            title: "On this device"
        }

        Text {
            visible: Downloads.library.count === 0
            width: parent.width
            text: "Nothing saved yet. Use the download button on any track: in search results, on Home, or in the player bar."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        Column {
            visible: Downloads.library.count > 0
            width: parent.width

            Repeater {
                model: Downloads.library

                delegate: Item {
                    id: saved

                    required property int index
                    required property string videoId
                    required property string title
                    required property string artist
                    required property string artwork
                    required property string durationText
                    required property string format
                    required property string sizeText

                    readonly property bool isCurrent: Player.currentTrack.sourceId !== undefined
                                                      && Player.currentTrack.sourceId === videoId
                    // Deleting takes a second click, so a stray one cannot lose a file.
                    property bool armed: false

                    width: parent ? parent.width : 0
                    height: 56

                    Rectangle {
                        anchors.fill: parent
                        color: savedHover.hovered ? Theme.rowHover : "transparent"
                    }

                    Artwork {
                        id: savedArt
                        width: 40
                        height: 40
                        anchors.verticalCenter: parent.verticalCenter
                        placeholder: ""
                        source: saved.artwork
                    }

                    Column {
                        anchors.left: savedArt.right
                        anchors.leftMargin: Theme.space4
                        anchors.right: savedMeta.left
                        anchors.rightMargin: Theme.space4
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        Text {
                            width: parent.width
                            text: saved.title
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            font.weight: saved.isCurrent ? Font.Bold : Theme.weightRegular
                            color: saved.isCurrent ? Theme.accent700 : Theme.text
                        }
                        Text {
                            width: parent.width
                            text: saved.artist
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }

                    Row {
                        id: savedMeta
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space3

                        Row {
                            visible: savedHover.hovered || saved.armed
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.space1

                            IconButton {
                                iconName: "folder"
                                iconColor: Theme.text
                                iconSize: 15
                                onClicked: Downloads.revealFile(saved.videoId)
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: "Show in folder"
                            }
                            IconButton {
                                iconName: "trash"
                                iconColor: saved.armed ? Theme.accent : Theme.text
                                iconSize: 15
                                onClicked: {
                                    if (saved.armed) {
                                        Downloads.remove(saved.videoId)
                                    } else {
                                        saved.armed = true
                                        disarm.restart()
                                    }
                                }
                                ToolTip.visible: hovered || saved.armed
                                ToolTip.delay: saved.armed ? 0 : 600
                                ToolTip.text: saved.armed ? "Click again to delete the file" : "Delete from this device"
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: saved.format + " · " + saved.sizeText
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            font.letterSpacing: Theme.tracking(12, 0.04)
                            color: Theme.neutral700
                        }

                        Text {
                            width: 44
                            anchors.verticalCenter: parent.verticalCenter
                            horizontalAlignment: Text.AlignRight
                            text: saved.durationText
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            color: saved.isCurrent ? Theme.accent700 : Theme.text
                        }
                    }

                    Timer {
                        id: disarm
                        interval: 3000
                        onTriggered: saved.armed = false
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.hairline
                    }

                    HoverHandler { id: savedHover; cursorShape: Qt.PointingHandCursor }
                    // The offline set becomes the queue, starting here.
                    TapHandler { onTapped: Player.playModel(Downloads.library, saved.index) }
                }
            }
        }
    }
}
