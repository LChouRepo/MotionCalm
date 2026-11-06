# Design

## Context

动机见 proposal.md 的 Why，行为契约见 `specs/`。本文只讲实现方式。

本变更建立在变更 1–3 的实现之上（以 core-overlay design 为准）：

- **主循环**（core-overlay D1）：`MsgWaitForMultipleObjectsEx` 等待，`PeekMessage` 排空消息，到期才 `tick()`。消息本身不触发 tick。
- **宿主窗口** `MotionCalm.Host`（D2）：隐藏的顶层窗口，承接 `RegisterHotKey`、托盘回调（`NOTIFYICON_VERSION_4`，`NIN_SELECT` / `WM_CONTEXTMENU` 弹菜单）。
- **配置**（D9）：`Config` 结构 + 静态"模式表"（节、键、类型、范围、最少小数位），解析 / 校验 / 写回共用；保存按模板重建、原子写；自身保存靠内容哈希避免触发重载。
- **热键**（D11）：`tray/hotkeys.h` 的 `constexpr` 动作表（id、INI 键、中文名、默认）；`RegisterHotKey(host, 序号+1, mods|MOD_NOREPEAT, vk)`；`parse_hotkey` / `format_hotkey` 纯函数；按表序先到先得处理重复。
- **资源**（D14）：`app.rc` 首行 `#pragma code_page(65001)`，`rc /c65001`；manifest 已声明 PerMonitorV2 与 Common-Controls v6；链接 `comctl32`。
- 变更 2：校准状态机与 `[calibration]` 动态节；变更 3：截屏键、`CaptureState`。

约束：零第三方依赖、只用 comctl32 v6 控件、界面中文、源码 UTF-8、不用异常传递业务错误。

## Goals / Non-Goals

**Goals:**
- 键、范围、默认值只有一个来源（模式表），界面不再另写一份。
- 设置窗口与托盘、热键、外部编辑、校准走同一条"改配置 → 通知 → 应用 → 保存"路径，任何入口的改动都能在 1 个 tick 内反映到其他入口。
- 打开设置窗口不拖慢 overlay：不阻塞 tick（不可避免的模态循环有兜底），滑块拖动不引起磁盘风暴。

**Non-Goals:**
- 不做"取消 / 撤销"（改动即时生效是产品决策）、配置导入导出、按游戏的配置档案。
- 不做可调整大小的窗口、颜色拾取器、深色主题、界面多语言。
- 不改变校准状态机（"重测"只给指引，见 proposal 待确认）。

## Decisions

### D1. 文件与职责

| 文件 | 职责 |
|---|---|
| `src/config/config_store.h/.cpp` | `ConfigStore`：唯一的改配置入口、变更通知、保存策略（立即 / 300 ms 防抖 / flush）；`SaveDebouncer` 为纯类（时间由调用方传入，可单测）；`restore_defaults()`、`clear_known_games()` 纯函数 |
| `src/ui/settings_dlg.h/.cpp` | `SettingsWindow`：单实例打开 / 激活 / 关闭、Tab 与 6 个子页、Ctrl+Tab、`pre_translate()`（供主循环调用 `IsDialogMessage`）、预览值、模态循环兜底 tick、订阅配置通知并刷新 |
| `src/ui/pages/page.h` | `IPage` 接口：`create(parent)`、`load(const Config&, const UiContext&)`、`on_command / on_hscroll / on_notify`、`has_dirty_focus()` |
| `src/ui/pages/page_common.h/.cpp` | 控件绑定：`SliderBinding`、`NumberEditBinding`、`ColorEditBinding`、`CheckBinding`、`ComboBinding`、`RadioBinding`；`value↔position` 换算与格式化（纯函数）；气泡提示 |
| `src/ui/pages/page_mask.cpp` … `page_general.cpp` | 6 个分页，每页一张静态"控件 ↔ 键"表 + 页内特殊逻辑 |
| `src/ui/hotkey_edit.h/.cpp` | 按键捕获框（子类化 Edit） |
| `src/tray/hotkeys.h/.cpp`（修改） | 动作表加 `open_settings`；`suspend_all` / `resume_all` / `rebind` / `trial`；纯函数 `analyze_bindings()` |
| `src/tray/tray.cpp`（修改） | "设置…"默认项；单击延迟、双击打开设置 |
| `src/calib/*`（修改） | 记录带日期的解析 / 写出；按 exe 删除 |
| `src/app/app.*`（修改） | 订阅通知后应用到各模块；预览下限；主循环接入 `pre_translate`；打开设置动作 |

