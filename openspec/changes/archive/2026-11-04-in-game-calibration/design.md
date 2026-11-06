# Design

## Context

- 动机见 proposal.md 的 Why，行为契约见 `specs/turn-calibration` 及其他四个 delta。本变更建立在 `core-overlay` 的实现之上，用到它的这些接缝：
  - 单线程主循环与 `tick(dt)`（core-overlay design D1、D13）；
  - Raw Input 处理（D3）；
  - 热键动作表与"菜单 / 热键共用动作分发"（D11）；
  - `OverlayFrame::message` 字段与 D2D 文字层（D7）；
  - INI 模板重建中的"动态节"钩子（D9）；
  - 仅文字模式与帧键比较（D6、D8）。
- 约束与 core-overlay 相同：
  - 纯逻辑不含 Win32，才能放进 tests.exe；
  - 除 `src/app/main.cpp` 外不得定义入口函数；
  - 不用异常传递业务错误；
  - 中文写成 `L"…"`。

## Goals / Non-Goals

**Goals:**
- 校准逻辑（状态机、计数、合法性、一致性、超时、离开游戏、文案、结果换算）全部放在 `src/calib/calibration.*`，是纯类，可以完全由单元测试覆盖。
- app 层只做三件事：把事件转交给校准对象、把校准对象产出的效果（toast、保存、提示）落到对应模块、按 exe 查 `counts_per_degree`。
- 校准结果从下一个 tick 起生效，无需重启。

**Non-Goals:**
- 不记录校准日期（设置窗口变更 4 如需要再加字段）。
- 校准期间不抑制遮罩（见 Open Questions）。
- 不在托盘点击时直接开始校准（前台已不是游戏，见 proposal 待确认第 5 条）。

## Decisions

### D1. 纯逻辑的 `Calibration` 类

```cpp
// calib/calibration.h —— 纯逻辑，不含 Win32
enum class CalibState { Idle, Run1, Between, Run2, Done };
struct CalibContext { bool in_game = false; bool cursor_locked = false; std::wstring exe; };
struct CalibEffect {                          // 一次调用的全部副作用，由 app 执行
    std::wstring toast;                       // 空串 = 无
    std::optional<std::pair<std::wstring, double>> save;   // (exe, counts_per_degree)
};
class Calibration {
public:
    CalibEffect on_hotkey(double now_s, const CalibContext&);
    void        on_mouse_dx(long dx);                        // 只在 Run1/Run2 中累加
    CalibEffect update(double now_s, const CalibContext&);   // 超时 / 离开游戏，每个 tick 调用
    CalibState  state() const;
    long long   current_abs_count() const;                   // |Σdx|
    std::wstring message(std::wstring_view hotkey_text) const;   // Idle 时返回空串
    void        cancel();                                    // 重载配置或退出时使用，不产生 toast
};
bool         calib_count_valid(long long counts_360);              // [800, 400000]
bool         calib_runs_consistent(long long c1, long long c2);    // |c1−c2| / ((c1+c2)/2) ≤ 0.15
double       calib_counts_per_degree(long long c1, long long c2);  // ((c1+c2)/2) / 360
std::wstring calib_result_toast(double cpd);                       // "校准完成：每度 X 计数"
double       counts_per_degree_for(std::wstring_view exe,
                                   const std::map<std::wstring, double>& table,
                                   double fallback);               // 不区分大小写查找
```

- 为什么"返回效果对象"而不是让它自己调用 toast 或保存：保持纯逻辑，测试只需断言返回值。
- 时间用秒（double，来自 app 的 QPC 时钟），与 tick 用同一个时钟源；测试直接传入合成时间。
- Σdx 用 `long long`：400000 计数离 32 位上限很远，但用户可能在 Run 中挂着不动、累计很久，用 64 位最稳。
- `Done` 只在 `on_hotkey` 内部经过：结算后立刻置回 Idle。`state()` 在外部永远看不到 Done，但测试可以通过返回的 `save` 和 toast 断言结算发生过。
- 备选方案：把状态机放进 app.cpp。放弃，因为不可测，而且会让 app.cpp 继续膨胀。

### D2. 状态转移表

