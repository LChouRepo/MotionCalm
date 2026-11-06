# motion-trigger Specification

## Purpose
把玩家的鼠标、键盘和手柄输入换算成 0..1 的遮罩强度：只在确实"在转视角或在移动"时触发，约 0.1 s 出现，约 0.5 s 淡出。整个过程不注入、不装钩子、不读游戏内存。

## Requirements

### Requirement: 无钩子的后台输入采集

系统 SHALL 通过系统的原始输入（Raw Input）后台接收机制获取鼠标和键盘输入，在其他程序处于前台时也能收到。系统 MUST NOT 安装任何 Windows 钩子（包括低级鼠标 / 键盘钩子），MUST NOT 向任何进程注入代码，也 MUST NOT 以读取内存的权限打开游戏进程。

#### Scenario: 游戏在前台时也能收到输入

- **GIVEN** 游戏窗口在前台，MotionCalm 在后台运行
- **WHEN** 用户移动鼠标或按下 W
- **THEN** MotionCalm 能收到对应的相对位移或按键状态

#### Scenario: 没有钩子与注入

- **WHEN** 用 Process Explorer 等工具检查 MotionCalm 进程
- **THEN** 它没有安装任何全局钩子，没有加载到其他进程中的模块，对游戏进程至多只持有 `PROCESS_QUERY_LIMITED_INFORMATION` 句柄

### Requirement: 只统计相对鼠标位移

鼠标输入只有标记为相对移动的事件 SHALL 计入位移，标记为绝对坐标的事件（远程桌面、绘图板、触控模拟等）MUST 被忽略。

#### Scenario: 普通鼠标

- **WHEN** 一只普通鼠标产生相对位移 (dx=+12, dy=-3)
- **THEN** 该位移被计入速度窗口

#### Scenario: 绝对坐标设备

- **WHEN** 绘图板或远程桌面会话产生绝对坐标的移动事件
- **THEN** 这些事件对鼠标角速度的贡献为 0

### Requirement: 鼠标角速度换算

系统 SHALL 维护一个带时间戳的位移窗口，计算 `counts_per_s = |Σ(dx, dy)| / window`，其中求和范围是最近 `velocity_window_ms` 内的事件，`|·|` 为向量和的欧氏长度。再用 `mouse_deg_per_s = counts_per_s / counts_per_degree` 换算成角速度。`counts_per_degree` MUST 取 `[calibration]` 中当前前台 exe（小写）的校准值，没有该条目时取 `[trigger] default_counts_per_degree`（默认 18.2）。校准值的新增、覆盖或删除 MUST 从下一个 tick 起生效，无需重启。

#### Scenario: 匀速水平转动

- **GIVEN** 默认配置（窗口 50 ms，每度 18.2 计数），前台 exe 没有校准条目
- **WHEN** 持续以 8000 counts/s 水平移动（例如每 1 ms 发 dx=+8）
- **THEN** counts_per_s = 8000（误差 ±2%），mouse_deg_per_s ≈ 439.6°/s

#### Scenario: 停止后归零

- **WHEN** 匀速移动突然停止
- **THEN** 停止后 50 ms 加一个 tick 之内 mouse_deg_per_s 变为 0

#### Scenario: 窗口内来回抵消

- **WHEN** 50 ms 内先 dx=+100，再 dx=-100
- **THEN** 该窗口的 counts_per_s = 0

#### Scenario: 斜向移动

- **WHEN** 50 ms 内累计 dx=+30、dy=+40
- **THEN** counts_per_s = 50 / 0.05 = 1000

#### Scenario: 已校准的游戏

- **GIVEN** `[calibration]` 中有 `fakegame.exe=36.400`，fakegame.exe 在前台
- **WHEN** 以 8000 counts/s 水平移动
- **THEN** mouse_deg_per_s ≈ 219.8°/s（误差 ±2%），中档灵敏度下 t_mouse ≈ 0.549（若用默认值 18.2 则为 1.0）

#### Scenario: 未校准的游戏回退默认值

