# MotionCalm 总设计（v1）

> 本文件是全项目的"共同理解"。OpenSpec 各变更的 proposal / design / specs / tasks 都以本文为准；
> 实现时如需偏离，先改本文（并说明原因），再改代码。

## 0. 一句话

一个通用的 Windows 外部 overlay：根据玩家的鼠标 / 键盘 / 手柄输入（以及可选的截屏运动估计）判断
"镜头正在转动 / 画面正在晃动"，按程度把屏幕边缘平滑压暗，同时常驻一个固定的中心参照点，
用来缓解第一人称游戏的 3D 眩晕。**不注入游戏进程、不读游戏内存、不装钩子。**

目标用户：用户的朋友。玩任何第一人称游戏半小时就晕，主诉"荧幕晃动感太严重"。主要玩带反作弊的多人 FPS。

## 1. 范围

### v1 做（按 OpenSpec 变更拆分，依次实施）

| # | 变更名 | 内容 |
|---|---|---|
| 1 | `core-overlay` | D3D11 + DirectComposition 点击穿透 overlay；鼠标 / 键盘 / 手柄输入检测；"是否在游戏里"判定与排除名单；强度模型；遮罩与参照点渲染；托盘菜单；全局热键（可自定义，INI 里改）；INI 配置；日志；单实例；多显示器跟随 |
| 2 | `in-game-calibration` | 游戏内水平转一整圈 ×2 的校准；按游戏 exe 保存"每度鼠标计数"；阈值统一用 °/s |
| 3 | `capture-motion-detection` | 桌面复制（DXGI Desktop Duplication）+ 投影互相关估计全局位移；与输入检测取最大值；开启时 overlay 对截屏隐藏 |
| 4 | `settings-window` | 完整设置窗口（6 个分页，含快捷键分页），改动即时生效，预览强度滑块 |

### v1 不做

ReShade 版、校准方案 (b) 瞄准练习场 / (c) 舒适度测试、代码签名、自动更新、任何形式的注入 / 内存读取 / 低级钩子、云同步。

## 2. 已定的产品决策（来自设计问答）

| 主题 | 决定 |
|---|---|
| 触发源 | 鼠标原始输入；WASD + 方向键；XInput 手柄（右摇杆 = 转视角，左摇杆 = 移动）；截屏运动（变更 3）。跳、蹲不算 |
| 鼠标什么时候算"在转视角" | **只有光标被锁住时**：光标隐藏且位置不随鼠标移动，或光标被 ClipCursor 限制在窗口内 |
| 键盘 / 截屏什么时候算 | 只在"已知游戏"里算（这个 exe 曾经出现过光标锁定，或用户手动标记为游戏） |
| 手柄什么时候算 | 前台窗口全屏或光标隐藏即可 |
| 排除名单 | 浏览器、视频播放器等永远不触发；托盘可增删 |
| 遮罩外观 | 纯黑；偏圆的椭圆；边缘柔和；强档：最深 85% 不透明、满强度时中间留出 ≈ 屏幕高度 55% 的清晰区 |
| 遮罩速度 | 约 0.1 s 出现，约 0.5 s 淡出 |
| 遮罩档位 | 轻 / 中 / 强 |
| 参照物 | 中心白点（半径 3 px，深色描边）常驻；屏幕 4 角 + 4 边中点共 8 个淡色点，遮罩越强越明显。**不做**会移动的点 |
| 灵敏度 | 触发灵敏度 低 / 中 / 高；默认按 800 DPI 估算；校准后用 °/s |
| 校准 | 变更 2 方案 (a)：游戏里转一圈，测两次取平均，按 exe 保存 |
| 操作 | 托盘菜单 + 可自定义全局热键 + INI；变更 4 加完整设置窗口；变更 6 起界面文字有简体中文 / 繁體中文（台湾用语）/ English 三种语言（默认跟随 Windows 显示语言，日志与 INI 注释不翻译） |
| 录屏可见性 | 截屏检测开启时 overlay 对截屏 / 录屏 / 共享隐藏（否则会截到自己）；关闭时可见。截屏检测默认开 → 默认不可见 |
| 多显示器 | 只盖游戏窗口所在显示器，跟着游戏窗口换屏 |
| 开机自启 | 默认关，托盘可勾 |
| 技术栈 | C++20，Win32 + D3D11 + DirectComposition + Direct2D/DirectWrite（文字），MSVC，零第三方依赖 |
| 交付 | 免安装：`MotionCalm.exe` + `MotionCalm.ini`，打 zip；不签名 |
| 测试 | 单元测试 exe + 假游戏窗口 + 模拟鼠标 + 截图的集成验证脚本，在用户机器上跑 |

