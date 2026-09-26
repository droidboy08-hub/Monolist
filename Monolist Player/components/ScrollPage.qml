import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// A page that scrolls. Every view is one, so the wheel (SmoothWheel), the
// bounds and the scroll bar are the same everywhere, and set here once.
Flickable {
    id: root

    contentWidth: width
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    // Room below the end for whatever stands over the page's foot (the mini
    // video): without it the last rows sit under it and cannot be reached.
    bottomMargin: Nav.pageClearance

    ScrollBar.vertical: MonoScrollBar {}

    SmoothWheel {
        id: wheel
        flickable: root
    }

    // While the wheel moves the page, the rows and cards passing under a still
    // pointer are not told so one after another. Each lighting up and going
    // out again changed what the scene holds, and with the pointer over a long
    // list that cost every other frame (--scroll-test --scroll-away shows the
    // difference). The one under the pointer lights once the page is still,
    // as in a browser, and meanwhile the pointer keeps the shape it had. Over
    // the page, not in it, so it does not scroll; it takes no presses and no
    // wheel, only hover.
    Item {
        id: hoverHold
        // Read as the page starts to move, before hover has been taken away
        // from what is under the pointer.
        property int pointer: Qt.ArrowCursor

        parent: root
        anchors.fill: parent
        z: 1
        visible: wheel.scrolling
        onVisibleChanged: if (visible) pointer = Chrome.cursorShape()

        HoverHandler {
            blocking: true
            cursorShape: hoverHold.pointer
        }
    }
}
