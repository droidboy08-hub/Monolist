import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// A numbered row of cards that scrolls sideways, with arrows in its header for
// a mouse that has no horizontal wheel. The header also carries what the shelf
// offers as a whole: SHOW ALL, where YouTube Music has a page of everything the
// shelf shows a few of, and PLAY ALL, where every card is a song or a video.
Item {
    id: root

    property string number: ""
    property string title: ""
    property string strapline: ""
    property var items: []
    // Where SHOW ALL goes (Catalog.shelves' `more`); empty for none.
    property var more: ({})
    // Which surface this is, for the player (see Player.playModel).
    property string origin: ""
    signal cardActivated(var card)

    implicitHeight: body.implicitHeight

    readonly property bool hasMore: root.more !== undefined && root.more !== null
                                    && (root.more.browseId || "").length > 0
    // Songs and videos only: an album among them would have to be fetched
    // before it could be queued, and a shelf that plays half of what it shows
    // is worse than one that offers no PLAY ALL.
    readonly property bool allSongs: {
        if (!root.items || root.items.length < 2)
            return false
        for (let i = 0; i < root.items.length; ++i) {
            if (!root.items[i].videoId)
                return false
        }
        return true
    }

    function page(direction) {
        var step = Math.max(list.cardWidth, list.width - list.cardWidth)
        var target = list.contentX + direction * step
        var max = Math.max(0, list.contentWidth - list.width)
        slide.to = Math.max(0, Math.min(max, target))
        slide.restart()
    }

    // The cards as songs, in the shape the player takes, the credit read from
    // the card rather than its whole subtitle (see HomeView.openCard).
    function playAll() {
        const tracks = []
        for (let i = 0; i < root.items.length; ++i) {
            const card = root.items[i]
            tracks.push({
                sourceId: card.videoId,
                title: card.title,
                artist: card.artist ? card.artist : card.subtitle,
                artwork: card.artwork,
                durationMs: 0,
                isVideo: card.type === "video",
                primaryArtist: card.primaryArtist ? card.primaryArtist : ""
            })
        }
        Player.playTracks(tracks, 0, root.origin)
    }

    // A header link, set as SectionHeader sets its own.
    component HeaderLink: Text {
        id: link
        signal triggered()
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: Font.Bold
        font.letterSpacing: Theme.tracking(12, 0.12)
        color: linkHover.hovered ? Theme.accent700 : Theme.neutral700

        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: link.triggered() }
    }

    Column {
        id: body
        width: parent.width
        spacing: Theme.space6

        Item {
            width: parent.width
            height: heading.implicitHeight

            Column {
                id: heading
                width: Math.max(0, parent.width - actions.width - Theme.space4)
                spacing: Theme.space1

                Text {
                    visible: root.strapline.length > 0
                    width: parent.width
                    text: root.strapline.toUpperCase()
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(11, 0.14)
                    color: Theme.neutral700
                }

                Row {
                    width: parent.width
                    spacing: Theme.space4
                    baselineOffset: shelfTitle.y + shelfTitle.baselineOffset

                    Text {
                        id: shelfNumber
                        text: root.number
                        anchors.baseline: shelfTitle.baseline
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Theme.weightBlack
                        color: Theme.accent700
                    }
                    Text {
                        id: shelfTitle
                        width: Math.min(implicitWidth, parent.width - shelfNumber.width - Theme.space4)
                        text: root.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 28
                        font.weight: Theme.weightBlack
                        font.letterSpacing: Theme.tracking(28, -0.02)
                        color: Theme.text
                    }
                }
            }

            Row {
                id: actions
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: Theme.space4

                HeaderLink {
                    visible: root.allSongs
                    anchors.verticalCenter: parent.verticalCenter
                    text: "PLAY ALL"
                    onTriggered: root.playAll()
                }
                HeaderLink {
                    visible: root.hasMore
                    anchors.verticalCenter: parent.verticalCenter
                    text: "SHOW ALL"
                    onTriggered: Nav.openMore(root.more, root.title)
                }

                Row {
                    visible: list.contentWidth > list.width
                    spacing: Theme.space1

                    IconButton {
                        iconName: "arrow-left"
                        iconSize: 15
                        enabled: list.contentX > 0
                        opacity: enabled ? 1 : 0.3
                        onClicked: root.page(-1)
                    }
                    IconButton {
                        iconName: "arrow-right"
                        iconSize: 15
                        enabled: list.contentX < list.contentWidth - list.width - 1
                        opacity: enabled ? 1 : 0.3
                        onClicked: root.page(1)
                    }
                }
            }
        }

        ListView {
            id: list

            readonly property real cardWidth: 188

            width: parent.width
            height: cardWidth + 100   // AlbumCard: the square, then its text
            orientation: ListView.Horizontal
            spacing: Theme.space6
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.items

            delegate: ShelfCard {
                required property var modelData

                width: list.cardWidth
                height: list.height
                card: modelData
                origin: root.origin
                onActivated: function(card) { root.cardActivated(card) }
            }

            // Paging moves the shelf under a still pointer, so it is eased at
            // both ends rather than thrown.
            NumberAnimation {
                id: slide
                target: list
                property: "contentX"
                duration: Theme.page
                easing.type: Theme.moveCurve
            }
        }
    }
}
