# Design

## Context

- 动机见 proposal.md 的 Why。DESIGN.md 是权威设计；本文只写实现 `core-overlay` 必须定下来、而 DESIGN.md 没写细或容易做错的地方。行为契约（数值、文案、INI 键）以 `specs/` 为准，本文不重复大表。
- 这是全新项目：仓库里已有构建与测试基础设施（`build.cmd`、`scripts/env.cmd` 导出 `MC_ROOT/MC_CONFIG/MC_CFLAGS/MC_LDFLAGS/MC_LIBS/MC_OUT/MC_OBJ`；`scripts/build_tests.cmd` 把 `tests/*.cpp` 和除 `src/app/main.cpp` 外的全部 `src/**/*.cpp` 编进 `out\<cfg>\tests.exe` 并运行；`tests/test_framework.h` 提供 `TEST(name)`、`CHECK`、`CHECK_EQ`、`CHECK_NEAR`、`CHECK_TRUE`、`CHECK_FALSE`；`tests.exe <子串>` 可按测试名过滤）。工具 `fakegame`、`inputsim`、`verify.ps1` 由并行任务提供。
- **硬约束**：
  - 除 `src/app/main.cpp` 外，任何 `.cpp` 都 MUST NOT 定义 `main` / `wWinMain`，也 MUST NOT 含有构造时有副作用的全局对象，否则 tests.exe 链接失败或行为异常；
  - 纯逻辑必须与 Win32 采样分开，才能在 tests.exe 里测；
  - 源码 UTF-8，编译用 `/utf-8`，中文字符串一律写成 `L"…"`；
  - 不用异常传递业务错误：初始化失败返回 `bool` 并写日志。

## Goals / Non-Goals

**Goals:**
- 单线程、事件驱动、无忙等的主循环；空闲时几乎零 CPU。
- 每个可测的规则（强度、阈值、门控、游戏判定、INI、热键解析）都是不含 Win32 句柄的纯函数或纯类。
- 模块接缝遵循 DESIGN.md §3.3，变更 2–4 只需"往里加"：`OverlayFrame::message`、`IntensityInputs::capture_deg_per_s`、`counts_per_degree` 查表、热键动作表追加、模板追加节。

**Non-Goals:**
- 不做截屏线程（变更 3）、校准状态机（变更 2）、设置窗口（变更 4）。
- 不做 GPU 降采样或任何读取游戏画面的操作。
- 不做"光标锁定"锁存、hCursor==NULL 判定等启发式增强（见 Open Questions）。

## Decisions

### D1. 主循环：MsgWaitForMultipleObjectsEx + 两种节拍

- 循环每轮等待：`MsgWaitForMultipleObjectsEx(n, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE)`，然后用 `PeekMessage` 排空消息（WM_INPUT 在窗口过程里处理），最后在"到期"时调用一次 `tick()`。消息本身 MUST NOT 触发 tick，否则 1–8 kHz 的鼠标会把 tick 打成千次每秒。
- **活动模式**（overlay 可见，并且 value > 0、target > 0、有 toast、调试条开着或帧为脏）：
  - 上一个 tick Present 过 → 等 swap chain 的 frame-latency waitable 句柄（`SetMaximumFrameLatency(1)`）；
  - 上一个 tick 没 Present → **不能再等 latency 句柄**（它会一直处于触发状态，变成忙等），改等一个高精度 waitable timer（`CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`，不可用时退回普通 waitable timer），周期 = 1 / 显示器刷新率（取自 `EnumDisplaySettingsW` 的 `dmDisplayFrequency`，取不到按 60 Hz）。
- **空闲模式**：timeout = 距下一次 30 ms 轮询的剩余时间，不 Present。不调用 `timeBeginPeriod`（它会改全系统的计时器精度）。
- 句柄集合里始终有配置目录的变更通知事件（D9）。
- `dt` 用 `QueryPerformanceCounter` 计算，截断到 [0, 0.1] s，防止睡眠或断点后的跳变。
- 备选：独立渲染线程 + 固定 `Sleep(16)`。放弃，因为它与消息线程的数据需要同步、空闲时也在空转，而且 Sleep 精度差。

