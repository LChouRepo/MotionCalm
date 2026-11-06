# Proposal

## Why

`core-overlay` 用一个固定的默认值 `default_counts_per_degree=18.2`（约等于 Source 引擎灵敏度 2.5）把鼠标计数换算成 °/s。可是"每度多少计数"取决于每个游戏的视角系数和玩家的游戏内灵敏度，同一只手的动作在不同游戏里可能差好几倍：Valorant 灵敏度 0.4 约为每度 35.7 计数，按默认值换算会把转速高估一倍，遮罩就会过早、过强地出现。按 DESIGN.md §10 让用户在游戏里水平转一整圈、测两次取平均，按 exe 保存结果，阈值就能真正以"屏幕上的 °/s"为准。

## What Changes

- 新增游戏内校准流程：状态机 `Idle → Run1 → Between → Run2 → Done`，全程只用 `calibrate` 一个热键（默认 `Ctrl+Alt+F7`）推进。不用 Esc，因为 Esc 可能被游戏占用。
- 只能在 `in_game && cursor_locked` 时开始。每次记录期间累计有符号的 Σdx，结束时取 `counts_360 = |Σdx|`。合法范围是 [800, 400000]；两次结果相差超过 15% 就丢弃重做；任一状态停留超过 60 s 自动取消；中途离开游戏也会取消。
- 结果 `counts_per_degree = 平均 / 360`，写入 `[calibration] <exe>=<值>` 并立即生效于强度模型，同时弹出 toast "校准完成：每度 18.4 计数"。
- overlay 在屏幕中上方显示多行校准提示，包括实时的"已转 N 计数"。
- 托盘新增"校准当前游戏…"（说明怎么操作）和"校准列表 ▸"（逐项删除）。
- INI 新增 `[calibration]` 节，以及 `[hotkeys] calibrate=Ctrl+Alt+F7`。

## Capabilities

### New Capabilities

- `turn-calibration`：游戏内转一圈校准，包括状态机、开始条件、计数、合法性与一致性规则、超时与取消、结果计算、提示文案和 toast。

### Modified Capabilities

以下能力由 `core-overlay` 新建，目前尚未归档。本变更必须在 `core-overlay` 归档之后再归档。

- `motion-trigger`：`counts_per_degree` 不再恒为默认值，改为按前台 exe 查校准值，查不到再回退默认值（修改"鼠标角速度换算"）。
- `tray-and-hotkeys`：菜单结构加入两个校准项；动作表加入 `calibrate`；热键提示里加入 calibrate 的行为；新增"校准菜单项"需求。
- `configuration`：节集合加入 `[calibration]`；`[hotkeys]` 的默认值加入 `calibrate`；新增"[calibration] 节"需求。
- `overlay-rendering`：新增"校准提示"文字层需求。

## Impact

- 新建：`src/calib/calibration.h/.cpp`（纯逻辑，不含 Win32）、`tests/test_calibration.cpp`。
- 修改：
  - `src/app/app.cpp`：接入 WM_INPUT、WM_HOTKEY、tick，查找 counts_per_degree；
  - `src/input/raw_input.h/.cpp`：增加"每个相对鼠标事件的原始 dx"回调；
  - `src/config/config.h/.cpp`：`[calibration]` 动态节的读、校验、写；
  - `src/res/MotionCalm.default.ini`：加 `calibrate` 键和 `[calibration]` 节；
  - `src/tray/hotkeys.cpp`：动作表加一项；
  - `src/tray/tray.cpp`：两个菜单项；
  - `src/overlay/overlay.h` 与 `src/overlay/renderer.cpp`：渲染 `OverlayFrame::message`；
  - `tests/test_ini.cpp`、`tests/test_hotkey_parse.cpp`：随之更新。
- 不新增依赖，也不新增任何系统层面的副作用（仍然只写 exe 同目录的 INI）。
- 依赖 `core-overlay` 已实现的接缝：`OverlayFrame::message` 字段、热键动作表、INI 模板重建的"动态节"钩子（core-overlay design D9）、托盘与热键共用的动作分发。

