# Changelog

All notable changes to MotionCalm are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The design notes behind each release are in [`openspec/changes/archive/`](openspec/changes/archive/).

## [Unreleased]

### Added
- Public repository layout: English / Simplified Chinese / Traditional Chinese READMEs, contributing
  guides, security policy, issue and pull request templates.
- GitHub Actions: build and unit tests on every push and pull request; release workflow that packages
  the zip and attaches it to the GitHub Release for `v*` tags.
- Offline recording and replay of screen motion detection: `capturetest --record PATH` saves the
  grayscale grids fed to the estimators as a versioned binary clip (`*.mcgrid`), and
  `capturetest --replay PATH [--csv OUT]` runs a clip through `GridPipeline`, a pure-logic mirror of
  the capture thread's estimation path, without capturing the screen. An optional `<clip>.expect` file
  holds bounds on the replay summary. New unit tests for the clip format and replay (synthetic clips;
  local clips from the `MC_REPLAY_DIR` environment variable); see `tests/fixtures/README.md`.
  Recordings are git-ignored and never committed.
- `CaptureService::set_grid_sink()`: a capture-thread callback that receives every grid fed to the
  estimators (used by tools only; the app never sets it).

### Changed
- **Capture frame-rate limit** (OpenSpec change `capture-frame-rate-limit`): on high-refresh-rate
  monitors the capture thread still acquires every desktop frame but only downscales and estimates
  the one closest to a 13.75 ms interval (by the DDA present timestamps); the others are released
  unprocessed. 60 / 75 / 90 / 100 Hz are not decimated; 120 / 144 / 165 / 240 Hz are processed at
  60 / 72 / 82.5 / 80 frames per second. The `capture perf:` debug line gains a trailing
  `skipped=<n>` field, and `frames=` now counts processed frames only.
- Release and debug builds link the C runtime statically (`/MT`, `/MTd`), so the `.exe` files no
  longer need the Visual C++ Redistributable.
- HLSL shaders are compiled at build time by `scripts/build_shaders.cmd` (`fxc.exe` from the Windows
  SDK, warnings as errors) and embedded as byte arrays; there is no `D3DCompile` at run time and
  `d3dcompiler.lib` is no longer linked.
- End-user guides moved to `docs/user-guide.{zh-CN,zh-TW,en}.md`. The release zip now contains
  `LICENSE` and the guides as `USER-GUIDE.zh-CN.md`, `USER-GUIDE.zh-TW.md` and `USER-GUIDE.en.md`.
- `scripts/env.cmd` finds Visual Studio with `vswhere.exe` (any edition with the x64 C++ tools);
  `MC_VS_PATH` (install folder) and `MC_VCVARS` (path of `vcvars64.bat`) remain as overrides.

### Fixed
- Screen motion detection no longer reports a huge pan speed on strictly periodic textures (floor
  tiles, fences, grids). The projection correlation has several equally high peaks there, one per
  texture period, and the estimator used to take whichever was marginally highest (for example
  574 °/s for a real 34 °/s pan). It now picks the peak closest to the previous valid shift, or the
  smallest shift when there is none. Non-periodic scenes have a single peak and are unaffected.

## [1.1.0] - 2026-11-06

### Added
- **Forward / backward and roll detection** (OpenSpec change `forward-and-roll-detection`): screen
  motion detection now estimates zoom (moving forward or backward) and roll (camera tilt) on the
  captured 160×90 grid with a robust similarity-transform Lucas–Kanade estimator, in addition to
  panning. Both feed the mask through their own threshold tables, selected by the trigger sensitivity.
- New `[capture]` settings: `forward_gain` (default 1.0), `detect_roll`, `baseline_ms` and the
  `zoom_*` / `roll_*` threshold keys; matching controls on the Trigger page (advanced options).
- Debug bar and `frame` log lines show the forward and roll speeds.
- **Localization** (OpenSpec change `localization`): the whole user interface (settings window, tray
  menu, toasts, calibration prompts, debug bar, About box) is available in Simplified Chinese,
  Traditional Chinese (Taiwan usage) and English. The default follows the Windows display language;
  it can be changed under Settings → General → Language (`[ui] language`).
- Traditional Chinese and English end-user guides.
- Test tools: `fakegame --zoom / --roll / --selfcheck`, `settingsdemo --lang`, and
  `scripts/verify.ps1 -Forward`.

### Fixed
- Settings window: a control that should be hidden on the General page could stay visible after
  switching to that page on first load.

## 1.0.0 - 2026-11-04

First release.

### Added
- **Core overlay** (OpenSpec change `core-overlay`): click-through Direct3D 11 + DirectComposition
  overlay that darkens the screen edges with a soft elliptical mask while the camera turns, with a
  fixed centre reference dot and eight faint edge dots. Triggers from raw mouse input (only while the
  game locks the cursor), WASD / arrow keys and XInput controllers. Game detection with an exclusion
  list for browsers and video players, three mask levels and three trigger sensitivities, tray menu,
  customizable global hotkeys, INI configuration with live reload, log file, single instance,
  follows the game's monitor, optional start with Windows.
- **In-game calibration** (OpenSpec change `in-game-calibration`): turn a full circle twice
  (Ctrl+Alt+F7) to measure mouse counts per degree for each game, so thresholds are in °/s.
- **Screen motion detection** (OpenSpec change `capture-motion-detection`): DXGI Desktop Duplication
  plus projection cross-correlation estimates global screen motion (screen shake, head bob, cutscenes,
  vehicles); the overlay is hidden from screen capture while it is on.
- **Settings window** (OpenSpec change `settings-window`): six pages (Mask, Reference dots, Trigger,
  Calibration, Hotkeys, General), changes apply and save immediately, preview strength slider,
  hotkey capture boxes with conflict detection, restore defaults.
- Simplified Chinese end-user guide; unit tests (`tests.exe`) and live verification scripts.

[Unreleased]: https://github.com/LChouRepo/MotionCalm/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/LChouRepo/MotionCalm/releases/tag/v1.1.0