### D2. 宿主窗口：隐藏的普通顶层窗口，而不是 HWND_MESSAGE

- 类名 `MotionCalm.Host`，`WS_POPUP`，从不显示。它负责 Raw Input 目标、`RegisterHotKey`、托盘回调、单实例通知和 `TaskbarCreated` 消息。
- 不用 message-only 窗口的原因：`TrackPopupMenu` 要先 `SetForegroundWindow(owner)`，而 message-only 窗口不能成为前台；`TaskbarCreated` 只广播给顶层窗口；第二个实例用 `FindWindowW(L"MotionCalm.Host", nullptr)` 也只能找到顶层窗口。
- overlay 窗口单独一个（类名 `MotionCalm.Overlay`），只负责显示。

### D3. Raw Input

- 注册：`RAWINPUTDEVICE{0x01, 0x02, RIDEV_INPUTSINK, host}` 和 `{0x01, 0x06, RIDEV_INPUTSINK, host}`。
- WM_INPUT：用 `GetRawInputData(RID_INPUT)` 读到栈上的对齐缓冲里；处理完 MUST 调用 `DefWindowProc`（wParam 为 RIM_INPUT 时系统要靠它做清理）。
- 鼠标：`usFlags & MOUSE_MOVE_ABSOLUTE` 直接忽略；否则把 `(qpc_now, lLastX, lLastY)` 推入两个窗口：
  - 速度窗口（`velocity_window_ms`）：用于 §4.1 的 `|Σ(dx,dy)| / window`；
  - 路径窗口（100 ms）：用于锁定判定的 `raw_path_100ms`（累加 √(dx²+dy²)）。
  - 两者都是 `model/intensity.h` 中的纯类 `VelocityWindow`（按时间戳淘汰，容量上限 8192 个事件，超出时丢最旧的），在 tests 里用合成事件测。
- 键盘：按 `VKey` 跟踪 `'W' 'A' 'S' 'D' VK_UP VK_DOWN VK_LEFT VK_RIGHT`；`Flags & RI_KEY_BREAK` 表示抬起；忽略 `VKey == 0xFF`。任何键盘事件都会刷新"最后键盘时间"。前台变化时，或 `now − 最后键盘时间 ≥ 5 s` 时，清空按下状态（§5）。
- 时间戳取处理时刻的 QPC（`GetMessageTime` 只有毫秒精度且会回绕）。
- 如果 8 kHz 鼠标导致 CPU 超过目标，可在 WM_INPUT 中先读当前包，再用 `GetRawInputBuffer` 批量排空；这是可选优化，先测再说。

### D4. XInput

- 每个 tick 对 0–3 号手柄调用 `XInputGetState`（链接 `xinput9_1_0`）。返回 `ERROR_DEVICE_NOT_CONNECTED` 的槽位，此后每 2 s 才重新探测一次（对空槽轮询有可观的开销）。
- 摇杆幅值和死区重映射用纯函数 `apply_radial_deadzone(x, y, dz) → m'`，放在 `model/intensity.h`，便于测试。

### D5. 游戏判定：采样与规则分离

- `GameDetector::poll()` 只负责采集一个 `GameSample`（纯数据），规则由纯函数 `evaluate_game(sample, lists, lock_rule, force_game_mode) → GameContext` 和 `compute_gates(ctx, enabled, trigger_flags) → Gates` 计算，二者都在 `detect/game_detect.h` 中，由 `tests/test_game_rules.cpp` 覆盖。
- 采集要点：
  - `GetForegroundWindow` → `GetWindowThreadProcessId` → 按 pid 缓存的 exe（`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW`，取文件名转小写）；查询失败时用 `GetClassNameW` 的小写；
  - 窗口矩形用 `GetWindowRect`，显示器用 `MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)` + `GetMonitorInfoW().rcMonitor`；
  - 光标用 `GetCursorInfo`（`flags & CURSOR_SHOWING`）和 `GetClipCursor`，虚拟屏幕用 `GetSystemMetrics(SM_X/Y/CX/CYVIRTUALSCREEN)`；
  - 光标位置 `GetCursorPos` 每次采样存入一个约 200 ms 的历史环，`cursor_disp_100ms` = |当前位置 − 历史中最接近"100 ms 前"的那个样本|。
