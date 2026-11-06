# Spec Delta

## ADDED Requirements

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

## MODIFIED Requirements

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
