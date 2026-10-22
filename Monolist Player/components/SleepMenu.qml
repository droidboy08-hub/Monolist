import QtQuick
import Monolist
import Monolist.Backend

// The sleep timer's choices as a submenu: the player bar's "…" (TrackMenu).
// The same list as the moon's popup on the bar and the tray's submenu: 15,
// 30, 45 minutes, an hour or the end of this song, and once it runs +10
// minutes and Turn off. Its row says the time left, so a timer started from
// here can be read from here too.
MonoMenu {
    title: !SleepTimer.active ? "Sleep timer"
         : SleepTimer.fading ? "Sleep timer · fading out"
         : SleepTimer.endOfSong ? "Sleep timer · end of this song"
         : "Sleep timer · " + SleepTimer.remainingText + " left"

    MonoMenuItem {
        text: "15 minutes"
        onTriggered: SleepTimer.start(15)
    }
    MonoMenuItem {
        text: "30 minutes"
        onTriggered: SleepTimer.start(30)
    }
    MonoMenuItem {
        text: "45 minutes"
        onTriggered: SleepTimer.start(45)
    }
    MonoMenuItem {
        text: "1 hour"
        onTriggered: SleepTimer.start(60)
    }
    MonoMenuItem {
        text: "End of this song"
        current: SleepTimer.active && SleepTimer.endOfSong
        onTriggered: SleepTimer.startEndOfSong()
    }

    MonoMenuRule { visible: SleepTimer.active }

    // Hidden entries are disabled as well, so the arrow keys never land on
    // one out of sight (TrackMenu says why).
    MonoMenuItem {
        visible: SleepTimer.active
        enabled: SleepTimer.active
        text: "+10 minutes"
        onTriggered: SleepTimer.extend(10)
    }
    MonoMenuItem {
        visible: SleepTimer.active
        enabled: SleepTimer.active
        text: "Turn off"
        onTriggered: SleepTimer.cancel()
    }
}
