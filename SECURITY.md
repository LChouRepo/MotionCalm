# Security Policy

## Supported versions

Only the latest release receives fixes. Please update to the
[latest release](https://github.com/LChouRepo/MotionCalm/releases/latest) before reporting.

| Version | Supported |
|---|---|
| 1.1.x (latest) | Yes |
| < 1.1 | No |

## What MotionCalm does and doesn't do

MotionCalm is a portable, unsigned Windows program. To help you judge it:

**It does not**

- inject code into games or any other process, read or write other processes' memory, or modify game
  files;
- install input hooks (`SetWindowsHookEx` low-level mouse / keyboard hooks), drivers or services;
- open network connections — it links no networking libraries and has no telemetry, update check or
  crash upload;
- need administrator rights (it only needs them if the game itself runs as administrator).

**It does**

- read raw mouse and keyboard input with Raw Input (only relative mouse movement and the state of
  W A S D and the arrow keys are used; no text is recorded) and poll controllers with XInput;
- look at the foreground window (process name, window size, cursor state) to decide whether a game is
  running;
- when screen motion detection is on (default), capture the monitor the game is on with **DXGI Desktop
  Duplication**, downscale each frame on the GPU to a 160×90 grayscale grid and compare consecutive
  grids to estimate motion. Frames are processed in memory only; they are never written to disk or
  sent anywhere. While this is on, the overlay window marks itself as excluded from capture;
- write `MotionCalm.ini` (settings) and `MotionCalm.log` (log; contains program names such as the
  game's `.exe`) next to `MotionCalm.exe`;
- write `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MotionCalm` only if you turn on
  "Start with Windows".

Everything stays on your computer. Deleting the folder (and turning off "Start with Windows" first)
removes MotionCalm completely.

Because releases are not code-signed, download them only from this repository's
[Releases page](https://github.com/LChouRepo/MotionCalm/releases). Release zips are built by GitHub
Actions from the tagged source ([`.github/workflows/release.yml`](.github/workflows/release.yml)), and
you can build the same binary yourself (see [README.md](README.md#build-from-source)).

## Reporting a vulnerability

Please **do not** open a public issue for security problems. Use GitHub's private reporting instead:
go to the repository's **Security** tab and click **Report a vulnerability**
([direct link](https://github.com/LChouRepo/MotionCalm/security/advisories/new)).

Include the MotionCalm version, Windows version, what an attacker could do, and steps to reproduce.
You can expect an initial reply within a week. Once a fix is released, the advisory is published and
you are credited unless you prefer otherwise.
