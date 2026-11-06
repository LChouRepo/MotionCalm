# configuration Specification

## Purpose
定义 MotionCalm 的全部持久化设置（exe 同目录、UTF-8 编码的 MotionCalm.ini）及其读写与重载规则，以及命令行参数和日志输出，让用户可以放心地用记事本改配置而不丢注释。

## Requirements

### Requirement: 配置文件位置与首次生成

配置文件 SHALL 默认为 exe 所在目录下的 `MotionCalm.ini`，`--config <path>` 可以指定其他路径。文件不存在时，程序 MUST 把内嵌的默认模板（带中文注释）原样写到该路径，然后再读取它。文件无法创建或写入时，程序 SHALL 用默认值继续运行，禁用保存，记 warn，并弹出 toast "无法写入配置文件"。

#### Scenario: 首次运行

- **GIVEN** exe 目录下没有 MotionCalm.ini
- **WHEN** 启动 MotionCalm.exe
- **THEN** 目录下出现 MotionCalm.ini，其字节内容与内嵌模板 `src/res/MotionCalm.default.ini` 完全相同，并且生效的全是默认值

#### Scenario: 指定路径

- **WHEN** 以 `--config D:\cfg\mc.ini` 启动，该文件不存在
- **THEN** 在 D:\cfg\mc.ini 写出模板，之后的读、写、监视都针对这个文件

#### Scenario: 目录只读

- **WHEN** exe 目录不可写
- **THEN** 程序照常运行，使用默认值；托盘里的改动当次有效但不落盘；日志有 warn

### Requirement: INI 语法

解析器 SHALL 按以下规则处理：
- 编码为 UTF-8，可以有 BOM；换行接受 CRLF 或 LF；
- `[节名]` 后面可以跟行尾注释；整行以 `;` 或 `#` 开头为注释；
- `键=值` 按第一个 `=` 切分，键和值都去掉首尾空白；节名、键名不区分大小写；同一节内重复的键以最后一次为准；
- 标量键的值里，从第一个"空白 + `;` 或 `#`"处起算作行尾注释；列表键（`exclude`、`force_game`、`known_games`）MUST NOT 识别行尾注释，整值按 `;` 切分。

#### Scenario: 行尾注释

- **WHEN** 读到 `mask_level=strong              ; light | medium | strong`
- **THEN** mask_level = strong

#### Scenario: 列表不受注释符影响

- **WHEN** 读到 `exclude=chrome.exe;msedge.exe; my game.exe ;;`
- **THEN** exclude = ["chrome.exe", "msedge.exe", "my game.exe"]

#### Scenario: BOM 与 LF

- **WHEN** 文件带 UTF-8 BOM 且使用 LF 换行
- **THEN** 解析结果与无 BOM、CRLF 的同内容文件完全相同

#### Scenario: 节名带注释、键名大小写

- **WHEN** 读到 `[Hotkeys]   ; 格式说明`，下一行为 `TOGGLE=Ctrl+Alt+F9`
- **THEN** 得到 hotkeys.toggle = "Ctrl+Alt+F9"

#### Scenario: 中文注释

- **WHEN** 模板中含中文注释行 `; 满强度时清晰区直径 / 屏幕高度`
- **THEN** 解析不报错，该行被忽略

### Requirement: 值类型

各键的值 SHALL 按下列类型解析：
- 布尔：只接受 `0` / `1`；
- 颜色：6 位十六进制 `RRGGBB`，不区分大小写，不带 `#`；
- 数值：一律以 `.` 为小数点，与系统区域设置无关；整数键写成小数时四舍五入；
- 枚举：小写的固定取值，读取时不区分大小写；
- 列表：项去掉首尾空白，丢弃空项，忽略大小写去重并保留第一次出现的写法。

#### Scenario: 区域设置无关

- **GIVEN** 系统区域格式为德语（小数点是逗号）
- **WHEN** 读到 `feather=0.35`
- **THEN** feather = 0.35；保存时仍写 `0.35`

#### Scenario: 颜色

- **WHEN** 读到 `edge_dot_color=c8c8c8`
- **THEN** 颜色为 (200,200,200)，保存时写成 `C8C8C8`

#### Scenario: 列表去重

- **WHEN** 读到 `force_game=Game.exe;game.exe`
- **THEN** force_game = ["Game.exe"]

### Requirement: INI 节集合与顺序

INI SHALL 依次包含 `[general]`、`[mask]`、`[dots]`、`[trigger]`、`[detect]`、`[hotkeys]`、`[calibration]`、`[capture]` 八节；只有存在未知键时，才在文件末尾追加 `[extra]`。不在已知节中的节，其键 MUST 按未知键处理。