| 当前 | 事件 | 条件 | 下一状态 | 效果 |
|---|---|---|---|---|
| Idle | 热键 | `in_game && cursor_locked` | Run1 | 记录 target_exe；Σdx=0；t_enter=now |
| Idle | 热键 | 否则 | Idle | toast "请先在游戏里锁定视角再开始校准" |
| Run1 | 热键 | `calib_count_valid(abs(Σdx))` | Between | c1=abs(Σdx)；t_enter=now |
| Run1 | 热键 | 不合法 | Idle | toast "计数异常，请重试" |
| Between | 热键 | — | Run2 | Σdx=0；t_enter=now |
| Run2 | 热键 | 不合法 | Between | toast "计数异常，请重试"；保留 c1；t_enter=now |
| Run2 | 热键 | 合法且不一致 | Idle | toast "两次结果相差超过 15%，请重新校准" |
| Run2 | 热键 | 合法且一致 | Done→Idle | save(target_exe, cpd)；toast `calib_result_toast(cpd)` |
| Run1/Between/Run2 | update | `now − t_enter > 60` | Idle | toast "校准超时，已取消" |
| Run1/Between/Run2 | update 或热键 | `ctx.exe ≠ target_exe` | Idle | toast "已离开游戏，校准取消" |

- "离开游戏"按 exe 比较，不区分大小写。在热键路径上要先检查它，再执行其他转移。在 Between 和 Run 中，不要求每一刻都 `cursor_locked`（静止时 strict 规则可能短暂判为未锁定）。
- `update` 在每个 tick 都调用；只在非 Idle 时做检查，开销可以忽略。

### D3. 接入 core-overlay 的三个钩子

1. **WM_INPUT**：`raw_input` 增加一个可选回调 `std::function<void(long dx)> on_relative_dx`，在处理相对鼠标事件时（绝对坐标已被过滤）调用。app 把它接到 `calibration.on_mouse_dx`。回调在消息处理里同步执行，不经过 tick，所以计数不受 tick 频率影响，也不会漏。
2. **WM_HOTKEY**：动作表新增 `calibrate`（位于 `debug_bar` 之后），分发到 `app.on_calibrate_hotkey()`：用最新的 `GameContext` 组装 `CalibContext`（前台 exe、`in_game`、`cursor_locked`；`--force-game` 模式下 locked 也为真），调用 `on_hotkey`，再执行返回的效果。
3. **tick()**：在 GameDetector 采样之后、组装 `IntensityInputs` 之前：
   - `calibration.update(now, ctx)` → 执行效果；
   - `counts_per_degree = counts_per_degree_for(ctx.exe, cfg.calibration, cfg.trigger.default_counts_per_degree)`；
   - 组装 `OverlayFrame` 时，`frame.message = calibration.message(format_hotkey(当前 calibrate 绑定))`。
- 执行效果：`toast` 非空就交给 overlay 的 toast；`save` 有值时写入 `cfg.calibration[exe_lower] = cpd`，立即 `save_config`（core D9 的原子写，自身写入不会触发重载），并记一行 info `calib saved exe=<exe> c1=<n> c2=<n> cpd=<x>`。校准的每次状态转移都记一行 debug `calib state=<from>-><to> count=<n>`，用于手动验证。
- 重新加载配置时调用 `calibration.cancel()`：配置可能删除或改动了校准对象，而且重载期间热键会被重新注册。

### D4. overlay 的 message 渲染

- `OverlayFrame::message`（core 中已有该字段）纳入帧键：文字不同就 Present。计数每帧变化时，Present 频率等于 tick 频率（最多到刷新率），只在校准期间如此，可以接受。
- 渲染在 D2D 阶段，位于 toast 之后：
  - 用 `IDWriteFactory::CreateTextLayout`，按 `\n` 分行，`DWRITE_TEXT_ALIGNMENT_CENTER`，字号 18 DIP × DPI / 96，最大宽度为 overlay 宽度的 80%；
  - 取 `GetMetrics` 得到的包围盒，四周加 12 DIP 的内边距，画 alpha 0.6 的黑色圆角矩形（圆角半径 8 DIP），上沿位于 `0.22 × H`；
  - 白色文字。
- message 非空时，overlay 的可见性与 toast 一样，可以进入仅文字模式（core 的仅文字模式条件里加上 `!message.empty()`）。实际上开始条件已经要求 `in_game`，只有在 `enabled=0` 时才会走到仅文字模式。
- 活动模式条件里加上 `!message.empty()`，保证超时和计数刷新能被及时处理。

### D5. 持久化：`[calibration]` 动态节

