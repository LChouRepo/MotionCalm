# Spec Delta

## Purpose

让用户在游戏里水平转一整圈、测两次，测出该游戏"每度多少鼠标计数"并按 exe 保存，使触发阈值真正对应屏幕上的角速度（°/s）。

## ADDED Requirements

### Requirement: 校准状态机

校准 SHALL 是一个状态机 `Idle → Run1 → Between → Run2 → Done`，全程只用 `calibrate` 热键推进：
- 在 Idle 按：开始第 1 次（满足开始条件时）；
- 在 Run1 按：结束第 1 次；
- 在 Between 按：开始第 2 次；
- 在 Run2 按：结束第 2 次并结算。

Done 是瞬时状态，结算完成后 MUST 立即回到 Idle。Esc 及其他任何按键都 MUST NOT 推进或取消校准。

#### Scenario: 完整流程

- **GIVEN** 在游戏中，光标已锁定，状态为 Idle
- **WHEN** 依次按 4 次 `calibrate`，每次 Run 中都水平转一整圈
- **THEN** 状态依次经过 Run1、Between、Run2、Done，最后回到 Idle

#### Scenario: Esc 无效

- **GIVEN** 状态为 Run1
- **WHEN** 用户按 Esc
- **THEN** 状态仍为 Run1，计数继续

### Requirement: 开始条件

只有当前前台满足 `in_game`，并且此刻 `cursor_locked` 或本次前台期间曾出现过 `cursor_locked`（见 game-detection 的"当前程序"）时，从 Idle 开始校准才 SHALL 成功，并记下当前 exe 作为校准对象。（单屏全屏游戏常只回中光标而不限制，strict 规则下静止时不算锁定，所以要接受"本次前台期间曾锁定"。）条件不满足时 MUST 保持 Idle，并弹 toast "请先在游戏里锁定视角再开始校准"。

#### Scenario: 在桌面按热键

- **WHEN** 前台是记事本时按 `calibrate`
- **THEN** 状态仍为 Idle，并出现 toast "请先在游戏里锁定视角再开始校准"

#### Scenario: 游戏菜单里光标未锁定

- **GIVEN** 已知游戏处于菜单界面，光标可见
- **WHEN** 按 `calibrate`
- **THEN** 不开始校准，出现同样的 toast

#### Scenario: 锁定后开始

- **GIVEN** cs2.exe 在前台且 cursor_locked = true
- **WHEN** 按 `calibrate`
- **THEN** 进入 Run1，校准对象为 cs2.exe，计数从 0 开始

#### Scenario: 静止时按热键（本次前台期间曾锁定）

- **GIVEN** game.exe 在前台、in_game = true，只回中光标不限制；玩家刚转过视角（当时 cursor_locked = true），现在鼠标静止所以 cursor_locked = false
- **WHEN** 按 `calibrate`
- **THEN** 进入 Run1，校准对象为 game.exe

### Requirement: 计数规则

在 Run1 和 Run2 中，系统 SHALL 把每个相对鼠标事件的水平位移 dx 带符号累加到 Σdx。绝对坐标事件和垂直位移 MUST NOT 计入。其他状态下的位移不计。结束一次记录时取 `counts_360 = |Σdx|`。

#### Scenario: 向右转一圈

- **WHEN** Run1 中共收到 dx 之和为 +6552
- **THEN** 第 1 次结果为 6552

#### Scenario: 向左转一圈

- **WHEN** Run1 中共收到 dx 之和为 −6552
- **THEN** 第 1 次结果为 6552

#### Scenario: 来回晃动会抵消

- **WHEN** Run1 中先向右 +7000，再向左 −448
- **THEN** 第 1 次结果为 6552

#### Scenario: 垂直位移不计

- **WHEN** Run1 中 dx 之和为 6552，同时 dy 之和为 3000
- **THEN** 第 1 次结果为 6552

### Requirement: 单次计数合法性

每次记录的 `counts_360` MUST 在闭区间 [800, 400000] 内才算合法。不合法时弹 toast "计数异常，请重试"，并且：Run1 结束时不合法 → 回到 Idle；Run2 结束时不合法 → 回到 Between，保留第 1 次的结果。

#### Scenario: 边界值

- **WHEN** counts_360 分别为 799、800、400000、400001
- **THEN** 依次判为 不合法、合法、合法、不合法

#### Scenario: 第 1 次转太少

- **WHEN** Run1 结束时 counts_360 = 500
- **THEN** 出现 toast "计数异常，请重试"，状态回到 Idle

#### Scenario: 第 2 次异常

- **GIVEN** 第 1 次结果为 6552
- **WHEN** Run2 结束时 counts_360 = 0
- **THEN** 出现 toast "计数异常，请重试"，状态回到 Between，第 1 次结果仍为 6552

### Requirement: 两次结果一致性

