# Spec Delta

## MODIFIED Requirements

### Requirement: 结果计算、保存与生效

结算时系统 SHALL 计算 `counts_per_degree = ((c1 + c2) / 2) / 360`，写入 `[calibration] <校准对象 exe>=<值>;<YYYY-MM-DD>`（值 3 位小数，日期为结算时的本地日期；已有同名条目则连同旧日期一起覆盖），立即保存 INI，并且从下一个 tick 起强度模型就使用这个值。随后弹 toast `校准完成：每度 {counts_per_degree，1 位小数} 计数`（不附等效灵敏度：按 Source 系视角系数换算的"灵敏度"对其他游戏没有意义，会误导玩家）。设置窗口打开时，其校准页 SHALL 在 1 个 tick 内刷新。

#### Scenario: 两次都是 6552

- **GIVEN** 本地日期为 2026-11-03
- **WHEN** c1 = c2 = 6552
- **THEN** INI 中为 `fakegame.exe=18.200;2026-11-03`
- **AND** toast 为"校准完成：每度 18.2 计数"

#### Scenario: 两次都是 13104

- **GIVEN** 本地日期为 2026-11-03
- **WHEN** c1 = c2 = 13104
- **THEN** INI 中为 `fakegame.exe=36.400;2026-11-03`，toast 为"校准完成：每度 36.4 计数"

#### Scenario: 取平均

- **WHEN** c1 = 6552，c2 = 6700
- **THEN** counts_per_degree = 6626 / 360 ≈ 18.406，INI 中的值部分为 `18.406`，toast 显示"每度 18.4 计数"

#### Scenario: 重新校准覆盖旧值

- **GIVEN** INI 中已有 `cs2.exe=18.200`（旧格式，无日期）
- **WHEN** 2026-11-03 对 cs2.exe 再校准，得到 20.000
- **THEN** INI 中只剩一条 `cs2.exe=20.000;2026-11-03`

#### Scenario: 设置窗口打开时完成校准

- **GIVEN** 设置窗口停在校准页
- **WHEN** 完成 valorant.exe 的校准
- **THEN** 列表在 1 个 tick 内出现或更新 valorant.exe 一行，日期为当天

## ADDED Requirements

### Requirement: 在设置窗口删除校准记录
设置窗口校准页确认删除某条记录后，系统 SHALL 从 `[calibration]` 中移除该 exe 的条目，该 exe 从下一个 tick 起改用 `[trigger] default_counts_per_degree`；托盘"校准列表"随之不再显示该项。删除按设置窗口改动的保存时机写入 INI，并写一条 info 日志；不弹 toast（确认框与列表变化已是反馈）。

#### Scenario: 删除后立即改用默认值
- **GIVEN** cs2.exe 的校准为每度 9.100 计数，默认值为 18.2
- **WHEN** 在设置窗口删除 cs2.exe 的记录后回到 cs2.exe，以同样速度转动鼠标
- **THEN** 算出的鼠标角速度是删除前的一半；托盘"校准列表"中没有 cs2.exe

### Requirement: 从设置窗口发起重测
设置窗口的"重测"SHALL NOT 改变校准状态机，也不直接开始记录；它只给出操作指引（切回该游戏、锁定视角后按 `calibrate` 热键），实际校准仍按「开始条件」与后续流程进行，结算时按「结果计算、保存与生效」覆盖旧记录。校准状态不是 Idle 时，设置窗口的"重测"按钮 SHALL 灰显。

#### Scenario: 重测后覆盖
- **GIVEN** cs2.exe 已有记录 `18.400;2026-10-01`，用户在设置窗口点了"重测"
- **WHEN** 用户切回 cs2.exe、锁定视角，按 Ctrl+Alt+F7 完成两次转圈，得到每度 17.900 计数
- **THEN** INI 中 cs2.exe 的记录变为 `17.900;<当天日期>`，设置窗口列表同步显示 17.90 与当天日期

#### Scenario: 校准进行中
- **GIVEN** 校准状态为 Run1
- **WHEN** 打开设置窗口校准页并选中一行
- **THEN** "重测"按钮灰显，"删除"可用
