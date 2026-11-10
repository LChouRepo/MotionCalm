# Contributing to MotionCalm

**English** | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md)

Thanks for helping! Bug reports, game compatibility notes ("works / doesn't work in game X"),
translations and pull requests are all welcome. For security issues, see [SECURITY.md](SECURITY.md).

## Development setup

- Windows 10 (2004+) or Windows 11, x64.
- Visual Studio 2022 or 2026 (any edition, or Build Tools) with the **Desktop development with C++**
  workload (MSVC x64 tools + Windows 10/11 SDK). No other dependencies, no package manager.
- `scripts\env.cmd` finds Visual Studio with `vswhere.exe`. If it can't, set `MC_VS_PATH` to your
  Visual Studio install folder (the folder that contains `VC\Auxiliary\Build`), or `MC_VCVARS` to the
  full path of `vcvars64.bat`.
- Optional: Python 3 (only for `tools\make_icon.py`), Node.js (only for the OpenSpec CLI).

## Build and test

Run these from a normal `cmd` prompt in the repository root (a Developer Command Prompt works too):

```bat
build.cmd all release        :: app + tools + tests; tests.exe runs automatically
build.cmd tests debug        :: targets: all | app | tools | tests; configs: debug | release
out\release\tests.exe        :: unit tests only; non-zero exit code on failure
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.0   :: release zip in out\
```

Output goes to `out\<config>\`, objects to `out\<config>\obj\`; nothing is written into the source tree.
The unit tests are the same ones CI runs on every push and pull request.

### Live verification scripts

These scripts check the real app end to end. They **take over your screen, mouse and keyboard for about
20 seconds**: `fakegame.exe` opens a full-screen window that hides and locks the cursor, and
`inputsim.exe` sends synthetic mouse movement. Close other full-screen apps, don't touch the mouse or
keyboard while they run (any key makes `fakegame` quit early), and don't run them over Remote Desktop.
They are **not** run in CI. Screenshots and logs go to `test-output\` (ignored by git).

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1             # mouse turn → mask appears and fades
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Capture    # screen motion detection (panning)
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Forward    # forward / backward and roll detection
powershell -ExecutionPolicy Bypass -File scripts\verify_calibration.ps1 # two-run in-game calibration
```

Each script prints PASS / FAIL per check. If you have a released MotionCalm running, exit it first: it
holds the default hotkeys and can make hotkey checks fail.

Other tools: `capturetest.exe` (capture and estimator timing), `fakegame.exe --selfcheck`
(estimator self-check without a window), and `settingsdemo.exe --lang <zh-CN|zh-TW|en> --advanced
--screenshots test-output\i18n\<lang>` (screenshots of every settings page with a fake host).

To tune the capture estimators on real game footage, record a clip with
`capturetest.exe --seconds 15 --record tests\fixtures\recordings\<name>.mcgrid` and replay it offline with
`capturetest.exe --replay <clip> [--csv OUT]`, or set `MC_REPLAY_DIR` to the folder and run
`tests.exe replay_local`; see [tests/fixtures/README.md](tests/fixtures/README.md). Never commit recordings:
they are frames of commercial games (`*.mcgrid` and `tests/fixtures/recordings/` are git-ignored).

## OpenSpec workflow

