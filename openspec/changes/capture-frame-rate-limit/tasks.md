# Tasks

## 1. 限速逻辑（纯逻辑，可单测）

- [x] 1.1 `src/capture/capture_policy.h/.cpp`：`kTargetFrameIntervalS = 0.01375`；`FrameRateLimiter`（`on_frame(t_s)`、`reset()`、`target()`），规则 `G + g/2 ≥ target`，首帧 / 时间戳非有限或 ≤ 0 / 不递增时处理并重新锚定（design D2、D3）。验证：`tests/test_capture_policy.cpp` 的限速用例。
- [x] 1.2 `PerfWindow::skipped`；`format_perf_line` 在末尾追加 ` skipped=<n>`（design D4）。验证：`tests/test_capture_policy.cpp` 的性能行用例（字段顺序、`skipped=` 在末尾）。
- [x] 1.3 单元测试：60 / 75 / 90 / 100 Hz（带抖动）不抽帧；120 / 144 / 165 / 240 Hz 的处理帧率；51–499 Hz 扫频的边界；首帧与异常时间戳；暂停后恢复（`reset()`）；目标间隔为 0 时不限速。

## 2. 截屏线程接入

- [x] 2.1 `CaptureService::Impl::step()`：只有指针更新的判断之后、降采样之前调用 `limiter.on_frame(LastPresentTime)`；跳过时释放帧、计入 `skipped`、`drain()`、按保持规则发布、`perf_tick()`（design D1）。
- [x] 2.2 `open_session()`、`close_session()` 与暂停（`enter_idle`）时 `limiter.reset()`。
- [x] 2.3 `CaptureDebug::skipped`；`frames` 的注释说明只数处理帧。验证：`tests/test_capture_service.cpp` 检查未取帧时 `skipped == 0`。
- [x] 2.4 capturetest 汇总行打印 `skipped=`。

## 3. 文档

- [x] 3.1 DESIGN.md §11（处理帧限速、性能数字按处理帧计）、CHANGELOG.md。

## 4. 验证

- [ ] 4.1 `build.cmd all debug` 与 `build.cmd all release`：0 警告，单元测试全部通过。 <!-- 未验证：编写时机器上还没有编译器 -->
- [ ] 4.2 在 144 Hz 或 240 Hz 显示器上运行 `out\release\capturetest.exe --seconds 10`（游戏或画面以满刷新率运动）：汇总行的 fps 约为 72 / 80，`skipped` 约为其余的帧；`capture perf:` 行末尾有 `skipped=`。 <!-- 未验证：需要真实的高刷新率显示器 -->
- [ ] 4.3 实机回归 `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Capture`（60 Hz 下结果不变，`skipped=0`）。 <!-- 未验证：需要用户在场 -->

## Workflow follow-up

- 4.1–4.3 通过后勾选并记录结果，再归档：`openspec archive capture-frame-rate-limit`。