### D2. 资源与 ID 方案

`src/res/resource.h` 按区间分配，避免与变更 1 冲突：

```
// 100–199  变更 1：IDI_APP、IDR_DEFAULT_INI 等
// 200–299  对话框模板
#define IDD_SETTINGS      200
#define IDD_PAGE_MASK     201
#define IDD_PAGE_DOTS     202
#define IDD_PAGE_TRIGGER  203
#define IDD_PAGE_CALIB    204
#define IDD_PAGE_HOTKEYS  205
#define IDD_PAGE_GENERAL  206
// 控件 ID：外壳 1000–1099（IDC_TAB 1001；"关闭"按钮直接用 IDCANCEL）
//          遮罩 1100–1199、参照物 1200–1299、触发 1300–1399、
//          校准 1400–1499、快捷键 1500–1599、通用 1600–1699
// 快捷键页的行在运行时创建：标签 1510+i、捕获框 1530+i、状态 1550+i（i = 动作表序号 0..11）
// 菜单命令 ID 沿用变更 1 的区间；新增 IDM_SETTINGS
```

`app.rc`（已有 `#pragma code_page(65001)`）新增 7 个 `DIALOGEX`：

- `IDD_SETTINGS`：`DS_SHELLFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX`，`EXSTYLE WS_EX_APPWINDOW`（任务栏按钮），`CAPTION "MotionCalm 设置"`，`FONT 9, "Microsoft YaHei UI", 400, 0, 0x86`，约 380×300 DLU；含 `SysTabControl32`（IDC_TAB）与右下角 `PUSHBUTTON "关闭", IDCANCEL`。没有 `IDOK` 按钮。
- 6 个页模板：`DS_CONTROL | DS_SHELLFONT | WS_CHILD`（`DS_CONTROL` 让页参与父对话框的 Tab 导航），同一字体，约 360×260 DLU。
- 用 DLU 布局，标签按中文宽度留足余量（中文标签宽度 ≈ 字数 × 9 DLU）。

**备选**：全部控件运行时用代码创建。否决：资源脚本在 DPI 自动缩放、对话框导航、字体继承上都是现成的，代码量最少。唯一的例外是快捷键页的 12 行，它们按动作表在运行时生成（D6），保证与动作表单一来源。

### D3. 外壳窗口、Tab 与子页

- 打开：`CreateDialogParamW(hinst, IDD_SETTINGS, nullptr, Proc, this)`（无 owner，以便有独立任务栏按钮；对话框模板不带 `WS_VISIBLE`）→ 算出鼠标指针所在显示器 `rcWork` 的居中位置 → `SetWindowPos`（PMv2 下移到不同 DPI 的显示器会收到 `WM_DPICHANGED`，系统按 PMv2 对话框缩放规则重排）→ `ShowWindow(SW_SHOW)` + `SetForegroundWindow`。已存在则 `ShowWindow(SW_RESTORE)`（若最小化）+ `SetForegroundWindow`。
- 子页：`WM_INITDIALOG` 中一次性创建 6 个子对话框，父窗口为外壳对话框（与 Tab 控件同级），位置取 `TabCtrl_AdjustRect(FALSE)` 后的显示区；`TCN_SELCHANGE` 时 `ShowWindow` 切换。子页背景：运行时 `GetProcAddress(LoadLibraryW(L"uxtheme.dll"), "EnableThemeDialogTexture")(page, ETDT_ENABLETAB)`，取不到就保持默认底色。动态加载是为了不改变变更 1 的链接清单。
- 键盘：主循环在 `PeekMessage` 之后先调用 `settings_.pre_translate(&msg)`：若目标属于设置窗口且为 Ctrl+Tab / Ctrl+Shift+Tab → 切页并吞掉；否则 `IsDialogMessageW(settings_hwnd, &msg)`。`DS_CONTROL` 使一次 `IsDialogMessage` 覆盖嵌套子页的 Tab 导航。
- Enter：对话框会把 Enter 转成 `WM_COMMAND(IDOK)`；外壳处理 `IDOK` 时只"提交当前焦点编辑框"，不关窗口。Esc → `IDCANCEL` → 关闭（捕获框通过 `WM_GETDLGCODE` 自己接管 Esc，见 D6）。
- 同一次运行内记住上次的分页索引（`SettingsWindow` 的静态成员），不写 INI。
- 关闭（`IDCANCEL` / `WM_CLOSE`）：`store.flush()` → 预览值清零 → 若热键处于暂停则 `resume_all()` → `DestroyWindow`；`WM_NCDESTROY` 中把 `hwnd_` 置空并取消订阅。

