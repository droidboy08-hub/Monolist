import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// Same vocabulary as Home: a section header and the track table. Results come
// from YouTube Music's own API, with yt-dlp as the fallback.
Flickable {
    id: root

    property string term: ""

    // YouTube Music answers in a few hundred milliseconds, so results can
    // follow the typing; the pause only keeps one request per word or so.
    onTermChanged: debounce.restart()

    Timer {
        id: debounce
        interval: 250
        onTriggered: Extractor.search(root.term)
    }

    contentWidth: width
    contentHeight: column.height
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    ScrollBar.vertical: ScrollBar {
        width: 10
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle { color: Theme.neutral300 }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        SectionHeader {
            width: parent.width
            number: "01"
            title: root.term.length > 0 ? "Results for " + root.term : "Search"
        }

        Row {
            spacing: Theme.space4

            // Negative spacing lets neighbouring segments share one 2px rule.
            Row {
                spacing: -Theme.ruleWidth

                ChoiceChip {
                    label: "SONGS"
                    selected: Extractor.filter === "songs"
                    onPicked: Extractor.filter = "songs"
                }
                ChoiceChip {
                    label: "VIDEOS"
                    selected: Extractor.filter === "videos"
                    onPicked: Extractor.filter = "videos"
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.term.length > 0 && !Extractor.busy && Extractor.results.count > 0
                text: Extractor.source === "yt-dlp"
                      ? "FROM YT-DLP · YOUTUBE MUSIC DID NOT ANSWER"
                      : "FROM " + Extractor.source.toUpperCase()
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.neutral700
            }
        }

        Text {
            visible: root.term.length === 0
            width: parent.width
            text: "Type in the field above to search YouTube Music."
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 14
            color: Theme.neutral700
        }

        Text {
            visible: Extractor.busy
            width: parent.width
            text: "Searching…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            font.letterSpacing: Theme.tracking(13, 0.02)
            color: Theme.neutral700
        }

        Text {
            visible: !Extractor.busy && Extractor.lastError.length > 0
            width: parent.width
            text: Extractor.lastError
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        Text {
            visible: !Extractor.busy && root.term.length > 0
                     && Extractor.results.count === 0
                     && Extractor.lastError.length === 0
            width: parent.width
            text: "No results."
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }

        TrackTable {
            visible: Extractor.results.count > 0
            width: parent.width
            model: Extractor.results
            activeIndex: -1
            showDownloads: true
            onTrackActivated: function(index) {
                var item = Extractor.results.get(index)
                Player.playSource(item.sourceId, item.title, item.artist,
                                  item.artwork, item.durationMs, item.album)
            }
        }
    }
}
