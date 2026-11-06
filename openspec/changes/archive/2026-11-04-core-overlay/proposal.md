# Proposal

## Why

目标用户玩第一人称游戏约半小时就会 3D 眩晕，主诉"画面晃动感太严重"，而且主要玩带反作弊的多人 FPS，任何注入、读内存或低级钩子方案都不可接受。需要一个纯外部的 Windows overlay：判断"镜头正在转动 / 人物正在移动"时把屏幕边缘平滑压暗，并常驻一个固定的中心参照点。本变更交付可运行的产品核心（DESIGN.md §1 第 1 行），变更 2–4 都建立在它定下的模块接缝（DESIGN.md §3.3）之上。

## What Changes

- 新增单 exe、免安装的 `MotionCalm.exe`（C++20 / Win32 / D3D11 + DirectComposition + Direct2D/DirectWrite，零第三方依赖）。
- 点击穿透、永不激活、不进任务栏 / Alt-Tab 的置顶 overlay：只覆盖前台游戏所在的显示器，并跟着游戏换屏；前台切换时立即重新置顶，另每 1 s 兜底一次；按每台显示器的 DPI 缩放。
- 遮罩：纯黑、偏宽的椭圆、边缘羽化，分轻 / 中 / 强三档（最深不透明度 0.60 / 0.75 / 0.85，满强度时清晰区占屏高 0.65 / 0.60 / 0.55）。参照点：常驻中心白点，外加 8 个边缘点，遮罩越强越明显。文字层：顶部 toast、底部调试条。
- 输入采集：Raw Input 后台接收鼠标（只算相对位移）和键盘（WASD + 方向键），XInput 轮询手柄。**不装任何钩子、不注入、不读游戏内存**。
- 游戏判定：识别前台 exe、判断是否全屏、读取光标隐藏 / 限制状态、按 strict / hidden 两种规则判定光标锁定；有排除名单；"已知游戏"首次锁定后即写入 INI；可手动标记；提供测试用的 `--force-game`。
- 强度模型：按 °/s 设阈值（低 / 中 / 高三档）+ smoothstep；移动强度 0.35；各输入源先过门控再取最大值；用非对称一阶平滑（attack 100 ms / release 500 ms）。
- 中文托盘菜单；全局热键可在 INI 里按 `Ctrl+Alt+Shift+Win+<Key>` 语法自定义，占用冲突会提示，每次触发都弹 toast；开机自启写 `HKCU\...\Run`；用命名互斥量保证单实例。
- `MotionCalm.ini`：放在 exe 同目录，UTF-8，首次运行从带中文注释的内嵌模板生成；每次保存都按模板重建整个文件，未知键保留在 `[extra]`；外部修改后 500 ms 防抖自动重载。
- `MotionCalm.log`：1 MB 轮转一份。命令行参数：`--config`、`--verbose`、`--no-capture`、`--force-game`、`--exit-after`、`--portable`。
- 截屏可见性开关 `hide_from_capture`（auto / always / never）。
- 交付应用构建脚本 `scripts/build_app.cmd`、资源 `src/res/*` 和 4 个单元测试文件，用已有的 `scripts/verify.ps1` 做集成验收。

## Capabilities

### New Capabilities

- `overlay-rendering`：overlay 窗口行为（点击穿透、置顶维持、显示器跟随、DPI、截屏排除）、遮罩与参照点的外观公式、文字层（toast / 调试条）、按需 Present、渲染节拍与性能目标。
- `motion-trigger`：鼠标 / 键盘 / 手柄输入采集；强度模型，包括角速度换算、阈值与 smoothstep、移动强度、门控、max 合成和非对称平滑。
- `game-detection`：前台程序识别、全屏与光标状态、锁定判定（strict / hidden）、排除名单、已知游戏及其持久化、`in_game` 判定、手动标记、`--force-game` 测试模式。
- `tray-and-hotkeys`：托盘图标与菜单（变更 1 版本）、状态行、热键字符串语法、动作表、注册与冲突处理、热键 toast、开机自启、单实例、关于与退出。
- `configuration`：INI 的位置、生成、语法、各节键与默认值、值校验、按模板重建的保存策略、立即保存与自动重载、命令行参数、日志与调试帧日志格式。

### Modified Capabilities

（无。`openspec/specs/` 目前为空，本变更只新增能力。）

## Impact

