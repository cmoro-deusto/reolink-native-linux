import QtQuick
import QtQuick.Layouts
import ReolinkApp

// Compact month calendar. Emits dateSelected(year, month, day) — month is
// 1-based. Days that have recordings can be marked via markedDays.
Column {
    id: root
    spacing: Theme.px(6)

    property int year: 2026
    property int month: 7           // 1-based
    property int selDay: 9
    property var markedDays: []      // days (1..31) with recordings → blue dot

    signal dateSelected(int year, int month, int day)

    function daysInMonth(y, m) { return new Date(y, m, 0).getDate(); }
    function firstWeekday(y, m) { return new Date(y, m - 1, 1).getDay(); } // 0=Sun

    RowLayout {
        width: parent.width
        Rectangle {
            width: Theme.px(24); height: Theme.px(24); radius: Theme.px(4)
            color: prevHover.hovered ? Theme.surfaceAlt : "transparent"
            Text { anchors.centerIn: parent; text: "‹"; color: Theme.text; font.pixelSize: Theme.px(16) }
            HoverHandler { id: prevHover }
            TapHandler { onTapped: {
                if (root.month === 1) { root.month = 12; root.year--; }
                else root.month--;
            } }
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: Qt.locale().standaloneMonthName(root.month - 1) + " " + root.year
            color: Theme.text
            font.pixelSize: Theme.px(13)
            font.bold: true
        }
        Rectangle {
            width: Theme.px(24); height: Theme.px(24); radius: Theme.px(4)
            color: nextHover.hovered ? Theme.surfaceAlt : "transparent"
            Text { anchors.centerIn: parent; text: "›"; color: Theme.text; font.pixelSize: Theme.px(16) }
            HoverHandler { id: nextHover }
            TapHandler { onTapped: {
                if (root.month === 12) { root.month = 1; root.year++; }
                else root.month++;
            } }
        }
    }

    Grid {
        columns: 7
        spacing: Theme.px(2)
        Repeater {
            model: ["S", "M", "T", "W", "T", "F", "S"]
            Text {
                required property string modelData
                width: Theme.px(30); height: Theme.px(18)
                horizontalAlignment: Text.AlignHCenter
                text: modelData
                color: Theme.textMuted
                font.pixelSize: Theme.px(10)
            }
        }
    }

    Grid {
        columns: 7
        spacing: Theme.px(2)
        Repeater {
            model: 42
            Item {
                required property int index
                property int dayNum: index - root.firstWeekday(root.year, root.month) + 1
                property bool valid: dayNum >= 1 && dayNum <= root.daysInMonth(root.year, root.month)
                property bool selected: valid && dayNum === root.selDay
                width: Theme.px(30); height: Theme.px(28)
                Rectangle {
                    anchors.fill: parent
                    radius: Theme.px(4)
                    visible: parent.valid
                    color: parent.selected ? Theme.accentDim
                         : dayHover.hovered ? Theme.surfaceAlt : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: parent.parent.valid ? parent.parent.dayNum : ""
                        color: parent.parent.selected ? Theme.text : Theme.textMuted
                        font.pixelSize: Theme.px(11)
                    }
                    Rectangle { // recording marker
                        visible: root.markedDays.indexOf(parent.parent.dayNum) >= 0
                        width: Theme.px(4); height: Theme.px(4); radius: Theme.px(2)
                        color: Theme.accent
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: Theme.px(2)
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    HoverHandler { id: dayHover; enabled: parent.parent.valid }
                    TapHandler {
                        enabled: parent.parent.valid
                        onTapped: {
                            root.selDay = parent.parent.dayNum;
                            root.dateSelected(root.year, root.month, parent.parent.dayNum);
                        }
                    }
                }
            }
        }
    }
}
