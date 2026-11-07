import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

Rectangle {
    id: root
    height: Theme.navHeight
    color: Theme.surface

    property int currentIndex: (typeof initialPage !== "undefined") ? initialPage : 0
    signal fullscreenRequested()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacing * 2
        anchors.rightMargin: Theme.spacing * 2
        spacing: Theme.spacing

        Text {
            text: qsTr("Reolink Client")
            color: Theme.accent
            font.pixelSize: Theme.px(16)
            font.bold: true
        }

        // App version, always visible (set via setApplicationVersion in main.cpp).
        Text {
            text: "v" + Qt.application.version
            color: Theme.textMuted
            font.pixelSize: Theme.px(11)
            Layout.alignment: Qt.AlignVCenter
            Layout.topMargin: Theme.px(3)
        }

        Item { width: Theme.spacing * 2; height: 1 }

        Repeater {
            model: [qsTr("Live View"), qsTr("Playback"), qsTr("Events"), qsTr("Recordings"),
                    qsTr("Device Settings")]

            Rectangle {
                required property int index
                required property string modelData
                Layout.fillHeight: true
                implicitWidth: tabLabel.implicitWidth + Theme.spacing * 4
                color: "transparent"

                Rectangle { // active-tab underline
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.px(2)
                    color: Theme.accent
                    visible: root.currentIndex === parent.index
                }
                Text {
                    id: tabLabel
                    anchors.centerIn: parent
                    text: parent.modelData
                    color: root.currentIndex === parent.index ? Theme.text : Theme.textMuted
                    font.pixelSize: Theme.px(14)
                }
                // Unread badge on the Events tab (index 2).
                Rectangle {
                    visible: parent.index === 2 && Events.unread > 0
                    anchors.left: tabLabel.right
                    anchors.leftMargin: Theme.px(2)
                    anchors.verticalCenter: tabLabel.verticalCenter
                    anchors.verticalCenterOffset: Theme.px(-6)
                    width: Math.max(Theme.px(16), badgeText.implicitWidth + Theme.px(8))
                    height: Theme.px(16)
                    radius: Theme.px(8)
                    color: Theme.danger
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: Events.unread > 99 ? "99+" : Events.unread
                        color: "white"
                        font.pixelSize: Theme.px(10)
                        font.bold: true
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.currentIndex = parent.index
                }
            }
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            width: Theme.px(32)
            height: Theme.px(32)
            radius: Theme.radius
            color: fullscreenArea.containsMouse ? Theme.surfaceAlt : "transparent"
            Text {
                anchors.centerIn: parent
                text: "⛶"
                color: Theme.textMuted
                font.pixelSize: Theme.px(16)
            }
            MouseArea {
                id: fullscreenArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.fullscreenRequested()
            }
            ToolTip {
                visible: fullscreenArea.containsMouse
                delay: 500
                x: (parent.width - width) / 2
                y: parent.height + Theme.px(6)
                contentItem: Text { text: qsTr("Fullscreen (F11)"); color: Theme.text; font.pixelSize: Theme.px(11) }
                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: Theme.px(4) }
            }
        }
    }
}