### D4. 配置变更通知（观察者模式）

```cpp
enum class ConfigSource { Settings, Tray, Hotkey, Reload, Calibration, Detector, RestoreDefaults };
enum ConfigSection : uint32_t { General=1, Mask=2, Dots=4, Trigger=8, Detect=16,
                                Hotkeys=32, Calibration=64, Capture=128, AllSections=0xFF };
struct ConfigChange { ConfigSource source; uint32_t sections; };

class ConfigStore {
public:
    const Config& get() const;
    // 唯一的写入口：fn 改副本 → 按模式表校验 / 截断 → 替换 → 通知 → 按来源保存
    void edit(ConfigSource, uint32_t sections, const std::function<void(Config&)>& fn);
    int  subscribe(std::function<void(const ConfigChange&)>);
    void unsubscribe(int token);
    void flush();                       // 立即写出未落盘的改动
    void on_timer(int64_t now_qpc);     // 宿主窗口的防抖计时器回调
};
```

- 通知在主线程同步分发。分发期间若有订阅者再次 `edit`（例如自启写注册表失败要回滚），这次改动先排队，当前分发结束后再处理，避免重入。
- 订阅者（全部在主线程）：
  - app：`Mask|Dots|Trigger|General` → `model.configure()` 与渲染样式；`Capture|Trigger` → `capture.update_params()`；`Detect` → 游戏判定名单；`General` 中的 `hide_from_capture` / `log_level` / `autostart` → 对应同步。
  - 热键：`Hotkeys` → 来源为 Settings 时只 `rebind` 改动的动作；Reload / RestoreDefaults 时先注销全部再注册。
  - 设置窗口：来源不是 Settings 时，对每页调用 `load()`，跳过"有焦点且有未提交输入"的编辑框（`has_dirty_focus()`）；来源是 Settings 时不刷新（页面本身已是新值），页内联动（例如边缘点最淡 / 最浓互推）由页自己处理。
- 变更 1 中直接改 `Config` 再调用 `apply` / `save` 的代码路径（托盘、热键、重载、`known_games` 追加）统一改为 `store.edit()`，这是本变更最主要的重构点。

### D5. 保存时机

- `edit()` 按来源决定：Settings → `SaveDebouncer.touch(now)`，宿主窗口 `SetTimer(kSaveTimer, 300 ms)`（每次改动都重设）；其他来源 → 立即保存，并顺带清掉挂起的防抖。
- `flush()` 的调用点：设置窗口关闭、"打开配置文件"（先 flush 再 `ShellExecuteW`）、程序有序退出（写 `exit` 行之前）、"恢复默认设置"与"清空已知游戏列表"（属于不可逆操作，立即保存）。
- 自身保存不触发重载：沿用变更 1 的内容哈希比较，防抖保存同样会更新"最后一次自己写入的内容"。
- `SaveDebouncer` 是纯类：`touch(t)`、`due(t) → bool`、`clear()`，在 `tests/test_config_store.cpp` 中用模拟时间测试"拖动 2 s 只写一次""关闭时 flush"。

### D6. 按键捕获框（子类化 Edit）

