import QtQuick
import QtQuick.Controls.Basic
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

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    Component.onCompleted: {
        allCountries = Library.countries()
        // Versions cost a process each to read, so they are asked for when
        // this page opens rather than while the app is starting.
        if (!About.componentsKnown)
            About.refreshComponents()
    }

    ScrollBar.vertical: MonoScrollBar {}

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
        // licensed for non-commercial use only. So it is pointed at rather
        // than bundled, and the app works perfectly well without one.
        SectionHeader {
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
                onClicked: Recs.refresh()
            }
        }

        HRule { width: parent.width }

        // — connections —
        //
        // Designed, not built. The app works entirely without any of these and
        // is meant to keep working that way; what an account buys is your own
        // library and your own history, not a better player.
        SectionHeader {
            width: parent.width
            number: "05"
            title: "Connections"
        }

        Note {
            text: "Nothing is signed in. Monolist plays without an account, and keeps your library "
                  + "on this computer. Connecting one would add what only an account can know."
        }

        ServiceRow {
            width: parent.width
            name: "Last.fm"
            detail: "Scrobble what you play, and see what you have been listening to."
            steps: [
                "Monolist opens Last.fm in your browser, where you approve it. Your password is never typed into this app.",
                "Last.fm hands back a session key, which is stored on this computer and can be revoked from your Last.fm account at any time.",
                "From then on, a track counts as played once you have heard half of it, and scrobbles go out in the background. Nothing else is sent."
            ]
        }

        ServiceRow {
            width: parent.width
            name: "YouTube Music"
            detail: "Your own playlists, likes and listening history, instead of this computer's."
            caution: "Use an account you can afford to lose. Google restricts accounts used by outside players, and that would take the account with it."
            steps: [
                "Sign in to YouTube Music in your own browser, in a private window, and export the cookies for that tab to a file.",
                "Point Monolist at that file. It is read once, kept in this computer's keychain, and never written to the music database or to any log.",
                "Your library, likes and history then come from your account. Sign out here and the file and the key are both deleted.",
                "It buys none of the speed: playback is exactly as fast signed out, and signing in never becomes required for anything."
            ]
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
                text: "Songs, search, lyrics and artwork come from YouTube Music, LRCLIB, yt-dlp and FFmpeg. "
                      + "Nothing is signed in: no account, and nothing about you leaves this computer."
            }
        }
    }

}

