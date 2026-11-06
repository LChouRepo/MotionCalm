# Spec Delta

## MODIFIED Requirements

### Requirement: INI 节集合与顺序

本变更的 INI SHALL 依次包含 `[general]`、`[mask]`、`[dots]`、`[trigger]`、`[detect]`、`[hotkeys]`、`[calibration]` 七节；只有存在未知键时，才在文件末尾追加 `[extra]`。不在已知节中的节，其键 MUST 按未知键处理。

#### Scenario: 模板的节顺序

- **WHEN** 查看首次生成的 MotionCalm.ini
- **THEN** 节依次为 general、mask、dots、trigger、detect、hotkeys、calibration，且没有 [extra]；`[calibration]` 节只有注释，没有条目

#### Scenario: 未知节

- **GIVEN** 用户加了一个 `[foo]` 节，里面有 `bar=1`
- **WHEN** 程序读取后保存
- **THEN** `[foo]` 不再出现，`[extra]` 中有 `foo.bar=1`

#### Scenario: 旧版 INI 补齐新节

- **GIVEN** 由 core-overlay 版本生成、不含 `[calibration]` 的 INI
- **WHEN** 新版本读取后因任一改动保存
- **THEN** 文件中在 `[hotkeys]` 之后出现带注释的 `[calibration]` 节，其余用户值保持不变

### Requirement: [hotkeys] 节

`[hotkeys]` SHALL 按 tray-and-hotkeys 中"热键动作表"的顺序包含每个动作的 INI 键，值为热键字符串。程序保存时 MUST 原样写回用户提供的字符串（包括非法字符串），不做规范化改写。

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** `[hotkeys]` 依次为 toggle=Ctrl+Alt+F9、cycle_level=Ctrl+Alt+F10、toggle_center_dot=Ctrl+Alt+F11、debug_bar=Ctrl+Alt+F8、calibrate=Ctrl+Alt+F7、cycle_sensitivity=None、toggle_edge_dots=None、mark_game=None、exclude_app=None、exit=None

#### Scenario: 保留用户写法

- **GIVEN** 用户写了 `toggle=alt + ctrl + f9`
- **WHEN** 程序因其他改动保存 INI
- **THEN** 该行仍为 `toggle=alt + ctrl + f9`

## ADDED Requirements

### Requirement: [calibration] 节

`[calibration]` SHALL 存放各游戏的校准结果，每行一条：`<exe 小写>=<counts_per_degree>`。键名不固定，读取时转为小写；同名条目以最后一条为准。值 MUST 是数字，并且值 × 360 落在 [800, 400000] 内（约 2.222–1111.111）；否则忽略该条目并记 warn，这类条目在下次保存时不再写出。保存时，条目按 exe 字母序写在该节的注释之后，值固定 3 位小数，以 `.` 为小数点。

#### Scenario: 读取条目

- **WHEN** 读到 `[calibration]` 下的 `CS2.exe=18.4` 和 `valorant.exe=35.714`
- **THEN** 得到 {cs2.exe: 18.4, valorant.exe: 35.714}

#### Scenario: 写回格式

- **WHEN** 保存上述两条
- **THEN** 节中依次为 `cs2.exe=18.400`、`valorant.exe=35.714`

#### Scenario: 非法条目

- **WHEN** 读到 `bad.exe=abc`、`tiny.exe=1.0`（× 360 = 360 < 800）、`huge.exe=2000`（× 360 = 720000 > 400000）
- **THEN** 三条都被忽略，各记一条 warn；下次保存后它们不再出现

#### Scenario: 默认值保存仍与模板相同

- **WHEN** 把全为默认值、没有任何校准条目的配置按模板保存
- **THEN** 输出与模板逐字节相同
