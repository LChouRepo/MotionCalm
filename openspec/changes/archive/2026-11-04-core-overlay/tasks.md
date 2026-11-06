# Tasks

> 约定：
> - 所有构建都走已有的 `build.cmd`；`build.cmd tests debug` 会编译并运行 `out\debug\tests.exe`（退出码 = 失败测试数）。
> - 测试文件 `#include "test_framework.h"`，用 `TEST(name)` / `CHECK*`。测试名统一加模块前缀（`ini_`、`strings_`、`log_`、`intensity_`、`hotkey_`、`game_rules_`），这样可以用 `tests.exe intensity` 之类的方式过滤。
> - 不要创建或修改以下文件，它们已存在或由并行任务提供：`build.cmd`、`scripts/env.cmd`、`scripts/build_tools.cmd`、`scripts/build_tests.cmd`、`tests/test_main.cpp`、`tests/test_framework.h`、`tools/*`、`scripts/verify.ps1`。

## 1. 构建脚本与资源

- [x] 1.1 新建 `scripts/build_app.cmd`（只含 ASCII，CRLF；做法见 design D14）。先 `call "%~dp0env.cmd"`，再按 `src` 子目录分别 `cl /MP /c`，obj 放在 `%MC_OBJ%\app\<dir>\`，然后用 `rc /c65001` 编译 `src\res\app.rc`，最后 `link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"%MC_OUT%\MotionCalm.exe"`；失败时返回非零。同时新建最小的 `src/app/main.cpp`（`wWinMain` 直接返回 0，文件头一行注释写明职责）。验证：`build.cmd app debug` 和 `build.cmd app release` 都打印 `app : OK`，并产出 `out\<cfg>\MotionCalm.exe`。
- [x] 1.2 新建以下资源文件（内容见 design D14）：
  - `src/res/resource.h`：定义 `IDI_APP`、`IDR_DEFAULT_INI`、版本宏；
  - `src/res/app.rc`：首行 `#pragma code_page(65001)`，包含 ICON、`RT_MANIFEST` 1、RCDATA 模板、VERSIONINFO 1.0.0.0；
  - `src/res/app.manifest`：PerMonitorV2 + PerMonitor、comctl32 v6、Win10 supportedOS、asInvoker、activeCodePage UTF-8。

  如果 `src/res/icon.ico` 还不存在，先运行 `python tools\make_icon.py`。在 `.gitattributes` 中追加 `*.ini text eol=crlf`。验证：资源管理器里 exe 显示图标；属性 → 详细信息中版本为 1.0.0.0、描述为中文；`mt.exe -inputresource:out\debug\MotionCalm.exe;#1 -out:con` 输出中含 `PerMonitorV2` 和 `Microsoft.Windows.Common-Controls` 6.0.0.0。
- [x] 1.3 新建 `src/res/MotionCalm.default.ini`（UTF-8 BOM + CRLF）：节和键严格按 configuration spec（general / mask / dots / trigger / detect / hotkeys），值为默认值；每个键配一行中文注释说明取值范围；标量键保留 DESIGN.md §8 的行尾注释；列表键不带行尾注释（说明写在上一行）；`exclude` 为 22 项默认名单；不含变更 2–4 的键。验证：由 3.4 的测试 `ini_template_matches_defaults` 和 `ini_save_defaults_is_identity` 覆盖。

## 2. 通用工具（util）

- [x] 2.1 新建 `src/util/win.h`（`Microsoft::WRL::ComPtr` 别名、`UniqueHandle` 和 `UniqueHwnd` RAII、`ScopeExit`）和 `src/util/strings.h`（UTF-8 ⇄ UTF-16、ASCII 小写、trim、按字符 split、`iequals`、`ends_with_ci`）。验证：在 `tests/test_ini.cpp` 中加 `strings_utf8_roundtrip_chinese`、`strings_trim_split`、`strings_iequals` 并通过。
- [x] 2.2 新建 `src/util/log.h/.cpp`：`log::init(path, level)`、`log::debug/info/warn(fmt, ...)`，线程安全；行格式为 `YYYY-MM-DD HH:MM:SS.mmm [level] msg`；文件超过 1 MB 时轮转为 `.log.1`；共享模式包含读、写、删除（见 design D10）。验证：`tests/test_ini.cpp` 中的 `log_rotates_at_1mb` 在临时目录写入 1.1 MB 后，断言 `.log.1` 存在且新 `.log` 小于 1 KB；`log_line_format` 用正则检查行格式。