## 3. 架构

### 3.1 进程与线程

- 单 exe、单进程。
- **主线程**：消息循环（WM_INPUT、WM_HOTKEY、托盘消息、WinEvent 回调、设置窗口）+ 固定节拍 `tick()`。
  tick 里依次：采样输入 → 更新游戏判定 → 更新强度模型 → 组装 `OverlayFrame` → 渲染（仅在有变化时 Present）。
- **截屏线程**（变更 3）：独立线程跑 Desktop Duplication 与位移估计，把 `capture_deg_per_s`（double）以原子方式交给主线程。
- 节拍：overlay 可见且强度 > 0 时按显示器刷新率节拍（建议用 waitable swap chain，`SetMaximumFrameLatency(1)` + 等待 latency 对象）；
  空闲（不在游戏里，或强度 == 0 且参照点没变）时 30 ms 轮询一次、不 Present。
  **性能目标**：活动时 CPU < 2%（单核占比），空闲时 ≈ 0%，GPU 每帧 < 0.2 ms（1080p）。禁止忙等。

### 3.2 模块与目录

```
MotionCalm/
  build.cmd                  # 入口：build.cmd [all|app|tools|tests] [debug|release]
  scripts/
    env.cmd                  # 调 vcvars64.bat，设公共编译 / 链接参数（见 §13）
    build_app.cmd            # 编 MotionCalm.exe（变更 1 负责）
    build_tools.cmd          # 编 tools/*（测试工具代理负责）
    build_tests.cmd          # 编并运行 tests.exe
    verify.ps1               # 集成验证：起假游戏 → 模拟鼠标 → 截图 → 读日志
  src/
    app/        main.cpp, app.h/.cpp          # 装配、消息循环、tick、命令行参数、单实例
    config/     config.h/.cpp, ini.h/.cpp     # 配置结构、默认值、UTF-8 INI 读写（保留注释模板）
    input/      raw_input.h/.cpp              # Raw Input 鼠标 + 键盘（RIDEV_INPUTSINK，无钩子）
                gamepad.h/.cpp                # XInput 轮询
    detect/     game_detect.h/.cpp            # 前台窗口 / exe / 全屏 / 光标状态 / 锁定判定 / 排除名单 / 已知游戏
    model/      intensity.h/.cpp              # 纯逻辑：输入 → 目标强度 → 平滑强度（可单元测试）
    overlay/    overlay.h/.cpp                # 窗口、显示器跟随、置顶维持、WDA_EXCLUDEFROMCAPTURE
                renderer.h/.cpp               # D3D11 + DComp + 全屏 PS（遮罩 + 点）+ D2D/DWrite 文字层
                shaders.h                     # HLSL 以字符串嵌入，运行时 D3DCompile
    tray/       tray.h/.cpp                   # Shell_NotifyIcon、右键菜单、toast 请求
                hotkeys.h/.cpp                # RegisterHotKey 管理、字符串 ⇄ 组合键、冲突检测
    calib/      calibration.h/.cpp            # 变更 2
    capture/    desktop_dup.h/.cpp            # 变更 3：DDA 采集 + GPU 降采样
                motion_estimator.h/.cpp       # 变更 3：纯逻辑：两帧投影 → (dx, dy, 置信度)
    ui/         settings_dlg.h/.cpp, pages/*  # 变更 4
    util/       log.h/.cpp, strings.h, win.h（ComPtr、RAII 小工具）
    i18n/       i18n.h/.cpp, strings.h, strings_{zh_cn,zh_tw,en}.cpp   # 变更 6：界面文字表（简体 / 繁體 / English）
    res/        app.rc, resource.h, app.manifest, icon.ico, MotionCalm.default.ini
  tools/
    fakegame/   main.cpp     # 全屏无边框黑底网格窗口，隐藏 + 限制光标并每帧回中，N 秒后退出
    inputsim/   main.cpp     # SendInput 相对移动，按给定 counts/s 曲线发送
    make_icon.py             # 仅用标准库生成 res/icon.ico
  tests/
    test_main.cpp            # 自带的极简测试框架（无第三方）
    test_intensity.cpp, test_ini.cpp, test_hotkey_parse.cpp, test_game_rules.cpp,
    test_calibration.cpp（变更 2）, test_motion_estimator.cpp（变更 3）
  openspec/                  # OpenSpec 变更与规格
  docs/       user-guide.{zh-CN,zh-TW,en}.md（面向玩家的使用说明）、settings-ui.md、images/
  DESIGN.md  README*.md（项目主页：英文 / 简体 / 繁體）
```

