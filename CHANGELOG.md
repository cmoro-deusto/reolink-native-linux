# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the project is pre-1.0, minor versions may still change behaviour.

## [Unreleased]

### Fixed

- **PTZ and zoom controls showed on cameras that can't use them.** Any
  camera reporting PTZ control got the joystick and the zoom −/+ rocker,
  so a pan/tilt-only camera had zoom buttons that did nothing, and battery
  cameras with no motor got a PTZ button. Capabilities now come from the
  camera's PTZ type, as in reolink_aio: the joystick only for cameras that
  move, the zoom rocker only for ones with an optical zoom. The PTZ pad
  also scales with the desktop font now.

## [0.3.2] — 2026-11-07

### Fixed

- **A camera without a battery on a Home Hub broke the others.** Adding a
  mains-powered camera made start-up take ~20 s and left the existing
  cameras' live view as garbage. The battery fix in 0.2.0 asked every
  camera for its battery level, and the hub answers that for a camera with
  no battery by stalling ~16 s, then failing — and drops every other answer
  in the same request, so no camera's codec was learned and HEVC cameras
  were decoded as H.264. Battery is now only asked of cameras that report
  one, in a request of its own, and a codec that can't be learned is
  detected from the stream instead of assumed H.264.

## [0.3.1] — 2026-11-07

### Added

- **Recordings page** — a new tab between Events and Device Settings listing
  every recording the devices hold for a day, newest first: camera, date and
  time (24-hour), duration and size. Step through days with ‹ ›, filter by
  camera, and download any recording on hover — the device's own file
  (rebuilt over Baichuan on firmware without downloads). The button shows a
  progress ring and percentage, which turns into a green tick when done; its
  ▾ menu offers Save as… and a remembered download folder (default
  ~/Videos/Reolink).
  A recording that overlaps a detection shows its type and snapshot.

## [0.3.0] — 2026-11-07

### Added

- **Smooth live view** — a "Smooth" button on every live pane, on by default
  and remembered per camera. Frames were never lost, but they arrived in
  bursts (on a Home Hub Pro every HD keyframe came 220–300 ms late, and the
  SD stream stalled for up to ~0.6 s), which showed as a freeze followed by a
  rush of catch-up frames. With Smooth on, frames are shown on the camera's
  own clock behind a small delay that adapts to how late they arrive: it
  starts at 0.15 s and never exceeds 0.3 s. Measured on the hub, gaps over
  100 ms dropped from 72 to 2 in HD and from 40 to 1 in SD. Turn it off for
  the lowest delay.

### Fixed

- **The interface ignored the desktop's font size.** Every text size and most
  dimensions were fixed pixels designed around a 10 pt system font, so with a
  larger font (e.g. KDE at 13 pt) the app's text came out about a third
  smaller than the rest of the desktop. The whole UI — text, controls,
  spacing, the sidebar, timeline and dialogs — now scales with the system
  font and follows it if it changes while the app runs. Display scaling
  (e.g. 145%) was already handled by Qt; at the default font nothing
  changes.
- **Video kept streaming while the window was hidden in the tray.** Live View
  and Playback now stop their streams when the window is closed to the tray
  (monitoring, events and notifications carry on) and Live View resumes when
  it's reopened — about 3% CPU hidden instead of 25% with four cameras.
  Ported from upstream PR #23 by greenfanDev.
- **A dropped Baichuan connection pinned a CPU core.** When a camera or hub
  closed the connection, the HD stream's network loop spun at 100% CPU and
  the pane froze on its last frame. The stream now ends, and HD panes and
  pop-outs reconnect after 3 s; a silent live stream is closed after 15 s.
  Ported from upstream PR #15 by greenfanDev.
- **Streams weren't torn down, and quitting left them open.** Stopping a
  stream now sends RTSP TEARDOWN so cameras drop the session, quitting waits
  briefly for streams to close, and logout/shutdown (SIGTERM/SIGINT) go
  through the normal quit instead of killing the app mid-stream.
  Ported from upstream PR #22 by greenfanDev.
- **Short recordings were hard to click on the timeline.** At day zoom a
  30 s clip is a fraction of a pixel wide. A click in a gap within reach of
  a recording now starts that recording, the target is outlined and the
  hover time shows "▶ hh:mm", clips are drawn at least 3 px wide, and an
  event jump that lands just before its recording starts at that recording.
  Ported from upstream PR #25 by greenfanDev.