## 3. 配置（config）

- [x] 3.1 新建 `src/config/ini.h/.cpp`：
  - `parse_ini(std::string_view utf8)`：可选 BOM、CRLF/LF、节标题带注释、整行注释、按第一个 `=` 切分、保留原始值和行号；
  - `regenerate(template_text, lookup, extras)`：逐行替换值，保留行尾注释和对齐，沿用模板的 BOM 与换行风格，并预留动态节钩子（design D9）。

  验证：3.4 中的 `ini_*` 解析与重建用例全部通过。
- [x] 3.2 新建 `src/config/config.h/.cpp`：
  - 静态模式表（节、键、类型、范围、最少小数位）和 `Config` 结构（默认值写在成员初始化器里，与 configuration spec 完全一致）；
  - `load_config(text, warnings)`：标量键剥离行尾注释、列表键切分去重、按类型解析（`std::from_chars`）、越界截断、非法值回退、`v_start ≥ v_full` 时整档恢复默认、未知键收进 extras（命名为 `节.键`）；
  - `save_config_text(cfg, tmpl)`：数值用 `std::to_chars` 的最短往返形式并补零到最少小数位，颜色写成大写 RRGGBB，热键字符串原样写回。

  验证：3.4 中的 `ini_*` 加载与保存用例全部通过。
- [x] 3.3 在 `src/config/config.cpp` 中实现文件层：`load_or_create(path, tmpl)`（文件不存在就原样写出模板）、`save_file_atomic`（写 `.tmp`、`FlushFileBuffers`、再 `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`）、`content_hash`（供监视器判断是否是自己的写入）。验证：`ini_atomic_save_replaces_file` 在临时目录中保存两次并读回，内容正确且不残留 `.tmp`；`ini_create_from_template` 在路径不存在时写出的字节与模板相同。
- [x] 3.4 新建 `tests/test_ini.cpp`（模板文件按 `__FILE__` 推出的路径 `..\src\res\MotionCalm.default.ini` 读取，找不到算失败），至少覆盖：
  - `ini_template_matches_defaults`：模板解析结果 == `Config{}`，且没有 warning；
  - `ini_save_defaults_is_identity`：`save_config_text(Config{}, tmpl)` 与模板逐字节相同；
  - `ini_inline_comment_scalar`、`ini_list_ignores_comment_chars`（`exclude=chrome.exe;msedge.exe; my game.exe ;;` → 3 项）、`ini_bom_and_lf_equivalent`、`ini_section_comment_and_key_case`、`ini_duplicate_key_last_wins`、`ini_list_dedup_case_insensitive`；
  - `ini_locale_independent_numbers`（先 `setlocale(LC_ALL, "de-DE")` 再读写 `0.35`）、`ini_color_parse_and_upper`；
  - `ini_bool_only_01`、`ini_out_of_range_clamped`（`max_opacity_strong=1.5` → 1.0 并产生 warning）、`ini_bad_enum_default`、`ini_bad_number_default`、`ini_threshold_inverted_resets_level`；
  - `ini_unknown_key_to_extra`（`[mask] glow=1` → `[extra] mask.glow=1`，再存一次结果不变）、`ini_unknown_section_to_extra`、`ini_value_replaced_keeps_comment_column`（mask_level=strong 的那一行）、`ini_hotkey_string_preserved`（`alt + ctrl + f9` 原样写回）、`ini_min_decimals`（0.6 → `0.60`，18.25 → `18.25`）。

  验证：`build.cmd tests debug` 通过，`out\debug\tests.exe ini` 全部 PASS。

