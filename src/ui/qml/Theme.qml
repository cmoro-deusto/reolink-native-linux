pragma Singleton
import QtQuick
import ReolinkApp.Core

// Dark palette approximating the official Reolink Client. Single source of
// truth for colors/metrics — no literal colors anywhere else in the UI.
QtObject {
    // Every size in the UI goes through px(): the layout was designed around
    // a 10 pt (13.33 px) system font, and scales with the font the user picked
    // so the app's text matches the rest of the desktop. Display scaling (e.g.
    // 145%) is applied by Qt on top and needs nothing here. Never shrinks below
    // the design size: Qt's no-theme fallback (9 pt) isn't a user choice.
    readonly property real uiScale: Math.max(1, Math.min(3, UiMetrics.fontPixelSize / 13.333))
    function px(n) { return Math.round(n * uiScale); }

    readonly property color window: "#10161d"
    readonly property color surface: "#1a222b"
    readonly property color surfaceAlt: "#222d38"
    readonly property color border: "#2c3945"
    readonly property color accent: "#2aa7ff"
    readonly property color accentDim: "#1d6faa"
    readonly property color text: "#e8eef4"
    readonly property color textMuted: "#8b98a5"
    readonly property color paneBackground: "#05080b"
    readonly property color danger: "#e5484d"
    readonly property color online: "#46a758"

    readonly property int radius: px(6)
    readonly property int spacing: px(8)
    // How far the pointer must move before a press becomes a drag rather than
    // a click, for dragging cameras between grid cells.
    readonly property int dragThreshold: px(8)
    readonly property int sidebarWidth: px(240)
    readonly property int navHeight: px(48)
}