- **GIVEN** `[calibration]` 中只有 cs2.exe 的条目
- **WHEN** 前台是 other.exe，以 8000 counts/s 移动
- **THEN** 使用 18.2，mouse_deg_per_s ≈ 439.6°/s

#### Scenario: 校准完成立即生效

- **GIVEN** fakegame.exe 原本没有校准条目
- **WHEN** 校准完成，写入 36.400
- **THEN** 不需要重启，下一个 tick 起同样的 8000 counts/s 算出约 219.8°/s

#### Scenario: 删除校准后回退

- **WHEN** 从托盘"校准列表"中删除 fakegame.exe 的条目
- **THEN** 下一个 tick 起 fakegame.exe 回到 18.2

#### Scenario: exe 名大小写

- **GIVEN** `[calibration]` 中写的是 `CS2.exe=20.000`
- **WHEN** 前台 exe 为 cs2.exe
- **THEN** 使用 20.0

### Requirement: 键盘移动状态

系统 SHALL 跟踪 W、A、S、D、↑、↓、←、→ 共 8 个键的按下状态；任一键按下即 `moving = true`。前台窗口一变化，或连续 5 s 没收到任何键盘消息，系统 MUST 清空全部按下状态，防止"卡键"。其他键（空格、Ctrl、Shift 等）MUST NOT 影响 `moving`。

#### Scenario: 按住 W

- **WHEN** 按下 W 且未松开
- **THEN** moving = true；松开后 moving = false

#### Scenario: 方向键

- **WHEN** 只按住 →
- **THEN** moving = true

#### Scenario: 跳和蹲不算

- **WHEN** 只按空格或 Ctrl
- **THEN** moving = false

#### Scenario: 切走前台时清空

- **GIVEN** 按住 W，此时切换前台（Alt+Tab），游戏收不到 W 的松开消息
- **WHEN** 前台窗口变化
- **THEN** moving 立即变为 false

#### Scenario: 5 s 无键盘消息时清空

- **GIVEN** 记录的状态是 W 按下，但此后 5 s 没有任何键盘消息（按住不放时的自动重复也算消息）
- **WHEN** 5 s 到期
- **THEN** moving 变为 false

### Requirement: 手柄输入

系统 SHALL 在每个 tick 读取 0–3 号 XInput 手柄。摇杆幅值 m = min(1, √(x²+y²) / 32767)，再按 `gamepad_deadzone`（默认 0.20）做径向死区并重新映射：m < dz 时取 0，否则取 (m − dz) / (1 − dz)。`pad_deg_per_s` = 右摇杆映射后的幅值 × `gamepad_max_deg_per_s`（默认 300），`pad_move` = 左摇杆映射后的幅值。多个手柄 MUST 各取最大值。未连接手柄 MUST NOT 产生错误或卡顿。

#### Scenario: 死区内

- **WHEN** 右摇杆幅值为 0.15
- **THEN** pad_deg_per_s = 0

#### Scenario: 推满与半推

- **WHEN** 右摇杆幅值为 1.0 或 0.6
- **THEN** pad_deg_per_s 分别为 300 和 150（(0.6 − 0.2) / 0.8 = 0.5）

#### Scenario: 左摇杆

- **WHEN** 左摇杆推满
- **THEN** pad_move = 1.0

#### Scenario: 没有手柄

- **WHEN** 系统未连接任何手柄
- **THEN** pad_deg_per_s = 0，pad_move = 0，tick 耗时不明显增加

### Requirement: 角速度阈值

系统 SHALL 按当前触发灵敏度取阈值对 (`v_start`, `v_full`)，用 `t = smoothstep(v_start, v_full, v)` 计算鼠标项（v = mouse_deg_per_s）和手柄转向项（v = pad_deg_per_s）。默认阈值（单位 °/s，可在 INI 中修改）：

| 灵敏度 | v_start | v_full |
|---|---|---|
| 低 | 120 | 600 |
| 中（默认） | 60 | 360 |
| 高 | 30 | 200 |

#### Scenario: 中档

- **GIVEN** 灵敏度为中
- **WHEN** v 分别为 30、60、210、360、1000 °/s
- **THEN** t 分别为 0、0、0.5、1.0、1.0