- 创建：快捷键页 `WM_INITDIALOG` 中按动作表逐行 `CreateWindowExW`（标签 `STATIC`、捕获框 `EDIT` 加 `ES_READONLY | ES_CENTER`、状态 `STATIC`），坐标用 DLU 经 `MapDialogRect` 换算，字体取页的 `WM_GETFONT`；`SetWindowSubclass(edit, HotkeyEditProc, id, ctx)`（comctl32 v6 子类化 API）。
- 关闭输入法：中文用户默认开着输入法，按键会先被 IME 截走（`VK_PROCESSKEY`）。捕获框创建后调用 `ImmAssociateContextEx(edit, nullptr, 0)`（从 `imm32.dll` 动态取函数地址，不改链接清单）。
- `WM_GETDLGCODE`：消息为 `VK_TAB` 时返回默认值（让对话框做焦点导航），否则返回 `DLGC_WANTALLKEYS`（Esc、Enter、方向键都交给捕获框，而不是对话框）。
- `WM_KEYDOWN` / `WM_SYSKEYDOWN`（都吞掉，防止 Alt 激活菜单或提示音）：
  - 修饰键（`VK_CONTROL/MENU/SHIFT/LWIN/RWIN` 及左右变体）→ 显示半成品"Ctrl+Alt+…"；
  - 无修饰的 `VK_BACK` / `VK_DELETE` → 提交 None；无修饰的 `VK_ESCAPE` → 恢复原值；
  - 其他键 → 用变更 1 的按键名表反查（`format_hotkey` 的逆表），支持则以 `GetKeyState` 读到的当前修饰键 + 该键提交，不支持则状态显示"不支持该按键"。
- `WM_CHAR` / `WM_SYSCHAR` / `WM_PASTE` / `WM_CONTEXTMENU` 全部吞掉。`WM_KEYUP` / `WM_SYSKEYUP` 时若修饰键已全部松开且本次没有提交，恢复显示原值。
- `WM_SETFOCUS` → `hotkeys.suspend_all()`；`WM_KILLFOCUS` → `hotkeys.resume_all(store.get())`。外壳在 `WM_ACTIVATE(WA_INACTIVE)` 时也调用一次 `resume_all()` 兜底；暂停用计数器实现，重复调用无副作用。
- 提交：`hotkeys.trial(hk)`——在暂停状态下 `RegisterHotKey(host, kTrialId, mods|MOD_NOREPEAT, vk)`，成功就立即 `UnregisterHotKey`，得到"外部是否占用"；然后 `store.edit(Settings, Hotkeys, …)` 写入规范字符串。真正的注册在 `resume_all()` 时发生。
- 状态评估：纯函数 `analyze_bindings(const std::array<Binding,12>&, const std::array<bool,12>& external_ok) → std::array<RowStatus,12>`（内部重复按表序先到先得，靠后者标"未生效"；无修饰键 → 警告；外部占用 → 冲突），放在 `tray/hotkeys.h`，在 `tests/test_hotkey_parse.cpp` 中测试。
- 着色：捕获框是只读 Edit，背景走 `WM_CTLCOLORSTATIC`：冲突 `RGB(255,214,214)`，警告 `RGB(255,242,179)`。高对比度模式（`SPI_GETHIGHCONTRAST`）下不着色，只靠状态文字。

### D7. 数值控件绑定

- 每页一张静态表，例如：
  ```cpp
  constexpr SliderSpec kMaskSliders[] = {
    { IDC_MASK_FEATHER, IDC_MASK_FEATHER_VAL, Key::mask_feather, 0.01, L"",   2 },
    { IDC_MASK_ATTACK,  IDC_MASK_ATTACK_VAL,  Key::mask_attack_ms, 10, L" ms", 0 },
    ...
  };
  ```
  最小 / 最大 / 默认值从模式表取（`schema_find(Key)`），页内只登记步长、单位和显示的小数位。
