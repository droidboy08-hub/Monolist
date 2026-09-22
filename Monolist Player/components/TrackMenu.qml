import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// What can be done with one track, from a row's "more" button or a right
// click. One instance per list: set `track` and popup().
Menu {
    id: menu

    // sourceId, title, artist, album, artwork, durationMs
    property var track: ({})
    readonly property string sourceId: track && track.sourceId ? track.sourceId : ""
    readonly property string downloadState: Downloads.revision >= 0 ? Downloads.stateFor(sourceId) : ""

    topPadding: Theme.ruleWidth
    bottomPadding: Theme.ruleWidth

    background: Rectangle {
        implicitWidth: 220
        color: Theme.bg
        border.width: Theme.ruleWidth
        border.color: Theme.text
    }

    delegate: MenuItem {
        id: item
        implicitHeight: 34
        leftPadding: Theme.space4
        rightPadding: Theme.space4

        contentItem: Text {
            text: item.text
            verticalAlignment: Text.AlignVCenter
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: item.enabled ? Theme.text : Theme.neutral500
        }
        background: Rectangle {
            color: item.highlighted ? Theme.rowHover : "transparent"
        }
    }

    Action {
        text: "Play next"
        onTriggered: Player.playNext(menu.track)
    }
    Action {
        text: "Add to queue"
        onTriggered: Player.addToQueue(menu.track)
    }
    Action {
        text: menu.downloadState === "done" ? "Downloaded"
            : menu.downloadState.length > 0 ? "Downloading…"
            : "Download"
        enabled: Downloads.available && menu.sourceId.length > 0 && menu.downloadState.length === 0
        onTriggered: Downloads.enqueue(menu.sourceId, menu.track.title, menu.track.artist,
                                       menu.track.artwork, menu.track.durationMs)
    }
}