- 采样频率：在 tick 内执行，但距上次采样不足 16 ms 时跳过（144 Hz 节拍下也最多 60 Hz）。前台事件回调触发时立即强制采样一次。
- "当前程序"（托盘和标记动作使用）：每次采样时，如果前台既不属于本进程、类名也不在 {`Shell_TrayWnd`, `Shell_SecondaryTrayWnd`, `NotifyIconOverflowWindow`, `TopLevelWindowForOverflowXamlIsland`} 中，就记下它的 exe、最近一次 `in_game`，以及本次前台期间是否曾锁定（状态行要用）。
- `known_games` 追加：由 app 层在 `ctx.cursor_locked && !ctx.excluded && !known && !force_game_mode` 时执行，之后立即保存。

### D6. Overlay 窗口

- 样式：`WS_POPUP`；扩展样式 `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`。创建后 MUST 调用一次 `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)`，否则 layered 窗口不会显示。`WM_NCHITTEST` 返回 `HTTRANSPARENT`，`WM_MOUSEACTIVATE` 返回 `MA_NOACTIVATE`，作为双保险。
- 显示与隐藏：需要显示时，先把首帧渲染并 Present，再 `ShowWindow(SW_SHOWNOACTIVATE)`，避免闪出上一帧的旧画面；不需要显示时用 `ShowWindow(SW_HIDE)`，让游戏在非活动期间不被合成（见风险中的独立翻转）。
- 显示器跟随 `follow_monitor(HMONITOR)`：取 `rcMonitor`（物理像素，因为进程是 PMv2），调用 `SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)`；尺寸变化时 `ResizeBuffers`（见 D7）；用 `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` 更新点和字号的缩放；标记帧为脏。仅文字模式使用前台窗口所在的显示器。
- `WM_DPICHANGED`：**不**采用 lParam 给出的建议矩形，直接重新调用 `follow_monitor`。`WM_DISPLAYCHANGE`：重新跟随当前显示器。
- 置顶维持：`SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, cb, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)`。回调里依次：`SetWindowPos(HWND_TOPMOST, 0,0,0,0, SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE)`、`follow_monitor(MonitorFromWindow(fg))`、清空键盘状态、强制采样。另外 tick 里距上次重新置顶 ≥ 1 s 时再做一次。
- 截屏排除：`SetWindowDisplayAffinity(hwnd, excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE)`。SDK 没有该宏时自行定义 `WDA_EXCLUDEFROMCAPTURE 0x00000011`。返回 FALSE 时只记 warn。生效值 = `always` → 排除；`never` → 不排除；`auto` → `capture_detection_active`（本变更恒为 false，`--no-capture` 也强制为 false）。

### D7. 渲染器

- 设备：`D3D11CreateDevice(nullptr, HARDWARE, ..., D3D11_CREATE_DEVICE_BGRA_SUPPORT [| DEBUG，仅 debug 构建且 SDK Layers 可用时])`，特性级别依次尝试 11_0、10_1、10_0。
- 交换链：`IDXGIFactory2::CreateSwapChainForComposition`，参数为：
  - `Width/Height` = 显示器像素，`B8G8R8A8_UNORM`，`SampleDesc{1,0}`，`BUFFER_USAGE_RENDER_TARGET_OUTPUT`；
  - `BufferCount=2`，`SCALING_STRETCH`，`SWAP_EFFECT_FLIP_SEQUENTIAL`，`ALPHA_MODE_PREMULTIPLIED`；
  - `Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`。
  - 之后 `IDXGISwapChain2::SetMaximumFrameLatency(1)`，并取得 `GetFrameLatencyWaitableObject()`（析构时 `CloseHandle`）。
- DComp：`DCompositionCreateDevice(dxgiDevice)` → `CreateTargetForHwnd(hwnd, TRUE)` → `CreateVisual` → `visual->SetContent(swapChain)` → `target->SetRoot(visual)` → `Commit()`。每帧只 Present，不需要重新 Commit。
- `ResizeBuffers` 的顺序（容易出错）：
  1. `d2dCtx->SetTarget(nullptr)`，释放 D2D 目标位图和 RTV；
  2. `ctx->ClearState(); ctx->Flush()`；
  3. `ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)`——flags 必须与创建时一致；
  4. 重新创建 RTV 和 D2D 目标位图。