- 滑块：`TBM_SETRANGEMIN/MAX = 0 .. round((max−min)/step)`，`TBM_SETPAGESIZE = 10`；`WM_HSCROLL`（含 `TB_THUMBTRACK`）→ `v = min + pos·step`，按显示位数取整 → `store.edit(Settings, …)`。载入时位置 = `round((v−min)/step)`，标签显示真实值；只有用户移动滑块才写回，所以 INI 中不在步长上的值不会被悄悄改掉。
- 编辑框：`EN_KILLFOCUS` 或 `IDOK`（Enter）时提交；解析失败 → 恢复原值 + `Edit_ShowBalloonTip`；越界 → 截断 + 气泡"范围 a–b"。阈值对在提交前检查 `v_start < v_full`。
- 颜色：`ES_UPPERCASE`、`EM_LIMITTEXT 6`；色块是一个 `STATIC`，在 `WM_CTLCOLORSTATIC` 中返回该颜色的实心画刷（随颜色变化重建，旧画刷释放）。
- `value↔position`、格式化、阈值顺序检查都是纯函数，在 `tests/test_settings_bind.cpp` 中测试（例如 `release_ms` 5000 → 位置 499 → 5000；`feather=0.357` 载入后位置 31、标签 "0.357"）。

### D8. 预览强度下限接入强度与渲染

```cpp
// app.cpp tick()，model.update 之后、组装 OverlayFrame 时
const float value = model_.update(in, dt);                    // 模型状态不受预览影响
const float p     = settings_.is_open() ? settings_.preview() : 0.f;   // 0..1
const bool  preview = p > 0.f;
frame.intensity = std::max(value, p);
const bool draw_mask_and_dots = preview || (cfg.enabled && ctx.in_game);
HMONITOR mon = ctx.in_game ? ctx.monitor
             : preview     ? MonitorFromWindow(settings_.hwnd(), MONITOR_DEFAULTTONEAREST)
             : text_only_monitor;                              // 变更 1 的仅文字模式规则
```

- `frame` 调试日志里的 `value=` 仍写模型值（verify.ps1 的解析契约不变）。
- 预览时 overlay 处于置顶层、点击穿透，会盖在设置窗口上方；强档满强度时设置窗口边缘会被压暗，但仍可操作。预览默认 0%，用户主动拖动才出现。
- 预览让"overlay 可见且强度 > 0"成立，按变更 1 D1 进入活动节拍；内容不变时不 Present。

### D9. 模态循环期间的 tick 兜底

主循环在两种情况下会被系统模态循环卡住：拖动设置窗口标题栏（`WM_ENTERSIZEMOVE` … `WM_EXITSIZEMOVE`），以及确认框 `MessageBoxW`。这期间 `tick()` 不运行，强度与 toast 冻结。处理：外壳收到 `WM_ENTERSIZEMOVE` 时 `SetTimer(hwnd, kModalTick, 16)`，`WM_TIMER` 中调用 `app.tick_from_modal()`（与主循环共用 `tick()`，dt 照常截断到 0.1 s），`WM_EXITSIZEMOVE` 时 `KillTimer`；确认框用一个 RAII 的 `ModalTickGuard` 包住，做同样的事。滑块拖动不进入模态循环（trackbar 只是捕获鼠标），无需处理。

### D10. 托盘：单击延迟与双击

- `NIN_SELECT`（版本 4 下的左键单击）：记下坐标，`SetTimer(host, kTrayClick, GetDoubleClickTime())`。
- 计时器到期 → 弹菜单（变更 1 的原流程）。
- `WM_LBUTTONDBLCLK`（`LOWORD(lParam)`）：`KillTimer` → 打开设置窗口；并在一个双击间隔内忽略随后可能到达的第二个 `NIN_SELECT`。
- `WM_CONTEXTMENU`（右键）与 `NIN_KEYSELECT`（键盘）立即弹菜单，不受影响。
- 菜单构建时 `SetMenuDefaultItem(hmenu, IDM_SETTINGS, FALSE)` 使"设置…"加粗。

### D11. 校准页与记录日期

