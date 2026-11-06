# Spec Delta

## Purpose

定义 MotionCalm 的全部持久化设置（exe 同目录、UTF-8 编码的 MotionCalm.ini）及其读写与重载规则，以及命令行参数和日志输出，让用户可以放心地用记事本改配置而不丢注释。

## ADDED Requirements

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

本变更的 INI SHALL 依次包含 `[general]`、`[mask]`、`[dots]`、`[trigger]`、`[detect]`、`[hotkeys]` 六节；只有存在未知键时，才在文件末尾追加 `[extra]`。不在已知节中的节，其键 MUST 按未知键处理。

#### Scenario: 模板的节顺序

- **WHEN** 查看首次生成的 MotionCalm.ini
- **THEN** 节依次为 general、mask、dots、trigger、detect、hotkeys，且没有 [extra]

#### Scenario: 未知节

- **GIVEN** 用户加了一个 `[foo]` 节，里面有 `bar=1`
- **WHEN** 程序读取后保存
- **THEN** `[foo]` 不再出现，`[extra]` 中有 `foo.bar=1`

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

`[trigger]` SHALL 包含下列键和默认值（本变更不含截屏相关的 `capture`、`capture_sensitivity`，它们由变更 3 加入）：

```
mouse=1
keyboard=1
gamepad=1
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

### Requirement: [detect] 节

`[detect]` SHALL 包含 `exclude`、`force_game`、`known_games` 三个列表键。`exclude` 的默认值为 game-detection 中定义的 22 项默认排除名单，按其顺序以 `;` 连接；另两项默认为空。程序写回列表时 MUST 用 `;` 连接，前后不加空白，也不带行尾注释。

#### Scenario: 默认排除名单

- **WHEN** 读取默认模板
- **THEN** exclude 共 22 项，第一项为 chrome.exe，最后一项为 obs64.exe；force_game 和 known_games 都为空

#### Scenario: 写回

- **WHEN** known_games 中先后加入 a.exe、b.exe 后保存
- **THEN** 文件中为 `known_games=a.exe;b.exe`

### Requirement: [hotkeys] 节

`[hotkeys]` SHALL 按 tray-and-hotkeys 中"热键动作表"的顺序包含每个动作的 INI 键，值为热键字符串。程序保存时 MUST 原样写回用户提供的字符串（包括非法字符串），不做规范化改写。

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** `[hotkeys]` 依次为 toggle=Ctrl+Alt+F9、cycle_level=Ctrl+Alt+F10、toggle_center_dot=Ctrl+Alt+F11、debug_bar=Ctrl+Alt+F8、cycle_sensitivity=None、toggle_edge_dots=None、mark_game=None、exclude_app=None、exit=None

#### Scenario: 保留用户写法

- **GIVEN** 用户写了 `toggle=alt + ctrl + f9`
- **WHEN** 程序因其他改动保存 INI
- **THEN** 该行仍为 `toggle=alt + ctrl + f9`

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
- `--no-capture`：本次运行强制关闭截屏运动检测（本变更中无实际效果，但必须被接受）；
- `--force-game`：测试模式，见 game-detection；
- `--exit-after <秒>`：从启动起计时，到时正常退出（退出码 0）；
- `--portable`：可移植模式，即默认行为，无额外效果。

#### Scenario: 集成验证的参数组合

- **WHEN** 以 `--no-capture --verbose --force-game --exit-after 20` 启动
- **THEN** 程序正常运行，约 20 s 后以退出码 0 退出，日志为 debug 级别

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
`frame t=<启动后秒数，3 位小数> in_game=<0|1> locked=<0|1> mouse=<°/s，1 位小数> moving=<0|1> pad=<°/s，1 位小数> target=<3 位小数> value=<3 位小数> exe=<exe，取到行尾>`。
满足任一条件时写一行：target 或 value 与上次写出的值相差 ≥ 0.001；in_game、locked 或 exe 发生变化；距上一行已过 250 ms。退出时 MUST 写一行 info：`exit reason=<menu|hotkey|exit-after> wall_s=<秒> cpu_s=<秒> cpu_pct=<百分比> presents=<总 Present 次数>`，其中 cpu 为进程的内核时间加用户时间。

#### Scenario: 静止段

- **GIVEN** 以 `--verbose` 运行，在游戏中没有输入
- **WHEN** 查看日志
- **THEN** 至少每 250 ms 有一行 `frame ... target=0.000 value=0.000 ...`

#### Scenario: 转动段

- **WHEN** inputsim 以 8000 counts/s 转动
- **THEN** 对应时间段的 frame 行中 mouse 约为 439.6，value 很快超过 0.800

#### Scenario: 退出摘要

- **WHEN** `--exit-after 20` 到时
- **THEN** 最后一行类似 `... [info] exit reason=exit-after wall_s=20.003 cpu_s=0.180 cpu_pct=0.90 presents=412`
