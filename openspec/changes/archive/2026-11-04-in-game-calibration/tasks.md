# Tasks

> 前提：`core-overlay` 已实现，`build.cmd all debug` 和 `build.cmd tests debug` 都是绿的。测试名统一加 `calib_` 前缀，这样可以用 `out\debug\tests.exe calib` 只跑本变更的测试。不要修改 `build.cmd`、`scripts/env.cmd`、`scripts/build_*.cmd`、`tests/test_main.cpp`、`tests/test_framework.h`、`tools/*`、`scripts/verify.ps1`。

## 1. 校准纯逻辑（calib）

- [x] 1.1 新建 `src/calib/calibration.h/.cpp`，文件头一行注释说明职责。内容：
  - `CalibState`、`CalibContext`、`CalibEffect`、`Calibration`；
  - 自由函数 `calib_count_valid`、`calib_runs_consistent`、`calib_counts_per_degree`、`calib_result_toast`、`counts_per_degree_for`（design D1）；
  - 状态转移严格按 design D2 的表实现：热键路径上先检查"离开游戏"；Done 只是瞬时状态；超时为每个状态 60 s；`cancel()` 不产生 toast；
  - `message()` 的文案与 turn-calibration spec 中"校准提示文案"完全一致，计数取 |Σdx| 的整数。

  验证：1.2 的测试全部通过。
- [x] 1.2 新建 `tests/test_calibration.cpp`，覆盖：
  - **状态机**
    - `calib_full_flow`：Idle→Run1→Between→Run2→Idle，返回 save(fakegame.exe, 18.2)；
    - `calib_start_requires_lock`：未在游戏中或未锁定时，保持 Idle 并返回开始失败的 toast；
    - `calib_esc_not_handled`：接口中不存在除热键外的推进方式，用编译期断言或注释说明；
  - **计数**
    - `calib_count_signed_sum`：+6552 / −6552 / +7000 后 −448 都得 6552；
    - `calib_count_only_in_run`：Idle 和 Between 中的 dx 不计入；
  - **合法性**
    - `calib_valid_bounds`：799 / 800 / 400000 / 400001；
    - `calib_run1_invalid_to_idle`；
    - `calib_run2_invalid_to_between_keeps_c1`；
  - **一致性**
    - `calib_inconsistent_6000_7000`：丢弃，返回 15% 的 toast，没有 save；
    - `calib_consistent_6000_6800`；
    - `calib_consistent_mean_base_6000_6950`：以平均为基准时通过；
  - **结果与文案**
    - `calib_cpd_average`：6552 和 6700 → 18.406±0.001；
    - `calib_toast_text`：18.2 → "校准完成：每度 18.2 计数"，36.4 → "校准完成：每度 36.4 计数"；
    - `calib_message_texts`：Run1 / Between / Run2 三种状态的文案，热键文字分别为 Ctrl+Alt+F7 和 Shift+F7；
  - **取消**
    - `calib_timeout_per_state`：60.1 s 超时；Run1 停 50 s 后 Between 再停 50 s 不超时；
    - `calib_leave_game_cancels`：在 Run1 和 Between 中 ctx.exe 变化都会取消，并返回对应 toast；
    - `calib_cancel_silent`；
  - **查表**
    - `calib_lookup_fallback_and_case`：CS2.exe 与 cs2.exe 视为同一项；查不到时回退到 fallback。

  验证：`build.cmd tests debug` 通过，`out\debug\tests.exe calib` 全部 PASS。

## 2. 配置与模板（config）

- [x] 2.1 修改 `src/config/config.h/.cpp`（design D5）：
  - `Config::calibration` 为 `std::map<std::wstring, double>`；
  - 加载 `[calibration]` 时把键转小写，值 × 360 不在 [800, 400000] 内或解析失败时记 warn 并跳过；这些键不得进入 `[extra]`；
  - 通过动态节钩子按字母序写回，值保留 3 位小数；
  - 在模式表的节顺序中，`calibration` 位于 `hotkeys` 之后、`extra` 之前。

  验证：2.3 的测试通过。
- [x] 2.2 修改 `src/res/MotionCalm.default.ini`（UTF-8 BOM、CRLF 保持不变）：
  - 在 `[hotkeys]` 的 `debug_bar` 之后插入 `calibrate=Ctrl+Alt+F7`，带行尾注释；
  - 在 `[hotkeys]` 之后新增只有注释、没有条目的 `[calibration]` 节（注释内容见 design D5）。

  验证：`ini_template_matches_defaults` 和 `ini_save_defaults_is_identity` 仍然通过。
