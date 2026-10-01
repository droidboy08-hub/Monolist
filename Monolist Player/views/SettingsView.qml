import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Monolist
import Monolist.Backend
import "../components"

// Settings: the country to browse, playback, how downloads are saved, and
// what this copy of the app is.
ScrollPage {
    id: root

    // Every country, fetched once; the picker filters this list.
    property var allCountries: []
    property string filter: ""

    readonly property bool automatic: Library.region.length === 0

    // A part of the page to open on, for a link elsewhere that names one
    // ("recommendations", "connections", or "ytmusic" for the YouTube Music
    // account's switches): scrolled to once, then said done, so the same
    // link works again later.
    property string section: ""
    signal sectionRevealed()

    contentHeight: column.implicitHeight

    Component.onCompleted: {
        allCountries = Library.countries()
        // Versions cost a process each to read, so they are asked for when
        // this page first opens (Main.qml builds it then, not at launch), and
        // in the background: "Reading versions…" shows until they answer.
        if (!About.componentsKnown)
            About.refreshComponents()
        if (section.length > 0)
            Qt.callLater(revealSection)
    }

    onSectionChanged: if (section.length > 0) Qt.callLater(revealSection)

    // After the page has been laid out, which a page built this moment has
    // not been yet: the column is asked to place everything first.
    function setAppearance(mode) {
        Theme.mode = mode
        Library.setSetting("appearance", mode)
    }

    function revealSection() {
        if (section.length === 0)
            return
        column.forceLayout()
        // "signin": the account's row, its sign-in open unless signed in.
        if (section === "signin" && ytmRow.serviceState !== "connected")
            ytmRow.openSignIn()
        var target = section === "recommendations" ? recommendationsHeader
                   : section === "downloads" ? downloadsHeader
                   : section === "connections" ? connectionsHeader
                   : section === "signin" ? ytmRow
                   : section === "ytmusic" ? (homeToggle.visible ? homeToggle : ytmRow) : null
        if (target)
            contentY = Math.max(0, Math.min(target.y - Theme.space4, contentHeight - height))
        sectionRevealed()
    }

    // The exported YouTube Music session. The system's own dialog, so the
    // file is picked the way every other file is.
    FileDialog {
        id: cookieFileDialog
        title: "Choose the exported cookies file"
        nameFilters: ["Cookie files (*.txt *.cookies)", "All files (*)"]
        onAccepted: {
            if (Account.importFile(selectedFile))
                ytmRow.importing = false
        }
    }

    // Where downloads are saved: the system's own folder picker, opened on
    // the folder in use.
    FolderDialog {
        id: downloadFolderDialog
        title: "Choose where downloads are saved"
        currentFolder: "file:///" + Downloads.downloadDirectory.replace(/\\/g, "/")
        onAccepted: Downloads.setDownloadDirectory(selectedFolder.toString())
    }

    function matches() {
        var text = filter.trim().toLowerCase()
        if (text.length === 0)
            return allCountries
        var found = []
        for (var i = 0; i < allCountries.length; ++i) {
            var country = allCountries[i]
            if (country.name.toLowerCase().indexOf(text) >= 0 || country.code.toLowerCase() === text)
                found.push(country)
        }
        return found
    }

    component Note: Text {
        width: column.width
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.neutral700
    }

    // The small tracked capitals that name a part of the page ("COUNTRY").
    // Not called Label: QtQuick.Controls has one.
    component SmallCaps: Text {
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(11, 0.08)
        color: Theme.neutral700
    }

    // A line of the YouTube Music guide: an instruction to follow, so ink.
    component GuideText: Text {
        wrapMode: Text.WordWrap
        font.family: Theme.fontFamily
        font.pixelSize: 13
        color: Theme.text
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
            title: "Settings"
        }

        // — region —
        Text {
            text: "COUNTRY"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }

        Note {
            text: "Which country's music YouTube Music offers: new releases, and how search results are ranked. "
                  + "Your connection still has a say, so a picked country steers the lists rather than replacing them."
        }

        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "AUTOMATIC"
                selected: root.automatic
                onPicked: Library.region = ""
            }
            ChoiceChip {
                label: "CHOOSE A COUNTRY"
                selected: !root.automatic
                // Opening the picker keeps whatever is set until one is picked.
                onPicked: if (root.automatic) Library.region = Library.regionInUse
            }
        }

        Note {
            text: root.automatic
                  ? "Following the system: " + Library.systemRegionName + " (" + Library.regionInUse + ")."
                  : "Browsing " + Library.regionInUseName + " (" + Library.regionInUse + ")."
        }

        // — the picker —
        Rectangle {
            visible: !root.automatic
            width: Math.min(parent.width, 520)
            height: 320
            color: "transparent"
            border.width: Theme.ruleWidth
            border.color: Theme.text

            // Opened to be typed in.
            onVisibleChanged: if (visible) filterField.forceActiveFocus()

            Rectangle {
                id: search
                width: parent.width
                height: 40
                color: "transparent"

                Icon {
                    id: searchIcon
                    name: "search"
                    width: 15
                    height: 15
                    x: Theme.space4
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.neutral600
                }

                TextInput {
                    id: filterField
                    anchors.left: searchIcon.right
                    anchors.leftMargin: Theme.space2
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space4
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.accentForeground
                    clip: true
                    onTextChanged: root.filter = text

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: filterField.text.length === 0
                        text: "Find a country…"
                        font: filterField.font
                        color: Theme.neutral500
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.ruleWidth
                    color: Theme.divider
                }
            }

            ListView {
                id: countryList
                anchors.top: search.bottom
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: Theme.ruleWidth
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.matches()

                ScrollBar.vertical: MonoScrollBar { width: 8 }

                delegate: Item {
                    id: entry

                    required property var modelData

                    readonly property bool current: modelData.code === Library.regionInUse

                    width: ListView.view ? ListView.view.width : 0
                    height: 34

                    Rectangle {
                        anchors.fill: parent
                        color: countryHover.hovered ? Theme.rowHover : "transparent"

                        Behavior on color {
                            enabled: !countryHover.hovered
                            ColorAnimation { duration: Theme.quick }
                        }
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space4
                        anchors.right: code.left
                        anchors.rightMargin: Theme.space3
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.name
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: entry.current ? Font.Bold : Theme.weightRegular
                        color: entry.current ? Theme.accent700 : Theme.text
                    }

                    Text {
                        id: code
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space4
                        anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.code
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.letterSpacing: Theme.tracking(11, 0.08)
                        color: entry.current ? Theme.accent700 : Theme.neutral500
                    }

                    HoverHandler { id: countryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: Library.region = entry.modelData.code }
                }
            }
        }

        HRule { width: parent.width }

        // — appearance —
        // Paper and ink, or ink and paper, or as Windows has it; and the
        // song's colour through it all.
        SectionHeader {
            width: parent.width
            number: "02"
            title: "Appearance"
        }

        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "LIGHT"
                selected: Theme.mode === "light"
                onPicked: root.setAppearance("light")
            }
            ChoiceChip {
                label: "DARK"
                selected: Theme.mode === "dark"
                onPicked: root.setAppearance("dark")
            }
            ChoiceChip {
                label: "MATCH WINDOWS"
                selected: Theme.mode !== "light" && Theme.mode !== "dark"
                onPicked: root.setAppearance("system")
            }
        }

        Note {
            visible: Theme.mode !== "light" && Theme.mode !== "dark"
            text: "Following Windows, which is set to " + (Theme.dark ? "dark" : "light")
                  + " (Settings > Personalisation > Colours)."
        }

        ToggleRow {
            width: parent.width
            label: "Ambient colour"
            hint: "The playing song's cover lends the whole app a little of its colour, changing with each song."
            checked: Theme.ambientEnabled
            onToggled: {
                Theme.ambientEnabled = !Theme.ambientEnabled
                Library.setSetting("ambient", Theme.ambientEnabled ? "1" : "0")
            }
        }

        HRule { width: parent.width }

        // — playback —
        SectionHeader {
            width: parent.width
            number: "03"
            title: "Playback"
        }

        ToggleRow {
            width: parent.width
            label: "Autoplay similar songs"
            hint: "When the queue runs out, carry on with YouTube Music's radio for the last song."
            checked: Player.autoplay
            onToggled: Player.autoplay = !Player.autoplay
        }

        ToggleRow {
            width: parent.width
            label: "Even out loudness"
            hint: "Songs that were mastered loud are turned down to the level of the rest, by what YouTube "
                  + "measured of each, as YouTube Music plays them. Quiet songs are never turned up."
            checked: Player.levelLoudness
            onToggled: Player.levelLoudness = !Player.levelLoudness
        }

        // Sound quality, chosen like the video size below. High is the one
        // that brings in a second company's service, so it is never on until
        // the listener picks it: on Standard, JioSaavn is not asked anything.
        Text {
            text: "SOUND"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }

        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "Standard"
                selected: !Player.saavnEnabled
                onPicked: Player.saavnEnabled = false
            }
            ChoiceChip {
                label: "High · 320 kbps"
                selected: Player.saavnEnabled
                onPicked: Player.saavnEnabled = true
            }
        }

        Note {
            text: Player.saavnEnabled
                  ? "Each song is also looked for on JioSaavn, an Indian music service, by title and artist, and plays "
                    + "from there at up to 320 kbps only when it is exactly the same recording. Otherwise YouTube "
                    + "plays, as on Standard."
                  : "YouTube's own stream (Opus, about 160 kbps). Nothing is asked of any other service."
        }

        // What JioSaavn is told, said plainly: the requests claim to come
        // from somewhere they do not.
        ToggleRow {
            visible: Player.saavnEnabled
            width: parent.width
            label: "Send Indian region headers to JioSaavn"
            hint: "Many songs are offered only in India, so each request claims to come from an Indian address, "
                  + "which it does not. Off, far fewer songs are found there."
            checked: Player.saavnIndiaHeaders
            onToggled: Player.saavnIndiaHeaders = !Player.saavnIndiaHeaders
        }

        // The mid-song move (QT7): a song YouTube started because JioSaavn
        // answered a moment too late goes over to JioSaavn where it is.
        ToggleRow {
            visible: Player.saavnEnabled
            width: parent.width
            label: "Switch to JioSaavn mid-song"
            hint: "When JioSaavn answers only after YouTube has started a song, the song moves over to JioSaavn's "
                  + "copy at the same moment, once that is ready to play, with a short crossfade and no gap. Off, "
                  + "it stays on YouTube until it is played again."
            checked: Player.saavnUpgrade
            onToggled: Player.saavnUpgrade = !Player.saavnUpgrade
        }

        Text {
            text: "VIDEO"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
        }

        // Negative spacing lets neighbouring segments share one 2px rule.
        Row {
            spacing: -Theme.ruleWidth

            ChoiceChip {
                label: "360p"
                selected: Library.videoQuality === 360
                onPicked: Library.videoQuality = 360
            }
            ChoiceChip {
                label: "720p"
                selected: Library.videoQuality === 720
                onPicked: Library.videoQuality = 720
            }
            ChoiceChip {
                label: "1080p"
                selected: Library.videoQuality === 1080
                onPicked: Library.videoQuality = 1080
            }
        }

        Note {
            text: Library.videoQuality === 360
                  ? "Smallest, and the one to choose on a machine that decodes video in software."
                  : Library.videoQuality === 1080
                    ? "As large as a music video usually comes. Heavy without hardware decoding."
                    : "What most music videos are published at. The next video uses the new size."
        }

        // Asked of LRCLIB and YouTube Music only, never of anyone else, and
        // not for the next song on a metered connection (Lyrics.background).
        ToggleRow {
            width: parent.width
            label: "Look up lyrics in the background"
            hint: "Lyrics for the song playing, and the one after it, are fetched from LRCLIB and YouTube Music "
                  + "as each song starts, so they are there when you open them. Off, a song's lyrics are looked "
                  + "up only once you open them."
            checked: Lyrics.background
            onToggled: Lyrics.background = !Lyrics.background
        }

        HRule { width: parent.width }

        // — downloads —
        SectionHeader {
            id: downloadsHeader
            width: parent.width
            number: "04"
            title: "Downloads"
            action: "OPEN FOLDER →"
            onActionTriggered: Downloads.openDownloadFolder()
        }

        DownloadOptions {
            width: parent.width
        }

        Item {
            width: parent.width
            height: Math.max(folderPath.implicitHeight, folderButtons.implicitHeight)

            Text {
                id: folderPath
                anchors.left: parent.left
                anchors.right: folderButtons.left
                anchors.rightMargin: Theme.space4
                anchors.verticalCenter: parent.verticalCenter
                text: Downloads.downloadDirectory
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.text
            }

            Row {
                id: folderButtons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space2

                // Back to Music\Monolist, once another folder was chosen.
                ActionButton {
                    visible: Downloads.customDirectory
                    text: "DEFAULT"
                    enabled: Downloads.activeCount === 0
                    onClicked: Downloads.setDownloadDirectory("")
                }
                ActionButton {
                    text: "CHANGE…"
                    enabled: Downloads.activeCount === 0
                    onClicked: downloadFolderDialog.open()
                }
            }
        }

        Note {
            text: Downloads.directoryNote.length > 0
                  ? Downloads.directoryNote
                  : Downloads.activeCount > 0
                    ? "The folder can be changed once the downloads under way have finished."
                    : "New downloads are saved here. Songs already downloaded stay where they are; a folder "
                      + "you moved yourself is found again once you choose its new place."
        }

        HRule { width: parent.width }

        // — recommendations —
        //
        // The catalogue is not shipped with the app and cannot be: it is
        // licensed for non-commercial use only. So it is downloaded from a
        // repository of its own, or pointed at, rather than bundled, and the
        // app works perfectly well without one.
        SectionHeader {
            id: recommendationsHeader
            width: parent.width
            number: "05"
            title: "Recommendations"
        }

        Note {
            text: Recs.available
                  ? "Reading from the folder below. Search suggests songs from it when nothing is typed."
                  : (Recs.message.length > 0
                     ? Recs.message
                     : "Point this at a folder holding the four embeat_v1_*.bin files to get suggestions in Search.")
        }

        // — the data, downloaded —
        //
        // Offered whenever there is nothing to recommend from, and kept in
        // view once it is here, or part of it is, with Remove. Hidden while a
        // folder of the listener's own is in use: that needs nothing from it.
        Column {
            visible: RecData.busy || RecData.removing || RecData.installed || RecData.partial
                     || RecData.failed || (!Recs.available && !Recs.busy)
            width: parent.width
            spacing: Theme.space3

            Text {
                text: "DATA"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: Theme.tracking(11, 0.08)
                color: Theme.neutral700
            }

            RecDataPanel {
                width: parent.width
                inSettings: true
            }
        }

        // The folders themselves, for a copy kept anywhere else. The download
        // fills the first in when it is done; typing over it still works.
        Text {
            text: "CATALOGUE"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
            topPadding: Theme.space2
        }

        Item {
            width: parent.width
            height: Math.max(pathField.implicitHeight, rebuild.implicitHeight)

            TextField {
                id: pathField
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - rebuild.width - Theme.space4
                text: Recs.dataDirectory
                placeholderText: "Folder holding embeat_v1_vectors.bin…"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.text
                placeholderTextColor: Theme.neutral500
                selectionColor: Theme.accent
                selectedTextColor: Theme.accentForeground
                background: Rectangle {
                    color: "transparent"
                    border.width: Theme.ruleWidth
                    border.color: pathField.activeFocus ? Theme.accent : Theme.neutral300
                }
                onEditingFinished: Recs.dataDirectory = text
            }

            ActionButton {
                id: rebuild
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: Recs.busy ? "WORKING…" : "REBUILD"
                enabled: Recs.available && !Recs.busy
                onClicked: Recs.rebuild()
            }
        }
        Text {
            text: "GRAPH"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
            topPadding: Theme.space2
        }

        Note {
            // Said plainly, because it decides what "Popular in" means: the
            // shards rank artists by how many MusicBrainz ratings they have,
            // not by how much they are played.
            text: Recs.graphAvailable
                  ? "Regional charts for “Popular in " + Library.regionInUseName + "”. Found beside the catalogue unless set here."
                  : "The regional graph shards (US.sqlite, JP.sqlite and so on). Looked for beside the catalogue unless set here."
        }

        TextField {
            id: graphField
            width: parent.width
            text: Recs.graphDirectory
            placeholderText: "Automatic — the GraphData folder beside the catalogue"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.text
            placeholderTextColor: Theme.neutral500
            selectionColor: Theme.accent
            selectedTextColor: Theme.accentForeground
            background: Rectangle {
                color: "transparent"
                border.width: Theme.ruleWidth
                border.color: graphField.activeFocus ? Theme.accent : Theme.neutral300
            }
            onEditingFinished: Recs.graphDirectory = text
        }

        // Only the suggestions: what someone searches for, or opens, they
        // asked for by name. The note says it goes by the title because the
        // catalogue has no explicit flag, so a clean title can still hide an
        // explicit song.
        ToggleRow {
            width: parent.width
            label: "Hide explicit titles"
            hint: "Keep songs whose titles are crude or sexual, or marked Explicit, off the suggestions in Search. "
                  + "It goes by the title alone; search results and albums are left as they are."
            checked: Recs.hideExplicit
            onToggled: Recs.hideExplicit = !Recs.hideExplicit
        }

        // The licences ask for it wherever the data is used.
        DataCredit {
            width: parent.width
        }

        HRule { width: parent.width }

        // — connections —
        //
        // Last.fm scrobbles; YouTube Music signs in, and what it brings comes
        // next. The app works entirely without any of these and is meant to
        // keep working that way; what an account buys is your own library
        // and your own history, not a better player.
        SectionHeader {
            id: connectionsHeader
            width: parent.width
            number: "06"
            title: "Connections"
        }

        Note {
            // Follows the rows, so it cannot go on saying nothing is signed
            // in once something is. Plain text: account names are not markup.
            textFormat: Text.PlainText
            text: {
                var said = []
                if (lastFmRow.connected)
                    said.push("Last.fm is connected as " + lastFmRow.accountName)
                // A session being checked is held, and its cookies go with
                // the check: not "nothing signed in".
                if (Account.state === "active")
                    said.push("YouTube Music is signed in"
                              + (Account.accountName.length > 0 ? " as " + Account.accountName : ""))
                else if (Account.state === "checking")
                    said.push("a YouTube Music sign-in is being checked")
                else if (Account.state === "unreachable")
                    said.push("a YouTube Music sign-in is held, waiting to be checked")
                if (said.length > 0)
                    said[0] = said[0].charAt(0).toUpperCase() + said[0].slice(1)
                if (said.length === 0)
                    return "Nothing is signed in. Monolist plays without an account, and keeps your library "
                           + "on this computer. Connecting one would add what only an account can know."
                return said.join("; ") + ". Everything else still works without an account, and your library "
                       + "stays on this computer."
            }
        }

        ServiceRow {
            id: lastFmRow
            width: parent.width
            name: "Last.fm"
            detail: "Scrobble what you play, and see what you have been listening to."
            // Where it stands comes from the Scrobbler: unavailable in a build
            // with no key (CONNECT greyed, and the line says why), otherwise
            // off, waiting on the browser, connected, or needing a reconnect.
            built: true
            serviceState: Scrobbler.state
            accountName: Scrobbler.accountName
            statusLine: Scrobbler.statusLine
            confirmText: "I'VE APPROVED IT"
            // A Disconnect that could not delete the key is tried again as a
            // Disconnect; and an account that stopped working can be let go
            // of rather than only reconnected.
            errorAction: Scrobbler.disconnectFailed ? "disconnect" : "connect"
            forgetText: Scrobbler.accountName.length > 0 ? "DISCONNECT" : ""
            // Last.fm's terms ask for the credit, and for the account to link
            // to its own page there.
            credit: "powered by <a href=\"https://www.last.fm\">AudioScrobbler</a>"
                    + (Scrobbler.accountName.length > 0
                       ? " · <a href=\"https://www.last.fm/user/" + encodeURIComponent(Scrobbler.accountName)
                         + "\">your Last.fm profile</a>"
                       : "")
            steps: [
                "Monolist opens Last.fm in your browser, where you approve it. Your password is never typed into this app.",
                "Last.fm hands back a session key, which is kept on this computer, encrypted with your Windows sign-in (the Keychain on a Mac), and can be revoked from your Last.fm account at any time.",
                "A track counts as played once you have heard half of it or four minutes, whichever comes first; tracks of 30 seconds or less never count. "
                + "While a track plays, Last.fm is also told what is playing now. Nothing else is sent."
            ]
            onConnectRequested: Scrobbler.connectAccount()
            onCancelRequested: Scrobbler.cancelConnect()
            onConfirmRequested: Scrobbler.checkApproval()
            onDisconnectRequested: Scrobbler.disconnectAccount()
        }

        // Only once there is an account to scrobble to. Off, nothing is kept
        // or sent; what was already waiting stays for when it is on again.
        ToggleRow {
            visible: Scrobbler.state === "connected" || Scrobbler.state === "expired"
            width: parent.width
            label: "Scrobble what I play"
            hint: "Off, Last.fm is told nothing, not even what is playing now."
            checked: Scrobbler.enabled
            onToggled: Scrobbler.enabled = !Scrobbler.enabled
        }

        // YouTube Music: a session imported from the user's own browser, since
        // Google allows no sign-in from inside an app like this one. The row
        // says Signed in only once YouTube Music has confirmed the session
        // (Account checks it online); while that is under way, or YouTube
        // Music cannot be reached, it says so, offers SIGN OUT, and uses the
        // session for nothing.
        //
        // Opened, the row is a guide someone can follow alone: the steps,
        // then the two ways to copy a sign-in out of a browser (a cookies.txt
        // file from an extension, or the Cookie header from the developer
        // tools), each with its own control, then what differs in each
        // browser. Nothing here signs in to anything or reads a browser's
        // own cookie store: the user copies the session, and hands it over.
        ServiceRow {
            id: ytmRow

            // Open while the user imports: the steps, and where the file or
            // the pasted header goes. CANCEL closes it, and so does an import
            // that was read.
            property bool importing: false
            readonly property string account: Account.state
            // Room for the two ways side by side, and the browsers as a table.
            readonly property bool wideGuide: width >= 900

            // What differs from browser to browser, for the table under the
            // two ways. The extensions are examples, named because people
            // ask; Monolist has nothing to do with them.
            readonly property var browsers: [
                {
                    name: "Chrome",
                    window: "The three-dot menu → New Incognito window, or Ctrl+Shift+N (Cmd+Shift+N on a Mac).",
                    extension: "For example “Get cookies.txt LOCALLY”, from the Chrome Web Store. In chrome://extensions, "
                               + "open its Details and turn on Allow in Incognito.",
                    tools: "F12 (Cmd+Option+I on a Mac), then Network. Right-click the request → Copy → Copy as "
                           + "cURL; bash or cmd, either works."
                },
                {
                    name: "Edge",
                    window: "The three-dot menu → New InPrivate window, or Ctrl+Shift+N (Cmd+Shift+N on a Mac).",
                    extension: "Edge takes Chrome's extensions: turn on Allow extensions from other stores in "
                               + "edge://extensions, add one such as “Get cookies.txt LOCALLY” from the Chrome Web "
                               + "Store, then turn on Allow in InPrivate in its Details.",
                    tools: "F12 (Cmd+Option+I on a Mac), then Network. Right-click the request → Copy → Copy as "
                           + "cURL; bash or cmd, either works."
                },
                {
                    name: "Firefox",
                    window: "The menu button → New private window, or Ctrl+Shift+P (Cmd+Shift+P on a Mac).",
                    extension: "For example “cookies.txt”, from addons.mozilla.org. In about:addons, open it and set "
                               + "Run in Private Windows to Allow.",
                    tools: "F12 (Cmd+Option+I on a Mac), then Network. Right-click the request → Copy Value → Copy "
                           + "as cURL; or, in its Headers, turn on Raw beside Request Headers and copy the Cookie line."
                },
                {
                    name: "Safari",
                    window: "File → New Private Window, or Cmd+Shift+N.",
                    extension: "No common cookies.txt extension: use B.",
                    tools: "First turn on Settings → Advanced → Show features for web developers. Then Develop → "
                           + "Show Web Inspector (Cmd+Option+I) → Network; right-click the request → Copy as cURL."
                }
            ]

            width: parent.width
            name: "YouTube Music"
            detail: "Your own playlists, likes and listening history, instead of this computer's."
            caution: "Use an account you can afford to lose: Google restricts accounts used by outside players, "
                     + "and that would take the account with it. Sign in only on Google's own page, in a private "
                     + "window of your own browser. Monolist never asks for your password."
            built: true
            serviceState: importing ? "waiting"
                          : account === "active" || account === "checking" || account === "unreachable"
                            ? "connected"
                          : account === "rejected" ? "expired"
                          : "off"
            accountName: account === "active" ? Account.accountName : ""
            // "Signed in as …", "Checking…", "Could not reach YouTube Music",
            // "Session expired", "Not signed in": Account's words, since it
            // knows why.
            headline: importing ? "" : Account.headline
            statusLine: importing ? "" : Account.statusLine
            // What SIGN OUT leaves undone, while there is a session for it to
            // sign out.
            credit: serviceState === "connected"
                    ? "Signing out deletes Monolist's copy only. To end the session at Google too: Google Account → "
                      + "Security → <a href=\"https://myaccount.google.com/device-activity\">Your devices</a>."
                    : ""
            actionText: serviceState === "connected" ? "SIGN OUT"
                        : serviceState === "expired" ? "IMPORT AGAIN"
                        : serviceState === "off" ? "IMPORT SIGN-IN"
                        : ""
            // A session that ended can be forgotten, name and all, instead
            // of imported again; one that never signed anyone in is only
            // put away.
            forgetText: Account.accountName.length > 0 ? "SIGN OUT" : "DISMISS"
            // The check could not be made: kept, and asked again later, or
            // now.
            retryText: account === "unreachable" ? "CHECK NOW" : ""
            steps: [
                "In your own browser, open a private window (how, in each browser, is below) and sign in at music.youtube.com, "
                + "on Google's own page. You are signed in once your picture shows at the top right. Firefox is the one "
                + "to prefer: Chrome on Windows can tie a session to itself, which may end a copied one sooner.",
                "Copy the sign-in out of that window, in one of the two ways below: A, with an extension that saves "
                + "a cookies.txt file; or B, with the developer tools, copying the Cookie header to paste.",
                "Close the private window, but do not sign out first. Signing out ends the session at Google, and the "
                + "copy Monolist holds would stop working at once; closing the window only throws away the browser's "
                + "own copy.",
                "Monolist reads the sign-in once and keeps it encrypted with your Windows sign-in (the Keychain on a Mac), "
                + "never in the music database or any log. It then asks YouTube Music whether it works, and says Signed "
                + "in only once YouTube Music does."
            ]
            // The last refusal's reason belongs to the last try: a panel
            // opened or closed starts clean.
            onConnectRequested: openSignIn()
            function openSignIn() {
                Account.clearImportError()
                importing = true
            }
            onCancelRequested: {
                importing = false
                pasteField.clear()
                Account.clearImportError()
            }
            onDisconnectRequested: Account.signOut()
            onRetryRequested: Account.checkNow()

            // — the import, in the open panel —
            Note {
                visible: !Account.remembered
                width: parent.width
                text: "Keeping a sign-in safely is not available on this system yet, so Monolist holds it "
                      + "only until it closes."
            }

            // The two ways, side by side where there is room: each is whole
            // on its own, with its own control, and the user takes one.
            Grid {
                id: ways
                width: parent.width
                columns: ytmRow.wideGuide ? 2 : 1
                columnSpacing: Theme.space8
                rowSpacing: Theme.space6
                readonly property real cell: ytmRow.wideGuide ? (width - columnSpacing) / 2 : width
                // Side by side, both explanations take the longer one's
                // height, so the two controls sit on one line.
                readonly property real textHeight: Math.max(wayA.implicitHeight, wayB.implicitHeight)

                Column {
                    width: ways.cell
                    spacing: Theme.space3

                    SmallCaps { text: "A · WITH AN EXTENSION" }
                    GuideText {
                        id: wayA
                        width: parent.width
                        height: ytmRow.wideGuide ? ways.textHeight : implicitHeight
                        text: "Install a cookies.txt extension from your browser's own store (the table below names "
                              + "some) and allow it in private windows, where extensions are off until you "
                              + "do. In the private window, with music.youtube.com open, have it export the site's "
                              + "cookies to a file, then choose that file here. Such an extension can read every "
                              + "site's cookies: pick a well-known one, and remove it when you are done."
                    }
                    ActionButton {
                        text: "CHOOSE FILE…"
                        onClicked: cookieFileDialog.open()
                    }
                }

                Column {
                    width: ways.cell
                    spacing: Theme.space3

                    SmallCaps { text: "B · WITH THE DEVELOPER TOOLS" }
                    GuideText {
                        id: wayB
                        width: parent.width
                        height: ytmRow.wideGuide ? ways.textHeight : implicitHeight
                        text: "No extension needed. In the private window, on music.youtube.com, open the developer "
                              + "tools, choose Network, and reload the page. Type browse into the filter and click a "
                              + "request to music.youtube.com. Copy its Cookie request header, or right-click it and "
                              + "copy it as cURL, and paste it here. What you paste stays hidden, as a password does."
                    }

                    // Masked, like a password: what is pasted is the session itself.
                    Item {
                        width: parent.width
                        height: Math.max(pasteField.implicitHeight, importButton.implicitHeight)

                        TextField {
                            id: pasteField
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - importButton.width - Theme.space4
                            implicitHeight: 40
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                            // A copied cURL command runs to several thousand characters.
                            maximumLength: 1048576
                            placeholderText: "Paste the Cookie header, or the request copied as cURL"
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.text
                            placeholderTextColor: Theme.neutral500
                            selectionColor: Theme.accent
                            selectedTextColor: Theme.accentForeground
                            background: Rectangle {
                                color: "transparent"
                                border.width: Theme.ruleWidth
                                border.color: pasteField.activeFocus ? Theme.accent : Theme.neutral300
                            }
                            onAccepted: if (importButton.enabled) importButton.clicked()
                        }

                        ActionButton {
                            id: importButton
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "IMPORT"
                            enabled: pasteField.text.length > 0
                            onClicked: {
                                if (Account.importText(pasteField.text)) {
                                    pasteField.clear()
                                    ytmRow.importing = false
                                }
                            }
                        }
                    }
                }
            }

            // Why the last file or paste was not taken, and what to do
            // instead (CookieImport's words). Under both ways, since either
            // could have been the one tried.
            Column {
                visible: Account.importError.length > 0
                width: parent.width
                spacing: Theme.space1

                SmallCaps {
                    text: "NOT IMPORTED"
                    color: Theme.accent
                }
                GuideText {
                    width: parent.width
                    text: Account.importError
                    textFormat: Text.PlainText   // never markup, whatever was pasted
                }
            }

            Note {
                width: parent.width
                text: "A session lasts days to weeks. When it ends, Monolist says so, plays on signed out, and you "
                      + "import a new one the same way. Songs play without it, except one YouTube will not play "
                      + "signed out; search, lyrics and radio always stay signed out. Once you are signed in, the "
                      + "switches under this row say what else it is used for."
            }

            // — each browser —
            //
            // A table where there is room (browser, then the private window,
            // A and B in columns under their names); stacked, each part
            // named, where there is not.
            Column {
                id: browserTable
                width: parent.width
                topPadding: Theme.space2

                readonly property int nameWidth: 88
                readonly property real partWidth: ytmRow.wideGuide
                                                  ? (width - nameWidth - Theme.space4 * 2) / 3
                                                  : width - nameWidth

                SmallCaps {
                    text: "IN YOUR BROWSER"
                    bottomPadding: Theme.space2
                }

                Item {
                    visible: ytmRow.wideGuide
                    width: parent.width
                    height: visible ? headings.implicitHeight + Theme.space2 : 0

                    Row {
                        id: headings
                        x: browserTable.nameWidth
                        spacing: Theme.space4

                        Repeater {
                            model: ["PRIVATE WINDOW", "A · EXTENSION", "B · DEVELOPER TOOLS"]

                            SmallCaps {
                                required property string modelData
                                width: browserTable.partWidth
                                text: modelData
                            }
                        }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: Theme.ruleWidth
                        color: Theme.text
                    }
                }

                Repeater {
                    model: ytmRow.browsers

                    Item {
                        id: browserRow
                        required property var modelData

                        width: browserTable.width
                        height: Math.max(browserName.implicitHeight, parts.implicitHeight) + Theme.space3 * 2

                        GuideText {
                            id: browserName
                            y: Theme.space3
                            width: browserTable.nameWidth - Theme.space3
                            text: browserRow.modelData.name
                            font.weight: Theme.weightBlack
                        }

                        Grid {
                            id: parts
                            x: browserTable.nameWidth
                            y: Theme.space3
                            columns: ytmRow.wideGuide ? 3 : 1
                            columnSpacing: Theme.space4
                            rowSpacing: Theme.space2

                            Repeater {
                                model: [
                                    { label: "PRIVATE WINDOW", text: browserRow.modelData.window },
                                    { label: "A · EXTENSION", text: browserRow.modelData.extension },
                                    { label: "B · DEVELOPER TOOLS", text: browserRow.modelData.tools }
                                ]

                                Column {
                                    id: part
                                    required property var modelData
                                    width: browserTable.partWidth
                                    spacing: 2

                                    // Stacked, each part says which it is;
                                    // in the table the headings do.
                                    SmallCaps {
                                        visible: !ytmRow.wideGuide
                                        text: part.modelData.label
                                    }
                                    GuideText {
                                        width: part.width
                                        text: part.modelData.text
                                        textFormat: Text.PlainText
                                    }
                                }
                            }
                        }

                        // One browser, then the next: a hairline, as between
                        // the rows of any list.
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.hairline
                        }
                    }
                }
            }
        }

        // Right after a file was read: from now on it is only a copy of the
        // session in plain text, and whether it goes is the user's call.
        // Nothing is ever deleted without this answer.
        Column {
            visible: Account.importedFileName.length > 0
            width: parent.width
            spacing: Theme.space3
            leftPadding: Theme.space4

            Note {
                width: parent.width - Theme.space4
                textFormat: Text.PlainText
                color: Theme.text
                text: "Monolist has read “" + Account.importedFileName + "” and keeps its own encrypted copy. "
                      + "The file itself still holds your sign-in in plain text, so anyone who opens it can use "
                      + "your account. Delete it now? It is deleted for good, not moved to the Recycle Bin or "
                      + "Trash, where it could still be read."
            }

            Row {
                spacing: Theme.space3

                ActionButton {
                    text: "DELETE THE FILE"
                    onClicked: Account.deleteImportedFile()
                }
                ActionButton {
                    text: "KEEP IT"
                    onClicked: Account.keepImportedFile()
                }
            }
        }

        // Only while a session is held, as with scrobbling above. On by
        // default: Home is the first thing an account changes. New releases
        // are the same for everyone, so they never go as the account.
        ToggleRow {
            id: homeToggle
            visible: Account.state === "active" || Account.state === "checking" || Account.state === "unreachable"
            width: parent.width
            label: "Use my account for Home"
            hint: "Quick picks and the shelves under them come from your YouTube Music account, once YouTube Music "
                  + "has confirmed the sign-in. New releases stay the same for everyone. Off, Home is the one "
                  + "anyone would see."
            checked: Account.useForHome
            onToggled: Account.useForHome = !Account.useForHome
        }

        // The account's library, read in and never written back. On by
        // default, as Home is: the owner asked for it with the sign-in.
        ToggleRow {
            visible: Account.state === "active" || Account.state === "checking" || Account.state === "unreachable"
            width: parent.width
            label: "Import my YouTube Music library"
            hint: "Your liked songs, as the playlist “Liked on YouTube Music”, your library’s playlists (private "
                  + "ones too) and your recent history are read into Monolist, a page at a time and gently, then "
                  + "read again twice a day at most. Nothing is ever changed in your account, and your own likes "
                  + "and playlists here stay as they are. Off, nothing is read, and what was is deleted."
            checked: AccountLibrary.enabled
            onToggled: AccountLibrary.enabled = !AccountLibrary.enabled
        }

        Column {
            visible: AccountLibrary.enabled
                     && (Account.state === "active" || Account.state === "checking" || Account.state === "unreachable")
            width: parent.width
            spacing: Theme.space3
            leftPadding: 18 + Theme.space3

            Note {
                width: parent.width - parent.leftPadding
                textFormat: Text.PlainText
                text: AccountLibrary.status
            }

            ActionButton {
                text: AccountLibrary.syncing ? "SYNCING…" : "SYNC NOW"
                enabled: AccountLibrary.canSyncNow
                onClicked: AccountLibrary.syncNow()
            }
        }

        // On by default, as the owner chose: a song the account can play is
        // better played than skipped. Only ever for a song YouTube refuses
        // signed out, and never ahead of time.
        ToggleRow {
            visible: Account.state === "active" || Account.state === "checking" || Account.state === "unreachable"
            width: parent.width
            label: "Play with my account when needed"
            hint: "A song YouTube will not play signed out (one it keeps behind an age check, or holds back to ask "
                  + "whether you are a bot) is asked for once more with your account, through yt-dlp, after yt-dlp "
                  + "signed out has tried too. Every other song plays signed out, and at most 15 an hour (60 a "
                  + "day) go through the account. Off, such a song is skipped."
            checked: Account.playWhenNeeded
            onToggled: Account.playWhenNeeded = !Account.playWhenNeeded
        }

        // On by default, as the owner chose: it is what the account's
        // recommendations learn from.
        ToggleRow {
            visible: Account.state === "active" || Account.state === "checking" || Account.state === "unreachable"
            width: parent.width
            label: "Send my listens to YouTube history"
            hint: "Each song you hear for half its length, or four minutes, is added to your YouTube history, as "
                  + "YouTube Music's own player adds it, so your recommendations there learn from what you play "
                  + "here. Off, YouTube is told nothing of what you play."
            checked: Account.reportListens
            onToggled: Account.reportListens = !Account.reportListens
        }

        HRule { width: parent.width }

        // — updates —
        SectionHeader {
            width: parent.width
            number: "07"
            title: "Updates"
        }

        Item {
            width: parent.width
            height: Math.max(appUpdate.implicitHeight, checkButton.implicitHeight)

            Column {
                id: appUpdate
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - checkButton.width - Theme.space4
                spacing: 2

                Text {
                    text: "Monolist " + About.fullVersion
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Theme.weightBlack
                    color: Theme.text
                }
                Text {
                    width: parent.width
                    text: About.updateMessage.length > 0 ? About.updateMessage
                                                         : "Not checked yet."
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    // A found update is the one thing here worth the accent.
                    color: About.updateAvailable ? Theme.accent : Theme.neutral700
                }
            }

            ActionButton {
                id: checkButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: About.updateAvailable ? "GET IT" : "CHECK"
                primary: About.updateAvailable
                enabled: !About.checking
                onClicked: About.updateAvailable ? About.openUpdatePage()
                                                   : About.checkForUpdate()
            }
        }

        // — the advanced update —
        //
        // Its own control because it fixes a different problem. What goes out
        // of date in a player like this is not usually the player: it is
        // yt-dlp, and a copy six months old is the ordinary reason a track
        // stops playing. This updates those without touching the app.
        Text {
            text: "COMPONENTS"
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Bold
            font.letterSpacing: Theme.tracking(11, 0.08)
            color: Theme.neutral700
            topPadding: Theme.space2
        }

        Column {
            id: componentList
            width: parent.width
            spacing: Theme.space2

            Repeater {
                model: About.components

                Item {
                    required property var modelData

                    width: componentList.width
                    height: 34

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 110
                        text: modelData.name
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Theme.weightBlack
                        color: Theme.text
                    }
                    Text {
                        x: 110
                        anchors.verticalCenter: parent.verticalCenter
                        width: 150
                        text: modelData.version.length > 0 ? modelData.version : "not installed"
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        // Missing is the only state worth marking: it is why
                        // something else in the app is not working.
                        color: modelData.version.length > 0 ? Theme.neutral700 : Theme.accent
                    }
                    Text {
                        x: 260
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 260
                        text: modelData.role
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.neutral700
                    }

                    Rectangle {
                        width: parent.width
                        height: Theme.ruleWidth
                        anchors.bottom: parent.bottom
                        color: Theme.neutral300
                    }
                }
            }

            Text {
                visible: !About.componentsKnown
                text: "Reading versions…"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.neutral700
            }
        }

        Item {
            width: parent.width
            height: Math.max(toolsNote.implicitHeight, toolsButton.implicitHeight)

            Note {
                id: toolsNote
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - toolsButton.width - Theme.space4
                text: About.toolsMessage.length > 0 ? About.toolsMessage : About.toolsDescription
            }

            ActionButton {
                id: toolsButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: About.toolsBusy ? "UPDATING…" : "UPDATE COMPONENTS"
                enabled: About.canUpdateTools && !About.toolsBusy
                onClicked: About.updateTools()
            }
        }

        HRule { width: parent.width }

        // — about —
        SectionHeader {
            width: parent.width
            number: "08"
            title: "About"
            action: "COPY FOR A BUG REPORT →"
            onActionTriggered: About.copyReport()
        }

        Column {
            width: parent.width
            spacing: 4

            Note { text: "Monolist " + About.fullVersion }
            Note {
                text: "Commit " + About.commit + ", built " + About.buildDate
                      + (About.modified ? ", with uncommitted changes" : "")
                      + " · Qt " + About.qtVersion
            }
            Note {
                // Once an account is connected, "nothing is signed in" would
                // no longer be true; nor while a YouTube Music session is
                // held and being checked, which sends its cookies.
                text: "Songs, search, lyrics and artwork come from YouTube Music, LRCLIB, yt-dlp and FFmpeg"
                      + (Player.saavnEnabled ? ", and the sound from JioSaavn where it has the same song. " : ". ")
                      + (Scrobbler.state === "connected" || Account.state === "active"
                         || Account.state === "checking" || Account.state === "unreachable"
                         ? (Account.state === "active"
                            && (Account.useForHome || Account.playWhenNeeded || Account.reportListens
                                || AccountLibrary.enabled)
                            ? "They are fetched without an account, but for what Connections says your YouTube "
                              + "Music account is used for; what a connected account is told is set out there."
                            : "They are fetched without an account; what a connected account is told is set out "
                              + "under Connections.")
                         : "Nothing is signed in: no account, and nothing about you leaves this computer.")
            }
            DataCredit {
                width: parent.width
            }
        }
    }

}

