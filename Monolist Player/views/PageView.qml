import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend
import "../components"

// An album or playlist: its cover — the one photograph in the system allowed
// its colour, since the whole page is about it — the title set large, the
// page's actions, and its songs.
ScrollPage {
    id: root

    readonly property var page: Catalog.page
    readonly property bool wide: width >= 900

    contentHeight: column.implicitHeight

    // The page it was showing, so a new one starts at the top rather than
    // wherever the last was left, and a page finishing loading, or growing,
    // does not throw the reader back up.
    property string shownId: ""

    // A long playlist comes a hundred songs at a time. The next hundred is
    // asked for while the reader is still a screen away from the end, so
    // the list keeps ahead of the scrolling; a new hundred landing while the
    // end is still in sight asks for the one after.
    function loadMoreIfNear() {
        if (visible && Catalog.pageHasMore && !Catalog.pageLoadingMore
                && contentY + height * 2 >= contentHeight)
            Catalog.loadMorePage()
    }
    onContentYChanged: loadMoreIfNear()
    onContentHeightChanged: loadMoreIfNear()
    onVisibleChanged: loadMoreIfNear()

    // What takes the whole playlist — Play, Shuffle, Download all, Add all —
    // waits for the rest of a long one to arrive first (Catalog.loadRestOfPage),
    // and then runs, with all of it or, if the rest would not load, with what
    // did (Catalog says why). `waiting` names the button, so it shows dots.
    // It waits for the songs, not for their rows: the table makes those a
    // few at a time and can be far behind, and the actions take their songs
    // from Catalog.pageTrackList, which has every song that has arrived.
    property var afterLoad: null
    property string waiting: ""

    function withWholePage(name, action) {
        if (!Catalog.pageHasMore && !Catalog.pageFetching) {
            action()
            return
        }
        afterLoad = action
        waiting = name
        Catalog.loadRestOfPage()
    }

    Connections {
        target: Catalog
        function onPageMoreChanged() {
            if (root.afterLoad === null || Catalog.pageFetching) {
                // A part just in, with the end still in sight: the next.
                root.loadMoreIfNear()
                return
            }
            const action = root.afterLoad
            root.afterLoad = null
            root.waiting = ""
            action()
        }
        function onPageChanged() {
            // Another page: what was waiting was for the last one.
            const id = Catalog.page.browseId !== undefined ? Catalog.page.browseId : ""
            if (id === root.shownId)
                return
            root.shownId = id
            root.afterLoad = null
            root.waiting = ""
            root.contentY = 0
        }
    }

    // In one call, video flags and all: queued one at a time, every row on
    // every page was asked again for every song.
    function downloadAll() {
        Downloads.enqueueAll(Catalog.pageTrackList())
    }

    // Asked again whenever a download starts, ends or fails, and as more of
    // the page arrives.
    readonly property var downloadCounts: Downloads.revision >= 0 && Catalog.pageTracks.count >= 0
                                          ? Downloads.downloadCounts(Catalog.pageTrackList()) : ({})

    readonly property bool saved: Library.revision >= 0 && Library.isSaved(page.browseId !== undefined ? page.browseId : "")

    MonoMenu {
        id: pageMenu

        Action {
            text: "Add all to queue"
            enabled: Catalog.pageTracks.count > 0 && root.waiting.length === 0
            onTriggered: root.withWholePage("queue", function() {
                var tracks = Catalog.pageTrackList()
                for (var i = 0; i < tracks.length; ++i)
                    Player.addToQueue(tracks[i])
            })
        }
        PlaylistSubmenu {
            title: "Add all to playlist"
            enabled: Catalog.pageTracks.count > 0 && root.waiting.length === 0
            onPicked: function(playlistId) {
                root.withWholePage("playlist", function() {
                    Library.addAllToPlaylist(playlistId, Catalog.pageTrackList())
                })
            }
        }
    }

    Column {
        id: column
        x: Theme.space8
        width: root.width - Theme.space8 * 2
        topPadding: Theme.space8
        bottomPadding: 96
        spacing: Theme.space6

        // — head —
        Item {
            width: parent.width
            height: Math.max(cover.height, info.implicitHeight)

            Artwork {
                id: cover
                width: root.wide ? 248 : 168
                height: width
                placeholder: ""
                colour: true
                source: root.page.artwork !== undefined ? root.page.artwork : ""
            }

            Column {
                id: info
                anchors.left: cover.right
                anchors.leftMargin: Theme.space6
                anchors.right: parent.right
                anchors.bottom: cover.bottom
                spacing: Theme.space2

                Text {
                    text: (root.page.subtitle !== undefined ? root.page.subtitle : "").toUpperCase()
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: Theme.tracking(12, 0.18)
                    color: Theme.accent700
                }

                Text {
                    width: parent.width
                    text: root.page.title !== undefined ? root.page.title
                          : (Catalog.pageLoading ? "Loading…" : "")
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: root.wide ? 56 : 40
                    font.weight: Theme.weightBlack
                    font.letterSpacing: Theme.tracking(root.wide ? 56 : 40, -0.03)
                    lineHeight: 0.95
                    lineHeightMode: Text.ProportionalHeight
                    color: Theme.text
                }

                // An album's artists, each opening their page. A playlist's
                // line is its maker: a link only when YouTube Music linked
                // it to a channel, since "YouTube Music" is no artist.
                ArtistLine {
                    visible: text.length > 0
                    width: parent.width
                    artist: root.page.artist !== undefined ? root.page.artist : ""
                    credits: root.page.credits
                    linksEnabled: root.page.type === "album"
                                  || (root.page.credits !== undefined && root.page.credits.length > 0)
                    font.family: Theme.fontFamily
                    font.pixelSize: 18
                    font.weight: Theme.weightMedium
                    color: Theme.text
                }

                Text {
                    visible: text.length > 0
                    text: root.page.details !== undefined ? root.page.details : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.neutral700
                }

                Flow {
                    width: parent.width
                    topPadding: Theme.space3
                    spacing: Theme.space3

                    ActionButton {
                        primary: true
                        iconName: root.waiting === "play" ? "dots" : "play"
                        text: "Play"
                        enabled: Catalog.pageTracks.count > 0
                        onClicked: root.withWholePage("play", function() {
                            Player.playTracks(Catalog.pageTrackList(), 0, "playlist")
                        })
                    }
                    ActionButton {
                        iconName: root.waiting === "shuffle" ? "dots" : "shuffle"
                        text: "Shuffle"
                        enabled: Catalog.pageTracks.count > 1
                        onClicked: root.withWholePage("shuffle", function() {
                            const tracks = Catalog.pageTrackList()
                            Player.shuffle = true
                            Player.playTracks(tracks, Math.floor(Math.random() * tracks.length), "playlist")
                        })
                    }
                    ActionButton {
                        iconName: root.saved ? "check" : "plus"
                        text: root.saved ? "Saved" : "Save"
                        enabled: root.page.title !== undefined && root.page.error === undefined
                        onClicked: Library.setSaved(root.page, !root.saved)
                    }
                    DownloadAllButton {
                        visible: Downloads.available
                        counts: root.downloadCounts
                        complete: !Catalog.pageHasMore && !Catalog.pageFetching
                        waiting: root.waiting === "download"
                        enabled: Catalog.pageTracks.count > 0
                        onDownloadAllRequested: root.withWholePage("download", root.downloadAll)
                        onRetryRequested: Downloads.retryFailed(Catalog.pageTrackList())
                    }
                    ActionButton {
                        id: moreButton
                        iconName: "dots"
                        enabled: Catalog.pageTracks.count > 0
                        onClicked: pageMenu.popup(moreButton, 0, moreButton.height + Theme.space1)
                    }
                }
            }
        }

        Text {
            visible: text.length > 0
            width: Math.min(parent.width, 760)
            text: root.page.description !== undefined ? root.page.description : ""
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 13
            lineHeight: 1.3
            color: Theme.neutral700
        }

        Text {
            visible: root.page.error !== undefined
            width: parent.width
            text: root.page.error !== undefined ? "This page could not be loaded: " + root.page.error : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.accent700
        }

        // A row plays at once, with the songs loaded so far after it: it is
        // one song asked for, and waiting on the rest of a long playlist
        // would make the click look broken.
        TrackTable {
            visible: Catalog.pageTracks.count > 0
            width: parent.width
            model: Catalog.pageTracks
            showDownloads: true
            onTrackActivated: function(index) { Player.playModel(Catalog.pageTracks, index, "playlist") }
        }

        Text {
            visible: Catalog.pageLoadingMore
            width: parent.width
            text: root.waiting.length > 0
                  ? "Loading the rest of the playlist… " + Catalog.pageTracks.count + " songs so far"
                  : "Loading more songs…"
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.neutral700
        }
    }
}