- `Config` 增加 `std::map<std::wstring, double> calibration;`，键为小写 exe。用 `std::map` 是为了写回时天然按字母序。
- 加载：遇到 `[calibration]` 节中的每个键，转小写后用 `std::from_chars` 解析值。值 × 360 不在 [800, 400000] 内，或解析失败，就记 warn 并跳过。同名条目以后读到的为准。这个节的键 MUST NOT 被当作未知键送进 `[extra]`。
- 保存：用 core D9 的"动态节"钩子，在模板 `[calibration]` 节的注释块之后，逐条写出 `exe=cpd`。值用 `std::to_chars(fixed, 3)`。没有条目时只保留注释，从而保证 `save_config_text(Config{}, tmpl) == tmpl`。
- 模板 `src/res/MotionCalm.default.ini` 的改动：
  - `[hotkeys]` 中，在 `debug_bar` 之后插入 `calibrate=Ctrl+Alt+F7`，行尾注释为 `; 校准当前游戏（游戏内转一圈 ×2）`；
  - 在 `[hotkeys]` 之后新增 `[calibration]` 节，节标题的行尾注释为 `; <exe 小写>=<每度鼠标计数>，由"校准当前游戏"自动写入`，下面再加两行注释说明可以手动删除，以及合法范围约 2.222–1111.111。
- 旧 INI 兼容：缺少 `calibrate` 键时用默认值；缺少 `[calibration]` 节时视为空。下次保存时按新模板补齐，这是 core D9 的天然行为。

### D6. 托盘

- "校准当前游戏…"：`MessageBoxW(nullptr, text, L"MotionCalm 校准", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND)`，文字中插入当前绑定的规范字符串；绑定为 None 时换成另一段提示。信息框是模态的，但它不在 tick 路径上：MessageBox 内部有自己的消息循环，WM_INPUT 和定时器仍会分发。tick 由主循环驱动，在信息框打开期间暂停是可以接受的，因为此时前台是信息框，本来就不在游戏中。
- "校准列表 ▸"：每次弹菜单时，从 `cfg.calibration` 构建子菜单。命令 ID 从 `ID_CALIB_DELETE_BASE` 起连续分配，用一个本地的 `std::vector<std::wstring>` 把 ID 映射回 exe。点击后 `erase`、保存，并弹 toast "已删除校准：{exe}"。如果被删的正是当前校准对象，就同时 `calibration.cancel()`。

## Risks / Trade-offs

- [MessageBox 打开期间 tick 暂停] 菜单里点"校准当前游戏…"后，信息框关闭前主循环不跑 tick → overlay 已经隐藏（前台不是游戏）。toast 计时会延后，但信息框本身就是反馈，可以接受。如果以后发现问题，可以改成非模态，或者改用托盘气泡通知。
- [计数在消息处理中同步累加] 8 kHz 鼠标每秒会回调 8000 次，但每次只是一个整数加法，可以忽略。
- [前台 exe 比较过严] 有的游戏会在启动器和主程序之间切换 exe，校准中途切换就会被取消 → 只能在主程序里校准，这是合理的。
- [strict 锁定只在开始时检查] 开始后游戏若退出锁定（例如打开菜单），计数仍会累加菜单里的鼠标移动 → 用户需要自己保证转圈时没有打开菜单；两次 15% 规则能兜住大部分误操作。
- [inputsim 的计数精度] 手动验证依赖 inputsim 精确发送"秒数 × 速率"个计数；如有舍入，验收容差取 ±1%。

## Migration Plan

- 必须先归档 `core-overlay`：本变更对 `motion-trigger`、`tray-and-hotkeys`、`configuration` 的 MODIFIED 需要它们的主规格已存在。
- INI 向前兼容：旧文件缺少的键和节都按默认值处理，下次保存时自动补齐，不需要迁移脚本。回退到旧版本时，`[calibration]` 会被旧版当作未知节，按 `calibration.<exe>=<值>` 收进 `[extra]`，数据不会丢；但再次升级后，这些条目需要用户手动移回 `[calibration]`（README 说明）。

## Open Questions

- 校准进行中是否临时把遮罩强度上限压到 0，以免遮挡视线？中心清晰区已经覆盖准星附近，先不做；如果用户反馈转圈时看不清参照物，再加一个只在运行期有效的开关，不需要改接口或任务拆分。