### 3.3 模块契约（关键类型，实施时保持这些"接缝"，细节可完善）

```cpp
// model/intensity.h —— 纯逻辑，不含 Win32
enum class MaskLevel { Light, Medium, Strong };
enum class Sensitivity { Low, Medium, High };

struct IntensityInputs {
    double mouse_deg_per_s   = 0;   // 已经除以 counts_per_degree
    bool   moving            = false; // WASD / 方向键任一按下
    double pad_deg_per_s     = 0;   // |右摇杆| * gamepad_max_deg_per_s
    double pad_move          = 0;   // |左摇杆| 0..1
    double capture_deg_per_s = 0;   // 变更 3，未启用时 0
    bool gate_mouse = false, gate_keys = false, gate_pad = false, gate_capture = false;
};

class IntensityModel {
public:
    void configure(const TriggerConfig&, const MaskConfig&);   // 阈值、attack/release、move_intensity
    float update(const IntensityInputs&, double dt_seconds);   // 返回平滑后的强度 0..1
    float value() const;        // 平滑后
    float target() const;       // 平滑前（调试条用）
};

// detect/game_detect.h
struct GameContext {
    HWND fg = nullptr; std::wstring exe;      // 前台窗口与其 exe 文件名（小写）
    HMONITOR monitor = nullptr;
    bool fullscreen = false;                  // 窗口矩形覆盖整个显示器
    bool cursor_hidden = false, cursor_clipped = false, cursor_locked = false;
    bool excluded = false;                    // 在排除名单且未被强制标记为游戏
    bool known_game = false;                  // 已知游戏（见 §6）
    bool in_game = false;                     // 是否显示 overlay（见 §6）
};
class GameDetector { public: void poll(const MouseLockHint&); const GameContext& ctx() const; /* 排除 / 标记接口 */ };

// overlay/overlay.h
struct OverlayFrame {
    float intensity = 0;                  // 0..1
    MaskLevel level = MaskLevel::Medium;
    bool show_center_dot = true, show_edge_dots = true;
    bool debug_bar = false; std::wstring debug_text;
    std::wstring toast; double toast_age_s = 0;        // 顶部居中 1.5 s 提示
    std::wstring message;                              // 变更 2：校准提示（屏幕中上方，多行）
};
class Overlay {
public:
    bool init(HINSTANCE);
    void follow_monitor(HMONITOR);            // 尺寸 / 位置贴合该显示器
    void set_visible(bool);
    void set_capture_excluded(bool);          // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
    void render(const OverlayFrame&, const MaskStyle&, const DotStyle&);   // 内部判断是否需要 Present
    void keep_topmost();                      // 前台切换时重新 SetWindowPos(HWND_TOPMOST)
};
```

## 4. 强度模型（§3.3 `IntensityModel` 的精确定义）

1. **鼠标角速度**：Raw Input 的 `(dx, dy)` 进入一个按时间戳的环形缓冲；
   `counts_per_s = |Σ(dx, dy) 在最近 velocity_window_ms 内| / window`；`mouse_deg_per_s = counts_per_s / counts_per_degree`。
   `counts_per_degree` 取当前前台 exe 的校准值（变更 2），没有则用 `default_counts_per_degree`（默认 18.2，对应 800 DPI、常见灵敏度）。
