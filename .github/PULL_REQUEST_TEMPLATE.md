## What and why

<!-- What does this change, and why? Link related issues: "Fixes #123". -->

## How it was tested

<!-- Unit tests, live verification scripts you ran (verify.ps1 / -Capture / -Forward, verify_calibration.ps1)
     and their results, games you tried. -->

## Checklist

See [CONTRIBUTING.md](../CONTRIBUTING.md) for details.

- [ ] `build.cmd all release` succeeds and all unit tests pass
- [ ] New logic has unit tests
- [ ] Live verification scripts run if input, detection, capture, calibration or rendering changed
- [ ] UI strings added in all three languages (zh-CN, zh-TW, en); user guides updated in all three if needed
- [ ] OpenSpec change added / updated and `openspec validate --strict` passes (behaviour changes)
- [ ] `CHANGELOG.md` updated under "Unreleased"
- [ ] No personal paths, user names or machine-specific settings
