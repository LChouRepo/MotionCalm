# Spec Delta

## MODIFIED Requirements

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

## ADDED Requirements

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
`[ui]` SHALL 保存设置窗口自己的界面状态，位于 `[capture]` 之后（`[extra]` 之前），由设置窗口维护：`show_advanced`（0 | 1，默认 0，各页是否显示"高级选项"）；`x`、`y`（整数，设置窗口上次关闭时左上角的屏幕坐标，默认留空 = 未设置，范围 −100000–100000）。`x` / `y` 留空时不记 warn；不是整数时记 warn 并按未设置处理；越界截断并记 warn。这些键按设置窗口改动的保存时机写入，"恢复默认设置"不改变它们。

#### Scenario: 默认值
- **WHEN** 读取默认模板
- **THEN** `[ui]` 依次为 `show_advanced=0`、`x=`、`y=`，默认配置按模板保存仍与模板逐字节相同

#### Scenario: 记住窗口位置
- **GIVEN** 设置窗口被拖到 (630, 196) 后关闭
- **WHEN** 之后再次打开设置窗口，鼠标指针在同一显示器上
- **THEN** INI 中 `x=630`、`y=196`，窗口出现在 (630, 196)

#### Scenario: 旧 INI 没有 [ui]
- **GIVEN** 由更早版本生成的 INI，没有 `[ui]` 节
- **WHEN** 新版本启动后因任一改动保存
- **THEN** 不记 warn，文件末尾（`[capture]` 之后）出现带注释的 `[ui]` 节