#### Scenario: 模板的节顺序

- **WHEN** 查看首次生成的 MotionCalm.ini
- **THEN** 节依次为 general、mask、dots、trigger、detect、hotkeys、calibration、capture，且没有 [extra]；`[calibration]` 节只有注释，没有条目

#### Scenario: 未知节

- **GIVEN** 用户加了一个 `[foo]` 节，里面有 `bar=1`
- **WHEN** 程序读取后保存
- **THEN** `[foo]` 不再出现，`[extra]` 中有 `foo.bar=1`

#### Scenario: 旧版 INI 补齐新节

- **GIVEN** 由 core-overlay 版本生成、不含 `[calibration]` 的 INI
- **WHEN** 新版本读取后因任一改动保存
- **THEN** 文件中在 `[hotkeys]` 之后出现带注释的 `[calibration]` 节，其余用户值保持不变

#### Scenario: 旧文件缺少 [capture]

- **GIVEN** 现有 INI 没有 `[capture]` 节，`[trigger]` 中也没有 `capture` 与 `capture_sensitivity`
- **WHEN** 启动并读取配置
- **THEN** 这些键全部使用默认值，不记 warn；下一次保存时按模板补写，位置符合上述节顺序

### Requirement: [general] 节

`[general]` SHALL 包含下列键和默认值：

```
enabled=1
mask_level=medium              ; light | medium | strong
sensitivity=medium             ; low | medium | high
autostart=0
hide_from_capture=auto         ; auto | always | never
log_level=info                 ; debug | info | warn
```

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** enabled=true，档位为中，灵敏度为中，autostart=false，hide_from_capture=auto，log_level=info

### Requirement: [mask] 节

`[mask]` SHALL 包含下列键和默认值（`clear_height_*` 是满强度时清晰区直径与屏幕高度之比；`ellipse_ratio` 是横向半径与纵向半径之比）：

```
color=000000
max_opacity_light=0.60
max_opacity_medium=0.75
max_opacity_strong=0.85
clear_height_light=0.65
clear_height_medium=0.60
clear_height_strong=0.55
ellipse_ratio=1.25
feather=0.35
attack_ms=100
release_ms=500
```

#### Scenario: 修改档位参数

- **WHEN** 把 `max_opacity_strong` 改为 0.70 并重载
- **THEN** 强档满强度时边缘的最深 alpha 为 0.70

### Requirement: [dots] 节

`[dots]` SHALL 包含下列键和默认值：

```
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
```

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** 中心点开启，半径 3，白色，带描边；边缘点开启，半径 3，颜色 C8C8C8，alpha 范围 0.15–0.75，内缩 24 px

### Requirement: [trigger] 节

`[trigger]` SHALL 包含下列键和默认值：

```
mouse=1
keyboard=1
gamepad=1
capture=1                      ; 截屏运动检测 0 | 1
capture_sensitivity=medium     ; low | medium | high
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
```

#### Scenario: 默认阈值

- **WHEN** 读取默认模板
- **THEN** 三档阈值分别为 120/600、60/360、30/200 °/s，每度 18.2 计数

#### Scenario: 截屏默认值

- **WHEN** 读取默认模板
- **THEN** capture = 1，capture_sensitivity = medium

### Requirement: [detect] 节

`[detect]` SHALL 包含 `exclude`、`force_game`、`known_games` 三个列表键。`exclude` 的默认值为 game-detection 中定义的 22 项默认排除名单，按其顺序以 `;` 连接；另两项默认为空。程序写回列表时 MUST 用 `;` 连接，前后不加空白，也不带行尾注释。

#### Scenario: 默认排除名单

- **WHEN** 读取默认模板
- **THEN** exclude 共 22 项，第一项为 chrome.exe，最后一项为 obs64.exe；force_game 和 known_games 都为空

#### Scenario: 写回

- **WHEN** known_games 中先后加入 a.exe、b.exe 后保存
- **THEN** 文件中为 `known_games=a.exe;b.exe`

### Requirement: [hotkeys] 节