- 记录结构：`struct CalibRecord { double cpd; std::wstring cpd_text; std::wstring date_text; bool date_valid; };`。解析 `[calibration]` 值时按第一个 `;` 切开，日期部分原样保存以便往返；写出新校准结果时用 `GetLocalTime` 生成 `YYYY-MM-DD`。
- 列表视图：`LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS`，扩展样式 `LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER`；列宽用 DLU 换算。按 exe 升序填充。
- "重测"可用性取 `calibrator.state() == Idle`；指引文字中的热键用 `format_hotkey(cfg.hotkeys.calibrate)`。
- 删除：确认框（`ModalTickGuard` 包住）→ `store.edit(Settings, Calibration, erase(exe))`；强度模型通过通知在下一个 tick 改用默认值。

### D12. 通用页

- 开机自启：调用变更 1 已有的自启同步函数（D12），改为返回 `bool`；失败时通过 `store.edit` 回滚 `autostart`（排队在当前分发之后），并弹 toast "无法设置开机自启"。复选框由随后的通知刷新回原状态。
- 名单：两个 `LISTBOX`（`LBS_NOTIFY | WS_VSCROLL`）+ 输入框 +"添加 / 删除"。互斥规则复用变更 1 游戏判定的"视为游戏 / 加入排除名单"逻辑，把它们重构为接受任意 exe 参数的纯函数 `mark_as_game(lists&, exe)` / `mark_as_excluded(lists&, exe)`，托盘与设置窗口共用，并在 `tests/test_game_rules.cpp` 中补测。
- "恢复默认设置"：`Config d{}`；把 `calibration`、`force_game`、`known_games`、`autostart`、`extras` 从当前配置拷到 `d`；`store.edit(RestoreDefaults, AllSections, [&](Config& c){ c = d; })`，之后立即 flush。纯函数 `restore_defaults(const Config&) → Config` 可单测。

### D13. DPI

- PMv2 下由资源模板创建的对话框会被系统自动缩放（非客户区、子控件位置尺寸、对话框字体）。运行时创建的快捷键行用页字体创建，`WM_DPICHANGED_AFTERPARENT` 时重新取页字体并 `WM_SETFONT`。
- 自绘部分（色块、状态底色）只依赖控件矩形，随控件一起缩放。
- 气泡提示、列表视图列宽：`WM_DPICHANGED` 后按新 DPI 重新设置列宽。

## Risks / Trade-offs

- [模态循环（拖窗口、确认框）期间 tick 停顿] → D9 的 16 ms 兜底计时器。托盘菜单弹出时的停顿属于变更 1 的现状，本变更不处理。
- [中文输入法吃掉捕获框的按键] → 捕获框解除 IME 关联（D6）。
- [Win+字母组合常被外壳先截走，捕获不到] → 状态栏不会误报；用户仍可在 INI 中手写。README 说明。
- [预览时遮罩盖住设置窗口边缘] → 点击穿透，不影响操作；默认 0%。
- [滑块拖动每秒上百次 edit] → 订阅者按 sections 掩码只做必要的工作；热键只在 `Hotkeys` 节变化时处理；保存有 300 ms 防抖。
- [变更 1 的直接改配置路径很多，统一改为 `store.edit` 有回归风险] → 先补 `tests/test_config_store.cpp`，并在任务 1 组完成后重跑 `scripts/verify.ps1`。
- [自动重载与用户正在输入冲突] → 刷新时跳过有焦点且有未提交输入的编辑框。
- [MODIFIED 的需求与变更 2、3 的版本漂移] → 归档前逐条对照合并（proposal 说明）。
- [`EnableThemeDialogTexture` / `ImmAssociateContextEx` 需要额外系统库] → 动态加载，取不到时功能降级（页底色默认、输入法不关闭），不影响主流程。

## Migration Plan

- INI：模板 `[hotkeys]` 在 `toggle_edge_dots` 之后加 `open_settings=None`；旧文件缺该键按默认值处理，下次保存补齐。
- `[calibration]`：新写入带日期，旧格式继续可读，不做批量迁移。
- 回滚：去掉"设置…"菜单项与双击处理即可；`ConfigStore` 重构本身不改变任何外部行为，可以保留。

## Open Questions