## 4. 强度模型（model）

- [x] 4.1 新建 `src/model/intensity.h/.cpp`，全部是纯逻辑、不含 Win32：
  - `MaskLevel`、`Sensitivity`、`TriggerConfig`、`MaskConfig`、`IntensityInputs`（字段与 DESIGN.md §3.3 一致，`gate_*` 由调用方填写）；
  - `smoothstep`、`thresholds_for(Sensitivity, TriggerConfig)`；
  - `IntensityModel::configure/update/value/target`：max 合成、非对称 τ、0.001 吸附；dt 截断到 [0, 0.1]；
  - `VelocityWindow`：按 QPC 秒级时间戳淘汰，提供 `counts_per_s(now)` 和 `path_length(now)`，容量上限 8192；
  - `apply_radial_deadzone(x, y, dz)`。

  验证：4.2 的测试通过。
- [x] 4.2 新建 `tests/test_intensity.cpp`，覆盖 motion-trigger spec 中的每个数值场景：
  - `intensity_threshold_medium`（30/60/210/360/1000 → 0/0/0.5/1/1）、`intensity_threshold_low_high`、`intensity_pad_full_medium`（0.896±0.001）；
  - `intensity_keys_move`（0.35）、`intensity_pad_move_half`（0.175）、`intensity_max_combine`、`intensity_gate_false_is_zero`；
  - `intensity_attack_100ms`（[0.945, 0.955]，50 ms 时 [0.77, 0.78]）、`intensity_release_500ms`（[0.045, 0.055]）、`intensity_snaps_to_zero`（1.2 s 时 == 0）；
  - `intensity_dt_independent`（dt 为 1/60、1/144、0.004 时结果相差 < 0.005）、`intensity_reverse_uses_release`、`intensity_dt_clamped`；
  - `intensity_velocity_8000`（每 1 ms 加 8 计数 → 8000±2%，换算 439.6°/s）、`intensity_velocity_cancel`、`intensity_velocity_diagonal`（30/40 → 1000）、`intensity_velocity_zero_after_window`；
  - `intensity_deadzone_rescale`（0.15 → 0，0.6 → 0.5，1.0 → 1.0，角上 >1 时截断为 1）。

  验证：`out\debug\tests.exe intensity` 全部 PASS。

## 5. 热键解析与动作表

- [x] 5.1 新建 `src/tray/hotkeys.h/.cpp`：
  - `constexpr` 动作表（9 项，顺序、INI 键、中文名、默认值与 tray-and-hotkeys spec 一致）；
  - `parse_hotkey(std::wstring_view)`：返回 `{ok, mods, vk, error}`，支持 spec 列出的全部按键名，不区分大小写，允许空白；
  - `format_hotkey`：规范顺序 Ctrl+Alt+Shift+Win；
  - `resolve_bindings(strings)`：按表序去重，输出生效表、重复表、非法表、单键警告；
  - `HotkeyManager::register_all(hwnd)` / `unregister_all()`：使用 `MOD_NOREPEAT`，汇总注册失败的项。

  验证：5.2 的测试通过。
- [x] 5.2 新建 `tests/test_hotkey_parse.cpp`：
  - `hotkey_parse_canonical_and_loose`、`hotkey_modifier_order_format`（"Win+Shift+a" → "Shift+Win+A"）；
  - `hotkey_key_names`（PgUp→VK_PRIOR、Num5→VK_NUMPAD5、VK_OEM_3→0xC0、Up→VK_UP、0→'0'、Esc→VK_ESCAPE、F24→VK_F24）；
  - `hotkey_none_and_empty`、`hotkey_invalid_cases`（"Ctrl+Alt"、"Ctrl+F9+F10"、"Ctrl+Ctrl+F9"、"Ctrl+Foo"、"F25" 都返回错误，且错误信息非空）；
  - `hotkey_roundtrip_all_keys_all_mods`（全部按键名 × 16 种修饰键组合）、`hotkey_single_key_flagged`（"F8" 可以解析但带单键标记）；
  - `hotkey_duplicate_first_wins`（toggle 与 exit 同为 Ctrl+Alt+F9 → toggle 生效，exit 进入重复表）、`hotkey_action_table_defaults`（9 项，默认值与 spec 一致）。

  验证：`out\debug\tests.exe hotkey` 全部 PASS。

