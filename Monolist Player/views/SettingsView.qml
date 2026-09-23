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

    Component.onCompleted: allCountries = Library.countries()

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

        // — about —
        SectionHeader {
            width: parent.width
            number: "04"
            title: "About"
        }

        Column {
            width: parent.width
            spacing: 4

            Note { text: "Monolist " + Qt.application.version }
            Note {
                text: "Songs, search, lyrics and artwork come from YouTube Music, LRCLIB, yt-dlp and FFmpeg. "
                      + "Nothing is signed in: no account, and nothing about you leaves this computer."
            }
        }
    }
}
