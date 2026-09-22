import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The download control for one track. State comes from Downloads, which answers
// from memory; the bindings ask again whenever Downloads.revision moves.
//
//   not saved    download glyph              click: save for offline
//   queued       three dots                  click: cancel
//   downloading  glyph over a 2px progress   click: cancel
//   processing   three dots in accent        FFmpeg is tagging; nothing to do
//   failed       retry glyph                 click: try again
//   saved        check                       click: show the file
Button {
    id: control

    property string videoId: ""
    property string title: ""
    property string artist: ""
    property string artwork: ""
    property real durationMs: 0
    property int side: 32
    property int iconSize: 15

    // Not "state": Item already has one.
    readonly property string downloadState: Downloads.revision >= 0 ? Downloads.stateFor(videoId) : ""
    readonly property real progress: Downloads.revision >= 0 ? Downloads.progressFor(videoId) : 0

    visible: videoId.length > 0 && Downloads.available
    implicitWidth: side
    implicitHeight: side
    padding: 0
    flat: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.ghostActive
             : control.hovered ? Theme.ghostHover
             : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.accent

        // The system's only progress mark: a 2px rule, here along the foot.
        Rectangle {
            visible: control.downloadState === "downloading"
            x: 5
            width: parent.width - 10
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4
            height: 2
            color: Theme.neutral300

            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, control.progress))
                height: parent.height
                color: Theme.accent
            }
        }
    }

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            width: control.iconSize
            height: control.iconSize
            name: control.downloadState === "done" ? "check-circle"
                : control.downloadState === "failed" ? "rotate-ccw"
                : (control.downloadState === "queued" || control.downloadState === "processing") ? "dots"
                : "download"
            color: control.downloadState === "" ? (control.hovered ? Theme.accent700 : Theme.neutral700)
                 : control.downloadState === "failed" ? Theme.accent700
                 : control.downloadState === "queued" ? Theme.neutral700
                 : Theme.accent
        }
    }

    onClicked: {
        switch (downloadState) {
        case "":
            Downloads.enqueue(videoId, title, artist, artwork, durationMs)
            break
        case "queued":
        case "downloading":
            Downloads.cancel(videoId)
            break
        case "failed":
            Downloads.retry(videoId)
            break
        case "done":
            Downloads.revealFile(videoId)
            break
        }
    }

    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: downloadState === "" ? "Download"
                : downloadState === "queued" ? "Queued. Click to cancel"
                : downloadState === "downloading" ? "Downloading " + Math.round(progress * 100) + "%. Click to cancel"
                : downloadState === "processing" ? "Finishing up"
                : downloadState === "failed" ? "Download failed. Click to try again"
                : "Downloaded. Click to show the file"
}