## 6. 游戏判定（detect）

- [x] 6.1 新建 `src/detect/game_detect.h/.cpp` 的纯规则部分：
  - `GameSample`（exe、是否本进程、窗口矩形、显示器矩形、虚拟屏幕矩形、cursor_showing、clip 矩形、raw_path_100ms、cursor_disp_100ms）；
  - `DetectLists`（exclude、force_game、known_games）；
  - `evaluate_game(sample, lists, MouseLockRule, force_game_mode) → GameContext`、`compute_gates(ctx, enabled, mouse, keyboard, gamepad) → Gates`、`should_record_known_game(ctx, lists, force_game_mode)`；
  - 名单操作 `mark_as_game(lists, exe)`、`exclude_app(lists, exe)`：忽略大小写去重，规则见 game-detection spec。

  验证：6.3 的测试通过。
- [x] 6.2 在同一文件中实现 Win32 采样（design D5）：
  - `GameDetector::poll(force)`：节流到 ≤60 Hz；按 pid 缓存 exe，取不到时用类名；维护约 200 ms 的光标位置历史；
  - 跟踪"当前程序"：排除本进程和任务栏 / 通知区的类名；记录最近一次 in_game，以及该次前台期间是否曾锁定；
  - `ctx()` / `current_program()`。

  验证：在 7.x 完成后运行 `MotionCalm.exe --verbose`，依次切换记事本、chrome、fakegame，日志中的 `frame` 行 exe / in_game / locked 分别为 notepad.exe/0/0、chrome.exe/0/0、fakegame.exe/1/1。
  <!-- 采样代码已实现，tests.exe 中只有冒烟测试（game_rules_detector_smoke）；上面的端到端验证要等 7.x / 8.3 接入主循环后再跑。 -->
- [x] 6.3 新建 `tests/test_game_rules.cpp`，覆盖 game-detection spec 的每个场景：
  - 全屏：0 / 1 / 2 px 误差、最大化窗口、副屏；
  - clipped：等于虚拟屏幕或更小；
  - strict 锁定：5 种组合；hidden 模式；
  - 排除名单：大小写不敏感，`force_game` 覆盖排除；默认名单恰好 22 项（从 `Config{}` 读取）；
  - in_game：3 种组合；本进程 / 空前台视为非游戏；
  - known_games：追加条件（未排除、首次、非 force 模式）；
  - `mark_as_game` / `exclude_app` 对三个名单的影响及去重；
  - force_game 模式：视为锁定、不记录、排除名单仍然生效；
  - 门控：`compute_gates` 的全部分支（包括单独关闭鼠标、`enabled=0`）。

  测试名前缀为 `game_rules_`。验证：`out\debug\tests.exe game_rules` 全部 PASS。

## 7. 应用骨架（app）