- 着色器：HLSL 以字符串形式放在 `overlay/shaders.h`，运行时用 `D3DCompile` 编译（`vs_4_0` / `ps_4_0`，release 用 `D3DCOMPILE_OPTIMIZATION_LEVEL3`，debug 用 `D3DCOMPILE_DEBUG`）。顶点着色器用 `SV_VertexID` 生成全屏三角形，`Draw(3, 0)`，不需要顶点缓冲。像素着色器一次算出最终的预乘颜色并直接覆盖写入，所以不需要混合状态，也不需要 Clear。
- 常量缓冲（16 字节对齐）：`size(W,H)`、`radii(rx,ry)`、`R`、`feather`、`intensity`、`max_opacity`、`mask_rgb`、中心点（半径 px、描边 px、颜色、开关、描边开关）、边缘点（半径 px、内缩 px、颜色、alpha、开关）。rx、ry、R 在 CPU 上算好；所有 px 量都已乘以 DPI / 96。
- 像素合成顺序：遮罩（rgb = mask_rgb·a）→ 边缘点 over → 中心点描边环 over → 中心点 over。`over` 运算为 `out = src_pm + dst_pm·(1 − src_a)`。点的覆盖率按 `saturate(r + 0.5 − dist)` 做 1 px 抗锯齿；描边环取 `[r, r + outline]` 区间的覆盖率。像素中心用 `SV_Position.xy`（已在 .5 处），p = pos − (W/2, H/2)。
- 文字层：`D2D1CreateFactory(SINGLE_THREADED)` 得到 `ID2D1Factory1` → `CreateDevice(dxgiDevice)` → `CreateDeviceContext` → 用后台缓冲的 `IDXGISurface` 调用 `CreateBitmapFromDxgiSurface`（`TARGET | CANNOT_DRAW`，`B8G8R8A8_UNORM` + `PREMULTIPLIED`，DPI 96）。D3D 的 Draw 完成后在同一个后台缓冲上 `BeginDraw` → 画 toast 或调试条 → `EndDraw` → `Present(1, 0)`。必须 `SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE)`，因为透明目标上不能用 ClearType。DirectWrite 字体用 "Microsoft YaHei UI"，toast 20 DIP、调试条 14 DIP，再乘以 DPI / 96。
- 设备丢失：`Present` 或 `EndDraw` 返回 `DXGI_ERROR_DEVICE_REMOVED`、`DXGI_ERROR_DEVICE_RESET` 或 `D2DERR_RECREATE_TARGET` 时，销毁并重建 D3D、DComp、D2D 的全部对象；失败则每 2 s 重试。
- GPU 计时（只在 `--verbose` 下）：用 `D3D11_QUERY_TIMESTAMP_DISJOINT` 加两个 `TIMESTAMP` 查询包住 D3D 与 D2D 的绘制；结果延迟两帧读取，避免等待 GPU。每 5 s 写一行 debug：`gpu frames=<n> avg_ms=<x> max_ms=<y>`。

### D8. Present 策略与帧差异

- `OverlayFrame` 加上派生出的 `MaskStyle` / `DotStyle` / 尺寸 / DPI 组成"帧键"，与上一次 Present 的帧键逐字段比较，完全相同就跳过。强度直接比较浮点值——模型的 0.001 吸附（spec：非对称平滑）保证有限步内到达 0，不会永远有微小变化。
- toast 的透明度由 `toast_age_s` 推出，属于帧键的一部分。toast 只在淡出的那 0.3 s 内逐帧变化，此前 1.2 s 只在出现时 Present 一次——但为了让 toast 准时消失，这期间要保持活动模式的节拍。
- 进入隐藏状态前不需要再画透明帧：窗口已经隐藏，下次显示时会先渲染再显示（D6）。

### D9. 配置与 INI