2. **阈值**（°/s，按触发灵敏度）：

   | 灵敏度 | 开始出现 `v_start` | 满强度 `v_full` |
   |---|---|---|
   | 低 | 120 | 600 |
   | 中（默认） | 60 | 360 |
   | 高 | 30 | 200 |

   `t_mouse = smoothstep(v_start, v_full, mouse_deg_per_s)`；手柄同式作用于 `pad_deg_per_s`；截屏同式作用于 `capture_deg_per_s × capture_scale`（变更 3，默认 capture_scale 使其偏保守：阈值 ×1.5）。
3. **移动**：`t_keys = moving ? move_intensity : 0`（默认 0.35）；`t_pad_move = pad_move × move_intensity`。
4. **合成**：`target = max(门控后的各项)`。门控位为 false 的项视为 0。
5. **平滑**（非对称一阶）：
   `τ = (target > value) ? attack_ms/3 : release_ms/3`；`value += (target − value) × (1 − exp(−dt/τ))`。
   默认 attack 100 ms、release 500 ms → 约 0.1 s 到 95%，约 0.5 s 退到 5%。
6. 输出 `value ∈ [0,1]`，渲染层再按档位映射到不透明度与清晰区半径（§7）。
7. 全部纯函数 / 纯类，用合成输入序列做单元测试（上升时间、下降时间、max 合成、门控）。

## 5. 输入采集

- **鼠标 / 键盘**：`RegisterRawInputDevices` 两个 usage（mouse 0x02、keyboard 0x06），`RIDEV_INPUTSINK`，目标是 overlay 的隐藏消息窗口。
  只累加 `MOUSE_MOVE_RELATIVE`；绝对模式（远程桌面、绘图板）忽略。键盘只跟踪 W A S D ↑ ↓ ← → 的按下状态；
  退出前台游戏或 5 s 没收到键盘消息时清空按下状态（防"卡键"）。
- **手柄**：每 tick `XInputGetState` 轮询 0..3 号手柄（`xinput9_1_0.lib`，兼容 Win7+ 不重要，用 `xinput1_4` 亦可）。死区默认 0.2（径向）。
- **不做**：WH_MOUSE_LL / WH_KEYBOARD_LL 低级钩子（增加游戏输入延迟，且更像外挂）。

## 6. "是否在游戏里"判定（§3.3 `GameDetector`）

每 tick（≤ 60 Hz）：

| 字段 | 判定 |
|---|---|
| `fg`, `exe` | `GetForegroundWindow` → pid → `QueryFullProcessImageNameW`（`PROCESS_QUERY_LIMITED_INFORMATION`），按 pid 缓存；取不到 exe 时用窗口类名代替 |
| `fullscreen` | 窗口矩形与其显示器矩形相差 ≤ 1 px |
| `cursor_hidden` | `GetCursorInfo` 的 flags 不含 `CURSOR_SHOWING` |
| `cursor_clipped` | `GetClipCursor` 的矩形严格小于虚拟屏幕 |
| `cursor_locked` | **strict（默认）**：`cursor_hidden && (cursor_clipped || 最近 100 ms 内原始位移 > 8 counts 而 GetCursorPos 移动 < 3 px)`；**hidden 模式**（INI `mouse_lock_rule=hidden`）：`cursor_hidden` 即可 |
| `excluded` | `exe` ∈ 排除名单（不区分大小写）且 ∉ `force_game` |
| `known_game` | `exe` ∈ `force_game` ∪ `known_games`；某 exe 第一次达到 `cursor_locked` 时加入 `known_games` 并持久化到 INI（设计问答里说的是"本次运行"，这里放宽为持久化：对游戏只会更准，浏览器因排除名单永远进不来；设置窗口可清空） |
| `in_game` | `!excluded && (known_game || (fullscreen && cursor_hidden))` |

门控：

```
gate_mouse   = enabled && !excluded && cursor_locked
gate_keys    = enabled && !excluded && known_game
gate_pad     = enabled && !excluded && (fullscreen || cursor_hidden)
gate_capture = enabled && !excluded && known_game          // 变更 3
overlay 可见（含参照点） = enabled && in_game
```

默认排除名单：`chrome.exe msedge.exe firefox.exe brave.exe opera.exe opera_gx.exe vivaldi.exe arc.exe vlc.exe PotPlayerMini64.exe PotPlayer64.exe PotPlayerMini.exe mpc-hc64.exe mpc-hc.exe mpv.exe wmplayer.exe Video.UI.exe Netflix.exe Spotify.exe Discord.exe explorer.exe obs64.exe`。
托盘提供"把当前程序视为游戏"（加入 `force_game`，同时从排除名单移除）与"把当前程序加入排除名单"。