- **Events showed "just now" forever.** The relative label was computed once
  per row and never refreshed. Rows now show the actual time on a 24-hour
  clock — "Today 16:48:19", "Yesterday 09:02:15", "Thu Nov 5, 09:02:15",
  with the year for older ones — refreshed hourly so Today/Yesterday turn
  over after midnight. Ported from upstream PR #29 by greenfanDev.

## [0.2.0] — 2026-11-07

First release of this fork (see the README); it diverges from upstream from here on.

### Added

- **2-pane live grid** — a new layout preset that stacks two cameras one on top
  of the other (1 column × 2 rows), between the 1 and 4 buttons in the Live
  View toolbar.
- **Export the whole recording** — a new item in Playback's Export menu saves
  the device's own file for the recording at the playhead, untrimmed. Where
  the firmware serves no downloads (Home Hub Pro v3.3.0.339) it is rebuilt
  from a Baichuan replay of the recording's full span instead.
- **Battery levels refresh while the app runs** — every minute, folded into
  the existing per-host status poll, instead of only at connect/Reconnect.
- **Live View reopens as you left it** — the grid preset (1/2/4/6/9/16) and
  each camera's SD/HD choice in the grid are remembered across restarts.
  Maximizing a camera still switches it to HD; restoring it returns to the
  remembered choice.

### Changed

- **The update checker is off.** It looked for new releases on upstream's
  GitHub, and this fork is built from source instead of shipped as releases.
- **Built from source, no release builds.** The Arch PKGBUILD packages the
  local checkout instead of downloading an upstream release, and the
  AppImage/Flatpak release workflow is removed.

### Fixed

- **Playback didn't play on the Home Hub.** SD playback streams over the
  device's HTTP-FLV endpoint, which the Home Hub doesn't serve (it closes the
  connection without replying). When a device refuses FLV that way, SD
  playback now switches to Baichuan for that device — the transport HD
  playback already used.
- **Clip export on the Home Hub** failed with a bare "clip request failed":
  the hub refuses NvrDownload, the command NVRs use to cut a clip. Timed
  exports (15 s – 2 min) now cut the exact range from a Baichuan replay there
  instead — stream-copied (no re-encode), video plus AAC audio, much faster
  than realtime. Device rejections now show their error code.
- **Sidebar battery badge spacing.** The percentage no longer touches the
  battery icon, and badges line up across cameras whether they read 92% or
  100%.
- **Only the first camera on a hub/NVR showed its battery level.** Battery
  state was requested for channel 0 only; every online channel is now asked,
  so each battery camera behind a Home Hub or NVR gets its own badge.

## [0.1.8] — 2026-08-12

### Fixed

- **Playback failed with a black screen that retried forever** on systems with
  FFmpeg 8 or newer. Reolink devices serve HTTPS with a self-signed
  certificate, which the app has always accepted; FFmpeg 8.0 changed its
  default to reject it. Live view was unaffected because it uses RTSP. This
  only ever hit builds from source (Arch and other current distros) — the
  AppImage and Flatpak bundle an older FFmpeg — but it would have reached
  those too as their base images move forward.
- **Playback stopped dead at every event marker.** The NVR streams one
  recording file per connection and stores motion events as separate files, so
  continuous footage ended exactly where an event began. Playback now
  continues across recording boundaries automatically.
- **Removing a device and adding it back left its header stuck** on the first
  camera's name and "connecting…" forever, and a re-add could briefly show
  "undefined" in place of the camera count.

### Changed

- Clicking an event now starts playback **5 seconds before** the moment, so the
  approach is visible rather than only the aftermath.

### Internal

- Continuous integration: every push and pull request now builds and runs the
  test suite, including a second build against the newest Qt and FFmpeg so
  upstream changes surface before they reach anyone building from source.
- New regression test covering the TLS behaviour above.

## [0.1.7] — 2026-08-05

### Added