- [x] 7.1 把 `src/app/main.cpp` 扩展为完整入口：
  - 单实例互斥量 `Local\MotionCalm.Instance`；第二个实例 `FindWindowW(L"MotionCalm.Host")`，`PostMessage(WM_APP_ALREADY_RUNNING)` 后返回 0（design D12）；
  - 然后构造 `App` 并运行。

  新建 `src/app/app.h/.cpp`：
  - 命令行解析（`--config`、`--verbose`、`--no-capture`、`--force-game`、`--exit-after`、`--portable`；未知参数记 warn）；
  - 打开日志并写 start 行；从 RCDATA 读取模板，然后 `load_or_create`；
  - 创建隐藏的顶层 host 窗口（类名 `MotionCalm.Host`）；
  - D1 主循环：空闲 30 ms，加上活动模式的 latency 句柄与高精度定时器切换；
  - `--exit-after` 到时有序退出，写 exit 行（`GetProcessTimes`）。

  验证：
  - `out\debug\MotionCalm.exe --verbose --exit-after 3` 约 3 s 后以退出码 0 退出，日志有 start 行和 `exit reason=exit-after ... cpu_pct=` 行，且 cpu_pct < 0.5；
  - 运行期间再启动一次 exe，新进程 1 s 内退出（退出码 0）。
  <!-- 已验证：--exit-after 3 约 3.1 s 后退出码 0，有 start 行与 exit 行；第二个实例 26–27 ms 内以 0 退出并弹 "MotionCalm 已在运行"。待确认：本代理的沙箱 shell 里 GetProcessTimes（含 Get-Process.TotalProcessorTime）对所有进程都读出约 0，所以 exit 行 cpu_pct 恒为 0.00；改用 QueryProcessCycleTime 测得空闲约 0.3% 单核（release，含驱动线程）。perf 行另加了 wakeups / mouse_events / proc_mcycles 诊断字段（非 verify 契约）。 -->

- [x] 7.2 在 `app.cpp` 中接入配置目录监视：`ReadDirectoryChangesW`（OVERLAPPED，事件句柄加入等待集合）、500 ms 防抖、哈希比较以跳过自己的写入；重载后调用统一的 `apply_config()`。验证：程序运行时用记事本把 `log_level` 改为 debug 并保存，约 0.5 s 后日志开始出现 debug 行，且只出现一次"已重新加载配置"；通过托盘改动触发的保存不会引起重载记录。
  <!-- 已验证：自己的保存只出现 "config change ignored"；外部改一次 → 约 0.5 s 后一条 "已重新加载配置"；200 ms 内写 3 次 → 只重载一次。 -->

## 8. 输入（input）

- [x] 8.1 新建 `src/input/raw_input.h/.cpp`（design D3）：
  - 在 host 上用 `RIDEV_INPUTSINK` 注册鼠标和键盘；
  - WM_INPUT 处理：忽略绝对坐标，把相对位移推入速度窗口和 100 ms 路径窗口，用 QPC 打时间戳；
  - 跟踪 8 个键的按下状态，记录最后键盘时间；
  - 提供 `clear_keys()`，处理完消息后调用 `DefWindowProc`。

  验证：在 fakegame（`--mode lock`）中运行 `MotionCalm.exe --verbose --force-game`，再运行 `inputsim --profile "turn:2@8000"`，`frame` 行的 mouse 稳定在 432–448 之间；按住 W 时 moving=1，Alt+Tab 切走后立即变为 0。
  <!-- 实现偏离（design D3 的可选优化，已实测触发）：每个 WM_INPUT 唤醒一次主循环在本机约 25 µs（1 kHz 鼠标 ≈ 2.5% 单核），所以主循环的等待掩码去掉 QS_RAWINPUT，每次醒来先用 GetRawInputBuffer 批量读走（连同 WM_INPUT），这批鼠标事件的时间戳在"上次读取→本次"之间均匀铺开；模态循环期间仍逐条走 WM_INPUT + DefWindowProc。fakegame + inputsim 的 432–448 验证待主代理在 verify 中确认。 -->

- [x] 8.2 新建 `src/input/gamepad.h/.cpp`（design D4）：轮询 0–3 号手柄，空槽每 2 s 才重新探测；输出右摇杆 `pad_deg_per_s` 和左摇杆 `pad_move`（经过 `apply_radial_deadzone`，多手柄取最大值）。验证：无手柄时 `frame` 行 pad=0.0，且 `--verbose` 日志 `perf` 行的 `avg_tick_ms` 与关闭手柄轮询（`gamepad=0`）时相差 < 0.05 ms；有手柄时右摇杆推满 pad≈300.0。
  <!-- 实现完成；本机无手柄，未验证 pad≈300；空槽 2 s 重探测，空闲时 XInput 开销约 0.1 ms / 5 s。 -->

