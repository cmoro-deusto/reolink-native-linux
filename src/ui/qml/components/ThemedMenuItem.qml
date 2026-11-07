import QtQuick
import QtQuick.Controls
import ReolinkApp

// A MenuItem matching the app theme; use inside ThemedMenu.
MenuItem {
    id: control
    implicitHeight: Theme.px(32)
    implicitWidth: Theme.px(180)
    contentItem: Text {
        text: control.text
        color: control.enabled ? Theme.text : Theme.textMuted
        font.pixelSize: Theme.px(13)
        verticalAlignment: Text.AlignVCenter
        leftPadding: Theme.px(8)
        rightPadding: Theme.px(8)
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.px(4)
        color: control.highlighted ? Theme.accentDim : "transparent"
    }
}