- [x] 2.3 扩展 `tests/test_ini.cpp`：
  - `ini_calibration_read_lowercase`；
  - `ini_calibration_write_sorted_3dp`：cs2.exe=18.400 排在 valorant.exe=35.714 之前；
  - `ini_calibration_invalid_dropped`：abc / 1.0 / 2000 都被丢弃，产生 3 条 warning，保存后不再出现；
  - `ini_calibration_not_extra`；
  - `ini_old_ini_gains_calibration_section`：用 core-overlay 版本的模板文本作为输入（写在测试文件内），读后用新模板保存，结果中出现 `[calibration]` 和 `calibrate=Ctrl+Alt+F7`，其余值保持不变；
  - 更新 `ini_template_matches_defaults` 对 `[hotkeys]` 的期望（10 个键）。

  验证：`out\debug\tests.exe ini` 全部 PASS。

## 3. 热键动作表（tray/hotkeys）

- [x] 3.1 在 `src/tray/hotkeys.cpp` 的动作表中，于 `debug_bar` 之后插入 `{calibrate, L"calibrate", L"校准当前游戏", L"Ctrl+Alt+F7"}`，共 10 项。同步更新 `tests/test_hotkey_parse.cpp` 中的 `hotkey_action_table_defaults`：10 项，顺序与 tray-and-hotkeys delta 一致。验证：`out\debug\tests.exe hotkey` 全部 PASS。

## 4. 输入钩子与 app 接线（input / app）

- [x] 4.1 在 `src/input/raw_input.h/.cpp` 中增加可选回调 `on_relative_dx(long dx)`：只对相对鼠标事件调用，绝对坐标事件已被过滤。验证：编译通过；4.2 完成后，`--verbose` 日志中 Run1 期间 `calib` 的 debug 行计数会随 inputsim 增长。
- [x] 4.2 在 `src/app/app.h/.cpp` 中接入校准（design D3）：
  - 持有 `Calibration` 对象，把 `on_relative_dx` 接到 `on_mouse_dx`；
  - `calibrate` 热键 → `on_hotkey`，`CalibContext` 来自最新的 `GameContext`；
  - tick 中依次调用 `update`，用 `counts_per_degree_for` 换算鼠标角速度，填 `frame.message`；
  - 统一的"执行效果"函数：toast 交给 overlay；save 时写 `cfg.calibration` 并立即保存，记一行 info `calib saved ...`；
  - 每次状态转移记一行 debug `calib state=A->B count=n`；
  - 重载配置时调用 `calibration.cancel()`。

  验证：以 `--verbose --force-game` 在 fakegame 中按 Ctrl+Alt+F7，日志出现 `calib state=Idle->Run1`。
- [x] 4.3 修改 `src/overlay/overlay.h/.cpp` 与 `src/overlay/renderer.cpp`（design D4）：
  - 把 `message` 纳入帧键，并加入"仅文字模式"和"活动模式"的判定条件；
  - D2D 渲染多行提示：上沿位于 22% H，18 DIP，背景为 alpha 0.6 的圆角矩形，内边距 12 DIP，乘以 DPI / 96；
  - 提示绘制在 toast 之后，两者互不遮挡。

  验证：Run1 中屏幕中上方出现两行提示，"已转 N 计数"随鼠标实时增长；强档满强度时提示不变暗；同时出现 toast 时两者都可见。

## 5. 托盘（tray）

- [x] 5.1 在 `src/tray/tray.cpp` 中加入"校准当前游戏…"：位于"把当前程序加入排除名单"之后，热键列显示 calibrate 的绑定；点击后弹出置顶的说明信息框，未绑定时显示另一段提示（design D6）；不改变校准状态。验证：默认配置下点击，信息框中含"Ctrl+Alt+F7"；把 INI 改成 `calibrate=None` 并重载后再点，显示"请先在 MotionCalm.ini 的 [hotkeys] 中为 calibrate 设置快捷键"。
- [x] 5.2 在 `src/tray/tray.cpp` 中加入"校准列表 ▸"子菜单：按字母序列出"删除 {exe}（每度 {x.x} 计数）"，为空时显示灰色的"（无）"；点击后删除、保存、弹 toast，若删的是正在校准的对象则同时取消校准。验证：先在 INI 中手写 `cs2.exe=18.4` 和 `valorant.exe=35.714`，然后在菜单中删除 cs2.exe，INI 中只剩 valorant.exe，并出现 toast "已删除校准：cs2.exe"。