两次都合法后，系统 SHALL 以两次平均为基准计算相对差：`|c1 − c2| / ((c1 + c2) / 2)`。相对差 > 0.15 时 MUST 丢弃两次结果、回到 Idle，并弹 toast "两次结果相差超过 15%，请重新校准"。相对差 ≤ 0.15 时进入结算。

#### Scenario: 相差太大

- **WHEN** c1 = 6000，c2 = 7000（相对差约 15.4%）
- **THEN** 两次结果被丢弃，回到 Idle，出现上述 toast，INI 不变

#### Scenario: 可以接受的差异

- **WHEN** c1 = 6000，c2 = 6800（约 12.5%）
- **THEN** 进入结算

#### Scenario: 以平均为基准

- **WHEN** c1 = 6000，c2 = 6950（相对平均约 14.7%，相对较小值约 15.8%）
- **THEN** 判为一致，进入结算

### Requirement: 结果计算、保存与生效

结算时系统 SHALL 计算 `counts_per_degree = ((c1 + c2) / 2) / 360`，写入 `[calibration] <校准对象 exe>=<值>`（3 位小数，已有同名条目则覆盖），立即保存 INI，并且从下一个 tick 起强度模型就使用这个值。随后弹 toast `校准完成：每度 {counts_per_degree，1 位小数} 计数`（不附等效灵敏度：按 Source 系视角系数换算的"灵敏度"对其他游戏没有意义，会误导玩家）。

#### Scenario: 两次都是 6552

- **WHEN** c1 = c2 = 6552
- **THEN** INI 中为 `fakegame.exe=18.200`
- **AND** toast 为"校准完成：每度 18.2 计数"

#### Scenario: 两次都是 13104

- **WHEN** c1 = c2 = 13104
- **THEN** INI 中为 `fakegame.exe=36.400`，toast 为"校准完成：每度 36.4 计数"

#### Scenario: 取平均

- **WHEN** c1 = 6552，c2 = 6700
- **THEN** counts_per_degree = 6626 / 360 ≈ 18.406，INI 中为 `18.406`，toast 显示"每度 18.4 计数"

#### Scenario: 重新校准覆盖旧值

- **GIVEN** INI 中已有 `cs2.exe=18.200`
- **WHEN** 对 cs2.exe 再校准，得到 20.000
- **THEN** INI 中只剩一条 `cs2.exe=20.000`

### Requirement: 超时取消

处于 Run1、Between 或 Run2 时，从进入当前状态起超过 60 s 还没有按热键，系统 SHALL 自动取消校准、回到 Idle，丢弃所有中间结果，并弹 toast "校准超时，已取消"。

#### Scenario: Run1 超时

- **GIVEN** 进入 Run1 后没有再按热键
- **WHEN** 经过 60.1 s
- **THEN** 状态回到 Idle，出现 toast "校准超时，已取消"，校准提示消失

#### Scenario: 每个状态单独计时

- **GIVEN** 在 Run1 停留了 50 s 后按热键进入 Between
- **WHEN** 在 Between 又停留 50 s
- **THEN** 不会超时（Between 的计时从进入它时开始）

### Requirement: 离开游戏即取消

处于 Run1、Between 或 Run2 时，一旦前台 exe 与校准对象不同（包括切到 MotionCalm 自己的托盘菜单），系统 SHALL 立即取消校准、回到 Idle，丢弃所有中间结果，并弹 toast "已离开游戏，校准取消"。

#### Scenario: 中途 Alt+Tab

- **GIVEN** 正在对 cs2.exe 进行 Run1
- **WHEN** 用户按 Alt+Tab 切到浏览器
- **THEN** 校准立即取消，toast 为"已离开游戏，校准取消"，INI 不变

### Requirement: 校准提示文案

校准进行中，overlay SHALL 显示下列多行提示（渲染位置与样式见 overlay-rendering）。`{hk}` 是 `calibrate` 当前绑定的规范字符串；`{n}` 是当前的 |Σdx|，取整数，并随输入实时更新。Idle 时不显示提示。
- Run1："校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 {hk}" / "已转 {n} 计数"
- Between："第 1 次：{c1} 计数" / "准星重新对准参照物，按 {hk} 开始第 2 次"
- Run2："校准 2/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 {hk}" / "已转 {n} 计数"

#### Scenario: 实时计数

- **GIVEN** 默认热键，状态为 Run1
- **WHEN** 已累计 Σdx = 3271
- **THEN** 提示为两行："校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7"和"已转 3271 计数"

#### Scenario: 中间状态

- **WHEN** 第 1 次结果为 6552，状态为 Between
- **THEN** 提示为"第 1 次：6552 计数"和"准星重新对准参照物，按 Ctrl+Alt+F7 开始第 2 次"

#### Scenario: 自定义热键

- **GIVEN** `calibrate=Shift+F7`
- **WHEN** 进入 Run1
- **THEN** 提示第一行以"然后再按 Shift+F7"结尾
