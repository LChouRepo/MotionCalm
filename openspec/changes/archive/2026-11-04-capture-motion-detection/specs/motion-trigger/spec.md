# Spec Delta

## MODIFIED Requirements

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

## ADDED Requirements

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