`[hotkeys]` SHALL 按 tray-and-hotkeys 中"热键动作表"的顺序包含每个动作的 INI 键，值为热键字符串。程序保存时 MUST 原样写回用户提供的字符串（包括非法字符串），不做规范化改写；由设置窗口按键捕获产生的值写成规范字符串。

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** `[hotkeys]` 依次为 toggle=Ctrl+Alt+F9、cycle_level=Ctrl+Alt+F10、toggle_center_dot=Ctrl+Alt+F11、debug_bar=Ctrl+Alt+F8、calibrate=Ctrl+Alt+F7、toggle_capture=Ctrl+Alt+F6、cycle_sensitivity=None、toggle_edge_dots=None、open_settings=None、mark_game=None、exclude_app=None、exit=None

#### Scenario: 保留用户写法

- **GIVEN** 用户写了 `toggle=alt + ctrl + f9`
- **WHEN** 程序因其他改动保存 INI
- **THEN** 该行仍为 `toggle=alt + ctrl + f9`

#### Scenario: 捕获产生规范字符串

- **WHEN** 在设置窗口中为 `exit` 捕获到 Shift+Ctrl+End
- **THEN** INI 中写入 `exit=Ctrl+Shift+End`

### Requirement: 值校验与回退

值非法（类型不符、枚举未知）时，系统 SHALL 改用该键的默认值；值越界时 SHALL 截断到合法范围。两种情况都 MUST 为每个键记一条 warn 日志。某档的 `v_start ≥ v_full` 时，该档两个阈值都恢复默认。缺失的键按默认值处理。合法范围如下：

```
max_opacity_* / edge_dot_*_alpha / move_intensity    [0, 1]
clear_height_*                                       [0.05, 1]
ellipse_ratio                                        [0.5, 3]
feather                                              [0.05, 1]
attack_ms                                            [10, 2000]（整数）
release_ms                                           [10, 5000]（整数）
*_radius_px                                          [1, 20]（整数）
edge_dot_margin_px                                   [0, 400]（整数）
default_counts_per_degree                            [1, 1000]
v_start_* / v_full_*                                 [1, 5000]
gamepad_max_deg_per_s                                [10, 2000]
gamepad_deadzone                                     [0, 0.9]
velocity_window_ms                                   [10, 500]（整数）
```

#### Scenario: 越界截断

- **WHEN** `max_opacity_strong=1.5`
- **THEN** 生效值为 1.0，日志有一条 warn

#### Scenario: 非法枚举

- **WHEN** `mask_level=extreme`
- **THEN** 生效值为 medium，日志有一条 warn

#### Scenario: 非法数字

- **WHEN** `attack_ms=abc`
- **THEN** 生效值为 100

#### Scenario: 阈值颠倒

- **WHEN** `v_start_high=300`，`v_full_high=200`
- **THEN** 高档阈值恢复为 30 / 200，日志有一条 warn

#### Scenario: 布尔只认 0/1

- **WHEN** `enabled=yes`
- **THEN** 生效值为 1（默认值），日志有一条 warn

### Requirement: 按模板重建的保存

每次保存 SHALL 以内嵌模板为骨架重新生成整个文件：模板中的注释行、空行、节标题、键顺序和行尾注释全部保留，只把每个键的值替换为当前生效值。数值写成能精确往返的最短形式，再补零到不少于该键默认值的小数位数（例如 0.6 写成 `0.60`）。未知键追加到文件末尾的 `[extra]` 节，写成 `<原节名>.<键>=<值>`；原本就在 `[extra]` 中的键原样保留。没有未知键时不写 `[extra]`。写入 MUST 是原子的：先写临时文件，再替换原文件。输出的 BOM 和换行风格与模板一致。

#### Scenario: 默认值保存后与模板完全相同

- **WHEN** 把全为默认值的配置按模板保存
- **THEN** 输出与模板逐字节相同

#### Scenario: 保留注释

- **GIVEN** 当前档位为强
- **WHEN** 保存
- **THEN** 文件中为 `mask_level=strong              ; light | medium | strong`（行尾注释保留，并尽量对齐到原来的列），其余注释原样保留

#### Scenario: 未知键

- **GIVEN** 用户在 `[mask]` 节加了 `glow=1`
- **WHEN** 程序保存
- **THEN** `[mask]` 中不再有 glow，文件末尾为 `[extra]` 加 `mask.glow=1`
- **AND** 再读再存一次，结果与这次逐字节相同

#### Scenario: 中途断电不留半个文件

- **WHEN** 保存过程中进程被强制结束
- **THEN** MotionCalm.ini 要么是旧内容，要么是新内容，不会出现截断的文件

### Requirement: 立即保存

通过托盘、热键或自动行为（例如追加 `known_games`）修改任何持久化设置后，系统 SHALL 立即保存 INI。只在运行期有效的状态（调试条开关、命令行覆盖项）MUST NOT 写入 INI。

