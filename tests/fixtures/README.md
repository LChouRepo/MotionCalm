# Test fixtures: capture grid recordings

The capture motion detection estimators (`src/capture/motion_estimator.*`, `similarity_estimator.*`) can be
replayed offline on the exact 160×90 grayscale grids they saw in a real game. This folder explains how to
record such a clip, where to keep it, and how to attach expectations to it.

**Real recordings are never committed.** They are frames of commercial games (this is a public MIT
repository) and are a few MB each. `.gitignore` ignores `tests/fixtures/recordings/` and every `*.mcgrid`
file. CI only runs the synthetic clips generated in `tests/test_replay.cpp`.

## Recording a clip

Build the tools (`build.cmd tools`), start the game in borderless / windowed full screen on the **primary
monitor**, then from a console:

```bat
out\release\capturetest.exe --seconds 15 --record tests\fixtures\recordings\mygame-turn.mcgrid
```

Switch to the game within the first seconds and play the motion you want to capture (turning, strafing,
walking forward, a scene with a big HUD, …). `--grid WxH` records at a different grid size; the other
estimator options (`--hfov`, `--max-shift`, `--min-corr`, `--hold-ms`) do not affect what is recorded.
The grids are kept in memory and written when the run ends (≈ 29 KB per frame at 160×90, so 15 s at
60 fps is about 26 MB). If the grid size or the display geometry changes mid-run (resolution change,
rotation) recording stops at that point with a message; the frames up to then are still written.

## Replaying a clip

```bat
out\release\capturetest.exe --replay tests\fixtures\recordings\mygame-turn.mcgrid
out\release\capturetest.exe --replay tests\fixtures\recordings\mygame-turn.mcgrid --hfov 100 --csv test-output\turn.csv
```

No screen capture happens. The clip is run through `GridPipeline` (`src/capture/grid_pipeline.h`), which
mirrors what the capture thread does with each grid, and a summary is printed: frames, valid fractions,
low-confidence count, mean / max of the published pan, forward and roll speeds. `--csv` writes one row per
frame (raw estimates and published speeds).

To replay every clip of a folder in the unit tests:

```bat
set MC_REPLAY_DIR=%CD%\tests\fixtures\recordings
out\release\tests.exe replay_local
```

Each `*.mcgrid` gets a one-line summary. Without `MC_REPLAY_DIR` that test does nothing.

## Expectations file (`<clip>.expect`)

Next to `mygame-turn.mcgrid`, an optional `mygame-turn.expect` (UTF-8 text) records what the clip is
supposed to show. Both `tests.exe` (with `MC_REPLAY_DIR`) and `capturetest --replay` check it; failed bounds
fail the test / make `capturetest` exit with code 5.

```ini
# mygame-turn: mouse turn right, then left, ~120 deg/s; HUD at the bottom
hfov=90                 # replay parameters (optional): hfov, max_shift, min_corr, hold_ms, baseline_ms, detect_roll (0/1)
valid_fraction_min=0.8
mean_speed_min=40
mean_speed_max=200
max_zoom_max=10         # turning in place must not look like moving forward
```

One `key=value` per line; `#` starts a comment (a whole line or the rest of a line); blank lines are
ignored. Keys:

| Key | Meaning |
|---|---|
| `hfov`, `max_shift`, `min_corr`, `hold_ms`, `baseline_ms`, `detect_roll` | Replay parameters, same meaning as the `[capture]` INI keys; `capturetest` options given on the command line take precedence |
| `<metric>_min`, `<metric>_max` | Bound on a summary metric (inclusive) |

Metrics: `frames`, `valid_fraction` (pan estimate valid / estimates), `lowconf_fraction`,
`sim_valid_fraction` (forward / roll estimate valid / non-discarded frames), `mean_speed`, `max_speed`,
`mean_zoom`, `max_zoom`, `mean_roll`, `max_roll` (published values in °/s, averaged over all frames).
Unknown or duplicate keys and unparsable numbers are errors, so a typo cannot silently disable a check.