## 7. 渲染

- **窗口**：`WS_POPUP`；扩展样式 `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`。
  点击穿透、不出现在任务栏 / Alt-Tab、永不激活。
- **合成**：`D3D11CreateDevice`（BGRA 支持）→ `CreateSwapChainForComposition`（`B8G8R8A8_UNORM`、`DXGI_ALPHA_MODE_PREMULTIPLIED`、flip、2 buffer、waitable）→ `DCompositionCreateDevice` → `CreateTargetForHwnd(hwnd, TRUE)` → visual.SetContent(swapchain) → Commit。
- **置顶维持**：`SetWinEventHook(EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT)`，前台变化时 `SetWindowPos(HWND_TOPMOST, SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE)` 并调用 `follow_monitor(MonitorFromWindow(fg))`；另每 1 s 兜底检查一次。
- **遮罩像素着色器**（全屏三角形）：
  ```
  p   = (pixel - center)                    // 像素
  ry  = clear_height[level] * H / 2         // 满强度时清晰区竖向半径
  rx  = ry * ellipse_ratio                  // 默认 1.25（略宽的椭圆）
  d   = length(p / (rx, ry))                // 1.0 = 满强度时的清晰区边界
  R   = length((W/2, H/2) / (rx, ry))       // 屏幕角的归一化距离
  r_in(i) = lerp(R + feather, 1.0, i)       // 强度低时边界推到屏幕外，强度高时收到清晰区
  a   = max_opacity[level] * i * smoothstep(r_in, r_in + feather, d)
  rgb = mask_color * a  （预乘）
  ```
  默认 `feather = 0.35`（相对清晰区半径）。档位默认：轻 0.60 / 0.65，中 0.75 / 0.60，强 0.85 / 0.55（最深不透明度 / 清晰区占屏幕高度）。
- **参照点**：同一着色器里用距离测试画。中心点：半径 3 px 白色（alpha 0.9）+ 1 px 深色描边（alpha 0.6）。
  8 个边缘点：四角与四边中点，向内缩 `edge_dot_margin_px`（默认 24），半径 3 px，颜色 #C8C8C8，alpha = lerp(0.15, 0.75, i)。
- **文字层**：D2D 设备上下文直接绑定 swap chain 后台缓冲（DXGI surface 互操作），DirectWrite 字体 "Microsoft YaHei UI"。
  用于：toast（顶部居中、黑底圆角、1.5 s 淡出）、调试条（底部居中：目标 / 平滑强度条 + 文本 "鼠标 312°/s · 移动 · 手柄 0 · 截屏 0.12 · cs2.exe 锁定"）、变更 2 的校准提示。
- **Present 策略**：仅当帧内容相对上一帧有变化（强度、档位、点开关、文字、尺寸）时 Present(1, 0)；强度回到 0 且无文字时再 Present 一帧全透明后停止。
- **截屏可见性**：`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`（Win10 2004+）。`hide_from_capture=auto` 时随截屏检测开关联动；`always` / `never` 强制。
- **DPI**：manifest 声明 PerMonitorV2；overlay 尺寸用显示器物理像素，点的像素尺寸按该显示器 DPI 缩放。

## 8. 配置文件（`MotionCalm.ini`，exe 同目录，UTF-8）

- 首次运行不存在时，从内嵌模板 `res/MotionCalm.default.ini` 写出（含中文注释）。
- **保存策略**：每次保存都用模板重新生成整个文件并填入当前值，这样注释永远在；未知键保留在文件末尾 `[extra]`。
- 自写的小型 INI 解析器：`;` / `#` 注释，`key=value`，值 trim，布尔 `0/1`，列表用 `;` 分隔，颜色 `RRGGBB`。
- 托盘 / 热键 / 设置窗口的每次改动立即保存。启动时与"重新加载配置"时读取；文件被外部修改（`ReadDirectoryChangesW`）时 500 ms 防抖后自动重载。

