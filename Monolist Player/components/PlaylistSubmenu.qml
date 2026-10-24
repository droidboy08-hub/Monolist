import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// "Add to playlist": the user's playlists, then a new one. Says which was
// picked; making a new one happens here, so `picked` always has an id.
//
// Said once the menus have closed, not from within the click: adding a song
// changes its playlist's count, which makes Library.playlists anew, and the
// entries here with it, so the entry clicked was taken out of the menu while
// its own click was still being handled, before the menu could close itself
// and the menus above it. They stayed open after every song added.
MonoMenu {
    id: submenu

    signal picked(int playlistId)

    function pickLater(playlistId) {
        Qt.callLater(function() { submenu.picked(playlistId) })
    }

    title: "Add to playlist"

    // Inserted at their own index, so they stay ahead of the fixed entries.
    Instantiator {
        model: Library.playlists
        delegate: MonoMenuItem {
            required property int playlistId
            required property string name
            text: name
            onTriggered: submenu.pickLater(playlistId)
        }
        onObjectAdded: function(index, object) { submenu.insertItem(index, object) }
        onObjectRemoved: function(index, object) { submenu.removeItem(object) }
    }

    MonoMenuRule { visible: Library.playlists.count > 0 }

    Action {
        text: "New playlist"
        onTriggered: {
            var created = Library.createPlaylist("")
            if (created > 0)
                submenu.pickLater(created)
        }
    }
}