#### Scenario: 托盘改档位

- **WHEN** 在托盘把档位改为轻
- **THEN** 不到 100 ms 后，磁盘上的 INI 中就是 `mask_level=light`

#### Scenario: --verbose 不落盘

- **GIVEN** INI 中 `log_level=info`，以 `--verbose` 启动
- **WHEN** 因其他改动触发保存
- **THEN** INI 中仍为 `log_level=info`

### Requirement: 加载与自动重载

启动时，以及用户执行"重新加载配置"时，系统 SHALL 读取 INI。此外它 MUST 监视配置文件所在目录：目标文件被外部修改后，最后一次变化 500 ms 后（防抖）自动重载；程序自己的保存 MUST NOT 触发重载。重载后所有设置 MUST 立即生效，包括强度模型参数、遮罩和点的样式、热键（先注销再重新注册）、开机自启的注册表同步、截屏可见性和日志级别。

#### Scenario: 记事本改完即生效

- **GIVEN** 程序在运行
- **WHEN** 用户在记事本中把 `mask_level` 改为 strong 并保存
- **THEN** 约 0.5 s 后档位变为强，无需任何操作

#### Scenario: 连续保存只重载一次

- **WHEN** 编辑器在 200 ms 内连续写了 3 次文件
- **THEN** 最后一次写入 500 ms 后只重载一次

#### Scenario: 自己的保存不触发重载

- **WHEN** 程序因热键改动保存了 INI
- **THEN** 日志中没有因这次保存而产生的"重新加载"记录

### Requirement: 命令行参数

应用 SHALL 支持以下参数，未知参数记 warn 后忽略：
- `--config <path>`：使用指定的 INI；
- `--verbose`：本次运行的日志级别为 debug；
- `--no-capture`：本次运行不启动截屏运动检测（不创建截屏线程与任何桌面复制对象），不修改 INI 中的 `capture`；
- `--force-game`：测试模式，见 game-detection；
- `--exit-after <秒>`：从启动起计时，到时正常退出（退出码 0）；
- `--portable`：可移植模式，即默认行为，无额外效果。

#### Scenario: 集成验证的参数组合

- **WHEN** 以 `--no-capture --verbose --force-game --exit-after 20` 启动
- **THEN** 程序正常运行，没有截屏线程，约 20 s 后以退出码 0 退出，日志为 debug 级别

#### Scenario: 未知参数

- **WHEN** 以 `--foo` 启动
- **THEN** 程序正常运行，日志中有一条说明忽略了 `--foo` 的 warn

### Requirement: 日志文件

程序 SHALL 把日志写到 exe 同目录的 `MotionCalm.log`（UTF-8），每行格式为 `YYYY-MM-DD HH:MM:SS.mmm [debug|info|warn] <消息>`。级别取 `log_level`，有 `--verbose` 时为 debug。文件超过 1 MB 时 MUST 改名为 `MotionCalm.log.1`（覆盖旧的那份）并新开一个文件。日志文件 MUST 允许被其他程序同时打开阅读。启动时写一行 info，内容包括版本、Windows 版本号、命令行参数、配置文件路径和进程 ID。

#### Scenario: 轮转

- **WHEN** MotionCalm.log 写满 1 MB
- **THEN** 它变成 MotionCalm.log.1，新的 MotionCalm.log 从空文件开始写；目录中最多只有这两份

#### Scenario: 运行中查看

- **WHEN** 程序运行时在记事本中打开 MotionCalm.log
- **THEN** 能打开，程序继续写日志不受影响

### Requirement: 调试帧日志

debug 级别下，程序 SHALL 输出 `frame` 行，供集成验证脚本解析，格式固定为：
`frame t=<启动后秒数，3 位小数> in_game=<0|1> locked=<0|1> mouse=<°/s，1 位小数> moving=<0|1> pad=<°/s，1 位小数> capture=<强度模型本 tick 使用的截屏角速度 °/s，1 位小数；截屏检测未开启、暂停或过期时为 0.0> target=<3 位小数> value=<3 位小数> exe=<exe>`，其后的字段见「frame 行尾的附加字段」。
满足任一条件时写一行：target 或 value 与上次写出的值相差 ≥ 0.001；in_game、locked 或 exe 发生变化；距上一行已过 250 ms。退出时 MUST 写一行 info：`exit reason=<menu|hotkey|exit-after> wall_s=<秒> cpu_s=<秒> cpu_pct=<百分比> presents=<总 Present 次数>`，其中 cpu 为进程的内核时间加用户时间。

