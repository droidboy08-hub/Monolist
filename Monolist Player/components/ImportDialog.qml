import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Monolist
import Monolist.Backend
import "../ImportText.js" as ImportText

// Importing a playlist (ROADMAP X03), in three steps on one sheet: where the
// songs come from (a link pasted, or a list of songs, or a CSV file); what
// that holds, shown before anything is made, with a name to change and the
// source's own caveats; then the songs being found on YouTube Music, one at
// a time, live. Paper in a 2px ink frame over a dimmed window, as the
// sign-in's caution is.
//
// It can be closed at any point. An import goes on without it: its
// progress shows in the sidebar, a toast says when it is done, and what is
// left to check waits on the playlist's page (ImportReview).
Popup {
    id: root

    // The import shown once one is started here, or asked for (the
    // sidebar's progress, a playlist's page); 0 while choosing what to import.
    property int jobId: 0
    // Where the songs come from: "link", or "list" (pasted, or a CSV file).
    property string tab: "link"
    // Stop asked once, to be confirmed.
    property bool confirmingStop: false
    // The list follows the song being looked for until it is scrolled by hand.
    property bool following: true
    // The song being looked for now, by its place in the source's order.
    property int lookingAt: -1
    // The link whose name was put in the field, so it is put there once.
    property string namedFor: ""
    signal playlistRequested(int playlistId)

    readonly property var preview: Importer.previewed
    readonly property string previewState: preview && preview.state ? preview.state : ""
    readonly property var job: Importer.job
    readonly property string step: jobId > 0 ? "job" : previewState.length > 0 ? "preview" : "source"
    // What the link field holds, as it is typed (no request: Importer.parse).
    readonly property var parsed: Importer.parse(linkField.text)
    readonly property bool wide: width >= 560
    readonly property real tallest: (parent ? parent.height : 720) - Theme.space6 * 2

    // On a link from elsewhere (Search's offer), read at once; or empty, as
    // left. What was being previewed stays, so closing by mistake loses
    // nothing.
    function openWith(text, read) {
        jobId = 0
        confirmingStop = false
        if (text.length > 0) {
            tab = "link"
            setLink(text)
            if (read)
                readLink()
        }
        open()
    }

    // On a list of songs, read at once if `read` (the demo's).
    function openList(text, read) {
        jobId = 0
        confirmingStop = false
        tab = "list"
        listArea.text = text
        if (read)
            readList()
        open()
    }

    // On an import already started.
    function openJob(id) {
        if (id <= 0)
            return
        if (job.id !== id)
            Importer.showJob(id)
        jobId = id
        confirmingStop = false
        following = true
        findLooking()
        open()
    }

    // A name put in a line of styled text, as text.
    function escaped(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    // The field's text set from here, not typed: no paste to read.
    function setLink(text) {
        linkField.text = text
        linkField.lengthBefore = text.length
    }

    // Asked on Enter or a paste, never on each key: a link is one request.
    function readLink() {
        const link = Importer.parse(linkField.text)
        if (link.looksLikeList) {
            listArea.text = linkField.text
            setLink("")
            tab = "list"
            return
        }
        if (link.ok)
            Importer.preview(linkField.text)
    }

    function readList() {
        if (listArea.text.trim().length > 0)
            Importer.previewList(listArea.text)
    }

    function importNow() {
        if (previewState !== "ready")
            return
        const id = Importer.start(nameField.text)
        if (id <= 0)
            return
        jobId = id
        following = true
        confirmingStop = false
        // What was imported is not left ready to import again: the sheet
        // opened next starts empty.
        setLink("")
        listArea.text = ""
        namedFor = ""
        findLooking()
    }

    function back() {
        Importer.clearPreview()
        namedFor = ""
    }

    function startOver() {
        jobId = 0
        confirmingStop = false
        setLink("")
        listArea.text = ""
        tab = "link"
        back()
        linkField.forceActiveFocus()
    }

    function openPlaylist() {
        const id = job.playlistId
        close()
        if (id > 0)
            playlistRequested(id)
    }

    // The first song not yet looked for: the next one the import takes.
    function findLooking() {
        const rows = Importer.rows
        for (let i = 0; i < rows.count; ++i) {
            const row = rows.get(i)
            if (row.status === "pending") {
                lookingAt = row.position
                liveList.positionViewAtIndex(i, ListView.Contain)
                return
            }
        }
        lookingAt = -1
    }

    // The source's name in the field, once, when its preview is read.
    Connections {
        target: Importer
        function onPreviewedChanged() {
            const p = Importer.previewed
            if (p.state === "ready" && p.link + p.name !== root.namedFor) {
                root.namedFor = p.link + p.name
                nameField.text = p.name
                // From its start, as a name is read, in a field too narrow for it.
                nameField.cursorPosition = 0
            }
        }
        // One song settled: the next is looked for, and the list keeps it in
        // view while it is being followed.
        function onRowSettled(jobId, position, status) {
            if (jobId !== root.jobId)
                return
            root.lookingAt = position + 1
            if (root.following && root.opened && !liveWheel.scrolling && !liveBar.pressed)
                liveList.positionViewAtIndex(Math.min(position + 1, liveList.count - 1), ListView.Contain)
        }
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(640, (parent ? parent.width : 640) - Theme.space4 * 2)
    height: step === "job" ? Math.min(700, tallest) : Math.min(implicitHeight, tallest)
    modal: true
    focus: true
    padding: Theme.space6
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        if (step === "source" && tab === "link")
            linkField.forceActiveFocus()
    }
    onClosed: confirmingStop = false

    Overlay.modal: Rectangle { color: Theme.dark ? Qt.rgba(0, 0, 0, 0.6) : "#66201e1d" }
    background: Rectangle {
        color: Theme.bg
        border.width: Theme.ruleWidth
        border.color: Theme.text
    }

    // A playlist's CSV: the system's own dialog, as Settings' cookie file is.
    FileDialog {
        id: csvDialog
        // Over the window the sheet is in (a Popup is not an item to take it from).
        parentWindow: root.parent ? root.parent.Window.window : null
        title: "Open a playlist's CSV file"
        nameFilters: ["CSV files (*.csv)", "Text files (*.txt *.tsv)", "All files (*)"]
        onAccepted: Importer.previewFile(selectedFile.toString())
    }

    // — the parts every step is made of —

    // The small tracked capitals that name a part ("FIRST SONGS").
    component SmallCaps: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    // A line of explanation.
    component Note: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 13
        lineHeight: 1.25
        color: Theme.neutral700
    }

    // A count over its name, as a table's figures are printed.
    component Figure: Column {
        property int value: 0
        property string label: ""
        spacing: 2

        Text {
            text: parent.value
            font.family: Theme.fontFamily
            font.pixelSize: 28
            font.weight: Theme.weightBlack
            font.letterSpacing: Theme.tracking(28, -0.02)
            color: Theme.text
        }
        Text {
            text: parent.label
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }
    }

    // How far it has come: a 4px bar, red while it is asking, ink when it
    // waits or is done.
    component Progress: Rectangle {
        property real value: 0
        property bool running: false
        height: 4
        color: Theme.neutral300

        Rectangle {
            width: parent.width * Math.max(0, Math.min(1, parent.value))
            height: parent.height
            color: parent.running ? Theme.accent : Theme.text
        }
    }

    contentItem: Item {
        implicitHeight: pageColumn.implicitHeight

        // — choosing, and the preview: a page that scrolls in a short window —
        Flickable {
            id: page
            visible: root.step !== "job"
            anchors.fill: parent
            contentWidth: width
            contentHeight: pageColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height

            ScrollBar.vertical: MonoScrollBar {}

            Column {
                id: pageColumn
                width: page.width - (page.interactive ? Theme.space4 : 0)
                spacing: Theme.space4

                Text {
                    width: parent.width
                    rightPadding: 32
                    text: "Import a playlist"
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 22
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(22, -0.02)
                    color: Theme.text
                }

                // ——— 1 · where the songs come from ———
                Column {
                    visible: root.step === "source"
                    width: parent.width
                    spacing: Theme.space4

                    Note {
                        text: "Bring a playlist or an album from Spotify, YouTube Music, JioSaavn, Deezer or Apple "
                              + "Music into a playlist of your own. Monolist finds each song on YouTube Music."
                    }

                    // An import under way, a click away: the one going on,
                    // whichever was started last.
                    Text {
                        readonly property var going: Importer.activeJob
                        visible: ImportText.active(going)
                        width: parent.width
                        text: visible ? "‘" + root.escaped(going.name) + "’ is still importing: "
                                        + ImportText.settled(going) + " of " + going.total + " looked for"
                                        + (going.others > 0 ? ", and " + going.others
                                                              + (going.others === 1 ? " more waits" : " more wait")
                                                              + " after it" : "")
                                        + ". <a href=\"show\">Show it</a>"
                                      : ""
                        textFormat: Text.StyledText
                        linkColor: Theme.text
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.neutral700
                        onLinkActivated: root.openJob(going.id)

                        HoverHandler {
                            cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                        }
                    }

                    // Negative spacing lets neighbouring segments share one 2px rule.
                    Row {
                        spacing: -Theme.ruleWidth

                        ChoiceChip {
                            label: "A LINK"
                            selected: root.tab === "link"
                            takesPress: true
                            onPicked: {
                                root.tab = "link"
                                linkField.forceActiveFocus()
                            }
                        }
                        ChoiceChip {
                            label: "A LIST OR A CSV FILE"
                            selected: root.tab === "list"
                            takesPress: true
                            onPicked: {
                                root.tab = "list"
                                listArea.forceActiveFocus()
                            }
                        }
                    }

                    // — a link —
                    Column {
                        visible: root.tab === "link"
                        width: parent.width
                        spacing: Theme.space3

                        TextField {
                            id: linkField
                            // A paste is read as soon as it lands; typing waits for
                            // Enter. A paste is the text growing by more than a few
                            // characters in one edit by the user, whether from the
                            // keyboard, a menu or a drop; set from elsewhere
                            // (Search's offer) it is no edit.
                            property int lengthBefore: 0
                            width: parent.width
                            implicitHeight: 40
                            placeholderText: "Paste a playlist's or an album's link"
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.text
                            placeholderTextColor: Theme.neutral500
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentForeground
                            selectByMouse: true
                            background: Rectangle {
                                color: "transparent"
                                border.width: Theme.ruleWidth
                                border.color: linkField.activeFocus ? Theme.accent : Theme.neutral300
                            }
                            // A list of songs pasted here goes where lists go,
                            // with the tab it is under.
                            onTextEdited: {
                                const pasted = text.length - lengthBefore > 8
                                lengthBefore = text.length
                                if (!pasted)
                                    return
                                const link = Importer.parse(text)
                                if (link.ok || link.looksLikeList)
                                    Qt.callLater(root.readLink)
                            }
                            onAccepted: root.readLink()
                        }

                        // What the link is, as it is typed: its platform and
                        // kind, or why it can't be imported.
                        Item {
                            width: parent.width
                            height: recognised.visible ? recognised.height : linkNote.implicitHeight

                            Row {
                                id: recognised
                                visible: root.parsed.ok === true
                                spacing: Theme.space2

                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 14
                                    height: 14
                                    name: "check"
                                    thickness: 2.5
                                    color: Theme.text
                                }
                                SmallCaps {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: (root.parsed.label || "").toUpperCase()
                                    color: Theme.text
                                }
                                SmallCaps {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "· PRESS ENTER TO READ IT"
                                }
                            }

                            Note {
                                id: linkNote
                                visible: !recognised.visible
                                text: root.parsed.reason && root.parsed.reason.length > 0
                                      ? root.parsed.reason
                                      : "A playlist's or an album's link from YouTube Music, YouTube, Spotify, JioSaavn, "
                                        + "Deezer or Apple Music. Only public ones: Monolist reads them signed out."
                                color: root.parsed.reason && root.parsed.reason.length > 0 ? Theme.accent700
                                                                                          : Theme.neutral700
                            }
                        }

                        ActionButton {
                            text: "CONTINUE"
                            primary: true
                            enabled: root.parsed.ok === true
                            onClicked: root.readLink()
                        }
                    }

                    // — a list of songs, or a CSV file —
                    Column {
                        visible: root.tab === "list"
                        width: parent.width
                        spacing: Theme.space3

                        Rectangle {
                            width: parent.width
                            height: root.wide ? 168 : 132
                            color: "transparent"
                            border.width: Theme.ruleWidth
                            border.color: listArea.activeFocus ? Theme.accent : Theme.neutral300

                            Flickable {
                                id: listFlick
                                anchors.fill: parent
                                anchors.margins: Theme.ruleWidth
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds

                                TextArea.flickable: TextArea {
                                    id: listArea
                                    wrapMode: TextEdit.Wrap
                                    padding: Theme.space3
                                    placeholderText: "One song a line, such as\na-ha - Take On Me\nBillie Jean by Michael Jackson (4:54)"
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    color: Theme.text
                                    placeholderTextColor: Theme.neutral500
                                    selectionColor: Theme.accent
                                    selectedTextColor: Theme.accentForeground
                                    selectByMouse: true
                                    background: null
                                }

                                ScrollBar.vertical: MonoScrollBar {}
                            }
                        }

                        Note {
                            text: "“Artist - Title” or “Title by Artist”, a line each; a length at the end helps. "
                                  + "A CSV file from Exportify or TuneMyMusic is read by its columns' names, ISRCs and all, "
                                  + "which is also the way to bring more than the 100 songs Spotify shows."
                        }

                        Flow {
                            width: parent.width
                            spacing: Theme.space3

                            ActionButton {
                                text: "READ THE LIST"
                                primary: true
                                enabled: listArea.text.trim().length > 0
                                onClicked: root.readList()
                            }
                            ActionButton {
                                text: "OPEN A CSV FILE…"
                                onClicked: csvDialog.open()
                            }
                        }
                    }
                }

                // ——— 2 · what it holds ———
                Column {
                    visible: root.step === "preview"
                    width: parent.width
                    spacing: Theme.space4

                    Note {
                        visible: root.previewState === "loading"
                        text: "Reading the " + (root.preview.label || "list") + "…"
                        color: Theme.text
                    }

                    Column {
                        visible: root.previewState === "failed"
                        width: parent.width
                        spacing: Theme.space1

                        SmallCaps {
                            text: "NOT READ"
                            color: Theme.accent700
                        }
                        Text {
                            width: parent.width
                            text: root.preview.error || ""
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            lineHeight: 1.25
                            color: Theme.text
                        }
                    }

                    // The cover in colour, as a playlist's page has it: this
                    // is the one the sheet is about.
                    Item {
                        visible: root.previewState === "ready"
                        width: parent.width
                        height: Math.max(previewCover.height, previewDetails.implicitHeight)

                        CollectionCover {
                            id: previewCover
                            width: root.wide ? 136 : 96
                            height: width
                            artworks: root.preview.artwork ? [root.preview.artwork] : []
                            colour: true
                        }

                        Column {
                            id: previewDetails
                            anchors.left: previewCover.right
                            anchors.leftMargin: Theme.space4
                            anchors.right: parent.right
                            anchors.bottom: previewCover.bottom
                            spacing: Theme.space2

                            SmallCaps {
                                width: parent.width
                                text: (root.preview.label || "").toUpperCase()
                                      + (root.preview.owner ? " · BY " + root.preview.owner.toUpperCase() : "")
                                elide: Text.ElideRight
                                font.letterSpacing: Theme.tracking(11, 0.14)
                                color: Theme.accent700
                            }

                            // The new playlist's name: the source's, to change.
                            TextField {
                                id: nameField
                                width: parent.width
                                leftPadding: 0
                                rightPadding: 0
                                topPadding: 0
                                bottomPadding: Theme.space1
                                maximumLength: 100
                                placeholderText: "Imported playlist"
                                font.family: Theme.fontFamily
                                font.pixelSize: root.wide ? 26 : 22
                                font.weight: Theme.weightBlack
                                font.letterSpacing: Theme.tracking(root.wide ? 26 : 22, -0.02)
                                color: Theme.text
                                placeholderTextColor: Theme.neutral500
                                selectionColor: Theme.accent
                                selectedTextColor: Theme.accentForeground
                                selectByMouse: true
                                onAccepted: root.importNow()
                                background: Item {
                                    // The field's rule, where the name sits.
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: Theme.ruleWidth
                                        color: nameField.activeFocus ? Theme.accent : Theme.neutral300
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                readonly property int readable: root.preview.readable || 0
                                readonly property int count: root.preview.count || 0
                                text: readable < count ? readable + " of " + count + " songs can be read"
                                                       : ImportText.songs(count)
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                color: Theme.neutral700
                            }
                        }
                    }

                    // The source's caveats, plainly, before anything is made.
                    Repeater {
                        model: root.previewState === "ready" && root.preview.warnings ? root.preview.warnings : []

                        delegate: Text {
                            required property string modelData
                            width: pageColumn.width
                            text: modelData
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            lineHeight: 1.25
                            color: Theme.accent700
                        }
                    }

                    // The first few songs, so the right list is known to be the one.
                    Column {
                        visible: root.previewState === "ready" && root.preview.sample && root.preview.sample.length > 0
                        width: parent.width

                        SmallCaps {
                            text: "FIRST SONGS"
                            bottomPadding: Theme.space2
                        }

                        Repeater {
                            model: root.previewState === "ready" && root.preview.sample ? root.preview.sample : []

                            delegate: Item {
                                id: sampleRow
                                required property var modelData
                                required property int index
                                width: pageColumn.width
                                height: 32

                                Rectangle {
                                    width: parent.width
                                    height: 1
                                    color: Theme.hairline
                                }
                                Text {
                                    id: sampleNumber
                                    width: 28
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: sampleRow.index + 1
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    color: Theme.neutral500
                                }
                                Text {
                                    anchors.left: sampleNumber.right
                                    anchors.right: sampleLength.left
                                    anchors.rightMargin: Theme.space3
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: sampleRow.modelData.title
                                          + (sampleRow.modelData.artist ? "  ·  " + sampleRow.modelData.artist : "")
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    color: Theme.text
                                }
                                Text {
                                    id: sampleLength
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: ImportText.length(sampleRow.modelData.durationMs)
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    color: Theme.neutral700
                                }
                            }
                        }
                    }

                    // What happens next, and how long it takes: a few seconds a
                    // song, and no faster than the searches an hour and a day
                    // allow (Importer.estimateSeconds).
                    Note {
                        visible: root.previewState === "ready"
                        readonly property bool fromYouTube: root.preview.platform === "ytmusic"
                                                            || root.preview.platform === "youtube"
                        readonly property int seconds: visible ? Importer.estimateSeconds(root.preview.readable || 0) : 0
                        text: fromYouTube
                              ? "The songs come with the list, so they go straight into a new playlist: nothing to look for."
                              : "Monolist makes the playlist now, then looks for each song on YouTube Music, signed out "
                                + "and a few seconds apart: about " + ImportText.duration(seconds) + " for these. "
                                + (seconds >= 86400
                                   ? "A list this long takes more than a day, as Monolist searches YouTube Music only "
                                     + "so many times a day; it goes on by itself, across restarts too. "
                                   : "")
                                + "You can close this meanwhile; the import goes on, and the sidebar "
                                + "shows how far it has got."
                    }

                    Flow {
                        width: parent.width
                        spacing: Theme.space3

                        ActionButton {
                            visible: root.previewState === "ready"
                            text: "IMPORT " + ImportText.songs(root.preview.readable || 0).toUpperCase()
                            primary: true
                            onClicked: root.importNow()
                        }
                        ActionButton {
                            text: "BACK"
                            onClicked: root.back()
                        }
                    }
                }
            }
        }

        // ——— 3 · finding the songs ———
        Item {
            id: jobView
            visible: root.step === "job"
            anchors.fill: parent

            readonly property bool running: ImportText.running(root.job)

            Column {
                id: jobHead
                width: parent.width
                spacing: Theme.space3

                SmallCaps {
                    width: parent.width
                    rightPadding: 32
                    text: ImportText.stateLine(root.job)
                    elide: Text.ElideRight
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: jobView.running ? Theme.accent700 : Theme.neutral700
                }

                Text {
                    width: parent.width
                    rightPadding: 32
                    text: root.job.name || ""
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: root.wide ? 28 : 22
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(root.wide ? 28 : 22, -0.02)
                    color: Theme.text
                }

                Progress {
                    width: parent.width
                    value: ImportText.progress(root.job)
                    running: jobView.running
                }

                // The four counts, side by side, as wide as the sheet.
                Row {
                    id: figures
                    width: parent.width
                    readonly property real cell: (width - spacing * 3) / 4
                    spacing: Theme.space3

                    Figure {
                        width: figures.cell
                        value: ImportText.added(root.job)
                        label: "FOUND"
                    }
                    Figure {
                        width: figures.cell
                        value: root.job.check || 0
                        label: "TO CHECK"
                    }
                    // A stop's songs never asked for are left, not "not found".
                    Figure {
                        width: figures.cell
                        value: (root.job.notFound || 0) - (root.job.notSearched || 0)
                        label: "NOT FOUND"
                    }
                    Figure {
                        width: figures.cell
                        value: (root.job.pending || 0) + (root.job.notSearched || 0)
                        label: "LEFT"
                    }
                }

                Text {
                    width: parent.width
                    text: ImportText.noteLine(root.job)
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    lineHeight: 1.25
                    color: root.job.state === "paused" || root.job.state === "resting" || root.job.state === "waiting"
                           ? Theme.text : Theme.neutral700
                }
            }

            // Every song of the source, as it was listed, and what became of it.
            Rectangle {
                id: listRule
                anchors.top: jobHead.bottom
                anchors.topMargin: Theme.space4
                width: parent.width
                height: Theme.ruleWidth
                color: Theme.divider
            }

            ListView {
                id: liveList
                anchors.top: listRule.bottom
                anchors.bottom: jobFoot.top
                anchors.bottomMargin: Theme.space4
                width: parent.width
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.step === "job" ? Importer.rows : null
                // Scrolled by hand, by a drag, the wheel or the scroll bar: it
                // stops following until the sheet is opened again.
                onMovementStarted: root.following = false

                ScrollBar.vertical: MonoScrollBar {
                    id: liveBar
                    onPressedChanged: if (pressed) root.following = false
                }
                SmoothWheel {
                    id: liveWheel
                    flickable: liveList
                    onScrollingChanged: if (scrolling) root.following = false
                }

                delegate: Item {
                    id: songRow
                    required property int position
                    required property string title
                    required property string artist
                    required property real durationMs
                    required property string status
                    required property string reason

                    readonly property bool looking: status === "pending" && position === root.lookingAt
                                                    && root.job.state === "finding"
                    readonly property string label: ImportText.statusLabel(status, reason, looking)

                    width: liveList.width - (liveList.contentHeight > liveList.height ? Theme.space4 : 0)
                    height: 44

                    Text {
                        id: songNumber
                        width: 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: songRow.position + 1
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: songRow.looking ? Theme.accent700 : Theme.neutral500
                    }

                    Column {
                        anchors.left: songNumber.right
                        anchors.right: songStatus.left
                        anchors.rightMargin: Theme.space3
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1

                        Text {
                            width: parent.width
                            text: songRow.title
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            font.weight: Theme.weightMedium
                            color: songRow.status === "pending" && !songRow.looking ? Theme.neutral600 : Theme.text
                        }
                        // The artist and the length, whichever the list gave.
                        Text {
                            width: parent.width
                            text: [songRow.artist, ImportText.length(songRow.durationMs)]
                                  .filter(function(part) { return part.length > 0 }).join("  ·  ")
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }

                    // What became of it: ink for what waits on the user, grey
                    // for what is settled, red for the one being looked for.
                    Text {
                        id: songStatus
                        width: 84
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: Text.AlignRight
                        text: songRow.label
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Bold
                        font.letterSpacing: Theme.tracking(11, 0.08)
                        color: songRow.looking ? Theme.accent700
                             : songRow.status === "check" || songRow.status === "notfound" ? Theme.text
                             : Theme.neutral600
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: Theme.hairline
                    }
                }
            }

            Column {
                id: jobFoot
                anchors.bottom: parent.bottom
                width: parent.width
                spacing: Theme.space3

                // Stopping, confirmed: what was found stays.
                Rectangle {
                    visible: root.confirmingStop && !root.job.done
                    width: parent.width
                    height: stopColumn.implicitHeight + Theme.space4 * 2
                    color: "transparent"
                    border.width: Theme.ruleWidth
                    border.color: Theme.accent

                    Column {
                        id: stopColumn
                        x: Theme.space4
                        y: Theme.space4
                        width: parent.width - Theme.space4 * 2
                        spacing: Theme.space3

                        Text {
                            width: parent.width
                            text: "Stop the import? The " + ImportText.songs(ImportText.added(root.job))
                                  + " found stay in the playlist; the rest wait on its page, to look for later or let go."
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            color: Theme.text
                        }
                        Flow {
                            width: parent.width
                            spacing: Theme.space3

                            ActionButton {
                                text: "STOP"
                                primary: true
                                onClicked: {
                                    root.confirmingStop = false
                                    Importer.cancel(root.jobId)
                                }
                            }
                            ActionButton {
                                text: "GO ON"
                                onClicked: root.confirmingStop = false
                            }
                        }
                    }
                }

                Flow {
                    visible: !root.confirmingStop || root.job.done
                    width: parent.width
                    spacing: Theme.space3

                    // Paused and resumed by hand. Not while it rests or waits
                    // on YouTube Music's account of it (a refusal, the
                    // hour's or the day's searches): that pause is the
                    // owner's rule, and goes on by itself when it says.
                    ActionButton {
                        readonly property bool held: root.job.state === "paused" && !(root.job.resumeAt > 0)
                        visible: !root.job.done && (held || root.job.state === "reading" || root.job.state === "finding"
                                                    || root.job.state === "queued")
                        text: held ? "RESUME" : "PAUSE"
                        primary: held
                        onClicked: held ? Importer.resume(root.jobId) : Importer.pause(root.jobId)
                    }
                    ActionButton {
                        visible: !root.job.done
                        text: "STOP"
                        onClicked: root.confirmingStop = true
                    }
                    ActionButton {
                        visible: root.job.playlistGone !== true
                        text: "OPEN PLAYLIST"
                        primary: root.job.done === true
                        onClicked: root.openPlaylist()
                    }
                    ActionButton {
                        visible: root.job.done === true
                        text: "IMPORT ANOTHER"
                        onClicked: root.startOver()
                    }
                }
            }
        }

        // Closing never stops anything: an import goes on without the sheet.
        IconButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: -Theme.space1
            anchors.rightMargin: -Theme.space1
            side: 28
            iconName: "x"
            iconSize: 16
            iconColor: Theme.neutral700
            onClicked: root.close()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: root.step === "job" && !root.job.done ? "Close; the import goes on" : "Close"
        }
    }
}
