import QtQuick
import QtQuick.Controls.Basic
import Monolist
import Monolist.Backend

// The recommendation data: whether it is here, fetching it, and removing it.
//
// One panel for Settings and for an empty Search page, so the offer reads the
// same wherever it is made. The title says the state and the buttons say the
// action: a glance takes in the title, and a button that only ever names what
// it does is never misread as a status.
Column {
    id: panel

    spacing: Theme.space3

    // Beside the text when there is room; under it when there is not, so a
    // narrow window never squeezes the explanation into a column of words.
    readonly property bool stacked: width < actions.implicitWidth + 360

    readonly property string title: RecData.busy ? "Downloading recommendation data"
                                  : RecData.removing ? "Removing recommendation data"
                                  : RecData.installed ? "Recommendation data, version " + RecData.version
                                  : RecData.failed ? "The download did not finish"
                                  : RecData.partial ? "Downloaded part-way"
                                  : "Recommendation data"

    readonly property string detail: {
        if (RecData.busy)
            return RecData.status + " · " + RecData.progressText
        if (RecData.removing || RecData.failed)
            return RecData.status
        if (RecData.installed)
            return RecData.sizeText
                   + (RecData.published.length > 0 ? ", published " + RecData.published : "")
                   + (RecData.inUse ? ". Search suggests from it."
                                    : ". Not in use: the catalogue folder below points somewhere else.")
        if (RecData.partial)
            return RecData.progressText + " is here. Downloading again keeps it and fetches the rest."
        return "400,000 songs and a chart for each country, which Search suggests from when nothing "
               + "is typed. Downloaded once, checked file by file, and kept on this computer."
    }

    Item {
        width: panel.width
        height: panel.stacked ? info.implicitHeight + Theme.space3 + actions.implicitHeight
                              : Math.max(info.implicitHeight, actions.implicitHeight)

        Column {
            id: info
            width: panel.stacked ? parent.width : parent.width - actions.implicitWidth - Theme.space4
            y: panel.stacked ? 0 : (parent.height - implicitHeight) / 2
            spacing: 2

            Text {
                width: parent.width
                text: panel.title
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Theme.weightBlack
                color: Theme.text
            }
            Text {
                width: parent.width
                text: panel.detail
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 13
                // A failure is the one state worth the accent: it is why the
                // suggestions are not there.
                color: RecData.failed && !RecData.busy ? Theme.accent700 : Theme.neutral700
            }
        }

        // Negative spacing lets neighbouring buttons share one 2px rule.
        Row {
            id: actions
            x: panel.stacked ? 0 : parent.width - implicitWidth
            y: panel.stacked ? info.implicitHeight + Theme.space3 : (parent.height - implicitHeight) / 2
            spacing: -Theme.ruleWidth

            ActionButton {
                visible: !RecData.busy && !RecData.removing && !RecData.installed
                primary: true
                text: RecData.failed ? "TRY AGAIN"
                      : RecData.partial ? "CARRY ON"
                      : "DOWNLOAD RECOMMENDATION DATA — 100 MB"
                onClicked: RecData.download()
            }
            ActionButton {
                visible: RecData.installed && !RecData.inUse && !RecData.busy && !RecData.removing
                text: "USE IT"
                onClicked: RecData.use()
            }
            ActionButton {
                visible: RecData.busy
                text: "CANCEL"
                onClicked: RecData.cancel()
            }
            ActionButton {
                visible: (RecData.installed || RecData.partial) && !RecData.busy
                text: RecData.removing ? "REMOVING…" : "REMOVE"
                enabled: !RecData.removing
                onClicked: RecData.remove()
            }
        }
    }

    // The same 2px bar as a download in the Downloads page.
    ProgressSlider {
        visible: RecData.busy
        width: panel.width
        height: 2
        interactive: false
        value: RecData.progress
    }
}
