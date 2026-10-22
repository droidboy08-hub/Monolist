import QtQuick
import Monolist
import Monolist.Backend

// Where the sound goes: the system's devices by the names it gives them,
// the one in use checked. Opened from the player bar's output button, or,
// in a window too narrow for that button, as a submenu of the bar's "…"
// (TrackMenu); one menu for both, so the two never list different things.
MonoMenu {
    id: menu

    title: "Output"
    width: 320

    Instantiator {
        model: Player.audioDevices
        delegate: MonoMenuItem {
            required property var modelData
            // Never hidden, so the height need not follow `visible`, which
            // the menu changes as it takes a replaced row out.
            implicitHeight: 34
            text: modelData.missing ? modelData.description + " · not connected"
                                    : modelData.description
            enabled: !modelData.missing
            current: modelData.name === Player.audioDevice
            onTriggered: Player.setAudioDevice(modelData.name)
        }
        onObjectAdded: function(index, object) { menu.insertItem(index, object) }
        onObjectRemoved: function(index, object) { menu.removeItem(object) }
    }
}