- Per-camera view rotation: right-click a camera in the sidebar →
  **Rotate view 90°** turns its picture in 90° steps, persisted per camera
  and applied in live view and both playback modes — for devices whose
  firmware misreports orientation. ([#3])

### Fixed

- HD live view failed ("Could not find codec parameters") on cameras whose
  newer firmware doesn't report the main-stream codec the way the app
  expected — it assumed H.264 and choked on H.265 streams. The codec report
  is now normalized across firmware spellings, and when it's genuinely
  unknown the stream is probed instead of assumed. ([#4])
- Cameras attached through a Home Hub could show rotated 90° because the hub
  declares their resolution transposed while the stream arrives upright; the
  auto-rotation heuristic now also requires the decoded frame to actually be
  portrait before correcting. ([#3])

[#3]: https://github.com/TodesengelX/reolink-native-linux/issues/3
[#4]: https://github.com/TodesengelX/reolink-native-linux/issues/4

## [0.1.6] — 2026-08-05

### Added

- **Synced multi-camera playback**: a 4-pane playback grid driven by one
  calendar and one timeline, like the official client's split playback. The
  timeline shows the union of every camera's recordings with a thin coverage
  lane per camera, and one playhead drives all panes to the same wall-clock
  moment; a camera with no footage there says so instead of looking broken.
- Playback grid interactions: drag cameras in from the sidebar or between
  panes (swaps, never drops), double-click to maximize a pane and again to
  restore, digital zoom with drag-to-pan in every pane, and an SD/HD toggle
  on the maximized pane that plays the full-resolution main stream with
  in-place seeking.
- Live view: drag cameras from the sidebar into any grid cell and drag panes
  between cells to rearrange; the arrangement persists across restarts.
- Sidebar recovery actions: right-click a device for **Reconnect** and
  **Update credentials…** — fix a wrong password in place instead of
  removing and re-adding the device.

### Fixed

- A device that was unreachable when the app started stayed "connecting…"
  forever; unreachable devices now retry automatically with backoff and
  recover on their own. Sign-in failures are shown with the device's
  attempts-left lockout warning and are deliberately never auto-retried.
- Adding a device now checks it first: a wrong address or rejected password
  is an inline message in the dialog instead of a phantom row.
- Rearranging grids no longer reconnects running streams — panes carry their
  streams with them, in live view and playback both. Restoring a maximized
  view reconnects nothing.
- Switching live-view layouts briefly holds hidden panes' streams so flipping
  between presets (or maximize and back) is instant.
- Concurrent commands to a device are now queued instead of racing, which
  was the main source of the NVR web server's intermittent 502 errors.

## [0.1.5] — 2026-08-02

### Fixed

- **Launching the app again raises the running window instead of starting a
  second copy.** Because closing the window leaves the app monitoring in the
  tray, relaunching from the launcher is the normal way back in — but every
  launch used to build a whole second client, with its own tray icon, NVR login
  and decoder set. They accumulated over a day of use.
- **Hardware video decoding now works in the AppImage.** The bundled `libva`
  could not load the host's VAAPI driver, so every stream silently fell back to
  software decode. The host's own `libva` is now used, with the bundled copy
  kept as a fallback so the app still starts on systems that have none.

## [0.1.4] — 2026-08-01

### Added

- System tray with background monitoring — close to tray, start on login, and
  an unread-event badge, so detection alerts keep working with the window shut.
- Offline and online alerts for cameras and for the NVR itself.
- Real thumbnails for each event in the inbox, and filtering events by camera.
- Detection events marked as red ticks on the playback timeline.
- Clip export — save the recording around the playhead to a file.
- Weekly recording-schedule editor: a 7×24 grid per recording type.

### Fixed

- Tray **Quit** always exits the app, instead of only removing the tray icon.

## [0.1.3] — 2026-07-30

### Added

- Clicking a detection notification opens the app, switches to Playback and
  plays that event back.
- The app version is shown in the nav bar.

### Fixed

- Clicking a notification now raises the window on Wayland. The compositor
  ignores a bare activation request as focus stealing, so the notification
  daemon's activation token is forwarded instead.

## [0.1.2] — 2026-07-30

### Fixed

- The AppImage runs natively on Wayland instead of falling back to XWayland;
  it now bundles the Qt Wayland platform and EGL client-buffer plugins.

## [0.1.1] — 2026-07-30

### Added

- Detection-zone editor — paint the parts of the image a camera should ignore.
- 6-camera live grid.
- Desktop notifications for detections, gated on each camera's Push setting.
- Built-in update checker, with one-click self-update for the AppImage.
- Event retention cap so the inbox stops growing without bound.

### Changed

- Device tree nests cameras under their NVR, with drawn NVR/camera icons and
  clearer online status.
- Right-click menus and the Add Device dialog restyled to match the rest of
  the app.

## [0.1.0] — 2026-07-11

Initial development release: live view, playback, events and device settings,
published as an AppImage and a Flatpak bundle.

[0.3.2]: https://github.com/cmoro-deusto/reolink-native-linux/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/cmoro-deusto/reolink-native-linux/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/cmoro-deusto/reolink-native-linux/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/cmoro-deusto/reolink-native-linux/compare/v0.1.8...v0.2.0
[0.1.8]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.7...v0.1.8
[0.1.7]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.6...v0.1.7
[0.1.6]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.5...v0.1.6
[0.1.5]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TodesengelX/reolink-native-linux/releases/tag/v0.1.0