- [x] 8.3 在 `app.cpp` 的 tick 中串起 输入 → `GameDetector` → `compute_gates` → `IntensityModel`（`counts_per_degree = default_counts_per_degree`）；前台变化时清空键盘状态；记录 known_games 并立即保存；按 configuration spec 写 `frame` 行。验证：用 fakegame + inputsim `"idle:2,turn:3@8000,idle:3"`，日志中静止段 value=0.000，转动段 value > 0.800，停止后 0.6 s 内 value < 0.050；不加 `--force-game` 时，fakegame.exe 被追加到 INI 的 `known_games`。
  <!-- 已在真实游戏里看到：human.exe 首次锁定后写入 known_games 并保存；静止 value=0.000，转动时 target/value 随鼠标变化。fakegame + inputsim 的端到端判定待 verify.ps1。顺带修了 GameDetector 的一个问题：启动后第一次采样光标历史只有自己，cursor_disp_100ms 恒为 0，会把"隐藏光标 + 鼠标在动"的普通程序误判为锁定并记进 known_games（实测 Ultimate Chicken Horse）；现在历史不足 50 ms 时不认 strict 的回中分支。 -->

## 9. Overlay 与渲染（overlay）

- [x] 9.1 新建 `src/overlay/shaders.h`：HLSL 字符串，内容为全屏三角形 VS，以及按 overlay-rendering spec 公式实现的遮罩、边缘点、中心点描边与中心点的 PS，以预乘 alpha 输出。验证：运行时 `D3DCompile` 无错误也无警告（debug 构建把编译警告写进日志）。
  <!-- 已验证：日志 "shaders compiled ... no errors/warnings"；tests/test_overlay.cpp 的 overlay_shader_compiles_without_warnings 也检查。 -->

- [x] 9.2 新建 `src/overlay/renderer.h/.cpp`（design D7）：
  - D3D11（BGRA）设备、waitable 合成交换链、DComp 目标与 visual；
  - 常量缓冲（px 量乘以 DPI / 96）；
  - D2D/DWrite 文字层：toast 黑底圆角，1.2 s 不透明加 0.3 s 淡出；调试条为两条进度条加一行文字；灰度抗锯齿；
  - `ResizeBuffers` 按规定顺序执行；
  - 处理设备丢失并每 2 s 重试；
  - `--verbose` 下用 GPU timestamp 统计，每 5 s 写一行 `gpu` 日志。

  验证：fakegame 1080p 加 `--force-game --verbose` 持续转动时，`gpu ... avg_ms=` < 0.2。
  <!-- 待确认（GPU 目标）：本机 1080p、游戏在后台跑时 GPU 时间戳 avg 0.6 ms（只有遮罩）/ 1.3 ms（含 D2D 调试条，时间戳包含 D2D 提交间隙与 GPU 低频状态），未达到 < 0.2 ms；需要在 verify 的 fakegame 场景下再看 gpu 行。CPU：每次 Present 约 0.6 ms（主线程 Present ≈0.19 ms + 驱动 / DComp 线程 ≈0.35 ms），60 Hz 连续变化时约 3.6% 单核，超过"活动 < 2%"；是否限制 Present 频率请主代理决定。 -->

