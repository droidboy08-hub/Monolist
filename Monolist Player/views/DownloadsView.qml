import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// The offline set, in three parts: how files are saved, what is in flight, and
// what is on this device.
ScrollPage {
    id: root

    contentHeight: column.height

    // A row's song menu (Menus), from its dots or a right click: the same as
    // anywhere else, with the download's own entries at its foot.
    function openMenu(videoId, title, artist, artwork, durationMs, isVideo) {
        Menus.openTrack({ sourceId: videoId, title: title, artist: artist, artwork: artwork,
                          durationMs: durationMs, isVideo: isVideo }, {})
    }

    // The note below says how to install yt-dlp; once that is done, opening
    // this page again is enough for downloads to switch on.
    onVisibleChanged: if (visible) Downloads.refreshToolsIfStale()

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
            text: "Downloading needs yt-dlp. " + Downloads.installHint
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
        DownloadOptions {
            width: parent.width
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
                    required property real durationMs
                    required property string phase
                    required property real progress
                    required property string detail

                    width: parent ? parent.width : 0
                    height: 64

                    function openMenu() {
                        root.openMenu(job.videoId, job.title, job.artist, job.artwork, job.durationMs, false)
                    }

                    HoverHandler { id: jobHover }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: job.openMenu()
                    }

                    // A picture only: a song still coming has nothing to play.
                    TrackCover {
                        id: jobArt
                        width: 44
                        height: 44
                        anchors.verticalCenter: parent.verticalCenter
                        source: job.artwork
                        sourceId: job.videoId
                        clickable: false
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
                            visible: jobHover.hovered
                            iconName: "dots"
                            iconColor: Theme.neutral700
                            iconSize: 15
                            onClicked: job.openMenu()
                        }
                        IconButton {
                            visible: job.phase === "failed"
                            iconName: "rotate-ccw"
                            iconColor: Theme.accent700
                            iconSize: 15
                            onClicked: Downloads.retry(job.videoId)
                        }
                        IconButton {
                            iconName: "x"
                            iconColor: Theme.neutral700
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

        ListFilter {
            visible: Downloads.library.count > 1
            width: parent.width
            model: savedView
            settingKey: "sort.downloads"
        }

        Text {
            visible: Downloads.library.count > 0 && savedView.count === 0
            text: "Nothing here matches."
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        // The saved songs as the filter and the order above leave them.
        TrackFilterModel {
            id: savedView
            sourceModel: Downloads.library
        }

        Column {
            visible: savedView.count > 0
            width: parent.width

            Repeater {
                model: savedView

                delegate: Item {
                    id: saved

                    required property int index
                    required property string videoId
                    required property string title
                    required property string artist
                    required property string artwork
                    required property real durationMs
                    required property string durationText
                    required property string format
                    required property string sizeText
                    required property bool isVideo

                    function openMenu() {
                        root.openMenu(saved.videoId, saved.title, saved.artist, saved.artwork,
                                      saved.durationMs, saved.isVideo)
                    }

                    readonly property bool isCurrent: Player.currentTrack.sourceId !== undefined
                                                      && Player.currentTrack.sourceId === videoId
                    // Deleting takes a second click, so a stray one cannot lose a file.
                    property bool armed: false

                    width: parent ? parent.width : 0
                    height: 56

                    Rectangle {
                        anchors.fill: parent
                        color: savedHover.hovered ? Theme.rowHover : "transparent"

                        Behavior on color {
                            enabled: !savedHover.hovered
                            ColorAnimation { duration: Theme.quick }
                        }
                    }

                    TrackCover {
                        id: savedArt
                        width: 40
                        height: 40
                        anchors.verticalCenter: parent.verticalCenter
                        source: saved.artwork
                        sourceId: saved.videoId
                        hovered: savedHover.hovered
                        active: saved.isCurrent
                        covered: Nav.pagesCovered
                        onPlayRequested: Player.playModel(savedView, saved.index, "library")
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
                        ArtistLine {
                            width: parent.width
                            artist: saved.artist
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
                                iconColor: Theme.neutral700
                                iconSize: 15
                                onClicked: Downloads.revealFile(saved.videoId)
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: "Show in folder"
                            }
                            IconButton {
                                iconName: "trash"
                                iconColor: saved.armed ? Theme.accent : Theme.neutral700
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
                            IconButton {
                                iconName: "dots"
                                iconColor: Theme.neutral700
                                iconSize: 15
                                onClicked: saved.openMenu()
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
                    TapHandler { onTapped: Player.playModel(savedView, saved.index, "library") }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: saved.openMenu()
                    }
                }
            }
        }
    }
}
