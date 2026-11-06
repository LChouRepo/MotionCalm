# Spec Delta

## MODIFIED Requirements

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

### Requirement: [hotkeys] 节

`[hotkeys]` SHALL 按 tray-and-hotkeys 中"热键动作表"的顺序包含每个动作的 INI 键，值为热键字符串。程序保存时 MUST 原样写回用户提供的字符串（包括非法字符串），不做规范化改写。

#### Scenario: 默认值

- **WHEN** 读取默认模板
- **THEN** `[hotkeys]` 依次为 toggle=Ctrl+Alt+F9、cycle_level=Ctrl+Alt+F10、toggle_center_dot=Ctrl+Alt+F11、debug_bar=Ctrl+Alt+F8、calibrate=Ctrl+Alt+F7、toggle_capture=Ctrl+Alt+F6、cycle_sensitivity=None、toggle_edge_dots=None、mark_game=None、exclude_app=None、exit=None

#### Scenario: 保留用户写法

- **GIVEN** 用户写了 `toggle=alt + ctrl + f9`
- **WHEN** 程序因其他改动保存 INI
- **THEN** 该行仍为 `toggle=alt + ctrl + f9`

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

### Requirement: 调试帧日志

debug 级别下，程序 SHALL 输出 `frame` 行，供集成验证脚本解析，格式固定为：
`frame t=<启动后秒数，3 位小数> in_game=<0|1> locked=<0|1> mouse=<°/s，1 位小数> moving=<0|1> pad=<°/s，1 位小数> capture=<强度模型本 tick 使用的截屏角速度 °/s，1 位小数；截屏检测未开启、暂停或过期时为 0.0> target=<3 位小数> value=<3 位小数> exe=<exe，取到行尾>`。
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

#### Scenario: 退出摘要

- **WHEN** `--exit-after 20` 到时
- **THEN** 最后一行类似 `... [info] exit reason=exit-after wall_s=20.003 cpu_s=0.180 cpu_pct=0.90 presents=412`

## ADDED Requirements

### Requirement: [capture] 节
`[capture]` SHALL 包含下列键和默认值：`grid_w=160`、`grid_h=90`（降采样网格的宽高，单位：格）、`max_shift=20`（每帧最大搜索位移，单位：格）、`min_correlation=0.60`（最低置信度）、`assumed_hfov_deg=90`（假定水平视场角）、`hold_ms=100`（低置信度时沿用上一速度的最长时间）。模板中每个键都带中文注释。

#### Scenario: 默认值
- **WHEN** 读取默认模板
- **THEN** grid 为 160×90，max_shift = 20，min_correlation = 0.60，assumed_hfov_deg = 90，hold_ms = 100

### Requirement: 截屏配置值的校验
截屏相关键 SHALL 遵循「值校验与回退」的同一套规则（非法值用默认值、越界截断、每键一条 warn），合法范围为：`grid_w` [64, 320]（整数）、`grid_h` [36, 180]（整数）、`max_shift` [2, 40]（整数）、`min_correlation` [0.10, 0.99]、`assumed_hfov_deg` [30, 170]、`hold_ms` [0, 1000]（整数）；`capture` 为布尔，`capture_sensitivity` 为枚举 low | medium | high。

#### Scenario: 越界截断
- **GIVEN** INI 中 `max_shift=100`、`min_correlation=1.5`
- **WHEN** 读取配置
- **THEN** 生效值为 `max_shift=40`、`min_correlation=0.99`，日志各有一条 warn

#### Scenario: 非法枚举
- **GIVEN** INI 中 `capture_sensitivity=extreme`
- **WHEN** 读取配置
- **THEN** 生效值为 medium，日志有一条 warn

### Requirement: 截屏配置热更新
重新加载配置或其他入口（托盘、热键、设置窗口）修改截屏相关键后，新值 SHALL 在截屏线程处理下一个新帧之前生效。`grid_w` 或 `grid_h` 变化时 SHALL 重建降采样资源并丢弃旧参考帧；`capture` 变化 SHALL 按截屏检测开启状态的规则启动或停止取帧。

#### Scenario: 修改网格尺寸
- **GIVEN** 截屏正在以 160×90 取帧
- **WHEN** 外部编辑 INI 把 `grid_w` 改为 192、`grid_h` 改为 108 并保存
- **THEN** 自动重载后，截屏线程用 192×108 的网格继续工作；切换后的第一帧只作为参考帧

#### Scenario: 修改视场角
- **WHEN** 把 `assumed_hfov_deg` 从 90 改为 120 并重载
- **THEN** 同一位移换算出的截屏角速度变为原来的 120/90 倍