Behaviour is specified with [OpenSpec](https://github.com/Fission-AI/OpenSpec). Specs and change
artifacts are written in Simplified Chinese (see `openspec/config.yaml`); the structural headings and
SHALL / MUST keywords stay in English.

- `openspec/specs/<capability>/spec.md` — the current requirements, one folder per capability.
- `openspec/changes/<change>/` — a change in progress: `proposal.md`, `design.md`, `tasks.md` and
  delta specs under `specs/`.
- `openspec/changes/archive/<date>-<change>/` — finished changes; archiving merges the delta specs into
  `openspec/specs/`.

For anything bigger than a bug fix: propose a change, implement the tasks, tick them in `tasks.md`,
then archive it. Run `openspec validate --strict` before you open the pull request. The repository
ships helpers for Claude Code in `.claude/skills/` and `.claude/commands/opsx/` (`/opsx:propose`,
`/opsx:apply`, `/opsx:archive`, …); they are optional — the plain `openspec` CLI works the same way.
Small fixes don't need a change folder.

[DESIGN.md](DESIGN.md) is the shared understanding of the whole project. If an implementation needs to
deviate from it, update DESIGN.md first and say why.

## Coding conventions

From [DESIGN.md §13](DESIGN.md):

- C++20, MSVC, `/W4 /permissive- /utf-8`. Win32 + Direct3D 11 + DirectComposition + Direct2D /
  DirectWrite. **No third-party dependencies.**
- COM objects via `Microsoft::WRL::ComPtr`; RAII for every handle.
- No exceptions for expected errors: initialization returns `bool` and writes a log line.
- `std::wstring` for Win32 strings; source files are UTF-8; wide string literals `L"…"`.
- Each source file starts with a one-line comment that states its responsibility.
- Keep logic that can be tested pure (no Win32), like `model/intensity` and the capture estimators,
  and add unit tests for it in `tests/`.
- Never inject into games, read game memory or install low-level hooks (`WH_MOUSE_LL` /
  `WH_KEYBOARD_LL`). MotionCalm only uses Raw Input, XInput and Desktop Duplication.
- `.cmd` files must stay ASCII-only with CRLF line endings (`.gitattributes` handles the endings).
- Settings window layout and wording rules: [docs/settings-ui.md](docs/settings-ui.md).

## Adding or changing a UI string

All user-facing text lives in `src/i18n/`; log lines are not translated.

1. Add an ID to `enum class StrId` in `src/i18n/strings.h`, in the matching group.
2. Add the text at **the same position** in all three tables: `strings_zh_cn.cpp` (source text),
   `strings_zh_tw.cpp` (Taiwan usage, not a character conversion: 設定, 滑鼠, 螢幕, 快速鍵, 程式, 預設, …)
   and `strings_en.cpp` (standard Windows terms, sentence-style capitalization, "…" on commands that
   open a dialog).
3. Use the same `{name}` placeholders and keep an access key (`&`) in every language if the source has
   one.
4. Use the string with `i18n::tr(StrId::…)`, or `i18n::format(StrId::…, {…})` for text with placeholders (see `src/i18n/i18n.h`).
5. `build.cmd tests` — `tests/test_i18n.cpp` fails if an ID is empty in any table or placeholders
   differ.
6. For settings window text, check that nothing is cut off:
   `out\release\settingsdemo.exe --lang en --advanced --screenshots test-output\i18n\en` (and `zh-CN`,
   `zh-TW`).

If the change affects the end-user guide, update all three: `docs/user-guide.zh-CN.md`,
`docs/user-guide.zh-TW.md`, `docs/user-guide.en.md`.

## Pull request checklist

- [ ] `build.cmd all release` succeeds and all unit tests pass.
- [ ] New logic has unit tests.
- [ ] Live verification scripts run if you changed input, detection, capture, calibration or rendering
      (say which ones you ran and the result in the PR).
- [ ] UI strings added in all three languages; user guides updated in all three languages if needed.
- [ ] OpenSpec change added / updated and `openspec validate --strict` passes (for behaviour changes).
- [ ] `CHANGELOG.md` updated under "Unreleased".
- [ ] No personal paths, user names or machine-specific settings in code, docs or scripts.

## Reporting bugs

Use the [bug report form](https://github.com/LChouRepo/MotionCalm/issues/new/choose). Please include
the game, Windows version, display mode (borderless / windowed / exclusive fullscreen), UI language,
MotionCalm version, and `MotionCalm.log` (tray menu → Open log). The log contains program names (for
example the game's `.exe`) — remove anything you don't want to share.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
