# Proposal

## Why

变更 1 的强度模型只能感知"玩家输入引起的转动"；过场动画、载具、被击退、镜头晃动等**非输入**画面运动同样会诱发眩晕，却完全不会触发遮罩。变更 3 用系统级的 DXGI Desktop Duplication 截取 overlay 所在显示器的画面，在 GPU 上降采样后用投影互相关估计全局位移，换算成 °/s 交给强度模型，与输入检测取最大值。它不注入、不读内存、不装钩子，符合 DESIGN §0 的约束；`IntensityInputs` 里 `capture_deg_per_s` / `gate_capture` 两个接缝已由变更 1 预留，现在补上数据源。

## What Changes

- 新增截屏线程：对 overlay 当前所在显示器做 Desktop Duplication；`AcquireNextFrame(16 ms)`；超时、显示器切换、`DXGI_ERROR_ACCESS_LOST` 的处理与 2 s 重试；连续失败后写日志并在本次运行内停用该功能。
- 新增 GPU 降采样：桌面纹理 → `grid_w × grid_h`（默认 160×90）`R32_FLOAT` 灰度 → staging → CPU 读回（57 600 B）。
- 新增纯逻辑位移估计：列 / 行投影、去均值 + 去线性趋势、`±max_shift` 内的归一化互相关、抛物线亚像素峰值、置信度 `c`；`c < min_correlation` 时沿用上一值至多 `hold_ms` 后归零。
- 新增像素 → 角度换算：`deg = shift_px_screen × assumed_hfov_deg / W`，`capture_deg_per_s = deg / dt_frame`，以原子 `double` 交给主线程。
- 强度模型：截屏项使用与鼠标相同的阈值（按 `[general] sensitivity`），再按 `[trigger] capture_sensitivity` 乘保守系数（低 ×2.0 / 中 ×1.5 / 高 ×1.0），门控 `gate_capture`，与其他项取 `max()`。
- 截屏可见性联动：`hide_from_capture=auto` 且截屏检测处于开启状态时强制 `set_capture_excluded(true)`。
- 托盘新增"☑ 截屏运动检测"项与热键 `toggle_capture`（默认 `Ctrl+Alt+F6`），改动立即写 INI。
- 命令行 `--no-capture`：本次运行完全不启动截屏线程，不改 INI。
- 配置：读取并校验 `[capture]` 六个键与 `[trigger] capture / capture_sensitivity`。
- 性能：`--verbose` 下每 5 s 打印一行截屏性能统计；目标 GPU < 0.2 ms / 帧、截屏线程 CPU < 2%（单核）。
- 单元测试：`tests/test_motion_estimator.cpp`，合成 160×90 帧验证位移误差、亮度突变、换图、无纹理与换算公式。

## Capabilities

### New Capabilities
- `capture-motion-detection`: 截屏运动检测：采集生命周期与失败恢复、GPU 降采样、投影互相关位移估计与置信度、角度换算与保持 / 归零、线程交接、性能统计与 `--no-capture`。

### Modified Capabilities
- `motion-trigger`: MODIFIED「输入源门控」，把变更 1 的 `gate_capture = false` 换成 `enabled && capture_on && !excluded && known_game`；ADDED 截屏角速度项（阈值同鼠标，再乘 `capture_sensitivity` 系数）。
- `configuration`: MODIFIED「INI 节集合与顺序」（加入 `[capture]`）、「[trigger] 节」（加入 `capture` / `capture_sensitivity`）、「[hotkeys] 节」（默认值加入 `toggle_capture`）、「命令行参数」（`--no-capture` 的实际效果）、「调试帧日志」（frame 行在 pad 之后加 `capture=`，验证脚本据此看截屏角速度）；ADDED `[capture]` 节、截屏键的合法范围、热更新。
- `tray-and-hotkeys`: MODIFIED「托盘菜单结构」（加入"截屏运动检测"）、「热键动作表」（加入 `toggle_capture`）、「热键触发提示」（`toggle_capture` 的 toast）；ADDED 截屏运动检测的切换动作、采集失败与 `--no-capture` 下的菜单文字。
- `overlay-rendering`: MODIFIED「截屏与录屏可见性」，明确 auto 下"截屏检测开启"的定义（含 `--no-capture` 与采集失败）和生效顺序；MODIFIED「调试条」，把变更 1 固定显示的"截屏 0"换成真实的截屏字段（°/s + 置信度 / 暂停 / 关 / 失败）。