#### Scenario: 静止段

- **GIVEN** 以 `--verbose` 运行，在游戏中没有输入
- **WHEN** 查看日志
- **THEN** 至少每 250 ms 有一行 `frame ... target=0.000 value=0.000 ...`

#### Scenario: 转动段

- **WHEN** inputsim 以 8000 counts/s 转动
- **THEN** 对应时间段的 frame 行中 mouse 约为 439.6，value 很快超过 0.800

#### Scenario: 画面滚动段

- **GIVEN** 截屏检测开启，以 `--verbose --force-game` 运行，鼠标静止
- **WHEN** 假游戏画面以约 400 °/s 横向滚动
- **THEN** 对应时间段的 frame 行中 mouse=0.0、capture 约为 400，value 超过 0.300

#### Scenario: 画面放大与旋转段

- **GIVEN** 截屏检测开启，以 `--verbose --force-game` 运行，鼠标静止
- **WHEN** 假游戏画面以 `--zoom 3` 放大，之后以 `--roll 75` 旋转
- **THEN** 放大段的 frame 行末尾 `zoom=` 约为 135，旋转段 `roll=` 约为 75，两段 mouse=0.0

#### Scenario: 退出摘要

- **WHEN** `--exit-after 20` 到时
- **THEN** 最后一行类似 `... [info] exit reason=exit-after wall_s=20.003 cpu_s=0.180 cpu_pct=0.90 presents=412`

### Requirement: [calibration] 节

`[calibration]` SHALL 存放各游戏的校准结果，每行一条：`<exe 小写>=<counts_per_degree>[;<校准日期>]`。键名不固定，读取时转为小写；同名条目以最后一条为准。第一个 `;` 之前的部分 MUST 是数字，并且值 × 360 落在 [800, 400000] 内（约 2.222–1111.111）；否则忽略该条目并记 warn，这类条目在下次保存时不再写出。`;` 之后的日期部分可选：形如 `YYYY-MM-DD` 的合法日期作为校准日期；缺失时日期为"无"，不记 warn；不是合法日期时日期也视为"无"，但保存时原样写回该文本。保存时，条目按 exe 字母序写在该节的注释之后，值固定 3 位小数，以 `.` 为小数点，有日期文本时后接 `;` 与日期文本。

#### Scenario: 读取条目

- **WHEN** 读到 `[calibration]` 下的 `CS2.exe=18.4`、`valorant.exe=35.714;2026-11-01`
- **THEN** 得到 {cs2.exe: 18.4, 无日期}、{valorant.exe: 35.714, 2026-11-01}

#### Scenario: 写回格式

- **WHEN** 保存上述两条
- **THEN** 节中依次为 `cs2.exe=18.400`、`valorant.exe=35.714;2026-11-01`

#### Scenario: 日期部分损坏

- **WHEN** 读到 `apex.exe=15.0;昨天`
- **THEN** apex.exe 每度 15.0 计数照常生效，日期为"无"；保存后该行为 `apex.exe=15.000;昨天`

#### Scenario: 非法条目

- **WHEN** 读到 `bad.exe=abc`、`tiny.exe=1.0`（× 360 = 360 < 800）、`huge.exe=2000;2026-11-01`（× 360 = 720000 > 400000）
- **THEN** 三条都被忽略，各记一条 warn；下次保存后它们不再出现

#### Scenario: 默认值保存仍与模板相同

- **WHEN** 把全为默认值、没有任何校准条目的配置按模板保存
- **THEN** 输出与模板逐字节相同

### Requirement: [capture] 节
`[capture]` SHALL 按顺序包含：`grid_w=160`、`grid_h=90`、`max_shift=20`、`min_correlation=0.60`、`assumed_hfov_deg=90`、`hold_ms=100`、`forward_gain=1.0`、`detect_roll=1`、`baseline_ms=66`（0 = 只与上一帧比较），随后是"前进 / 倾斜阈值"要求中的 12 个阈值键。模板中每个键都带中文注释。

#### Scenario: 键的顺序
- **WHEN** 按模板保存
- **THEN** `forward_gain`、`detect_roll`、`baseline_ms` 依次紧跟在 `hold_ms` 之后，12 个阈值键跟在 `baseline_ms` 之后

#### Scenario: 默认值
- **WHEN** 读取默认模板
- **THEN** grid 为 160×90，max_shift = 20，min_correlation = 0.60，assumed_hfov_deg = 90，hold_ms = 100，forward_gain = 1.0，detect_roll = 1，baseline_ms = 66，前进阈值 40 / 250、20 / 150、10 / 83，倾斜阈值 30 / 150、15 / 90、8 / 50

