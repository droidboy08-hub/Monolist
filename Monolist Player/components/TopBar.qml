import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

Rectangle {
    id: root

    property string breadcrumb: ""
    property alias searchText: searchField.text
    readonly property bool searchFocused: searchField.activeFocus
    property bool showMenuButton: false
    readonly property bool compact: width < 720

    // Search suggestions: shown while typing, highlighted with the arrow keys.
    property bool suggesting: false
    property int highlighted: -1
    readonly property bool showSuggestions: suggesting && searchField.text.trim().length > 0
                                            && Extractor.suggestions.length > 0

    signal menuRequested()
    signal backRequested()
    signal forwardRequested()
    signal searchActivated(string term)

    color: Theme.bg
    implicitHeight: Theme.titleBarHeight

    function pickSuggestion(text) {
        searchField.text = text
        suggesting = false
        searchActivated(text)
    }

    // The typed part stays regular and the completion goes bold, the way
    // YouTube Music sets its own suggestions.
    function suggestionMarkup(suggestion) {
        function escape(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;") }
        var typed = searchField.text.trim()
        if (typed.length > 0 && suggestion.toLowerCase().indexOf(typed.toLowerCase()) === 0)
            return escape(suggestion.substring(0, typed.length)) + "<b>" + escape(suggestion.substring(typed.length)) + "</b>"
        return escape(suggestion)
    }

    onShowSuggestionsChanged: showSuggestions ? suggestionPopup.open() : suggestionPopup.close()

    // The bar is the window's title bar: its empty parts move the window.
    // First, so the buttons and the search field sit above it.
    WindowDragArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: Theme.ruleWidth
        color: Theme.divider
    }

    WindowButtons {
        id: windowButtons
        visible: !Chrome.nativeButtons
        anchors.top: parent.top
        anchors.right: parent.right
        height: parent.height - Theme.ruleWidth
    }

    Row {
        id: leftGroup
        anchors.left: parent.left
        // With the sidebar folded away this bar starts at the window's edge,
        // where macOS keeps its traffic lights.
        anchors.leftMargin: Theme.space8 + (root.showMenuButton ? Chrome.nativeButtonsInset : 0)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space2

        IconButton {
            visible: root.showMenuButton
            iconName: "menu"
            onClicked: root.menuRequested()
        }
        IconButton { iconName: "arrow-left"; onClicked: root.backRequested() }
        IconButton { iconName: "arrow-right"; onClicked: root.forwardRequested() }
    }

    Text {
        anchors.left: leftGroup.right
        anchors.leftMargin: Theme.space4
        anchors.right: searchBox.left
        anchors.rightMargin: Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.compact
        text: root.breadcrumb
        elide: Text.ElideRight
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.14)
        color: Theme.neutral600
    }

    // Suggestions are cheap (one small request), but not one per keystroke.
    Timer {
        id: suggestTimer
        interval: 120
        onTriggered: Extractor.suggest(searchField.text)
    }

    Rectangle {
        id: searchBox
        anchors.right: windowButtons.visible ? windowButtons.left : parent.right
        anchors.rightMargin: windowButtons.visible ? Theme.space6 : Theme.space8
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(360, Math.max(200, root.width * 0.28))
        height: 36
        color: "transparent"
        border.width: Theme.ruleWidth
        border.color: searchField.activeFocus ? Theme.accent : Theme.text

        Icon {
            id: searchIcon
            name: "search"
            width: 15
            height: 15
            color: Theme.neutral600
            anchors.left: parent.left
            anchors.leftMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
        }

        TextInput {
            id: searchField
            anchors.left: searchIcon.right
            anchors.leftMargin: Theme.space2
            anchors.right: parent.right
            anchors.rightMargin: Theme.space3
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.accentForeground
            clip: true

            onTextEdited: {
                const typed = text.trim().length > 0
                root.suggesting = typed
                root.highlighted = -1
                if (typed) {
                    suggestTimer.restart()
                    // Results follow the typing, so go to them straight away.
                    root.searchActivated(text)
                } else {
                    suggestTimer.stop()
                    Extractor.clearSuggestions()
                }
            }

            onAccepted: {
                if (root.showSuggestions && root.highlighted >= 0
                        && root.highlighted < Extractor.suggestions.length) {
                    root.pickSuggestion(Extractor.suggestions[root.highlighted])
                } else {
                    root.suggesting = false
                    root.searchActivated(text)
                }
            }

            Keys.onDownPressed: {
                if (root.showSuggestions)
                    root.highlighted = Math.min(root.highlighted + 1, Extractor.suggestions.length - 1)
            }
            Keys.onUpPressed: {
                if (root.showSuggestions)
                    root.highlighted = Math.max(root.highlighted - 1, -1)
            }
            Keys.onEscapePressed: root.suggesting = false

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: searchField.text.length === 0
                text: "Artists, albums, tracks…"
                font: searchField.font
                color: Theme.neutral500
            }
        }

        // Hangs from the field and shares its bottom rule. A Popup, so it
        // draws above the view underneath without anything changing z-order.
        Popup {
            id: suggestionPopup
            y: searchBox.height - Theme.ruleWidth
            width: searchBox.width
            topPadding: 0
            bottomPadding: Theme.ruleWidth
            leftPadding: Theme.ruleWidth
            rightPadding: Theme.ruleWidth
            focus: false
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            onClosed: root.suggesting = false

            background: Rectangle {
                color: Theme.bg
                border.width: Theme.ruleWidth
                border.color: Theme.text
            }

            contentItem: Column {
                Repeater {
                    model: Extractor.suggestions

                    delegate: Rectangle {
                        id: suggestionRow

                        required property int index
                        required property string modelData

                        width: suggestionPopup.availableWidth
                        height: 34
                        color: index === root.highlighted || rowHover.hovered ? Theme.rowHover : "transparent"

                        Icon {
                            id: rowIcon
                            name: "search"
                            width: 13
                            height: 13
                            x: Theme.space3
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.neutral600
                        }

                        Text {
                            anchors.left: rowIcon.right
                            anchors.leftMargin: Theme.space2
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.space3
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.suggestionMarkup(suggestionRow.modelData)
                            textFormat: Text.StyledText
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            color: Theme.text
                        }

                        HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.pickSuggestion(suggestionRow.modelData) }
                    }
                }
            }
        }
    }
}
