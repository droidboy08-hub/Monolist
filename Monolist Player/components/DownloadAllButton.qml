import QtQuick
import Monolist

// A page's "Download all", which says where the list stands: "Downloaded"
// once every song is on disk, "5 failed" while some could not be fetched
// (a click queues those again), and "Download all" otherwise.
ActionButton {
    id: control

    // Downloads.downloadCounts() for the page's songs.
    property var counts: ({})
    // Whether the page has every song it lists: until then, one not yet
    // loaded may still need fetching, so it is never "Downloaded".
    property bool complete: true
    // The page is fetching the rest of its songs before queueing them.
    property bool waiting: false

    signal downloadAllRequested()
    signal retryRequested()

    readonly property int failedCount: counts.failed || 0
    readonly property bool allDone: complete && (counts.songs || 0) > 0 && counts.done === counts.songs

    iconName: waiting ? "dots" : failedCount > 0 ? "rotate-ccw" : allDone ? "check" : "download"
    text: failedCount > 0 ? failedCount + " failed" : allDone ? "Downloaded" : "Download all"
    onClicked: failedCount > 0 ? retryRequested() : downloadAllRequested()
}