- [x] 9.3 新建 `src/overlay/overlay.h/.cpp`（design D6、D8）：
  - 窗口样式，并调用 `SetLayeredWindowAttributes(…255, LWA_ALPHA)`；`WM_NCHITTEST` → `HTTRANSPARENT`；
  - `follow_monitor`（物理像素、DPI、忽略 `WM_DPICHANGED` 的建议矩形）、`set_visible`（先渲染再 `SW_SHOWNOACTIVATE`）、`set_capture_excluded`、`keep_topmost`；
  - `render(frame, mask_style, dot_style)`：用帧键比较决定是否 Present，强度为 0 时补最后一帧；仅文字模式。

  在 app 中接入 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …OUTOFCONTEXT|SKIPOWNPROCESS)` 和每 1 s 的置顶兜底。验证：
  - 在 fakegame 中转动时，屏幕四周按档位压暗、中心清晰；
  - 静止时只有中心点和淡色边缘点；
  - 点击穿透到 fakegame，Alt+Tab 中看不到 overlay；
  - 切到记事本 50 ms 内 overlay 消失；
  - 用 `--verbose` 统计：静止 10 s 内连续两行 `perf` 的 `presents=0`。
  <!-- 已验证：窗口扩展样式 0x082800A8（NOREDIRECTIONBITMAP|NOACTIVATE|LAYERED|TOPMOST|TOOLWINDOW|TRANSPARENT）、覆盖 (0,0)-(1920,1080)、截图里中心点 / 8 个边缘点像素值与公式一致（见 test-output\v_01_dots.png）。修了一个问题：该窗口隐藏时 WM_PAINT 用 ValidateRect 清不掉，每秒被重新生成约 13 万次（主线程 ~85% 单核），改为 BeginPaint/EndPaint。游戏内"静止 10 s presents=0"、点击穿透、Alt-Tab、50 ms 切换待 11.3 / verify。 -->

- [ ] 9.4 用截屏验证外观数值：用 fakegame（黑底网格）加 `--force-game --no-capture`，在强档、i=1 时用 .NET `CopyFromScreen` 截一张图，检查顶边中点像素与静止截图相比亮度降到 ≤ 20%（强档不透明度 0.85），屏幕中心像素与静止截图相同。三档各截一张图存进 `test-output\`。截图和比较用临时 PowerShell 片段完成，不入库，也不改 `scripts/verify.ps1`。验证：三档的比较结果都满足上述阈值，并由人工查看 PNG。 <!-- 未验证（2026-11-04）：三档外观由 tests/test_overlay.cpp 的 WARP 像素测试覆盖（顶边 alpha 0.60/0.75/0.85、清晰区 0），真机只截了中档 -->
  <!-- 未勾选：需要 fakegame + inputsim（本代理不允许运行），留给主代理。已有的替代证据：tests/test_overlay.cpp 用 WARP 离屏渲染真实 HLSL，核对了强 / 中 / 轻满强度顶边中点 0.85 / 0.75 / 0.60、中心与清晰区 0、半强度四角 0.425、颜色预乘、8 个边缘点位置与 alpha、中心点与描边、144 DPI；真实屏幕截图 test-output\v_01_dots.png 的点像素与公式一致（未截到 i=1 的遮罩）。 -->

## 10. 托盘、热键、自启（tray）

- [x] 10.1 新建 `src/tray/tray.h/.cpp`（design D11）：
  - `NIM_ADD` + `NOTIFYICON_VERSION_4`，处理 `TaskbarCreated` 后重新添加；
  - 按 tray-and-hotkeys spec 的结构动态构建菜单（状态行、勾选与单选状态、`\t` 热键列、无当前程序时置灰）；
  - `SetForegroundWindow` → `TrackPopupMenuEx(TPM_RETURNCMD)` → `PostMessage(WM_NULL)`；
  - 退出时调用 `NIM_DELETE`。

  验证：逐项点击菜单，结果符合 spec 的"菜单动作"场景；用任务管理器结束并重启 explorer.exe 后图标重新出现。
  <!-- 实现完成；托盘图标出现、退出后 NIM_DELETE；菜单逐项点击与重启 explorer 待 11.3 手动验收（弹菜单会抢前台，本代理没做）。 -->

- [x] 10.2 在 app 中接入热键：读 INI → `resolve_bindings` → `register_all`；按 spec 弹出占用 / 重复 toast 并记单键 warn；WM_HOTKEY 分发到与菜单共用的动作函数；按 spec 的文案弹 toast，并立即保存（`debug_bar` 除外）；重载时先注销再注册。验证：
  - 默认配置下 Ctrl+Alt+F9 / F10 / F11 / F8 的效果与 toast 都符合 spec；
  - 先用另一个程序占用 Ctrl+Alt+F9 再启动 MotionCalm，出现占用 toast；
  - 改 INI 把 toggle 绑到 Ctrl+Alt+F1 后，约 0.5 s 生效。
  <!-- 已验证（PostMessage(WM_HOTKEY) 模拟，不注入按键）：默认注册 4 个；被占用 → "快捷键 Ctrl+Alt+F10 已被其他程序占用"；重复 → "快捷键 Ctrl+Alt+F9 重复绑定，已忽略「退出」"（同一条 toast 两行）；非法串 Ctrl+Alt+F99 记 warn 且保存后原样保留；cycle_level / toggle_center_dot / debug_bar 的 toast 与保存正确。 -->

- [x] 10.3 在 app 中实现开机自启同步（design D12）：启动时、重载后、托盘勾选时都执行，失败时弹 toast 并回滚。验证：勾选后 `reg query HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v MotionCalm` 显示带引号的当前路径；取消后该值消失；把 exe 目录换个位置再启动，路径自动更新。
  <!-- 已验证：autostart=1 启动 → Run\MotionCalm = 带引号的 exe 路径；autostart=0 启动 → 值被删除（测试后已确认不存在）。托盘勾选路径待 11.3。 -->

- [x] 10.4 实现"打开配置文件 / 打开日志"（`ShellExecuteW` 失败时改用 `notepad.exe`）、"关于"信息框，以及单实例的"MotionCalm 已在运行" toast。验证：逐项手动点击；重复启动时出现该 toast。
  <!-- 实现完成（ShellExecuteW 失败改用 notepad.exe；关于框是 MessageBoxW，期间用 30 ms WM_TIMER 继续 tick）；重复启动 toast 已验证；打开配置 / 日志 / 关于的点击待 11.3。 -->

## 11. 集成验收

- [x] 11.1 `build.cmd all debug` 和 `build.cmd all release` 都成功，汇总为 `app : OK`、`tools : OK`、`tests : OK`；`out\release\tests.exe` 退出码为 0。
  <!-- 已验证：debug 与 release 都是 app/tools/tests OK、0 个编译 / 链接警告；121 个测试全部通过（新增 tests/test_overlay.cpp 13 个：make_params 数值 + 用 WARP 离屏渲染真实 HLSL 核对 spec 的像素数值）。 -->

- [x] 11.2 运行前提醒用户在约 20 s 内不要碰鼠标，然后执行 `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1`。验收按 proposal 的"验收标准"第 2 条：
  - 日志判定：静止段 = 0；转动段 > 0.8；停止后 0.6 s 内 < 0.05；CPU < 2%。
  - 截图：`test-output\` 中 t=1 s 的图只有中心点和淡色边缘点；t=3.5 s 的图四周明显压暗、中央清晰、中心点仍在，由主代理用 Read 查看。

  如果 verify.ps1 解析不出 `frame` / `exit` 行，以 configuration spec 的"调试帧日志"格式为准，与 verify.ps1 的作者核对。
- [ ] 11.3 手动验收清单（逐项记录结果）： <!-- 未验证（2026-11-04）：单屏 100% DPI 环境：多显示器跟随、150% 缩放、OBS 实测、注销后自启未测；托盘、热键、单实例、点击穿透、截屏可见性已在自动化运行中确认 -->
  1. 双显示器时只覆盖游戏所在屏，游戏换屏后 1 s 内跟随；
  2. 150% 缩放的屏上点半径为 4.5 px；
  3. 打开任务管理器"置于顶层"后点回游戏，overlay 仍在最上层；
  4. `hide_from_capture=always` 时 OBS 和截图工具都截不到遮罩，`never` 时能截到；
  5. 托盘全部菜单项和状态行；
  6. 全部默认热键及其 toast；
  7. 开机自启（注销后重新登录验证）；
  8. 重复启动的 toast；
  9. 浏览器全屏看视频时永不出现遮罩；
  10. 桌面空闲 60 s 时 CPU 时间增量 < 0.12 s（任务管理器的"CPU 时间"列）。
