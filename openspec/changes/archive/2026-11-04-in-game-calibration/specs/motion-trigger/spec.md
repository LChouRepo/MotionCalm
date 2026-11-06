# Spec Delta

## MODIFIED Requirements

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