#### Scenario: 低档与高档

- **WHEN** 低档 v = 360，或高档 v = 115
- **THEN** 两种情况下 t 都等于 0.5
- **AND** 高档 v = 200 时 t = 1.0

#### Scenario: 手柄推满

- **GIVEN** 灵敏度为中，右摇杆推满（300°/s）
- **WHEN** 计算手柄转向项
- **THEN** t_pad = smoothstep(60, 360, 300) = 0.896（误差 ±0.001）

### Requirement: 移动强度

系统 SHALL 计算 `t_keys = moving ? move_intensity : 0` 和 `t_pad_move = pad_move × move_intensity`，其中 `move_intensity` 默认 0.35。

#### Scenario: 按住 W

- **WHEN** moving = true，没有其他输入
- **THEN** t_keys = 0.35

#### Scenario: 左摇杆半推

- **WHEN** 左摇杆原始幅值为 0.6（映射后 0.5）
- **THEN** t_pad_move = 0.175

### Requirement: 输入源门控

每一项 SHALL 先经过门控，门控为 false 的项按 0 计。门控规则如下，`ctx` 中的字段定义见 game-detection；`capture_on` 表示截屏运动检测处于开启状态（`[trigger] capture=1`、未带 `--no-capture`、未处于"采集失败"）。

```
gate_mouse   = enabled && trigger.mouse    && !excluded && cursor_locked
gate_keys    = enabled && trigger.keyboard && !excluded && known_game
gate_pad     = enabled && trigger.gamepad  && !excluded && (fullscreen || cursor_hidden)
gate_capture = enabled && capture_on       && !excluded && known_game
```

#### Scenario: 桌面上甩鼠标不触发

- **GIVEN** 前台是普通窗口程序，光标可见且未被锁定
- **WHEN** 鼠标以 1000°/s 移动
- **THEN** 鼠标项为 0，target = 0

#### Scenario: 排除名单中的程序

- **GIVEN** 前台是 vlc.exe，全屏且光标隐藏，正在播放快速运动的视频
- **WHEN** 有任意鼠标、键盘、手柄输入或截屏运动
- **THEN** target = 0

#### Scenario: 非已知游戏不计键盘

- **GIVEN** 前台程序不在 `force_game` 和 `known_games` 中
- **WHEN** 按住 W
- **THEN** t_keys 按 0 计

#### Scenario: 非已知游戏不计截屏

- **GIVEN** 前台是全屏但从未锁定过光标、也未被标记的程序，截屏角速度为 1000°/s
- **WHEN** 强度模型更新
- **THEN** gate_capture 为 false，截屏项按 0 计

#### Scenario: 手柄要求全屏或光标隐藏

- **GIVEN** 前台是窗口化程序，光标可见
- **WHEN** 右摇杆推满
- **THEN** 手柄两项都按 0 计

#### Scenario: 单独关闭某个输入源

- **GIVEN** `[trigger] mouse=0`，光标已锁定
- **WHEN** 鼠标以 1000°/s 转动
- **THEN** 鼠标项为 0；键盘和手柄项不受影响

#### Scenario: 关闭截屏检测

- **GIVEN** 已知游戏中 `[trigger] capture=0`，或以 `--no-capture` 启动
- **WHEN** 画面快速平移、鼠标静止
- **THEN** 截屏项为 0，target = 0

### Requirement: 目标强度合成

系统 SHALL 计算 `target = max(gated t_mouse, gated t_keys, gated t_pad, gated t_pad_move, gated t_capture, gated t_zoom, gated t_roll)`，结果落在 [0, 1]。

#### Scenario: 转动加移动

- **WHEN** t_mouse = 0.5，t_keys = 0.35，两者都通过门控
- **THEN** target = 0.5

#### Scenario: 手柄加键盘

- **WHEN** t_pad = 0.896，t_keys = 0.35
- **THEN** target = 0.896

#### Scenario: 前进加键盘

