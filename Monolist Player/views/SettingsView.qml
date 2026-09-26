import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Monolist
import Monolist.Backend
import "../components"

// Settings: the country to browse, playback, how downloads are saved, and
// what this copy of the app is.
Flickable {
    id: root

    // Every country, fetched once; the picker filters this list.
    property var allCountries: []
    property string filter: ""

    readonly property bool automatic: Library.region.length === 0

    // A part of the page to open on, for a link elsewhere that names one
    // ("recommendations"): scrolled to once, then said done, so the same
    // link works again later.
    property string section: ""
    signal sectionRevealed()

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

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
    function revealSection() {
        if (section.length === 0)
            return
        column.forceLayout()
        var target = section === "recommendations" ? recommendationsHeader : null
        if (target)
            contentY = Math.max(0, Math.min(target.y - Theme.space4, contentHeight - height))
        sectionRevealed()
    }

    ScrollBar.vertical: MonoScrollBar {}

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

        // — playback —
        SectionHeader {
            width: parent.width
            number: "02"
            title: "Playback"
        }

        ToggleRow {
            width: parent.width
            label: "Autoplay similar songs"
            hint: "When the queue runs out, carry on with YouTube Music's radio for the last song."
            checked: Player.autoplay
            onToggled: Player.autoplay = !Player.autoplay
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

        HRule { width: parent.width }

        // — downloads —
        SectionHeader {
            width: parent.width
            number: "03"
            title: "Downloads"
            action: "OPEN FOLDER →"
            onActionTriggered: Downloads.openDownloadFolder()
        }

        DownloadOptions {
            width: parent.width
        }

        Note {
            text: Downloads.downloadDirectory
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
            number: "04"
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
            width: parent.width
            number: "05"
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
        // says Connected only once YouTube Music has confirmed the session
        // (Account checks it online); while that is under way, or YouTube
        // Music cannot be reached, it offers SIGN OUT and names no one.
        ServiceRow {
            id: ytmRow

            // Open while the user imports: the steps, and where the file or
            // the pasted header goes. CANCEL closes it, and so does an import
            // that was read.
            property bool importing: false
            readonly property string account: Account.state

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
            statusLine: importing ? "" : Account.statusLine
            actionText: serviceState === "connected" ? "SIGN OUT"
                        : serviceState === "expired" ? "IMPORT AGAIN"
                        : ""
            // A session that ended can be forgotten, name and all, instead
            // of imported again.
            forgetText: "SIGN OUT"
            steps: [
                "In your own browser, open a private window and sign in at music.youtube.com, on Google's own page. Firefox is the safest choice.",
                "Export that tab's youtube.com cookies to a file with a cookies.txt extension. Or open the developer tools, pick any browse request to music.youtube.com, and copy its cookie header, or the whole request as cURL.",
                "Choose the file, or paste into the box below. It is read once, encrypted with your Windows sign-in (the Keychain on a Mac), and never written to the music database or to any log.",
                "Monolist then asks YouTube Music, over the internet, whether the sign-in works: that is the only way to know. It says Connected only once YouTube Music does.",
                "Close the private window without using it again. Monolist offers to delete the exported file as soon as it has read it, and never deletes it by itself.",
                "A session lasts days to weeks; when it ends Monolist says so and keeps playing signed out. It buys none of the speed: playback, search, lyrics and radio always stay signed out."
            ]
            // The last refusal's reason belongs to the last try: a panel
            // opened or closed starts clean.
            onConnectRequested: {
                Account.clearImportError()
                importing = true
            }
            onCancelRequested: {
                importing = false
                pasteField.clear()
                Account.clearImportError()
            }
            onDisconnectRequested: Account.signOut()

            // — the import, in the open panel —
            Note {
                visible: !Account.remembered
                width: parent.width
                text: "Keeping a sign-in safely is not available on this system yet, so Monolist holds it "
                      + "only until it closes."
            }

            ActionButton {
                text: "CHOOSE FILE…"
                onClicked: cookieFileDialog.open()
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
                    placeholderText: "…or paste the cookie header, or the request copied as cURL"
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

            Text {
                visible: Account.importError.length > 0
                width: parent.width
                text: Account.importError
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: Theme.accent
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

        HRule { width: parent.width }

        // — updates —
        SectionHeader {
            width: parent.width
            number: "06"
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
                text: About.toolsMessage.length > 0
                      ? About.toolsMessage
                      : (About.canUpdateTools
                         ? "Fetches the newest yt-dlp, FFmpeg and Deno, and leaves the app itself alone."
                         : "These came with this build and are updated by whatever installed it.")
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
            number: "07"
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
                text: "Songs, search, lyrics and artwork come from YouTube Music, LRCLIB, yt-dlp and FFmpeg. "
                      + (Scrobbler.state === "connected" || Account.state === "active"
                         || Account.state === "checking" || Account.state === "unreachable"
                         ? "They are fetched without an account; what a connected account is told is set out "
                           + "under Connections."
                         : "Nothing is signed in: no account, and nothing about you leaves this computer.")
            }
            DataCredit {
                width: parent.width
            }
        }
    }

}

