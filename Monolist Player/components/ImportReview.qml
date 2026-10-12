import QtQuick
import Monolist
import Monolist.Backend
import "../ImportText.js" as ImportText

// What an import left on its playlist's page (ROADMAP X03). While it runs,
// how far it has got. Then the songs it was not sure of, to hear and keep
// or not (To check), and the ones it could not place, to look for in Search
// or let go (Not found), each beside the song as the source listed it.
// Nothing at all on a playlist no import made, or once all is settled.
Column {
    id: root

    property int playlistId: 0

    // Asked again whenever any import moves (Importer.revision).
    readonly property int jobId: playlistId > 0 && Importer.revision >= 0 ? Importer.jobForPlaylist(playlistId) : 0
    readonly property var job: jobId > 0 && Importer.revision >= 0 ? Importer.jobInfo(jobId) : ({})
    // The rows themselves only when one has come or gone: a song found
    // moves none of them, and remaking a long list every few seconds while
    // an import runs would be work for nothing.
    readonly property string reviewKey: jobId > 0
                                        ? [jobId, job.check, job.notFound, job.notSearched, job.dismissed].join("|") : ""
    readonly property var review: reviewKey.length > 0 ? Importer.review(playlistId) : ({})
    readonly property var check: review.check ? review.check : []
    readonly property var notFound: review.notFound ? review.notFound : []
    // A stop's songs, never asked for: apart from those YouTube Music had
    // nothing for, and with the way to ask for them after all.
    readonly property var notSearched: review.notSearched ? review.notSearched : []
    // Not done and not stopped.
    readonly property bool going: jobId > 0 && job.done === false
    // The links beside a row, where there is room; under it where not.
    readonly property bool wide: width >= 640

    visible: going || check.length > 0 || notFound.length > 0 || notSearched.length > 0
    spacing: Theme.space6

    // A song to check, heard where it is: the song alone, as a search's
    // result plays, and YouTube Music's radio after it.
    function play(candidate) {
        if (!candidate || !candidate.videoId)
            return
        if (candidate.videoId === Player.currentSourceId) {
            Player.togglePlay()
            return
        }
        Player.playTracks([{
            sourceId: candidate.videoId,
            title: candidate.title,
            artist: candidate.artist,
            album: candidate.album,
            artwork: candidate.artwork,
            durationMs: candidate.durationMs,
            isVideo: candidate.isVideo === true
        }], 0, "search")
    }

    function lengthPart(ms) {
        const text = ImportText.length(ms)
        return text.length > 0 ? "  ·  " + text : ""
    }

    // "“Yellow” by Coldplay", or the title alone where the list named no artist.
    function listed(row) {
        return "“" + row.title + "”" + (row.artist ? " by " + row.artist : "")
    }

    // A tracked link, as a shelf's header has: grey, red under the pointer.
    component RowLink: Text {
        id: link
        signal clicked()
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.12)
        color: linkHover.hovered ? Theme.accent700 : Theme.neutral700

        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: link.clicked() }
    }

    // A part's name and count over a 2px rule, and its links at the end.
    component Heading: Item {
        id: heading
        property string title: ""
        property string action: ""
        property string secondAction: ""
        signal actionTriggered()
        signal secondActionTriggered()
        width: root.width
        height: 36

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: heading.title
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.14)
            color: Theme.text
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space4

            RowLink {
                visible: heading.action.length > 0
                text: heading.action
                onClicked: heading.actionTriggered()
            }
            RowLink {
                visible: heading.secondAction.length > 0
                text: heading.secondAction
                onClicked: heading.secondActionTriggered()
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.ruleWidth
            color: Theme.divider
        }
    }

    component Lead: Text {
        width: root.width
        topPadding: Theme.space3
        bottomPadding: Theme.space1
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // — while it runs —
    Column {
        visible: root.going
        width: parent.width
        spacing: Theme.space2

        Item {
            width: parent.width
            height: Math.max(stateText.implicitHeight, jobLinks.implicitHeight)

            Text {
                id: stateText
                width: Math.max(0, parent.width - jobLinks.width - Theme.space4)
                anchors.verticalCenter: parent.verticalCenter
                text: root.going ? ImportText.stateLine(root.job) : ""
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.14)
                color: root.going && ImportText.running(root.job) ? Theme.accent700 : Theme.neutral700
            }

            Row {
                id: jobLinks
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space4

                // By hand only: a rest or a wait on YouTube Music goes on by itself.
                RowLink {
                    readonly property bool held: root.job.state === "paused" && !(root.job.resumeAt > 0)
                    visible: held || root.job.state === "reading" || root.job.state === "finding"
                             || root.job.state === "queued"
                    text: held ? "RESUME" : "PAUSE"
                    onClicked: held ? Importer.resume(root.jobId) : Importer.pause(root.jobId)
                }
                RowLink {
                    text: "SHOW PROGRESS"
                    onClicked: Nav.openImportJob(root.jobId)
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 4
            color: Theme.neutral300

            Rectangle {
                width: parent.width * (root.going ? ImportText.progress(root.job) : 0)
                height: parent.height
                color: root.going && ImportText.running(root.job) ? Theme.accent : Theme.text
            }
        }

        Text {
            width: parent.width
            text: root.going ? ImportText.noteLine(root.job) : ""
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }
    }

    // — to check: found, but not surely the song —
    Column {
        visible: root.check.length > 0
        width: parent.width

        Heading {
            title: "TO CHECK · " + root.check.length
        }
        Lead {
            text: "Monolist was not sure these are the songs on your list. Play one to hear it, then keep it or not; "
                  + "kept, it goes into the playlist in its place."
        }

        Repeater {
            model: root.check

            delegate: Item {
                id: checkRow
                required property var modelData
                readonly property var candidate: modelData.candidate ? modelData.candidate : ({})
                readonly property bool playing: candidate.videoId !== undefined && candidate.videoId === Player.currentSourceId

                width: root.width
                height: Math.max(40, checkText.implicitHeight) + Theme.space3 * 2
                        + (root.wide ? 0 : checkLinks.height + Theme.space2)

                HoverHandler { id: checkHover }
                Rectangle {
                    anchors.fill: parent
                    color: checkHover.hovered ? Theme.rowHover : "transparent"

                    Behavior on color {
                        enabled: !checkHover.hovered
                        ColorAnimation { duration: Theme.quick }
                    }
                }

                TrackCover {
                    id: checkCover
                    y: Theme.space3
                    width: 40
                    height: 40
                    source: checkRow.candidate.artwork ? checkRow.candidate.artwork : ""
                    sourceId: checkRow.candidate.videoId ? checkRow.candidate.videoId : ""
                    hovered: checkHover.hovered
                    active: checkRow.playing
                    covered: Nav.pagesCovered
                    onPlayRequested: root.play(checkRow.candidate)
                }

                // What YouTube Music has, over the song as the list has it.
                Column {
                    id: checkText
                    anchors.left: checkCover.right
                    anchors.leftMargin: Theme.space3
                    anchors.right: root.wide ? checkLinks.left : parent.right
                    anchors.rightMargin: Theme.space4
                    y: Theme.space3
                    spacing: 2

                    Text {
                        width: parent.width
                        text: checkRow.candidate.title ? checkRow.candidate.title : ""
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Theme.weightMedium
                        color: checkRow.playing ? Theme.accent700 : Theme.text
                    }
                    Text {
                        width: parent.width
                        text: (checkRow.candidate.artist ? checkRow.candidate.artist : "")
                              + root.lengthPart(checkRow.candidate.durationMs)
                              + (checkRow.candidate.isVideo ? "  ·  VIDEO" : "")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                    Text {
                        width: parent.width
                        readonly property string listedLength: ImportText.length(checkRow.modelData.durationMs)
                        text: "Your list: " + root.listed(checkRow.modelData)
                              + (listedLength.length > 0 ? ", " + listedLength : "") + "."
                              + (checkRow.modelData.reason ? " " + ImportText.sentence(checkRow.modelData.reason) : "")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.neutral600
                    }
                }

                Row {
                    id: checkLinks
                    x: root.wide ? parent.width - width : checkText.x
                    y: root.wide ? Theme.space3 + (Math.max(40, checkText.implicitHeight) - height) / 2
                                 : Theme.space3 + Math.max(40, checkText.implicitHeight) + Theme.space2
                    spacing: Theme.space4

                    RowLink {
                        text: checkRow.playing && Player.playing ? "PAUSE" : "PLAY"
                        onClicked: root.play(checkRow.candidate)
                    }
                    RowLink {
                        text: "USE THIS"
                        onClicked: Importer.useCandidate(checkRow.modelData.rowId)
                    }
                    RowLink {
                        text: "NOT THIS"
                        onClicked: Importer.notThis(checkRow.modelData.rowId)
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

    // A song not in the playlist: as the list had it, and what to do about it.
    component MissingRow: Item {
        id: missingRow
        required property var modelData
        // Why, under it; not where the part's lead already says it.
        property bool showReason: true

        width: root.width
        height: Math.max(40, missingText.implicitHeight) + Theme.space3 * 2
                + (root.wide ? 0 : missingLinks.height + Theme.space2)

        HoverHandler { id: missingHover }
        Rectangle {
            anchors.fill: parent
            color: missingHover.hovered ? Theme.rowHover : "transparent"

            Behavior on color {
                enabled: !missingHover.hovered
                ColorAnimation { duration: Theme.quick }
            }
        }

        // Where a cover would be: there is none to show.
        TrackCover {
            id: missingCover
            y: Theme.space3
            width: 40
            height: 40
            clickable: false
        }

        Column {
            id: missingText
            anchors.left: missingCover.right
            anchors.leftMargin: Theme.space3
            anchors.right: root.wide ? missingLinks.left : parent.right
            anchors.rightMargin: Theme.space4
            y: Theme.space3
            spacing: 2

            Text {
                width: parent.width
                text: missingRow.modelData.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Theme.weightMedium
                color: Theme.text
            }
            Text {
                visible: text.length > 0
                width: parent.width
                text: [missingRow.modelData.artist, missingRow.modelData.album, ImportText.length(missingRow.modelData.durationMs)]
                      .filter(function(part) { return part && part.length > 0 }).join("  ·  ")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral700
            }
            Text {
                visible: missingRow.showReason && text.length > 0
                width: parent.width
                text: ImportText.sentence(missingRow.modelData.reason)
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.neutral600
            }
        }

        Row {
            id: missingLinks
            x: root.wide ? parent.width - width : missingText.x
            y: root.wide ? Theme.space3 + (Math.max(40, missingText.implicitHeight) - height) / 2
                         : Theme.space3 + Math.max(40, missingText.implicitHeight) + Theme.space2
            spacing: Theme.space4

            RowLink {
                text: "SEARCH"
                onClicked: Nav.pickImportSong(missingRow.modelData, root.playlistId)
            }
            RowLink {
                text: "DISMISS"
                onClicked: Importer.dismiss(missingRow.modelData.rowId)
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.hairline
        }
    }

    // — not found —
    Column {
        visible: root.notFound.length > 0
        width: parent.width

        Heading {
            title: "NOT FOUND · " + root.notFound.length
            action: "COPY LIST"
            onActionTriggered: Importer.copyNotFound(root.jobId)
        }
        Lead {
            text: "YouTube Music had nothing close enough to these. Search for one and choose it there, and it goes "
                  + "into the playlist in its place; or dismiss it."
        }

        Repeater {
            model: root.notFound
            delegate: MissingRow {}
        }
    }

    // — not searched: the import was stopped before it got to these —
    Column {
        visible: root.notSearched.length > 0
        width: parent.width

        Heading {
            title: "NOT LOOKED FOR · " + root.notSearched.length
            action: root.going ? "" : "LOOK FOR THEM"
            secondAction: "DISMISS ALL"
            onActionTriggered: Importer.lookForStopped(root.jobId)
            onSecondActionTriggered: Importer.dismissStopped(root.jobId)
        }
        Lead {
            text: "The import was stopped before it looked for these. Look for them now, a few seconds apart as "
                  + "before; search for one yourself; or let them go."
        }

        Repeater {
            model: root.notSearched
            delegate: MissingRow { showReason: false }
        }
    }
}
