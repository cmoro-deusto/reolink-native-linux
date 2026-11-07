import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Recordings: every main-stream recording the devices hold for one day, newest
// first, filterable by camera, each downloadable as the device stored it. Laid
// out like the Events page. A recording that overlaps a detection in the event
// inbox borrows its type and snapshot.
Item {
    id: page

    readonly property int columnWidth: Math.min(width - Theme.spacing * 4, Theme.px(820))

    // The day shown, as local midnight.
    property date day: startOfDay(new Date())
    // Camera filter: a "hostId:channel" key, or "" for all cameras.
    property string cameraKey: ""

    // All loaded recordings for `day`: {row, key, camera, hostId, channel, name,
    // start, end, size, type, thumbnail}, newest first.
    property var recordings: []
    property int pendingSearches: 0
    property var failures: []          // camera names whose search failed
    property bool loadedOnce: false
    // Download state per recording ("row:start"): "busy" | "saved" | "error".
    property var downloads: ({})
    property var downloadErrors: ({})
    property var downloadProgress: ({}) // "row:start" -> fraction 0..1, once known

    readonly property var shown: cameraKey === ""
        ? recordings : recordings.filter(r => r.key === cameraKey)

    function startOfDay(d) { return new Date(d.getFullYear(), d.getMonth(), d.getDate()); }
    function isToday(d) { return startOfDay(new Date()).getTime() === d.getTime(); }
    function shiftDay(n) {
        var d = new Date(page.day);
        d.setDate(d.getDate() + n);
        page.day = startOfDay(d);
        refresh();
    }
    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function clock(epoch) {
        var d = new Date(epoch * 1000);
        return pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
    }
    function dayLabel(d) {
        var today = startOfDay(new Date());
        var diff = Math.round((today.getTime() - d.getTime()) / 86400000);
        if (diff === 0) return qsTr("Today");
        if (diff === 1) return qsTr("Yesterday");
        return d.toLocaleDateString(Qt.locale(), d.getFullYear() === today.getFullYear()
                                    ? "ddd MMM d" : "ddd MMM d yyyy");
    }
    function duration(secs) {
        secs = Math.max(0, Math.round(secs));
        if (secs < 60) return qsTr("%1 s").arg(secs);
        return qsTr("%1 min %2 s").arg(Math.floor(secs / 60)).arg(secs % 60);
    }
    function sizeText(bytes) {
        if (bytes <= 0) return "";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + " KB";
        return (bytes / (1024 * 1024)).toFixed(1) + " MB";
    }
    function labelFor(type) {
        switch (type) {
        case "person": return qsTr("Person");
        case "vehicle": return qsTr("Vehicle");
        case "pet": return qsTr("Pet");
        case "visitor": return qsTr("Visitor");
        case "motion": return qsTr("Motion");
        default: return "";
        }
    }
    function colorFor(type) {
        switch (type) {
        case "person": return Theme.accent;
        case "vehicle": return "#e08a3c";
        case "pet": return Theme.online;
        case "visitor": return Theme.danger;
        case "motion": return Theme.textMuted;
        default: return Theme.border;
        }
    }

    // Cameras with recordings to search (stream-URL devices have none).
    function cameraRows() {
        var rows = [];
        for (var r = 0; r < Devices.count; ++r) {
            var c = Devices.cameraInfo(r);
            if (c && c.hostId !== undefined && c.kind !== "stream")
                rows.push(r);
        }
        return rows;
    }

    function refresh() {
        page.recordings = [];
        page.failures = [];
        var rows = cameraRows();
        page.pendingSearches = rows.length;
        page.loadedOnce = true;
        if (rows.length > 0)
            Devices.listRecordings(rows, page.day.getFullYear(), page.day.getMonth() + 1,
                                   page.day.getDate());
    }

    // Load on first show, and catch up on today's new recordings when returning.
    onVisibleChanged: if (visible && (!loadedOnce || isToday(page.day))) refresh()
    Component.onCompleted: if (visible) refresh()

    Connections {
        target: Devices
        // Devices connect (and their camera lists change) after startup: search
        // again so a page opened early doesn't stay empty.
        function onCountChanged() { if (page.visible) page.refresh(); }
        function onRecordingsListed(row, year, month, dayOfMonth, files) {
            if (year !== page.day.getFullYear() || month !== page.day.getMonth() + 1
                || dayOfMonth !== page.day.getDate())
                return; // a search for a day no longer shown
            var c = Devices.cameraInfo(row);
            var key = c.hostId + ":" + c.channel;
            var add = [];
            for (var i = 0; i < files.length; ++i) {
                var f = files[i];
                var ev = Events.eventDuring(c.hostId, c.channel, f.start, f.end);
                add.push({ row: row, key: key, camera: Devices.nameAt(row),
                           hostId: c.hostId, channel: c.channel, name: f.name,
                           start: f.start, end: f.end, size: f.size,
                           type: ev.type || "", thumbnail: ev.thumbnail || "" });
            }
            page.recordings = page.recordings.concat(add).sort((a, b) => b.start - a.start);
            page.pendingSearches = Math.max(0, page.pendingSearches - 1);
        }
        function onRecordingsListFailed(row, year, month, dayOfMonth, error) {
            if (year !== page.day.getFullYear() || month !== page.day.getMonth() + 1
                || dayOfMonth !== page.day.getDate())
                return;
            page.failures = page.failures.concat([Devices.nameAt(row) + ": " + error]);
            page.pendingSearches = Math.max(0, page.pendingSearches - 1);
        }
        function onRecordingDownloadProgress(row, startEpoch, fraction) {
            var p = Object.assign({}, page.downloadProgress);
            p[row + ":" + startEpoch] = fraction;
            page.downloadProgress = p;
        }
        function onRecordingDownloaded(row, startEpoch, path) {
            page.setDownload(row + ":" + startEpoch, "saved", "");
        }
        function onRecordingDownloadFailed(row, startEpoch, error) {
            page.setDownload(row + ":" + startEpoch, "error", error);
        }
    }

    function setDownload(key, state, error) {
        var d = Object.assign({}, page.downloads);
        d[key] = state;
        page.downloads = d;
        var e = Object.assign({}, page.downloadErrors);
        e[key] = error;
        page.downloadErrors = e;
    }
    // Where downloads go: a folder the user picked (remembered), else the
    // app's default (~/Videos/Reolink).
    Settings {
        id: dlStore
        category: "recordings"
        property string folder: ""
    }
    readonly property string downloadFolder: dlStore.folder !== "" ? dlStore.folder
                                                                   : Devices.defaultRecordingsDir()
    readonly property string homePath:
        urlToPath(StandardPaths.writableLocation(StandardPaths.HomeLocation))
    function urlToPath(u) { return decodeURIComponent(String(u).replace(/^file:\/\//, "")); }
    function pathToUrl(p) { return "file://" + encodeURI(p); }
    function prettyPath(p) {
        return homePath !== "" && p.indexOf(homePath) === 0 ? "~" + p.slice(homePath.length) : p;
    }

    // Download a recording; destPath (a full file path) overrides the folder.
    function download(rec, destPath) {
        var key = rec.row + ":" + rec.start;
        var p = Object.assign({}, page.downloadProgress);
        delete p[key];
        page.downloadProgress = p;
        setDownload(key, "busy", "");
        Devices.downloadRecording(rec.row, rec.start, rec.end, rec.name, rec.size,
            destPath || (page.downloadFolder + "/" + Devices.recordingFileName(rec.row, rec.start)));
    }

    property var saveAsRec: null
    FileDialog {
        id: saveAsDialog
        title: qsTr("Save recording as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("MP4 video (*.mp4)")]
        onAccepted: if (page.saveAsRec) page.download(page.saveAsRec, page.urlToPath(selectedFile))
    }
    function saveAs(rec) {
        page.saveAsRec = rec;
        saveAsDialog.currentFolder = pathToUrl(page.downloadFolder);
        saveAsDialog.selectedFile =
            pathToUrl(page.downloadFolder + "/" + Devices.recordingFileName(rec.row, rec.start));
        saveAsDialog.open();
    }
    FolderDialog {
        id: folderDialog
        title: qsTr("Download recordings to")
        onAccepted: dlStore.folder = page.urlToPath(selectedFolder)
    }
    function chooseFolder() {
        folderDialog.currentFolder = pathToUrl(page.downloadFolder);
        folderDialog.open();
    }

    // Download status: a spinning quarter-arc until progress is known, then a
    // ring filling clockwise, which morphs into a green tick when done (the
    // ring fades as the tick draws itself).
    component StatusRing: Canvas {
        id: ring
        property real value: 0 // 0..1
        property bool indeterminate: false
        property bool done: false
        // The waiting arc spins by redrawing at a moving start angle; the item
        // itself is never rotated, so the ring and tick always draw upright.
        property real spin: 0
        NumberAnimation on spin {
            running: ring.indeterminate && ring.visible
            from: 0; to: 2 * Math.PI; duration: 900; loops: Animation.Infinite
        }
        onSpinChanged: if (indeterminate) requestPaint()
        property real morph: done ? 1 : 0
        Behavior on morph { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
        implicitWidth: Theme.px(16)
        implicitHeight: Theme.px(16)
        onValueChanged: requestPaint()
        onIndeterminateChanged: requestPaint()
        onMorphChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var w = width, h = height;
            var lw = Math.max(1.5, w / 8);
            var r = (Math.min(w, h) - lw) / 2;
            ctx.lineWidth = lw;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            if (morph < 1) {
                ctx.globalAlpha = 1 - morph;
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.25);
                ctx.beginPath(); ctx.arc(w / 2, h / 2, r, 0, 2 * Math.PI); ctx.stroke();
                ctx.strokeStyle = Theme.text;
                ctx.beginPath();
                var from = -Math.PI / 2 + (indeterminate ? spin : 0);
                ctx.arc(w / 2, h / 2, r, from,
                        from + 2 * Math.PI * (indeterminate ? 0.25
                                              : Math.min(1, Math.max(value, morph))));
                ctx.stroke();
                ctx.globalAlpha = 1;
            }
            if (morph > 0) {
                // The tick, drawn along its length as morph goes 0 -> 1.
                var pts = [[0.18, 0.54], [0.42, 0.78], [0.84, 0.28]];
                var seg = [];
                var total = 0;
                for (var i = 1; i < pts.length; ++i) {
                    var dx = (pts[i][0] - pts[i - 1][0]) * w, dy = (pts[i][1] - pts[i - 1][1]) * h;
                    seg.push(Math.sqrt(dx * dx + dy * dy));
                    total += seg[seg.length - 1];
                }
                var left = total * morph;
                ctx.strokeStyle = Theme.online;
                ctx.lineWidth = lw * 1.2;
                ctx.beginPath();
                ctx.moveTo(pts[0][0] * w, pts[0][1] * h);
                for (var j = 1; j < pts.length && left > 0; ++j) {
                    var t = Math.min(1, left / seg[j - 1]);
                    ctx.lineTo((pts[j - 1][0] + (pts[j][0] - pts[j - 1][0]) * t) * w,
                               (pts[j - 1][1] + (pts[j][1] - pts[j - 1][1]) * t) * h);
                    left -= seg[j - 1];
                }
                ctx.stroke();
            }
        }
    }

    // A rounded chip, as on the Events page.
    component Chip: Rectangle {
        id: chip
        property string label: ""
        property bool selected: false
        property bool enabledChip: true
        signal activated()
        implicitWidth: chipLabel.implicitWidth + Theme.px(24)
        height: Theme.px(30)
        radius: Theme.px(15)
        opacity: enabledChip ? 1 : 0.4
        color: selected ? Theme.accentDim : (chipHover.hovered && enabledChip ? Theme.surfaceAlt : Theme.surface)
        border.color: selected ? Theme.accent : Theme.border
        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            color: chip.selected ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.px(12)
        }
        HoverHandler { id: chipHover }
        TapHandler { onTapped: if (chip.enabledChip) chip.activated() }
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: Theme.spacing * 2
        anchors.bottomMargin: Theme.spacing
        width: page.columnWidth
        spacing: Theme.spacing

        // ---- Header ----
        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Recordings")
                color: Theme.text
                font.pixelSize: Theme.px(20)
                font.bold: true
            }
            Text {
                text: page.shown.length > 0 ? page.shown.length : ""
                color: Theme.textMuted
                font.pixelSize: Theme.px(13)
                Layout.alignment: Qt.AlignBottom
                bottomPadding: Theme.px(2)
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                visible: page.pendingSearches > 0
                running: visible
                Layout.preferredWidth: Theme.px(20)
                Layout.preferredHeight: Theme.px(20)
            }
            Chip {
                label: qsTr("Refresh")
                enabledChip: page.pendingSearches === 0
                onActivated: page.refresh()
            }
        }

        // ---- Filters: day + camera ----
        Flow {
            Layout.fillWidth: true
            spacing: Theme.px(6)

            Chip { label: "‹"; onActivated: page.shiftDay(-1) }
            Chip { label: page.dayLabel(page.day); selected: true }
            Chip { label: "›"; enabledChip: !page.isToday(page.day); onActivated: page.shiftDay(1) }
            Chip {
                label: qsTr("Today")
                visible: !page.isToday(page.day)
                onActivated: { page.day = page.startOfDay(new Date()); page.refresh(); }
            }

            Item { width: Theme.px(10); height: 1 }

            // Camera filter: a chip-styled dropdown listing the cameras.
            Rectangle {
                id: camChip
                property bool active: page.cameraKey !== ""
                property string activeName: ""
                implicitWidth: camChipText.implicitWidth + Theme.px(34)
                height: Theme.px(30)
                radius: Theme.px(15)
                color: active ? Theme.accentDim : (camChipHover.hovered ? Theme.surfaceAlt : Theme.surface)
                border.color: active ? Theme.accent : Theme.border
                Row {
                    anchors.centerIn: parent
                    spacing: Theme.px(5)
                    Text {
                        id: camChipText
                        text: camChip.active ? camChip.activeName : qsTr("All cameras")
                        color: camChip.active ? Theme.text : Theme.textMuted
                        font.pixelSize: Theme.px(12)
                    }
                    Text { text: "▾"; color: Theme.textMuted; font.pixelSize: Theme.px(10)
                           anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: camChipHover }
                TapHandler { onTapped: camMenu.popup() }
                ThemedMenu {
                    id: camMenu
                    ThemedMenuItem {
                        text: qsTr("All cameras")
                        onTriggered: { page.cameraKey = ""; camChip.activeName = ""; }
                    }
                    ThemedMenuSeparator {}
                    Instantiator {
                        model: Devices
                        delegate: ThemedMenuItem {
                            required property string name
                            required property int index
                            text: name
                            onTriggered: {
                                var c = Devices.cameraInfo(index);
                                page.cameraKey = c.hostId + ":" + c.channel;
                                camChip.activeName = name;
                            }
                        }
                        onObjectAdded: (i, o) => camMenu.insertItem(i + 2, o)
                        onObjectRemoved: (i, o) => camMenu.removeItem(o)
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: page.failures.length > 0
            text: qsTr("Couldn't search: %1").arg(page.failures.join(", "))
            color: Theme.danger
            font.pixelSize: Theme.px(12)
            wrapMode: Text.Wrap
        }

        // ---- Recording list ----
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: page.shown
            clip: true
            spacing: Theme.px(6)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: rowItem
                required property var modelData
                readonly property string dlKey: modelData.row + ":" + modelData.start
                readonly property string dlState: page.downloads[dlKey] || ""
                readonly property var dlFraction: page.downloadProgress[dlKey]
                readonly property bool dlKnown: dlState === "busy" && dlFraction !== undefined

                width: ListView.view.width
                height: Theme.px(72)
                radius: Theme.radius
                color: rowHover.hovered ? Theme.surfaceAlt : Theme.surface
                border.color: rowHover.hovered ? Theme.border : Qt.rgba(1, 1, 1, 0.04)

                // Left accent bar: the detection's type color, if any.
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Theme.px(3)
                    radius: Theme.radius
                    color: page.colorFor(rowItem.modelData.type)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.px(12)
                    anchors.rightMargin: Theme.px(14)
                    anchors.topMargin: Theme.px(8)
                    anchors.bottomMargin: Theme.px(8)
                    spacing: Theme.px(12)

                    // Snapshot of the detection during this recording, or a film icon.
                    Rectangle {
                        Layout.preferredWidth: Theme.px(96)
                        Layout.fillHeight: true
                        radius: Theme.px(4)
                        color: Theme.paneBackground
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: rowItem.modelData.thumbnail.length > 0
                                    ? "file://" + rowItem.modelData.thumbnail : ""
                            fillMode: Image.PreserveAspectCrop
                            visible: rowItem.modelData.thumbnail.length > 0
                            asynchronous: true
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: rowItem.modelData.thumbnail.length === 0
                            text: "🎞"
                            font.pixelSize: Theme.px(24)
                            opacity: 0.7
                        }
                    }

                    // Camera, detection type, date-time
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.px(3)
                        RowLayout {
                            spacing: Theme.px(8)
                            Text {
                                text: rowItem.modelData.camera
                                color: Theme.text
                                font.pixelSize: Theme.px(14)
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: text.length > 0
                                text: page.labelFor(rowItem.modelData.type)
                                color: page.colorFor(rowItem.modelData.type)
                                font.pixelSize: Theme.px(12)
                                font.bold: true
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: page.dayLabel(page.startOfDay(new Date(rowItem.modelData.start * 1000)))
                                  + " " + page.clock(rowItem.modelData.start)
                                  + " – " + page.clock(rowItem.modelData.end)
                            color: Theme.textMuted
                            font.pixelSize: Theme.px(12)
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: [page.duration(rowItem.modelData.end - rowItem.modelData.start),
                                   page.sizeText(rowItem.modelData.size)]
                                  .filter(s => s.length > 0).join(" · ")
                            color: Theme.textMuted
                            font.pixelSize: Theme.px(11)
                        }
                    }

                    // Download split button: the main part downloads to the
                    // download folder, the chevron offers Save as… and the folder.
                    // Shown on hover, and stays while busy, done or failed.
                    Rectangle {
                        id: dlButton
                        visible: rowHover.hovered || rowItem.dlState !== "" || dlMenu.opened
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: dlMain.width + dlChevron.width + 1
                        implicitHeight: Theme.px(30)
                        radius: Theme.radius
                        color: Theme.accentDim
                        clip: true
                        readonly property bool busy: rowItem.dlState === "busy"

                        Row {
                            anchors.fill: parent
                            // Main part
                            // Hover fills round only their outer corners, matching
                            // the button (clip doesn't follow the radius).
                            Rectangle {
                                id: dlMain
                                width: dlRow.implicitWidth + Theme.px(20)
                                height: parent.height
                                radius: dlButton.radius
                                topRightRadius: 0
                                bottomRightRadius: 0
                                color: dlHover.hovered && !dlButton.busy ? Theme.accent : "transparent"
                                Row {
                                    id: dlRow
                                    anchors.centerIn: parent
                                    spacing: Theme.px(6)
                                    // Spinner until the first bytes arrive, then
                                    // progress, then the green tick when saved.
                                    StatusRing {
                                        visible: dlButton.busy || rowItem.dlState === "saved"
                                        indeterminate: dlButton.busy && !rowItem.dlKnown
                                        value: rowItem.dlState === "saved" ? 1
                                             : rowItem.dlKnown ? rowItem.dlFraction : 0
                                        done: rowItem.dlState === "saved"
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: dlButton.busy ? qsTr("Downloading…")
                                            : rowItem.dlState === "error" ? qsTr("⚠ Retry")
                                            : rowItem.dlState === "saved" ? qsTr("Download")
                                            : qsTr("⤓ Download")
                                        color: Theme.text
                                        font.pixelSize: Theme.px(12)
                                    }
                                    // Fixed width (as wide as "100%") and equal-width
                                    // digits, so the button doesn't jitter.
                                    Text {
                                        visible: rowItem.dlKnown
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: pctMetrics.advanceWidth
                                        horizontalAlignment: Text.AlignRight
                                        text: rowItem.dlKnown ? Math.floor(rowItem.dlFraction * 100) + "%" : ""
                                        color: Theme.text
                                        font.pixelSize: Theme.px(12)
                                        font.features: { "tnum": 1 }
                                        TextMetrics {
                                            id: pctMetrics
                                            text: "100%"
                                            font.pixelSize: Theme.px(12)
                                            font.features: { "tnum": 1 }
                                        }
                                    }
                                }
                                HoverHandler { id: dlHover }
                                TapHandler {
                                    onTapped: if (!dlButton.busy) page.download(rowItem.modelData)
                                }
                                ToolTip {
                                    id: dlTip
                                    visible: dlHover.hovered && text !== ""
                                    delay: 400
                                    text: rowItem.dlState === "error" ? (page.downloadErrors[rowItem.dlKey] || "")
                                        : rowItem.dlState === "" ? qsTr("Save to %1").arg(page.prettyPath(page.downloadFolder))
                                        : ""
                                    contentItem: Text { text: dlTip.text; color: Theme.text; font.pixelSize: Theme.px(11) }
                                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: Theme.px(4) }
                                }
                            }
                            Rectangle { // divider
                                width: 1
                                height: parent.height
                                color: Qt.rgba(1, 1, 1, 0.18)
                            }
                            // Chevron: destination options
                            Rectangle {
                                id: dlChevron
                                width: Theme.px(26)
                                height: parent.height
                                radius: dlButton.radius
                                topLeftRadius: 0
                                bottomLeftRadius: 0
                                color: chevHover.hovered || dlMenu.opened ? Theme.accent : "transparent"
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u25be"
                                    color: Theme.text
                                    font.pixelSize: Theme.px(11)
                                }
                                HoverHandler { id: chevHover }
                                TapHandler { onTapped: dlMenu.popup(dlChevron, 0, dlChevron.height) }
                                ThemedMenu {
                                    id: dlMenu
                                    ThemedMenuItem {
                                        enabled: false
                                        implicitWidth: Theme.px(260)
                                        text: qsTr("Saves to %1").arg(page.prettyPath(page.downloadFolder))
                                    }
                                    ThemedMenuSeparator {}
                                    ThemedMenuItem {
                                        text: qsTr("Save as…")
                                        enabled: !dlButton.busy
                                        onTriggered: page.saveAs(rowItem.modelData)
                                    }
                                    ThemedMenuItem {
                                        text: qsTr("Change download folder…")
                                        onTriggered: page.chooseFolder()
                                    }
                                    ThemedMenuItem {
                                        visible: dlStore.folder !== ""
                                        height: visible ? implicitHeight : 0
                                        text: qsTr("Use the default folder")
                                        onTriggered: dlStore.folder = ""
                                    }
                                }
                            }
                        }
                    }
                }

                HoverHandler { id: rowHover }
            }

            // Empty / loading state
            Column {
                anchors.centerIn: parent
                visible: page.shown.length === 0
                spacing: Theme.px(8)
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "🎞"; font.pixelSize: Theme.px(40); opacity: 0.7
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: page.pendingSearches > 0 ? qsTr("Searching recordings…")
                        : qsTr("No recordings on this day")
                    color: Theme.text
                    font.pixelSize: Theme.px(14)
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: page.pendingSearches === 0
                    text: qsTr("Use ‹ › to look at other days")
                    color: Theme.textMuted
                    font.pixelSize: Theme.px(12)
                }
            }
        }
    }
}