- **GIVEN** 已知游戏中按住 W（t_keys = 0.35）
- **WHEN** t_zoom 分别为 0.10（前进 45 °/s）与 0.96（前进 135 °/s）
- **THEN** target 分别为 0.35 与 0.96

#### Scenario: 平移、前进、倾斜取最大

- **WHEN** t_capture = 0.15、t_zoom = 0.5、t_roll = 0.8，都通过门控
- **THEN** target = 0.8

### Requirement: 非对称平滑

系统 SHALL 用一阶指数平滑把 target 变成输出强度 value：`τ = (target > value) ? attack_ms/3 : release_ms/3`，`value += (target − value) × (1 − exp(−dt/τ))`（默认 attack 100 ms，release 500 ms）。|target − value| < 0.001 时 MUST 直接令 value = target。dt MUST 取单调时钟测得的 tick 间隔，并截断到 [0, 0.1] s；结果与 tick 频率无关。

#### Scenario: 上升时间

- **GIVEN** value = 0，target 恒为 1
- **WHEN** 经过 100 ms
- **THEN** value ∈ [0.945, 0.955]（理论值 0.9502）
- **AND** 50 ms 时 value ∈ [0.77, 0.78]

#### Scenario: 下降时间

- **GIVEN** value = 1，target 恒为 0
- **WHEN** 经过 500 ms
- **THEN** value ∈ [0.045, 0.055]（理论值 0.0498）
- **AND** 1.2 s 时 value 恰好为 0

#### Scenario: 与 tick 频率无关

- **WHEN** 分别以 dt = 1/60 s、1/144 s、4 ms 步进，累计 100 ms 的上升
- **THEN** 三种结果彼此相差 < 0.005

#### Scenario: 中途反向

- **GIVEN** value = 0.6
- **WHEN** target 变为 0.2
- **THEN** 用 release 时间常数（约 166.7 ms）向下收敛，而不是 attack 的

#### Scenario: 端到端（集成验证）

- **GIVEN** 在 fakegame 中，光标已锁定，灵敏度为中
- **WHEN** inputsim 以 8000 counts/s 持续转动 3 s 后停止
- **THEN** 转动期间 value > 0.8
- **AND** 停止后 0.6 s 内 value < 0.05

### Requirement: 截屏角速度项按截屏灵敏度缩放阈值
系统 SHALL 用 t_capture = smoothstep(k·v_start, k·v_full, capture_deg_per_s) 计算截屏项。v_start / v_full 取当前触发灵敏度（`[general] sensitivity`）的阈值对，与鼠标项相同；k 由 `[trigger] capture_sensitivity` 决定：low = 2.0、medium = 1.5、high = 1.0。不存在单独的 `capture_scale` 配置键。截屏项经 `gate_capture` 门控后参与目标强度的 max 合成。

#### Scenario: 默认档位的阈值
- **GIVEN** `sensitivity=medium`（v_start 60、v_full 360）、`capture_sensitivity=medium`（k = 1.5）
- **WHEN** 截屏角速度分别为 90、315、540 °/s
- **THEN** t_capture 分别为 0、0.5、1.0（误差 < 0.001）

#### Scenario: 截屏灵敏度为高
- **GIVEN** `sensitivity=medium`、`capture_sensitivity=high`（k = 1.0）
- **WHEN** 截屏角速度为 360 °/s
- **THEN** t_capture = 1.0

#### Scenario: 截屏灵敏度为低
- **GIVEN** `sensitivity=medium`、`capture_sensitivity=low`（k = 2.0）
- **WHEN** 截屏角速度为 120 °/s
- **THEN** t_capture = 0

#### Scenario: 截屏比鼠标更保守
- **GIVEN** `sensitivity=medium`、`capture_sensitivity=medium`
- **WHEN** 鼠标角速度与截屏角速度都是 200 °/s
- **THEN** t_capture < t_mouse

#### Scenario: 截屏项最大时决定目标
- **GIVEN** 已知游戏中 t_mouse = 0.3、t_keys = 0.35、t_capture = 0.8，各门控均为 true
- **WHEN** 强度模型更新
- **THEN** target = 0.8