- 各页的具体 DLU 尺寸与控件排布在实现时按实际显示效果微调，不影响规格。
- 通用页是否需要展示已知游戏的名单（而不只是数量），可在实际使用后再决定。

## 实施记录（与上文 D1–D6、D11 的差异）

窗口侧先按 `src/ui/settings_host.h` 的 `SettingsHost` 接口实现（窗口只按 INI 节 / 键读写字符串，不接触 `Config`），app 侧随后接入。因此上文的 `ConfigStore` 方案没有照做，实际做法如下（行为契约不变）：

- **按键访问代替 ConfigStore**：`src/config/config.h` 的 `config_get_value` / `config_set_value` / `config_key_range` 都由 `kSchema` 模式表驱动（含 `[hotkeys]`、`[calibration]` 的 `<值>[;<日期>]`、新增 `[ui]`），INI 校验与界面范围是同一份定义；另有纯函数 `config_thresholds_ordered`、`restore_defaults(const Config&)`、`clear_known_games(Config&)`。单测在 `tests/test_config_access.cpp`。
- **宿主**：`App` 私有继承 `ui::SettingsHost`，实现在 `src/app/app_settings.cpp`。`set()` 先在副本上逐项解析，任一项失败或阈值顺序不对就整次拒绝；成功后替换 `cfg_`，按改动前后的字段差异只做必要的工作（模型 `configure`、速度窗口、手柄、锁定规则与名单 `reevaluate`、热键重新注册、自启同步失败回滚 + toast + `refresh()`、截屏可见性、截屏开关与参数），再挂上 300 ms 防抖保存（宿主窗口计时器 `kSaveTimerId`，同 id 重设即重新计时）。没有通用的订阅 / 通知对象，也没有纯类 `SaveDebouncer`。
- **其他来源**仍直接改 `cfg_` 后调用 `save_config()`（托盘、热键、重载、已知游戏追加、校准完成 / 托盘删除）。`save_config()` 每次都写出全部当前值并顺带清掉挂起的防抖，然后调用 `settings_.refresh()`；成功写盘后调用 `settings_.config_saved()`。重载（`apply_config`）、校准状态变化、"采集失败"状态变化也调用 `refresh()`。外部改 INI 触发重载时丢弃尚未落盘的设置窗口改动（以磁盘为准，写一条 info）。
- **热键**：`pause_hotkeys(true)` 注销全部热键；暂停期间 `register_hotkeys()` 只更新绑定不注册；`pause_hotkeys(false)` 按当前配置重新注册全部热键。设置窗口发起的注册（改键、恢复默认、捕获结束）走 `register_hotkeys(from_settings=true)`：失败 / 重复只写 warn，不弹 toast。改键时重新注册全部而不是只换一个动作，其他动作的组合不变。`hotkey_available()`：本程序已注册的组合视为可用，否则用 `RegisterHotKey(host, 0xBF00, …)` 试注册后立即注销。
- **托盘**：命令 `kTrayCmdSettings` 追加在枚举末尾（已有 id 不变），"设置…"放在"校准列表 ▸"之后、"打开配置文件"之前并 `SetMenuDefaultItem`；单击 / 双击按 D10，计时器 `kTrayClickTimerId`。
- **预览**：`tick()` 中 `f.intensity = max(模型值, p)`（p 只在窗口打开时生效），`f.scene = (enabled && in_game) || p > 0`，不在游戏里时显示器取 `MonitorFromWindow(settings_.hwnd())`。`frame` 行的 `value=` 仍是模型值，p > 0 时才在行尾追加 ` preview=<p>`。预览变化与设置改动都置 `tick_now_`，不改变活动节拍的判据。
- **校准记录**：`Config::calibration_dates`（exe → `;` 之后的原样文本）与 `calibration` 并列，结算时写 `GetLocalTime` 的 `YYYY-MM-DD`（`calib_format_date`）；设置窗口删除走防抖保存、不弹 toast，托盘删除仍立即保存并 toast。
- **退出**：`shutdown()` 最先 `settings_.close()`（提交编辑框 → flush → 清预览 → 恢复热键 → 销毁），再 `flush_config()`，之后才写 `exit` 行。