```ini
[general]
enabled=1
mask_level=medium              ; light | medium | strong
sensitivity=medium             ; low | medium | high
autostart=0
hide_from_capture=auto         ; auto | always | never
log_level=info                 ; debug | info | warn

[mask]
color=000000
max_opacity_light=0.60
max_opacity_medium=0.75
max_opacity_strong=0.85
clear_height_light=0.65        ; 满强度时清晰区直径 / 屏幕高度
clear_height_medium=0.60
clear_height_strong=0.55
ellipse_ratio=1.25             ; 横向半径 / 纵向半径
feather=0.35
attack_ms=100
release_ms=500

[dots]
center_dot=1
center_dot_radius_px=3
center_dot_color=FFFFFF
center_dot_outline=1
edge_dots=1
edge_dot_radius_px=3
edge_dot_color=C8C8C8
edge_dot_min_alpha=0.15
edge_dot_max_alpha=0.75
edge_dot_margin_px=24

[trigger]
mouse=1
keyboard=1
gamepad=1
capture=1                      ; 变更 3
capture_sensitivity=medium     ; low | medium | high（变更 3）
default_counts_per_degree=18.2
v_start_low=120
v_full_low=600
v_start_medium=60
v_full_medium=360
v_start_high=30
v_full_high=200
move_intensity=0.35
gamepad_max_deg_per_s=300
gamepad_deadzone=0.20
mouse_lock_rule=strict         ; strict | hidden
velocity_window_ms=50

[detect]
exclude=chrome.exe;msedge.exe;firefox.exe;...（§6 默认名单）
force_game=
known_games=

[hotkeys]                      ; 格式 Ctrl+Alt+Shift+Win+<Key>，None 表示不绑定
toggle=Ctrl+Alt+F9
cycle_level=Ctrl+Alt+F10
toggle_center_dot=Ctrl+Alt+F11
debug_bar=Ctrl+Alt+F8
calibrate=Ctrl+Alt+F7          ; 变更 2
toggle_capture=Ctrl+Alt+F6     ; 变更 3
cycle_sensitivity=None
toggle_edge_dots=None
open_settings=None             ; 变更 4
mark_game=None
exclude_app=None
exit=None

[calibration]                  ; 变更 2：<exe 小写>=<counts_per_degree>；变更 4 起后接 ;<校准日期 YYYY-MM-DD>

[capture]                      ; 变更 3
grid_w=160
grid_h=90
max_shift=20
min_correlation=0.60
assumed_hfov_deg=90
hold_ms=100

[ui]                           ; 变更 4：设置窗口自己的界面状态
language=auto                  ; 变更 6：auto | zh-CN | zh-TW | en（auto 按 Windows 显示语言）
show_advanced=0
x=                             ; 设置窗口上次的位置；留空 = 居中
y=
```

键名 → 功能的映射是热键系统与设置窗口共用的注册表（一个静态表：id、INI 键、中文名、默认组合键）。

## 9. 托盘与热键

托盘右键菜单（变更 1 的版本；后续变更按注释补项）：

```
MotionCalm v1.0 —— 状态行（灰）："游戏：cs2.exe · 已锁定 · 遮罩：中"
----------------------------
☑ 启用遮罩                      Ctrl+Alt+F9
   遮罩强度 ▸  轻 / ● 中 / 强     Ctrl+Alt+F10 循环
   触发灵敏度 ▸ 低 / ● 中 / 高
☑ 中心参照点                    Ctrl+Alt+F11
☑ 边缘参照点
☑ 截屏运动检测                  Ctrl+Alt+F6      （变更 3）
----------------------------
   把当前程序视为游戏
   把当前程序加入排除名单
   校准当前游戏…                 Ctrl+Alt+F7      （变更 2）
   设置…                                           （变更 4；变更 1 里先用"打开配置文件"）
   打开配置文件
   重新加载配置
☐ 显示调试信息                  Ctrl+Alt+F8
----------------------------
☐ 开机自动启动
   打开日志
   关于 MotionCalm
   退出
```

