import QtQuick
import ReolinkApp
import ReolinkApp.Core

// "● Talking" next to a camera's name while the microphone is live, with a
// small level meter so you can see it hears you. Shows "● Connecting…" first.
Row {
    id: badge
    property int deviceRow: -1
    property real fontSize: Theme.px(11)
    readonly property bool talking: deviceRow >= 0 && Devices.talkRow === deviceRow
    readonly property bool connecting: talking && Devices.talkConnecting

    visible: talking
    spacing: Theme.px(5)

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: badge.connecting ? qsTr("● Connecting…") : qsTr("● Talking")
        color: badge.connecting ? Theme.textMuted : Theme.danger
        font.pixelSize: badge.fontSize
        font.bold: !badge.connecting
    }
    // Microphone level.
    Rectangle {
        visible: !badge.connecting
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.px(36)
        height: Theme.px(5)
        radius: height / 2
        color: "#40ffffff"
        Rectangle {
            width: parent.width * Devices.talkLevel
            height: parent.height
            radius: parent.radius
            color: Devices.talkLevel > 0.9 ? Theme.danger : Theme.accent
            Behavior on width { NumberAnimation { duration: 60 } }
        }
    }
}