- 分两层：
  - `config/ini.h`：纯 UTF-8 文本层——`parse_ini(text)` 得到有序的 (节, 键, 原始值, 行号) 列表；`regenerate(template_text, lookup, extras)` 按模板逐行替换值。
  - `config/config.h`：带类型的 `Config` 结构（默认值写在成员初始化器里）、`load_config(text, &warnings)`、`save_config_text(cfg, template_text)`，以及只做文件 I/O 的 `load_file` / `save_file_atomic`。
  - 模板文本由调用方传入：app 从 RCDATA 资源 `IDR_DEFAULT_INI` 读取（`FindResourceW` / `LoadResource` / `LockResource`），tests 从磁盘 `src/res/MotionCalm.default.ini` 读取（按 `__FILE__` 推出的路径，build_tests.cmd 传给编译器的是绝对路径）。
- 行尾注释与列表键的规则见 spec（INI 语法）。实现上，解析器保留原始值，由 config 层按键的类型决定是否剥离行尾注释。这样 `ini.h` 不需要知道模式。
- 重建：对模板里的每个 `key=value` 行，定位值所在的区间（`=` 之后，到行尾注释起点之前，去掉右侧空白），替换为新值。如果原来有行尾注释，新值之后用空格补齐到原注释所在的列（至少一个空格）。BOM 与换行风格沿用模板。最后，若有未知键，追加 `[extra]` 节。为变更 2 预留"动态节"钩子：模板中某个节（例如 `[calibration]`）的键不固定时，在该节的注释块之后按字母序展开条目。
- 数值格式：`std::to_chars` 取最短往返形式，再补零到"模式里登记的最少小数位"（等于默认值字面量的小数位）；解析用 `std::from_chars`。两者都与区域设置无关。
- 原子写：写 `<path>.tmp` → `FlushFileBuffers` → `MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`。
- 监视：对配置所在目录调用 `ReadDirectoryChangesW`（OVERLAPPED + 事件句柄，事件放进 D1 的等待集合），过滤条件为 `FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME`，只关心文件名等于配置文件名（不区分大小写）的记录。收到后把"重载截止时间"设为 now + 500 ms，再次收到就顺延。到期时读出文件，与"最后一次自己写入或读取的内容"比较哈希，相同则跳过，这样自己的保存不会触发重载。
- 默认值的单一来源：成员初始化器与模板必须一致，由测试保证（模板解析结果 == `Config{}`，且 `save_config_text(Config{}, tmpl) == tmpl`）。
- 第 1 层模式表（每个键的节、名称、类型、范围、最少小数位）是一个静态数组，供解析、校验、写回共用；热键键名从 `tray/hotkeys.h` 的动作表生成。

### D10. 日志

- `util/log`：一个 `std::mutex` 保护的单例（变更 3 会有第二个线程写日志）。用 `CreateFileW(..., FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, ..., OPEN_ALWAYS)` 打开，每行写完直接 `WriteFile`，不额外缓冲，也不在每行后 flush 到磁盘。
- 轮转：写之前检查累计大小 ≥ 1 MB → 关闭句柄 → 删除 `.log.1` → 把 `.log` 改名为 `.log.1` → 重新打开。
- 行格式和 `frame` / `exit` 行见 spec（日志文件、调试帧日志）。`exit` 行的 CPU 来自 `GetProcessTimes`（内核 + 用户），墙钟来自 QPC。
- debug 级别下，每 5 s 额外写一行 `perf ticks=<n> avg_tick_ms=<x> max_tick_ms=<y> presents=<n>`，统计这 5 s 内的 tick 次数、tick 函数耗时（QPC）和 Present 次数。用于 tasks 8.2 / 9.3 的验证，不属于 verify.ps1 的解析契约。
- **契约**：`scripts/verify.ps1` 由并行任务编写，必须按 spec 中的 `frame` 行格式解析。判定规则：
  - "转动结束时刻" = 最后一行 `mouse ≥ v_start` 的 t；
  - "淡出时间" = 此后第一行 `value < 0.050` 的 t 减去转动结束时刻。

### D11. 托盘与热键