#### Scenario: 旧文件缺少新键
- **GIVEN** v1.0 生成的 INI，`[capture]` 只有前六个键
- **WHEN** 读取配置
- **THEN** 新键使用默认值，不记 warn；下一次保存时按模板补写在 `hold_ms` 之后，其余用户值不变

#### Scenario: v1.1 初版的 INI
- **GIVEN** v1.1 初版生成的 INI，`[capture]` 有 `forward_gain=1.5`，没有阈值键
- **WHEN** 读取配置
- **THEN** `forward_gain` 保持 1.5，12 个阈值键使用默认值，不记 warn

#### Scenario: 默认值保存仍与模板相同
- **WHEN** 把全为默认值的配置按模板保存
- **THEN** 输出与模板逐字节相同（`forward_gain=1.0` 写一位小数）

### Requirement: 前进 / 倾斜阈值

`[capture]` SHALL 为前进与倾斜各提供三档阈值（°/s，按 `[general] sensitivity` 选档，不乘截屏保守系数）：`zoom_start_low=40`、`zoom_full_low=250`、`zoom_start_medium=20`、`zoom_full_medium=150`、`zoom_start_high=10`、`zoom_full_high=83`、`roll_start_low=30`、`roll_full_low=150`、`roll_start_medium=15`、`roll_full_medium=90`、`roll_start_high=8`、`roll_full_high=50`。

#### Scenario: 中档前进阈值
- **GIVEN** 灵敏度为中
- **WHEN** 前进流速为 150°/s（已乘 `forward_gain`）
- **THEN** 前进项为 1.0

### Requirement: 截屏配置值的校验
截屏相关键 SHALL 遵循「值校验与回退」的同一套规则（非法值用默认值、越界截断、每键一条 warn），合法范围为：`grid_w` [64, 320]（整数）、`grid_h` [36, 180]（整数）、`max_shift` [2, 40]（整数）、`min_correlation` [0.10, 0.99]、`assumed_hfov_deg` [30, 170]、`hold_ms` [0, 1000]（整数）、`forward_gain` [0.5, 3.0]、`baseline_ms` [0, 150]（整数）、12 个 `zoom_*` / `roll_*` 阈值键 [1, 5000]；`capture` 与 `detect_roll` 为布尔，`capture_sensitivity` 为枚举 low | medium | high。前进 / 倾斜的每一档 SHALL 满足开始值 < 满强度值，否则该档两个值都恢复默认，并记一条 warn（与 `[trigger] v_start_* / v_full_*` 的规则相同）。

#### Scenario: 越界截断
- **GIVEN** INI 中 `max_shift=100`、`min_correlation=1.5`
- **WHEN** 读取配置
- **THEN** 生效值为 `max_shift=40`、`min_correlation=0.99`，日志各有一条 warn

#### Scenario: 非法枚举
- **GIVEN** INI 中 `capture_sensitivity=extreme`
- **WHEN** 读取配置
- **THEN** 生效值为 medium，日志有一条 warn

#### Scenario: 前进倍数与基线越界
- **GIVEN** INI 中 `forward_gain=5`、`baseline_ms=-3`
- **WHEN** 读取配置
- **THEN** 生效值为 `forward_gain=3.0`、`baseline_ms=0`，日志各有一条 warn

#### Scenario: 新键的非法值
- **GIVEN** INI 中 `forward_gain=fast`、`detect_roll=yes`、`baseline_ms=abc`
- **WHEN** 读取配置
- **THEN** 生效值为 1.0、1、66，日志各有一条 warn

#### Scenario: 前进 / 倾斜阈值顺序错误
- **GIVEN** INI 中 `zoom_start_medium=200`、`zoom_full_medium=150`
- **WHEN** 读取配置
- **THEN** 中档前进阈值恢复为 20 / 150，日志有一条 warn

#### Scenario: 前进 / 倾斜阈值越界
- **GIVEN** INI 中 `zoom_full_low=9000`、`roll_start_high=0`
- **WHEN** 读取配置
- **THEN** 生效值为 5000、1，日志各有一条 warn

### Requirement: 截屏配置热更新
重新加载配置或其他入口（托盘、热键、设置窗口）修改截屏相关键后，新值 SHALL 在截屏线程处理下一个新帧之前生效。`grid_w` 或 `grid_h` 变化时 SHALL 重建降采样资源并丢弃旧参考帧；`capture` 变化 SHALL 按截屏检测开启状态的规则启动或停止取帧。