## 范围外

- 设置窗口里的"校准"分页，即列表、日期、"重测"、"删除"（变更 4）。本变更不记录校准日期。
- DESIGN.md §1 中的校准方案 (b) 瞄准练习场和 (c) 舒适度测试。
- 截屏运动检测的像素到角度换算（变更 3 用 `assumed_hfov_deg`，与本校准无关）。

## 风险

- **转得不准**：手动转一圈很难正好 360°，±5° 约等于 ±1.4% → 两次取平均，相差超过 15% 就重做；对阈值来说几个百分点的误差无关紧要。
- **鼠标加速或平滑**：游戏开了鼠标加速时，"计数 → 角度"不是线性的，两次结果容易不一致 → 15% 规则会拦下；toast 和 README 建议关掉加速。
- **游戏不用原始输入**：游戏读的是加速后的光标位移，而本程序统计的是原始计数，两者不一致 → 校准测的正是"原始计数 → 游戏内角度"，反而把这种映射吸收进去了；只要游戏内的映射是线性的，结果就正确。
- **热键在游戏里不可用**：被占用，或游戏以管理员身份运行时 UIPI 拦截 → 沿用 core-overlay 的占用 toast；托盘"校准当前游戏…"会提示当前绑定的组合键。
- **与遮罩互相干扰**：转一圈时遮罩会出现，但中心清晰区始终保留，准星和参照物都在中心附近，不影响对准。

## 验收标准

1. `build.cmd all debug` 成功；`build.cmd tests debug` 全部通过，包括新的 `calib_*` 测试和更新后的 `ini_*`、`hotkey_*` 测试。
2. 手动验证（tasks.md 7.2–7.5）：在 fakegame 中用 inputsim 发送已知计数，跑两次，一次 13104 计数，得到 `[calibration] fakegame.exe` ≈ 36.400（±1%），toast 为"校准完成：每度 36.4 计数"；随后 8000 counts/s 的转动在 `frame` 行中约为 219.8°/s，不再是 439.6；非法计数、15% 规则、超时、离开游戏、删除条目这几种情况的行为都符合 spec。
3. `scripts\verify.ps1` 的结果与 core-overlay 相同（回归）。

## 待确认

以下是 DESIGN.md §10 中的空白或歧义，括号内是本变更采用的读法：

1. "两次相差 > 15%"的基准没说。（以两次平均为基准：|c1 − c2| / ((c1 + c2) / 2) > 0.15 时丢弃。）
2. "超时 60 s"从什么时候算起没说。（Run1、Between、Run2 任一状态，从进入该状态起停留超过 60 s 就自动取消。）
3. 校准途中前台变了怎么办没说。（前台 exe 与开始时不同就取消，并弹 toast。Alt+Tab 出去会中断校准。）
4. toast "约 800 DPI × 灵敏度 2.5"：每度计数其实与 DPI 无关。（已决定：toast 只写"校准完成：每度 36.4 计数"，不附等效灵敏度。按 Source 系视角系数 0.022°/count 换算的"灵敏度"只对 CS 等 Source 系游戏成立，对其他游戏会误导玩家。）
5. 从托盘点"校准当前游戏…"时前台已经不是游戏，无法满足开始条件。（托盘项不直接开始，而是弹出说明框，告诉用户回到游戏后按 `calibrate` 热键；未绑定热键时提示去 INI 里设置。）
6. "计数异常，请重试"后状态怎么走没说。（Run1 不合法 → 回到 Idle；Run2 不合法 → 回到 Between，保留第 1 次的结果，再按热键重测第 2 次。）
7. Done 状态的去向没说。（Done 是瞬时状态：保存并弹出 toast 后立即回到 Idle。）
8. "校准列表 ▸ 可删除单项"的交互没说。（子菜单每项的文字为"删除 cs2.exe（每度 18.4 计数）"，点击后直接删除并弹 toast，不再二次确认。）
9. INI 里数值的精度没说。（写 3 位小数；读取时 counts_per_degree × 360 不在 [800, 400000] 内的条目会被忽略并记 warn。）
