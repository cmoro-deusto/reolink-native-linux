import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// A detached live-view window for one camera — drag it to a second monitor for
// multi-screen viewing (DESIGN §6.2). Plays the main ("Clear") stream over
// native Baichuan — the NVR's RTSP output for 8K duo mains is corrupt (see
// LivePane) — falling back to RTSP main once if the Baichuan slot is busy.
// Closing stops the player; Esc closes.
Window {
    id: win
    property int deviceRow: -1
    property string label: ""
    // Owner of the app-wide "one audible source" rule (Main's window): holds
    // the pop-out that is playing sound, if any.
    property var audioController: null
    readonly property bool canTalk: deviceRow >= 0 && Devices.cameraInfo(deviceRow).capTalk === true
    readonly property bool audioOn: audioController !== null
                                    && audioController.audiblePopout === win
    function setAudio(on) {
        if (audioController)
            audioController.audiblePopout = on ? win : null;
    }

    width: Theme.px(960)
    height: Theme.px(540)
    minimumWidth: Theme.px(320)
    minimumHeight: Theme.px(180)
    visible: true
    color: Theme.paneBackground
    title: label + " — Reolink Client"

    // Digital zoom: wheel to zoom, drag to pan when zoomed (as in a live pane).
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    Item {
        anchors.fill: parent
        clip: true
        HoverHandler { id: winHover } // reveals the speaker button

        VideoOutput {
            id: video
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            visible: player.state === StreamPlayer.Streaming
            transform: [
                Scale {
                    origin.x: video.width / 2
                    origin.y: video.height / 2
                    xScale: win.zoom
                    yScale: win.zoom
                },
                Translate { x: win.panX; y: win.panY }
            ]
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            property real lastX: 0
            property real lastY: 0
            cursorShape: win.zoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
            // Pin the zoomed footage to the window edges (no panning it off into
            // empty space; zooming out reels it back in).
            function clampPan() {
                var mx = Math.max(0, (video.contentRect.width * win.zoom - video.width) / 2);
                var my = Math.max(0, (video.contentRect.height * win.zoom - video.height) / 2);
                win.panX = Math.max(-mx, Math.min(mx, win.panX));
                win.panY = Math.max(-my, Math.min(my, win.panY));
            }
            onPressed: (m) => { lastX = m.x; lastY = m.y; }
            onPositionChanged: (m) => {
                if (win.zoom > 1.0 && (m.buttons & Qt.LeftButton)) {
                    win.panX += (m.x - lastX);
                    win.panY += (m.y - lastY);
                    lastX = m.x; lastY = m.y;
                    clampPan();
                }
            }
            onWheel: (w) => {
                var z = win.zoom * (w.angleDelta.y > 0 ? 1.15 : 0.87);
                win.zoom = Math.max(1.0, Math.min(8.0, z));
                if (win.zoom <= 1.0) { win.panX = 0; win.panY = 0; }
                else clampPan();
            }
        }

        // Speaker toggle, bottom right: shown on hover, and kept up while audible so
        // it's clear where the sound comes from. It must be a CHILD of this Item:
        // as a sibling, hovering it un-hovered winHover, which hid it, which
        // re-hovered winHover — a flicker loop that also swallowed clicks.
        Rectangle {
            id: speaker
            visible: player.hasAudio && player.state === StreamPlayer.Streaming
                     && (winHover.hovered || speakerHover.hovered || win.audioOn)
            anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: Theme.px(8)
            width: Theme.px(34); height: Theme.px(34); radius: Theme.radius
            color: win.audioOn ? Theme.accentDim : (speakerHover.hovered ? Theme.surfaceAlt : "#cc0d141b")
            border.color: Theme.border
            Text {
                anchors.centerIn: parent
                text: win.audioOn ? "🔊" : "🔇"
                color: win.audioOn ? Theme.text : Theme.textMuted
                font.pixelSize: Theme.px(15)
            }
            HoverHandler { id: speakerHover }
            ToolTip {
                visible: speakerHover.hovered
                delay: 500
                y: -height - Theme.px(6)
                contentItem: Text {
                    text: (win.audioOn ? qsTr("Mute") : qsTr("Listen")) + " (M)"
                    color: Theme.text; font.pixelSize: Theme.px(11)
                }
                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: Theme.px(4) }
            }
            // Exclusive grab, so the pan/zoom MouseArea underneath can't take the tap.
            TapHandler {
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: win.setAudio(!win.audioOn)
            }
        }

        // Two-way talk, left of the speaker (or in its place when the stream
        // has no audio): on hover, and kept up while talking.
        TalkButton {
            id: talkBtn
            framed: true
            size: Theme.px(34)
            deviceRow: win.canTalk ? win.deviceRow : -1
            visible: win.canTalk && player.state === StreamPlayer.Streaming
                     && (winHover.hovered || talking)
            anchors.bottom: parent.bottom
            anchors.right: speaker.visible ? speaker.left : parent.right
            anchors.bottomMargin: Theme.px(8)
            anchors.rightMargin: Theme.px(8)
        }
    }
    StreamPlayer {
        id: player
        videoSink: video.videoSink
        muted: !win.audioOn
    }
    property bool bcFallback: false // Baichuan slot busy -> RTSP main this round
    function startStream() {
        if (win.deviceRow < 0)
            return;
        if (!bcFallback) {
            player.loop = false;
            Devices.startBaichuanLive(win.deviceRow, player, true);
            return;
        }
        var url = Devices.liveUrl(win.deviceRow, true); // degraded: RTSP main
        if (url.length > 0) {
            player.expectedSize = Devices.declaredSize(win.deviceRow, true); // main
            player.source = url;
            player.start();
        }
    }
    Component.onCompleted: startStream()
    Connections {
        target: player
        function onStateChanged() {
            if (player.state === StreamPlayer.Error && !win.bcFallback) {
                win.bcFallback = true;
                win.startStream();
            }
            // The device dropped the one-shot Baichuan session: reconnect after
            // a beat (startStream's own stop/start leaves Stopped immediately).
            if (player.state === StreamPlayer.Stopped && !win.bcFallback && !win.closed)
                bcReconnect.restart();
            else
                bcReconnect.stop();
        }
    }
    Timer {
        id: bcReconnect
        interval: 3000
        onTriggered: {
            if (player.state === StreamPlayer.Stopped && !win.bcFallback && !win.closed)
                win.startStream();
        }
    }
    property bool closed: false
    onClosing: {
        win.closed = true;
        bcReconnect.stop();
        if (win.audioOn)
            win.setAudio(false);
        if (talkBtn.owner && talkBtn.talking)
            Devices.stopTalk();
        player.stop();
    }

    Text {
        anchors.centerIn: parent
        visible: player.state !== StreamPlayer.Streaming
        text: player.state === StreamPlayer.Error ? player.errorString : qsTr("Connecting…")
        color: Theme.textMuted
        font.pixelSize: Theme.px(13)
    }

    Rectangle { // name overlay
        visible: player.state === StreamPlayer.Streaming
        anchors.top: parent.top; anchors.left: parent.left; anchors.margins: Theme.px(8)
        radius: Theme.px(3); color: "#80000000"
        width: nmRow.implicitWidth + Theme.px(12); height: nmRow.implicitHeight + Theme.px(6)
        Row {
            id: nmRow; anchors.centerIn: parent; spacing: Theme.px(6)
            Text { text: win.label; color: "white"; font.pixelSize: Theme.px(12) }
            TalkBadge { deviceRow: win.deviceRow; fontSize: Theme.px(12) }
            Text {
                visible: win.zoom > 1.01
                text: win.zoom.toFixed(1) + "×"
                color: Theme.accent; font.pixelSize: Theme.px(12)
            }
        }
    }

    Shortcut {
        sequence: "M"
        enabled: player.hasAudio
        onActivated: win.setAudio(!win.audioOn)
    }

    Shortcut { sequence: "Escape"; onActivated: win.close() }
}