#### Scenario: 修改网格尺寸
- **GIVEN** 截屏正在以 160×90 取帧
- **WHEN** 外部编辑 INI 把 `grid_w` 改为 192、`grid_h` 改为 108 并保存
- **THEN** 自动重载后，截屏线程用 192×108 的网格继续工作；切换后的第一帧只作为参考帧

#### Scenario: 修改视场角
- **WHEN** 把 `assumed_hfov_deg` 从 90 改为 120 并重载
- **THEN** 同一位移换算出的截屏角速度变为原来的 120/90 倍

### Requirement: 配置变更通知
所有对生效配置的改动（设置窗口、托盘、热键、自动或手动重载、校准完成或删除、已知游戏自动追加、恢复默认、清空已知游戏）SHALL 在主线程上完成，写入的值 MUST 符合「值校验与回退」的同一份模式表（设置窗口按键名逐项解析与截断，无法解析时整次拒绝；托盘与热键只产生合法值；重载走完整解析）。改动 SHALL 立即应用：强度模型、遮罩与参照点样式、热键注册、截屏参数、游戏判定名单在同一次 tick 内采用新值；设置窗口打开时 SHALL 在 1 个 tick 内刷新为新值（来源是设置窗口本身时除外）。

#### Scenario: 设置窗口改羽化
- **GIVEN** 在游戏中，强度为 1
- **WHEN** 在设置窗口把"边缘柔和度"从 0.35 拖到 0.60
- **THEN** 50 ms 内 overlay 按 feather = 0.60 绘制

#### Scenario: 托盘改动同步到设置窗口
- **GIVEN** 设置窗口打开在遮罩页，档位为中
- **WHEN** 在托盘把档位改为轻
- **THEN** 设置窗口的单选组在 1 个 tick 内变为"轻"

### Requirement: 设置窗口改动的保存时机
来源为设置窗口的改动 SHALL 在最后一次改动 300 ms 后写入 INI，期间的新改动重新计时；改动本身立即生效，不等待保存。关闭设置窗口、点击"打开配置文件"、退出程序时 SHALL 立即写入尚未落盘的改动。托盘与热键改动仍按「立即保存」处理。防抖保存同样 MUST NOT 触发自动重载。

#### Scenario: 拖动滑块只写一次
- **WHEN** 用户连续拖动一个滑块 2 s（产生约 100 次改动）后松手
- **THEN** 拖动期间不写 INI；最后一次改动 300 ms 后写入 1 次，日志中没有因此产生的重载记录

#### Scenario: 改完立刻关窗
- **WHEN** 改动一个复选框后 50 ms 内关闭设置窗口
- **THEN** 窗口关闭前 INI 已包含新值

### Requirement: 恢复默认设置
"恢复默认设置"SHALL 把 `[general]`（`autostart` 除外）、`[mask]`、`[dots]`、`[trigger]`、`[hotkeys]`、`[capture]` 的全部键和 `[detect] exclude` 恢复为模板默认值，并 SHALL 保留 `[calibration]`（含日期）、`[detect] force_game`、`[detect] known_games`、`autostart`、`[ui]` 与 `[extra]`。恢复后发出一次变更通知，先注销全部热键再按默认值重新注册，并立即写入 INI。

#### Scenario: 保留校准与名单
- **GIVEN** `mask_level=strong`、`toggle=Ctrl+Alt+K`、排除名单多了 tool.exe，`force_game=mygame.exe`，`[calibration] cs2.exe=18.400;2026-11-01`，`autostart=1`
- **WHEN** 执行"恢复默认设置"
- **THEN** `mask_level=medium`、`toggle=Ctrl+Alt+F9`，排除名单恢复为 22 项默认值（不含 tool.exe）
- **AND** `force_game=mygame.exe`、cs2.exe 的校准与 `autostart=1` 都不变

### Requirement: 清空已知游戏列表
"清空已知游戏列表"SHALL 把 `[detect] known_games` 置空并立即写入 INI，`force_game` 与排除名单不变。清空后，不在 `force_game` 中的程序 SHALL 从下一次采样起不再算已知游戏，直到它再次达到光标锁定（届时按「已知游戏与持久化」重新追加）。

#### Scenario: 清空后重新识别
- **GIVEN** `known_games=newgame.exe;other.exe`
- **WHEN** 清空已知游戏列表，然后进入 newgame.exe 但还没有锁定光标
- **THEN** INI 中 `known_games=` 为空；此时按住 W 不触发（t_keys 按 0 计）
- **AND** 在 newgame.exe 中第一次锁定光标后，它被重新追加到 `known_games`

