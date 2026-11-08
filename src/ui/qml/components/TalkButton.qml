import QtQuick
import QtQuick.Controls
import QtQml
import ReolinkApp
import ReolinkApp.Core

// The 🎙 two-way talk control, shared by live panes and pop-out windows.
// Click: start talking; click again to stop. Press and hold: talk while held,
// and letting go ends it (push-to-talk). Right-click: choose the microphone.
//
// `owner` is true while talk on this camera was started from this button, so
// only the surface that started it ends it when that surface goes away (a
// hidden grid pane must not cut off talk started from a pop-out).
Rectangle {
    id: btn
    property int deviceRow: -1
    // Pop-out style: a dark box with a border, like the speaker button there.
    property bool framed: false
    property real size: Theme.px(28)

    readonly property bool talking: deviceRow >= 0 && Devices.talkRow === deviceRow
    readonly property bool connecting: talking && Devices.talkConnecting
    property bool owner: false
    onTalkingChanged: if (!talking) owner = false

    // Held at least this long, letting go ends talk.
    readonly property int holdMs: 500
    property double pressedAt: 0
    property bool talkingAtPress: false

    width: size
    height: size
    radius: framed ? Theme.radius : Theme.px(4)
    color: talking && !connecting ? Theme.danger
         : talking ? Theme.accentDim
         : hover.hovered ? Theme.surfaceAlt
         : framed ? "#cc0d141b" : "transparent"
    border.color: framed ? Theme.border : "transparent"

    Text {
        anchors.centerIn: parent
        text: "🎙"
        color: btn.talking ? "white" : Theme.textMuted
        font.pixelSize: btn.framed ? Theme.px(15) : Theme.px(14)
    }

    HoverHandler { id: hover }

    // ReleaseWithinBounds takes an exclusive grab on press, so the pane's
    // pan/select MouseArea underneath can't take the gesture.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onPressedChanged: {
            if (pressed) {
                btn.pressedAt = Date.now();
                btn.talkingAtPress = btn.talking;
                if (!btn.talking) {
                    btn.owner = true;
                    Devices.startTalk(btn.deviceRow);
                }
            } else if (btn.talking
                       && (btn.talkingAtPress || Date.now() - btn.pressedAt >= btn.holdMs)) {
                Devices.stopTalk();
            }
        }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: {
            micList.model = Devices.microphones();
            micMenu.popup();
        }
    }

    ThemedMenu {
        id: micMenu
        ThemedMenuItem {
            text: (Devices.talkMicrophone === "" ? "✓ " : "") + qsTr("System default microphone")
            onTriggered: Devices.talkMicrophone = ""
        }
        ThemedMenuSeparator {}
        Instantiator {
            id: micList
            model: []
            delegate: ThemedMenuItem {
                required property var modelData
                text: (Devices.talkMicrophone === modelData.id ? "✓ " : "") + modelData.name
                onTriggered: Devices.talkMicrophone = modelData.id
            }
            onObjectAdded: (index, object) => micMenu.insertItem(index + 2, object)
            onObjectRemoved: (index, object) => micMenu.removeItem(object)
        }
    }

    ToolTip {
        visible: hover.hovered && !micMenu.visible
        delay: 500
        x: (parent.width - width) / 2
        y: -height - Theme.px(8)
        contentItem: Text {
            text: btn.connecting ? qsTr("Connecting talk…")
                : btn.talking ? qsTr("Stop talking")
                : qsTr("Talk: click to start, or hold to talk while held. Right-click to choose the microphone")
            color: Theme.text
            font.pixelSize: Theme.px(11)
        }
        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: Theme.px(4) }
    }
}
