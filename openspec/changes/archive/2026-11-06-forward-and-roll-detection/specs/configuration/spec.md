# Spec Delta

## ADDED Requirements

### Requirement: frame 行尾的附加字段
`frame` 行在 `exe=<exe>` 之后 SHALL 依次追加：设置窗口预览强度 > 0 时的 ` preview=<3 位小数>`；然后总是 ` zoom=<°/s，1 位小数> roll=<°/s，1 位小数>`，即强度模型本 tick 使用的前进 / 后退流速（未乘 forward_gain）与倾斜角速度，截屏检测未开启、暂停或过期时为 0.0。`zoom` 与 `roll` SHALL 总是行尾的最后两个字段，已有字段的顺序与写法不变。

#### Scenario: 截屏关闭时的行尾
- **GIVEN** 以 `--no-capture --verbose` 运行
- **WHEN** 查看 frame 行
- **THEN** 每行都以 ` zoom=0.0 roll=0.0` 结尾

#### Scenario: 已有解析不受影响
- **WHEN** 用 v1.0 验证脚本的正则（`... value=(?<v>[0-9.]+) exe=(?<exe>\S*)`）解析新的 frame 行
- **THEN** 各字段与 v1.0 相同，exe 取到 `zoom=` 之前

## MODIFIED Requirements

### Requirement: [capture] 节
`[capture]` SHALL 包含下列键和默认值：`grid_w=160`、`grid_h=90`（降采样网格的宽高，单位：格）、`max_shift=20`（每帧最大搜索位移，单位：格）、`min_correlation=0.60`（最低置信度，平移与前进 / 倾斜估计共用）、`assumed_hfov_deg=90`（假定水平视场角）、`hold_ms=100`（低置信度时沿用上一速度的最长时间），以及变更 5 的 `forward_gain=1.0`（前进 / 后退流速进入强度模型前乘的倍数）、`detect_roll=1`（是否识别画面倾斜）、`baseline_ms=66`（前进 / 倾斜估计与多久之前的画面比较，毫秒；0 = 只与上一帧比较），与前进 / 倾斜各自的三档阈值（°/s）：`zoom_start_low=40`、`zoom_full_low=250`、`zoom_start_medium=20`、`zoom_full_medium=150`、`zoom_start_high=10`、`zoom_full_high=83`、`roll_start_low=30`、`roll_full_low=150`、`roll_start_medium=15`、`roll_full_medium=90`、`roll_start_high=8`、`roll_full_high=50`。`forward_gain`、`detect_roll`、`baseline_ms` 依次写在 `hold_ms` 之后，12 个阈值键按上面的顺序写在 `baseline_ms` 之后（前面有说明选档与比例的注释）。模板中每个键都带中文注释。

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