- 热键：`RegisterHotKey`；字符串解析 `Ctrl+Alt+Shift+Win+<Key>`，Key 支持 F1–F24、A–Z、0–9、Space、Tab、Esc、Pause、Insert、Delete、Home、End、PgUp、PgDn、方向键、小键盘 Num0–9、OEM 符号键按 VK 名（`VK_OEM_3` 等）。
- 注册失败（被占用）→ 日志 + 托盘 toast "快捷键 X 已被其他程序占用"，并在设置窗口标红。
- 不带修饰键的单键允许，但保存时警告（会吃掉游戏里的那个键）。
- 每次热键触发在 overlay 上显示 toast（"遮罩：强"、"已关闭遮罩"…）。
- 开机自启：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MotionCalm`。
- 单实例：命名互斥量 `Local\MotionCalm.Instance`；第二个实例向第一个发自定义消息弹出 toast "MotionCalm 已在运行"后退出。

## 10. 变更 2：游戏内校准

- 状态机：`Idle → Run1 → Between → Run2 → Done`；热键 `calibrate` 在 Idle 开始、在 Run 中结束、在 Between 开始第二次；Esc 不可用（可能被游戏占），用同一热键；超时 60 s 自动取消。
- 开始条件：`in_game && cursor_locked`，否则 toast "请先在游戏里锁定视角再开始校准"。
- 过程：overlay 显示多行提示（"校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7"）与实时计数 "已转 3271 计数"。
  记录期间 Σdx（有符号）；结束时 `counts_360 = |Σdx|`。
- 合法性：`counts_360 ∈ [800, 400000]`，否则 "计数异常，请重试"。两次相差 > 15% → 丢弃并提示重做。
- 结果：`counts_per_degree = 平均 / 360`，写入 `[calibration] <exe>=<值>`，toast "校准完成：每度 18.4 计数"。
- 立即生效于强度模型；托盘 "校准列表 ▸" 可删除单项。
- 单元测试：状态机、合法性、平均、15% 规则。

## 11. 变更 3：截屏运动检测

- **采集**：`IDXGIOutput1::DuplicateOutput` 对 overlay 所在显示器；工作线程 `AcquireNextFrame(16 ms)`；超时 → 帧未变 → 速度衰减为 0。
  显示器切换、`DXGI_ERROR_ACCESS_LOST`（全屏切换 / UAC）→ 重建，重建失败则 2 s 后重试；连续失败则在日志里说明并关闭该功能。
- **降采样**：GPU：把桌面纹理以线性采样渲染到 `grid_w × grid_h`（默认 160×90）的 `R32_FLOAT` 灰度 RT（三步双线性或 3×3 taps），`CopyResource` 到 staging，`Map` 读回 CPU（57 KB）。
- **位移估计**（`MotionEstimator`，纯逻辑、可单测）：
  - 列投影 `C[x] = Σ_y I(x,y)`，行投影 `R[y] = Σ_x I(x,y)`，各自去均值、去线性趋势。
  - 与上一帧的投影做归一化互相关，位移范围 `±max_shift`（默认 20 格 = 1080p 下 240 px）；峰值做抛物线亚像素插值 → `(dx, dy)` 与置信度 `c`。
  - `c < min_correlation`（默认 0.60）：视为场景切换 / 闪光 / 无纹理 → 不更新速度，沿用上一值最多 `hold_ms`（100 ms）后衰减。
  - 像素 → 角度：`deg = shift_px_screen × assumed_hfov_deg / W`（默认 HFOV 90°）；`capture_deg_per_s = deg / dt_frame`。
- **接入**：主线程 tick 读取原子值放进 `IntensityInputs.capture_deg_per_s`，门控 `gate_capture`；阈值同鼠标，按 `capture_sensitivity` 再乘保守系数（低 ×2.0、中 ×1.5、高 ×1.0）。
- **自截屏问题**：截屏检测开启时强制 `set_capture_excluded(true)`（`hide_from_capture=auto`）。
- **性能**：记录并在 `--verbose` 日志里每 5 s 打印平均 GPU/CPU 每帧耗时；目标 GPU < 0.2 ms、CPU < 2%。
- 单元测试：合成 160×90 纹理帧（随机纹理 + 已知位移、加噪、亮度整体突变、完全换图），验证 dx/dy 误差 < 0.5 格、闪光不触发、换图置信度低。

## 12. 变更 4：设置窗口

- 非模态对话框（资源脚本定义），Tab 控件 + 6 个子对话框页：遮罩 / 参照物 / 触发 / 校准 / 快捷键 / 通用（内容见设计问答 Q20 的表）。
- 改动即时生效：所有控件改动 → 写 `Config` → 发出 `ConfigChanged` → app 重新 `configure()` 模型、渲染样式、热键 → 保存 INI。
- "预览强度"滑块（遮罩页）：窗口打开期间把该值作为强度下限，让用户不动鼠标也能看效果；关窗口即失效。
- 快捷键页：每项一行，自定义的"按键捕获"编辑框（子类化 Edit，WM_KEYDOWN 记录修饰键 + VK，Backspace/Delete 清空为 None），占用冲突标红，单键无修饰给黄色警告。
- 校准页：列表（exe、每度计数、日期），"重测"（进入变更 2 的流程）/"删除"。
- 通用页：开机自启、录屏可见性（auto/always/never）、日志级别、"恢复默认设置"、"打开配置文件"、"清空已知游戏列表"。
- 只用系统通用控件（comctl32 v6，manifest 声明），不引入第三方库。

## 13. 构建、规范、测试

- **编译参数**（`scripts/env.cmd`）：`cl /nologo /std:c++20 /EHsc /W4 /WX- /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus`；release 加 `/O2 /GL /DNDEBUG`，debug 加 `/Od /Zi /MDd`。
  链接：`user32 gdi32 shell32 ole32 advapi32 comctl32 dwmapi d3d11 dxgi dcomp d2d1 dwrite d3dcompiler xinput9_1_0 shcore`。
  app 用 `/SUBSYSTEM:WINDOWS`，tools 与 tests 用 `/SUBSYSTEM:CONSOLE`。资源：`rc` 编译 `app.rc`（图标、manifest、版本信息、变更 4 的对话框）。
  输出到 `out\<debug|release>\`。
- **代码规范**：C++20，`Microsoft::WRL::ComPtr`，RAII，不用异常传递业务错误（初始化失败返回 bool + 日志），`std::wstring` 处理 Win32 字符串，源码 UTF-8（`/utf-8`），中文字符串用 `L"…"`。文件头一行注释说明职责。
- **日志**：`util/log` 写 `MotionCalm.log`（exe 同目录，1 MB 轮转 1 份），级别 debug/info/warn，带时间戳；`--verbose` 等于 debug。
- **命令行参数**（app）：`--config <path>`、`--verbose`、`--no-capture`、`--force-game`（把当前前台当作已知游戏且视为锁定，仅测试）、`--exit-after <秒>`、`--portable`（默认就是）。
- **单元测试**：`tests.exe`，极简断言框架（`CHECK(expr)`、按名字分组、失败计数、非零退出码）。`build.cmd tests` 编译并运行。
- **集成验证**（`scripts/verify.ps1`，在用户机器上跑，每次 ≤ 20 s，**运行前提醒用户不要碰鼠标**）：
  1. 起 `MotionCalm.exe --no-capture --verbose --force-game --exit-after 20`；
  2. 起 `fakegame.exe --seconds 15 --mode lock`（全屏无边框 + 隐藏 + ClipCursor + 每帧回中 + 画网格）；
  3. `inputsim.exe --profile "idle:2,turn:3@8000,idle:3"`（counts/s）；
  4. 在 t=1 s（静止）与 t=3.5 s（转动中）用 .NET `CopyFromScreen` 各截一张 PNG 到 `test-output\`；
  5. 结束后解析日志：静止时强度 0，转动中强度 > 0.8，淡出在 0.6 s 内回到 < 0.05；CPU 时间（`GetProcessTimes`）< 2%。
  主代理用 Read 查看 PNG 做视觉验收。

## 14. 交付

- `build.cmd all release` → `out\release\MotionCalm.exe`；`scripts\package.ps1` 把 exe + `MotionCalm.ini`（模板）+ `LICENSE` + 三份使用说明（`docs/user-guide.*.md`，包内改名 `USER-GUIDE.<语言>.md`）打成 `MotionCalm-v<版本>-win64.zip`。
- README（中文，面向朋友）：怎么用、无边框窗口的要求、SmartScreen 提示怎么过、三个档位怎么选、校准怎么做、哪些游戏不建议用（反作弊说明）、问题反馈怎么截图（先关截屏检测）。