- 新建代码：`src/app`、`src/config`、`src/input`、`src/detect`、`src/model`、`src/overlay`、`src/tray`、`src/util`、`src/res`（文件清单见 DESIGN.md §3.2 与 tasks.md），以及 `scripts/build_app.cmd` 和 `tests/test_intensity.cpp`、`tests/test_ini.cpp`、`tests/test_hotkey_parse.cpp`、`tests/test_game_rules.cpp`。
- 依赖的基础设施（已存在或由并行任务提供，本变更不创建）：`build.cmd`、`scripts/env.cmd`、`scripts/build_tools.cmd`、`scripts/build_tests.cmd`（把 `tests/*.cpp` 与除 `src/app/main.cpp` 外的全部 `src/**/*.cpp` 链进 `tests.exe`）、`tests/test_main.cpp` + `tests/test_framework.h`（`TEST` / `CHECK*`）、`tools/fakegame`、`tools/inputsim`、`tools/make_icon.py`（生成 `src/res/icon.ico`）、`scripts/verify.ps1`。
- 只触及用户级系统资源：注册表值 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MotionCalm`、命名互斥量 `Local\MotionCalm.Instance`，以及 exe 同目录下的 `MotionCalm.ini` / `MotionCalm.log`。截屏排除需要 Win10 2004 及以上。
- 链接库以 `scripts/env.cmd` 的 `MC_LIBS` 为准（DESIGN.md §13 的清单加上 winmm / uuid / dxguid）。

## 范围外

- **变更 2 `in-game-calibration`**：转一圈校准、`[calibration]` 节、`calibrate` 热键、"校准当前游戏… / 校准列表"菜单项、overlay 的多行 `message` 提示。本变更中 `counts_per_degree` 恒为 `default_counts_per_degree`。
- **变更 3 `capture-motion-detection`**：桌面复制运动估计、`[trigger] capture` 与 `capture_sensitivity`、`[capture]` 节、`toggle_capture` 热键、"截屏运动检测"菜单项。本变更中 `capture_deg_per_s` 恒为 0，`gate_capture` 恒为 false，截屏检测视为未启用。
- **变更 4 `settings-window`**：设置窗口、`open_settings` 热键、"设置…"菜单项（本变更用"打开配置文件"代替），以及快捷键冲突标红、单键黄色警告等 UI。
- 发布打包 `scripts/package.ps1` 和 README（最后单独做），以及 DESIGN.md §1"v1 不做"列出的全部内容。

## 风险

- **被反作弊或比赛规则误解**：本程序不注入、不读内存、不装钩子，只用 Raw Input 后台接收和置顶透明窗口，做法与 Discord、OBS 等外部 overlay 相同。但中心白点在效果上等同于"准星叠加层"，部分赛事或平台规则可能禁止 → 中心点可以随时关；README 写明风险（README 不在本变更内）。
- **独占全屏**：真正的独占全屏游戏会盖住或挤掉 overlay → 要求游戏使用无边框窗口化；日志记录前台全屏状态便于排查。
- **置顶被挤下**：其他置顶窗口（游戏自身、录屏软件、聊天 overlay）可能盖过 overlay → 前台切换时立即重新置顶，另每 1 s 兜底一次；如果对方也在反复抢置顶，可能会闪烁，属已知限制。
- **光标锁定启发式漏判**：有的游戏不隐藏也不限制光标，或用别的方式隐藏光标，会导致鼠标转动不触发 → 提供 `mouse_lock_rule=hidden`，并可以"把当前程序视为游戏"让键盘 / 手柄门控生效；调试条显示锁定状态便于诊断。
- **权限不对等**：游戏以管理员身份运行而 MotionCalm 没有时，UIPI 可能挡住 Raw Input 和热键 → 记入已知限制，建议以相同权限运行。
- **合成延迟**：覆盖在游戏上的置顶窗口可能让游戏失去独立翻转（independent flip），多出约一帧合成延迟 → 不在游戏时隐藏 overlay；尽量少 Present。

## 验收标准

1. `build.cmd all debug` 产出 `out\debug\MotionCalm.exe`、`fakegame.exe`、`inputsim.exe`、`tests.exe`；`build.cmd tests debug` 全部通过（退出码 0）。
2. `scripts\verify.ps1` 一次运行不超过 20 s，运行前会提醒用户不要碰鼠标。它会启动 `MotionCalm.exe --no-capture --verbose --force-game --exit-after 20` 和 `fakegame.exe --seconds 15 --mode lock`，用 `inputsim.exe --profile "idle:2,turn:3@8000,idle:3"` 模拟鼠标，并在 t=1 s 与 t=3.5 s 各截一张 PNG 到 `test-output\`。从日志中必须得到：
   - 静止段强度 = 0；
   - 转动段（8000 counts/s ÷ 18.2 ≈ 440°/s，中档灵敏度下 target = 1.0）强度 > 0.8；
   - 转动停止后 0.6 s 内强度回到 < 0.05；
   - 整个运行期间 CPU 时间（`GetProcessTimes`）低于墙钟时间的 2%（按单核计）。
3. 视觉验收（主代理用 Read 查看 PNG）：t=1 s 的截图没有遮罩，只有中心白点和 8 个淡色边缘点；t=3.5 s 的截图四周明显压暗（中档顶边中点不透明度约 0.75），中央椭圆清晰区完整可见，中心点仍在。
4. tasks.md 第 11 组的手动验收清单全部通过（点击穿透、Alt-Tab、多显示器跟随、托盘、热键、开机自启、单实例、截屏可见性）。

## 待确认

以下是 DESIGN.md 内部的冲突或空白，括号内是本变更采用的读法（以 §2 决策表为准），已写进 specs / design：

1. §7 Present 策略说"强度回到 0 且无文字时再 Present 一帧全透明后停止"，但 §2 要求中心点常驻，§7 规定边缘点在 i=0 时 alpha 仍为 0.15。（最后一帧是"强度为 0 的帧"：点开着时只剩点，只有点全关时才全透明。）
2. §8 列表用 `;` 分隔，而 `;` 同时是注释符，模板里还有行尾注释。（只有"空白 + `;` 或 `#`"才算行尾注释，而且只对标量键生效；列表键 `exclude` / `force_game` / `known_games` 不认行尾注释，整值按 `;` 切分。）
3. §8 说"未知键保留在 `[extra]`"，但没说原来的节名怎么保留。（写成 `<原节名>.<键>=<值>`；本来就在 `[extra]` 里的键原样保留。）
4. §13 `--force-game` 说"把当前前台当作已知游戏且视为锁定"，可启动时的前台是控制台。（每个 tick 的前台程序，只要不是本进程，都视为已知游戏且已锁定；不写入 `known_games`；排除名单照常生效。）
5. §7 `hide_from_capture=auto` 要随截屏检测联动，但截屏检测属于变更 3。（本变更中截屏检测恒为"未启用"，所以 auto 等于不排除；`--no-capture` 会被接受并记录。）
6. §9 写"设置…（变更 4；变更 1 里先用'打开配置文件'）"。（变更 1 的菜单里不出现"设置…"，由已有的"打开配置文件"承担。）
7. §6 规定"overlay 可见 = enabled && in_game"，但 §9 关闭遮罩时和第二实例启动时都要弹 toast。（toast 存续期间，以及调试条开启时，overlay 以"仅文字"模式显示在前台窗口所在的显示器上，不画遮罩和参照点。）
8. 托盘菜单弹出时前台会变成任务栏或本进程，§6 / §9 的"当前程序"会落空。（"当前程序"取最近一个既不是本进程、也不是任务栏 / 托盘窗口的前台程序。）
9. §5 只说手柄"死区 0.2（径向）"，没说死区外是否重新映射。（重新映射：m' = (m − 0.2) / 0.8，m < 0.2 时为 0。）
10. §4 的指数平滑永远到不了 0，会一直触发 Present。（|target − value| < 0.001 时直接令 value = target。）
11. §6 门控公式里只出现 `enabled`。（再与 `[trigger] mouse` / `keyboard` / `gamepad` 各自的开关相与。）
12. §6 strict 规则里"原始位移 > 8 counts"的度量方式没说。（取最近 100 ms 内每个事件 √(dx²+dy²) 之和，即路径长度。）
13. §7 调试条示例里"截屏 0.12"的单位不明。（变更 1 固定显示"截屏 0"，含义留给变更 3 定义。）
14. 有几处 DESIGN 没给出，由本变更在 specs 里定稿：热键动作的中文名和 toast 文案、方向键键名 `Up` / `Down` / `Left` / `Right`、同一组合键被重复绑定时的处理、toast 淡出曲线（总长 1.5 s，最后 0.3 s 线性淡出）、日志里的 `frame` 行格式（verify.ps1 靠它解析）。
15. "把当前程序加入排除名单"时，如果该 exe 已在 `force_game` 里，按 §6 的定义排除不会生效。（加入排除名单时，同时把它从 `force_game` 和 `known_games` 中移除。）