- 托盘：`NOTIFYICONDATAW`，`uID=1`，`uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP`，`uCallbackMessage = WM_APP_TRAY`，`NIM_ADD` 之后 `NIM_SETVERSION(NOTIFYICON_VERSION_4)`。版本 4 下 `LOWORD(lParam)` 是事件：`WM_CONTEXTMENU`、`NIN_SELECT`、`NIN_KEYSELECT` 都弹出菜单，菜单位置取 `GET_X_LPARAM(wParam)` / `GET_Y_LPARAM(wParam)`。
- 弹菜单：每次重新构建 `HMENU`（状态行、勾选状态、热键文字都是动态的）→ `SetForegroundWindow(host)` → `TrackPopupMenuEx(TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY)` → `PostMessage(host, WM_NULL)` → 按返回的命令 ID 分发。菜单项文字的热键列用 `\t` 分隔。
- `TaskbarCreated`：用 `RegisterWindowMessageW(L"TaskbarCreated")` 取得消息号，收到后重新 `NIM_ADD`。
- 热键：动作表（`tray/hotkeys.h` 中的 `constexpr` 数组：id、INI 键、中文名、默认字符串）同时是 INI `[hotkeys]` 的模式。注册时用 `RegisterHotKey(host, id, mods | MOD_NOREPEAT, vk)`，id = 动作序号 + 1。先在解析阶段检测重复（按 mods + vk），按表序第一个生效。`parse_hotkey(std::wstring_view) → expected<Hotkey, wstring 错误>`（C++20 没有 `std::expected`，用小 struct 代替）与 `format_hotkey(Hotkey)` 都是纯函数。
- toast 文案集中放在 `app.cpp` 的一个小表里，与 spec 保持一致。

### D12. 开机自启与单实例

- 自启：`RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE)`；为 1 时，若值不等于 `"\"" + GetModuleFileNameW + "\""` 就写入（`REG_SZ`）；为 0 时删除 `MotionCalm` 值（不存在也算成功）。
- 单实例：`wWinMain` 最开始（日志初始化之前）`CreateMutexW(nullptr, FALSE, L"Local\\MotionCalm.Instance")`。若 `GetLastError() == ERROR_ALREADY_EXISTS`，就 `FindWindowW(L"MotionCalm.Host", nullptr)` → `PostMessageW(hwnd, WM_APP_ALREADY_RUNNING, 0, 0)` → `return 0`。互斥量句柄保持到进程结束。

### D13. 启动顺序与 tick 内容

```
wWinMain: 单实例 → 解析命令行 → 打开日志（写 start 行）→ 加载或生成 INI
        → 创建 host 窗口 → 注册 Raw Input → overlay.init（窗口 + D3D/DComp/D2D）
        → 托盘 → 热键 → WinEvent 钩子（out-of-context，不是注入式钩子）→ 目录监视
        → 自启同步 → 截屏可见性 → 主循环 → 有序退出（写 exit 行）
tick(dt): 读 XInput → GameDetector 采样（≤60 Hz）→ known_games 追加 / 保存
        → 组装 IntensityInputs（counts_per_degree = default）→ model.update
        → 组装 OverlayFrame（toast 计时、调试文本）→ overlay 显示 / 隐藏 / 跟随 / 置顶兜底
        → render（按需 Present）→ frame 日志 → --exit-after 检查 → 决定下轮的等待方式
```

### D14. 构建与资源

- `scripts/build_app.cmd`：`call "%~dp0env.cmd"`；对每个 `src` 子目录分别调用一次 `cl %MC_CFLAGS% /I"%MC_ROOT%\src" /MP /c`（与 build_tests 一样按目录分开放 obj，避免同名文件冲突），obj 输出到 `%MC_OBJ%\app\<dir>\`；`rc /nologo /c65001 /I"%MC_ROOT%\src\res" /fo"%MC_OBJ%\app\app.res" src\res\app.rc`；`link %MC_LDFLAGS% /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"%MC_OUT%\MotionCalm.exe" ... app.res %MC_LIBS%`。保持文件只含 ASCII，退出码反映成败。
- 必须加 `/MANIFEST:NO`：manifest 已由 rc 以 `RT_MANIFEST` id 1 嵌入。不加的话，链接器会另外生成一份外部 `.manifest`；若改用 `/MANIFEST:EMBED`，还会与 rc 嵌入的那份冲突，报重复资源错误 CVT1100。
- `app.rc` 首行写 `#pragma code_page(65001)`，以便版本信息中的中文字符串按 UTF-8 编译。它包含：
  - `IDI_APP ICON "icon.ico"`；
  - `1 RT_MANIFEST "app.manifest"`；
  - `IDR_DEFAULT_INI RCDATA "MotionCalm.default.ini"`；
  - `VERSIONINFO`，FILEVERSION 1,0,0,0，ProductName "MotionCalm"，FileDescription "MotionCalm 防晕遮罩"。