### Requirement: 界面范围与校验范围一致
设置窗口中每个数值控件的最小值、最大值与默认值 SHALL 与 INI 读取时「值校验与回退」（含变更 3 的截屏键范围）所用的范围和默认值完全相同，两者 MUST 来自同一份定义，不得各写一份。

#### Scenario: INI 越界值在界面上的显示
- **GIVEN** INI 中 `attack_ms=5`
- **WHEN** 启动后打开遮罩页
- **THEN** 生效值为 10（日志有 warn），"出现时间"滑块位于最左端，标签显示 10 ms

#### Scenario: 界面最大值可以往返
- **WHEN** 把"淡出时间"拖到最右端并保存后重启
- **THEN** INI 中 `release_ms=5000`，读取时没有 warn

### Requirement: [ui] 节
`[ui]` SHALL 保存界面语言与设置窗口自己的界面状态，位于 `[capture]` 之后（`[extra]` 之前），由设置窗口维护：`language`（auto | zh-CN | zh-TW | en，默认 auto，界面语言，规则见 localization「界面语言的选择」；读取时不区分大小写，写回时用这里列出的写法）；`show_advanced`（0 | 1，默认 0，各页是否显示"高级选项"）；`x`、`y`（整数，设置窗口上次关闭时左上角的屏幕坐标，默认留空 = 未设置，范围 −100000–100000）。`language` 为非法值时 SHALL 用 auto 并记一条 warn；缺少该键时用 auto、不记 warn。`x` / `y` 留空时不记 warn；不是整数时记 warn 并按未设置处理；越界截断并记 warn。这些键按设置窗口改动的保存时机写入，"恢复默认设置"不改变它们。

#### Scenario: 默认值
- **WHEN** 读取默认模板
- **THEN** `[ui]` 依次为 `language=auto`、`show_advanced=0`、`x=`、`y=`，默认配置按模板保存仍与模板逐字节相同

#### Scenario: 记住窗口位置
- **GIVEN** 设置窗口被拖到 (630, 196) 后关闭
- **WHEN** 之后再次打开设置窗口，鼠标指针在同一显示器上
- **THEN** INI 中 `x=630`、`y=196`，窗口出现在 (630, 196)

#### Scenario: 旧 INI 没有 [ui]
- **GIVEN** 由更早版本生成的 INI，没有 `[ui]` 节
- **WHEN** 新版本启动后因任一改动保存
- **THEN** 不记 warn，文件末尾（`[capture]` 之后）出现带注释的 `[ui]` 节

#### Scenario: v1.1 的 INI 没有 language
- **GIVEN** v1.1 生成的 INI，`[ui]` 只有 `show_advanced`、`x`、`y`
- **WHEN** 读取配置
- **THEN** 界面语言按 auto 处理，不记 warn；下一次保存时 `language=auto` 补写在 `[ui]` 第一项

#### Scenario: 非法语言
- **GIVEN** INI 中 `language=klingon`
- **WHEN** 读取配置
- **THEN** 按 auto 处理，日志有一条 warn

#### Scenario: 恢复默认保留语言
- **GIVEN** `language=en`
- **WHEN** 执行"恢复默认设置"
- **THEN** `language` 仍为 en，界面保持英文

### Requirement: frame 行尾的附加字段
`frame` 行在 `exe=<exe>` 之后 SHALL 依次追加：设置窗口预览强度 > 0 时的 ` preview=<3 位小数>`；然后总是 ` zoom=<°/s，1 位小数> roll=<°/s，1 位小数>`，即强度模型本 tick 使用的前进 / 后退流速（未乘 forward_gain）与倾斜角速度，截屏检测未开启、暂停或过期时为 0.0。`zoom` 与 `roll` SHALL 总是行尾的最后两个字段，已有字段的顺序与写法不变。

#### Scenario: 截屏关闭时的行尾
- **GIVEN** 以 `--no-capture --verbose` 运行
- **WHEN** 查看 frame 行
- **THEN** 每行都以 ` zoom=0.0 roll=0.0` 结尾

#### Scenario: 已有解析不受影响
- **WHEN** 用 v1.0 验证脚本的正则（`... value=(?<v>[0-9.]+) exe=(?<exe>\S*)`）解析新的 frame 行
- **THEN** 各字段与 v1.0 相同，exe 取到 `zoom=` 之前