#### Scenario: 鼠标静止时由截屏触发
- **GIVEN** 已知游戏中鼠标与键盘都无输入，画面以 600 °/s 平移（默认灵敏度）
- **WHEN** 持续 300 ms
- **THEN** 平滑后的强度 value > 0.9

### Requirement: 前进项与倾斜项
系统 SHALL 计算前进项 t_zoom = smoothstep(zoom_start, zoom_full, forward_gain × capture_zoom_deg_per_s) 与倾斜项 t_roll = smoothstep(roll_start, roll_full, capture_roll_deg_per_s)。zoom_start / zoom_full 与 roll_start / roll_full 取 `[capture]` 中 `[general] sensitivity` 那一档的前进 / 倾斜阈值（默认中档 20 / 150 °/s 与 15 / 90 °/s），MUST NOT 使用鼠标或截屏项的阈值，也 MUST NOT 乘 `capture_sensitivity` 的保守系数；forward_gain 取 `[capture] forward_gain`（默认 1.0），倾斜项不乘倍数。两项都经 `gate_capture` 门控后参与目标强度的 max 合成。键盘移动项（move_intensity）保留不变。

#### Scenario: 默认档位的前进项
- **GIVEN** 默认配置（`sensitivity=medium`，前进阈值 20 / 150，`forward_gain=1.0`）
- **WHEN** 前进流速分别为 20、60、85、90、150 °/s
- **THEN** t_zoom 分别为 0、0.2258、0.5、0.5576、1.0（误差 < 0.001）

#### Scenario: 截屏灵敏度不影响前进与倾斜
- **GIVEN** 默认配置，`capture_sensitivity` 为 low、medium 或 high
- **WHEN** 前进流速为 85 °/s、倾斜角速度为 52.5 °/s
- **THEN** t_zoom = t_roll = 0.5

#### Scenario: 缩放 1.02 在 66 ms 内
- **GIVEN** 默认配置，前进流速 = ln 1.02 / 0.066 × 45 ≈ 13.5 °/s
- **WHEN** 计算前进项
- **THEN** 13.5 < 20，t_zoom = 0

#### Scenario: 前进倍数
- **GIVEN** `forward_gain=2.0`（或 0.5）
- **WHEN** 前进流速为 42.5 °/s（或 170 °/s）
- **THEN** t_zoom = 0.5

#### Scenario: 按触发灵敏度选档
- **GIVEN** `sensitivity=low`（前进 40 / 250、倾斜 30 / 150）或 `sensitivity=high`（前进 10 / 83、倾斜 8 / 50）
- **WHEN** 前进流速为 145 °/s（low）或 46.5 °/s（high）；倾斜角速度为 90 °/s（low）或 29 °/s（high）
- **THEN** t_zoom = t_roll = 0.5

#### Scenario: 倾斜项不乘倍数
- **GIVEN** 默认配置，`forward_gain` 为 1.0 或 3.0
- **WHEN** 倾斜角速度为 52.5 °/s（或 30 °/s）
- **THEN** t_roll = 0.5（或 0.104）

#### Scenario: 门控为假
- **GIVEN** `gate_capture` 为 false
- **WHEN** 前进流速与倾斜角速度都是 1000 °/s
- **THEN** t_zoom = t_roll = 0

#### Scenario: 截屏关闭时键盘照常
- **GIVEN** 已知游戏中截屏检测关闭（`capture=0` 或 `--no-capture`）
- **WHEN** 按住 W
- **THEN** target = 0.35

#### Scenario: 假游戏放大段（集成验证）
- **GIVEN** 截屏检测开启、默认配置，鼠标静止
- **WHEN** fakegame 以 `--zoom 3`（每秒 ln s 增加 3）放大画面
- **THEN** frame 行 `zoom=` 约为 135，平滑后的强度 value > 0.5，mouse=0.0

#### Scenario: 假游戏旋转段（集成验证）
- **GIVEN** 截屏检测开启、默认配置，鼠标静止
- **WHEN** fakegame 以 `--roll 75` 旋转画面
- **THEN** frame 行 `roll=` 约为 75，平滑后的强度 value > 0.5
