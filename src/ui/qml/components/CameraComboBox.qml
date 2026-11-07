import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// Dark-themed camera selector backed by the Devices model. Auto-selects the
// first camera and re-selects after the list changes (an NVR replaces its
// placeholder row with N channel rows during validation, which would otherwise
// leave the box empty).
ComboBox {
    id: combo
    model: Devices
    textRole: "name"
    implicitHeight: Theme.px(34)

    function ensureSelection() {
        if (Devices.count > 0 && (currentIndex < 0 || currentIndex >= Devices.count))
            currentIndex = 0;
    }
    Component.onCompleted: ensureSelection()
    Connections {
        target: Devices
        function onCountChanged() { combo.ensureSelection(); }
    }

    contentItem: Text {
        leftPadding: Theme.px(10)
        rightPadding: Theme.px(28)
        text: combo.currentIndex >= 0 && combo.displayText.length > 0
              ? combo.displayText : qsTr("Select a camera")
        color: combo.currentIndex >= 0 ? Theme.text : Theme.textMuted
        font.pixelSize: Theme.px(13)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: Theme.surfaceAlt
        border.color: combo.activeFocus || combo.hovered ? Theme.accent : Theme.border
        radius: Theme.px(4)
    }

    indicator: Text {
        x: combo.width - width - Theme.px(10)
        y: (combo.height - height) / 2
        text: "▾"
        color: Theme.textMuted
        font.pixelSize: Theme.px(12)
    }

    delegate: ItemDelegate {
        width: ListView.view ? ListView.view.width : combo.width
        height: Theme.px(34)
        required property int index
        required property string name
        required property bool online
        highlighted: combo.highlightedIndex === index
        contentItem: Row {
            spacing: Theme.px(8)
            Rectangle {
                width: Theme.px(8); height: Theme.px(8); radius: Theme.px(4)
                anchors.verticalCenter: parent.verticalCenter
                color: online ? Theme.online : Theme.textMuted
            }
            Text {
                text: name
                color: Theme.text
                font.pixelSize: Theme.px(13)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle {
            color: highlighted ? Theme.accentDim : Theme.surface
        }
    }

    popup: Popup {
        y: combo.height + Theme.px(2)
        width: combo.width
        implicitHeight: Math.min(listView.contentHeight + Theme.px(2), Theme.px(320))
        padding: 1
        contentItem: ListView {
            id: listView
            clip: true
            implicitHeight: contentHeight
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.px(4)
        }
    }
}