## 6. 构建与回归

- [x] 6.1 `build.cmd all debug` 和 `build.cmd all release` 都成功；`out\release\tests.exe` 退出码为 0，`calib_`、`ini_`、`hotkey_` 测试全部 PASS。
- [x] 6.2 回归：提醒用户约 20 s 内不要碰鼠标，然后运行 `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1`。结果应与 core-overlay 的验收一致：静止段为 0、转动段 > 0.8、停止后 0.6 s 内 < 0.05、CPU < 2%。fakegame.exe 没有校准条目时，必须仍使用 18.2；如果之前做过 7.x，先在托盘里删掉 fakegame.exe 的条目。

## 7. 手动验证：在 fakegame 中用已知计数跑两次

> 每个场景开始前都要提醒用户：开始后只按 Ctrl+Alt+F7，不要碰鼠标，也不要切换窗口。inputsim 只发送水平 dx；如果它的总计数与"秒数 × 速率"不完全相等，容差取 ±1%。

- [x] 7.1 准备：
  - 删除 `out\debug\MotionCalm.ini`（或备份），然后启动 `out\debug\MotionCalm.exe --verbose --force-game`；
  - 在同一个控制台里执行 `start "" out\debug\fakegame.exe --seconds 90 --mode lock`；
  - 再执行 `out\debug\inputsim.exe --profile "idle:6,turn:2@6552,idle:10,turn:2@6552,idle:6"`（每次转动共 13104 计数）。

  验证：fakegame 全屏并成为前台；overlay 显示中心点。
- [x] 7.2 正常流程，两次都是 13104：
  1. 在第一个 idle 段（约 t=2–5 s）按 Ctrl+Alt+F7，屏幕中上方出现"校准 1/2：…"；
  2. 第一次转动结束后（t≈8–12 s）按 Ctrl+Alt+F7，提示变为"第 1 次：13104 计数…"；
  3. t≈13–15 s 再按一次，进入"校准 2/2"；
  4. 第二次转动结束后（t≥20 s）按 Ctrl+Alt+F7。

  验证：
  - 出现 toast "校准完成：每度 36.4 计数"；
  - `out\debug\MotionCalm.ini` 的 `[calibration]` 中有 `fakegame.exe=36.400`（允许 36.036–36.764）；
  - 日志中有 `calib saved exe=fakegame.exe c1=13104 c2=13104 cpd=36.400`。
- [x] 7.3 校准值立即生效：fakegame 仍在前台时，运行 `out\debug\inputsim.exe --profile "idle:2,turn:2@8000,idle:2"`。验证：转动段的 `frame` 行中 mouse ≈ 219.8（允许 215–224），不再是 439.6；value 的峰值明显低于未校准时（未校准时约为 1.0，校准后约为 0.55）。
- [ ] 7.4 异常路径，每个场景都用新的 inputsim 运行： <!-- 未验证（2026-11-04）：异常路径（计数越界、两次相差 >15%、超时、取消）由 tests/test_calibration.cpp 覆盖，未用 inputsim 真机逐一跑 -->
  - 计数过少：profile `"idle:6,turn:1@500,idle:6"`，开始后在转动结束时按热键 → toast "计数异常，请重试"，状态回到 Idle；
  - 两次不一致：profile `"idle:6,turn:2@3276,idle:10,turn:2@3900,idle:6"`，即 6552 与 7800，相对差约 17.4% → toast "两次结果相差超过 15%，请重新校准"，INI 不变；
  - 超时：开始后什么都不做，等 61 s → toast "校准超时，已取消"；
  - 离开游戏：开始后按 Alt+Tab → toast "已离开游戏，校准取消"；
  - 开始条件：先切到记事本再按热键 → toast "请先在游戏里锁定视角再开始校准"。

  验证：每个场景的 toast 与 INI 状态都符合 spec，日志的 `calib state=` 行记录了相应的转移。
- [ ] 7.5 删除与回退：在托盘"校准列表 ▸"中点"删除 fakegame.exe（每度 36.4 计数）"。验证： <!-- 未验证（2026-11-04）：删除改由设置窗口的校准页完成（settings-window 变更），托盘删除路径未真机点击 -->
  - 出现 toast "已删除校准：fakegame.exe"；INI 的 `[calibration]` 中只剩注释；
  - 再跑一次 `turn:2@8000`，`frame` 行的 mouse 回到 ≈ 439.6；
  - 最后恢复 7.1 中备份的 INI。