> 说明：以上能力都由 `core-overlay` 定义，目前尚未归档到 `openspec/specs/`。所有 MODIFIED 都是按 `core-overlay` 现有草稿逐字复制后修改的，`openspec validate` 会提示"目标 spec 尚不存在，归档会拒绝"，这在变更 1 归档之后自然消除（变更按 1→2→3→4 顺序实施与归档）。「托盘菜单结构」「热键动作表」「热键触发提示」「INI 节集合与顺序」「[hotkeys] 节」这几条也被变更 2 修改，本变更的版本以 `in-game-calibration` 的现有草稿为底（含 `calibrate`、"校准当前游戏…"、"校准列表 ▸"与 `[calibration]`）再加入截屏相关条目；若变更 2 的草稿在归档前又有改动，归档本变更前须逐条对照合并，否则会覆盖变更 2 的内容。

## Impact

- 新文件：`src/capture/desktop_dup.h/.cpp`、`src/capture/motion_estimator.h/.cpp`、`tests/test_motion_estimator.cpp`。`motion_estimator.*` 若已由并行原型实现，复用并补齐接口。
- 修改：`src/app/app.*`（启动 / 停止截屏线程、tick 接入、affinity 联动、`--no-capture`）、`src/model/intensity.*`（截屏项系数）、`src/config/*`（新键与校验）、`src/tray/tray.*` 与 `hotkeys.*`（菜单项与热键动作）、`src/res/MotionCalm.default.ini`（注释模板）、`scripts/build_app.cmd` / `build_tests.cmd`（加入新源文件）。
- 依赖：仅系统库 `d3d11`、`dxgi`、`d3dcompiler`（§13 已在链接清单中），无第三方依赖。
- 测试工具：手动验证需要 `tools/fakegame` 提供"滚动纹理"与"背景闪烁"模式；若尚无，则在本变更的任务中补上。
- 系统要求：`WDA_EXCLUDEFROMCAPTURE` 需 Windows 10 2004 及以上；更早系统上截屏检测仍可运行，但无法把 overlay 从截图中排除（见 design.md 风险）。

## 待确认

- **`capture_scale` 无对应 INI 键**：§4 写"截屏同式作用于 `capture_deg_per_s × capture_scale`（默认使其偏保守：阈值 ×1.5）"，§8 却没有 `capture_scale` 键，§11 改用"按 `capture_sensitivity` 再乘保守系数（低 ×2.0、中 ×1.5、高 ×1.0）"。本变更按 §11 执行：不新增 `capture_scale`，系数由 `capture_sensitivity` 决定，默认 medium 的 ×1.5 与 §4 一致。
- **"阈值同鼠标"的基准**：§11"阈值同鼠标，按 `capture_sensitivity` 再乘保守系数"有两种读法：基准阈值取 `[general] sensitivity` 对应的 `v_start/v_full`（与鼠标完全相同），或取 `capture_sensitivity` 那一档的阈值再乘系数。本变更采用前者（截屏只多乘一个系数），默认得到 v_start 90 / v_full 540 °/s。
- **"连续失败"的次数和"关闭该功能"的范围**：§11"连续失败则在日志里说明并关闭该功能"没有给出次数，也没说是否写回 INI。本变更定为：每 2 s 重试一次，`E_ACCESSDENIED`（UAC 安全桌面 / 锁屏）不计数，其他错误连续 10 次后放弃；只在本次运行内停用，不改 INI。
- **空闲功耗与持续采集的冲突**：§3.1 要求空闲时 CPU ≈ 0%，§11 的工作线程却持续 `AcquireNextFrame`。本变更规定不需要截屏时（`gate_capture` 为假）截屏线程暂停采集并阻塞等待，暂停超过 5 s 释放 duplication 对象。
- **超时就衰减会抖动**：§11"超时 → 帧未变 → 速度衰减为 0"对帧率低于显示器刷新率的游戏（如 40 fps 配 60 Hz）会让速度在"有值"与 0 之间来回跳。本变更规定超时与低置信度用同一条规则：沿用上一值至多 `hold_ms`，之后归零；可见的淡出由强度模型的 release 平滑完成。
- **`hide_from_capture=never` 与截屏检测同时开启**：此时 overlay 会进入自己的截屏画面（§11"自截屏问题"只处理了 auto）。本变更不强制改值，只在日志里写 warn。
- **调试条"截屏 0.12"含义不明**：§7 的调试条示例没说 0.12 是什么，变更 1 暂时固定显示"截屏 0"并留给本变更定义。本变更定为 `{°/s}°/s c{置信度}`，另有 `暂停` / `关` / `失败` 三种状态文字。
- **`[capture]` 键没有取值范围**：§8 只给了默认值，本变更补充了范围（见 `configuration` 增量），设置窗口（变更 4）使用同一套范围。
