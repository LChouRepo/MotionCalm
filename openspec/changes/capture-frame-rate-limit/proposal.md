# Proposal

## Why

截屏线程目前对每个新的桌面帧都做 GPU 降采样、读回与两个估计器（平移、相似变换）。规格的性能预算按 1080p / 60 Hz 写成，但很多玩家用 144 / 165 / 240 Hz 显示器：游戏帧率跟上刷新率时，截屏线程的 GPU 与 CPU 工作量随刷新率线性增长（240 Hz 是 60 Hz 的 4 倍），而运动检测并不需要这么高的采样率——强度模型的遮罩出现约需 0.1 s，60–110 次 / 秒的速度测量已经足够。本变更在高刷新率显示器上抽帧，把处理帧率限制在约 55–109 次 / 秒，60–100 Hz 显示器上的行为不变。

## What Changes

- 新增纯逻辑类 `FrameRateLimiter`（`src/capture/capture_policy.h/.cpp`），目标间隔常量 `kTargetFrameIntervalS = 0.01375`（13.75 ms）。
- 截屏线程（`CaptureService::Impl::step`）仍然取到每个桌面帧；对新的（非只有指针更新的）帧按 DDA `LastPresentTime` 判定：`G + g/2 ≥ 13.75 ms` 时处理（降采样 + 估计），否则直接释放（参考帧不变，发布值按保持规则刷新，计入 `skipped`）。`G` = 距上一处理帧的时间，`g` = 距上一到达帧的时间。即"离目标间隔最近的那一帧"。
- 60 / 75 / 90 / 100 Hz 的帧流一帧不丢；120 / 144 / 165 / 240 Hz 分别处理 60 / 72 / 82.5 / 80 帧 / 秒。
- 首帧、时间戳无效或不递增时一律处理并重新锚定；会话建立 / 关闭与暂停后恢复时重置限速器。
- `capture perf:` 行末尾追加 ` skipped=<n>`；`frames=` 只数处理过的帧。`CaptureDebug` 新增累计计数 `skipped`，capturetest 的汇总行打印它。
- 相似变换估计的历史帧只包含处理过的帧；`baseline_ms = 0` 时的"上一帧"指上一处理帧。

不在范围内：可配置的目标间隔（INI 键 / 设置窗口控件）、降低 60 Hz 下的处理帧率、按游戏帧率而非显示器刷新率限速的其他策略。

## Capabilities

### New Capabilities

（无）

### Modified Capabilities

- `capture-motion-detection`: ADDED「处理帧限速」「被跳过的帧」；MODIFIED「GPU 降采样为灰度网格」（只对处理帧降采样）、「截屏性能日志」（行尾 `skipped=<n>`，`frames` 只数处理帧）、「历史帧与比较基线」（历史与"上一帧"都指处理帧）。

## Impact

- 修改：`src/capture/capture_policy.*`（`FrameRateLimiter`、`kTargetFrameIntervalS`、`PerfWindow::skipped`、`format_perf_line`）、`src/capture/desktop_dup.*`（`step()` 里的限速、重置时机、`CaptureDebug::skipped`）、`tools/capturetest/main.cpp`（打印 `skipped`）、`tests/test_capture_policy.cpp`、`tests/test_capture_service.cpp`。
- 依赖：无新增。
- 性能：60–100 Hz 不变；更高刷新率下截屏线程每秒的降采样与估计次数降到约 55–109 次（尚无实测数据）。
- 兼容：`capture perf:` 行只在末尾追加字段，按前缀 / 字段名解析的旧脚本不受影响；但高刷新率显示器上 `frames=` 的数值会变小（`frames + skipped` 才是取到的新帧数）。