- `app.manifest`：
  - `<dpiAwareness>PerMonitorV2, PerMonitor</dpiAwareness>` 加 `<dpiAware>true/pm</dpiAware>`；
  - Common-Controls v6 依赖；
  - `supportedOS` 写 Win10 的 GUID；
  - `requestedExecutionLevel level="asInvoker"`；
  - `<activeCodePage>UTF-8</activeCodePage>`。
- `MotionCalm.default.ini` 以 UTF-8 BOM + CRLF 保存。`.gitattributes` 已对 `*.rc` 设了 CRLF，`*.ini` 需要补一条 `*.ini text eol=crlf`（tasks 1.x）。

## Risks / Trade-offs

- [独立翻转失效，合成多一帧延迟] 置顶 overlay 盖在游戏上时，DWM 可能无法让游戏走独立翻转 → 不在游戏时 `SW_HIDE`；只在内容变化时 Present；交换链只有 2 个缓冲、最大延迟 1。这是 DESIGN 的取舍，延迟敏感的用户可以关掉遮罩。
- [置顶互抢] 游戏或其他 overlay 也在反复设置 TOPMOST → 只在前台变化时和每 1 s 重新置顶，不做高频抢夺，宁可偶尔被盖住 1 s。
- [strict 锁定在"每帧回中、不限制光标"的游戏里抖动] 采样时机可能落在"游戏还没把光标拉回中心"的瞬间，导致 `cursor_disp_100ms ≥ 3 px` → 多数游戏同时会限制光标（走 clipped 分支）；剩下的用 `mouse_lock_rule=hidden`。调试条可以直接看到锁定状态。
- [Raw Input 被 UIPI 拦截] 游戏以管理员身份运行而本程序没有时，可能收不到 WM_INPUT 和热键 → 已列为已知限制，README 说明。如有需要，后续可以在日志里提示"前台进程权限更高"。
- [光标用 SetCursor(NULL) 隐藏导致漏判] 这种游戏的 `CURSOR_SHOWING` 仍为真 → 首版严格按 DESIGN 实现，实测发现后再考虑加上 `hCursor == NULL` 条件（见 Open Questions）。
- [8 kHz 鼠标的消息风暴] 每秒数千条 WM_INPUT → tick 与消息解耦（D1）；必要时用 `GetRawInputBuffer` 批量读取（D3）。
- [INI 编码] 记事本"另存为 ANSI"后中文注释会乱码 → 解析器只依赖 ASCII 的键和值，中文只出现在注释里，所以乱码不影响取值；下一次保存会按模板恢复注释。
- [exe 目录不可写]（例如放进了 Program Files）→ 按 spec 降级：用默认值运行、不保存、记 warn；README 建议放在用户目录。
- [仅文字模式会短暂出现在非游戏程序上方] 只在 toast 期间（≤ 1.5 s）或用户主动开启调试条时出现，可以接受。

## Migration Plan

全新组件，没有迁移。回退就是删除 exe 目录。注意：若开启过开机自启，删除前应先在托盘中取消勾选，否则 `HKCU\...\Run\MotionCalm` 会留下一个失效的值（README 说明）。

## Open Questions

- strict 锁定是否需要"前台期间锁存"（锁过一次后，只要光标保持隐藏就一直视为锁定）以减少状态抖动？不影响强度计算（静止时鼠标项本来就是 0），只影响状态行和调试条，可以实测后再定。
- 是否把 `hCursor == NULL` 也算作 `cursor_hidden`？需要在实际游戏里验证误判和漏判，不影响接口与任务拆分。
- 是否需要 `GetRawInputBuffer` 批量读取：以 1 kHz / 8 kHz 鼠标实测的 CPU 为准。
