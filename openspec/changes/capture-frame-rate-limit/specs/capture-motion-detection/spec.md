# Spec Delta

## ADDED Requirements

### Requirement: 处理帧限速
截屏线程 SHALL 照常取到每个桌面帧，但只把其中一部分新帧（不含只有指针更新的帧）作为"处理帧"送去降采样与估计。判定按帧的呈现时间（DDA `LastPresentTime`）：设 G 为本帧距上一处理帧的时间、g 为本帧距上一到达的新帧的时间，`G + g/2 ≥ 13.75 ms` 时本帧 SHALL 被处理，否则 SHALL 被跳过。首帧、呈现时间无效（非有限或 ≤ 0）或不递增的帧 SHALL 被处理，并作为之后判定的起点；采集建立或重建、释放、暂停后恢复时 SHALL 重新开始判定（下一个新帧一定被处理）。对均匀帧流，60 / 75 / 90 / 100 Hz SHALL 不跳过任何帧；更高刷新率下处理帧率 SHALL 落在约 55–109 次 / 秒之间（120 / 144 / 165 / 240 Hz 分别为 60 / 72 / 82.5 / 80 次 / 秒）。

#### Scenario: 60 Hz 帧流不跳帧
- **GIVEN** 60 Hz 显示器上游戏以 60 fps 运行，每帧呈现时间有 ±2 ms 的抖动
- **WHEN** 截屏连续取帧 10 s
- **THEN** 每个新帧都被处理，被跳过的帧数为 0

#### Scenario: 240 Hz 帧流
- **GIVEN** 240 Hz 显示器上游戏以 240 fps 运行，画面匀速平移
- **WHEN** 截屏连续取帧 10 s
- **THEN** 每 3 个新帧处理 1 个，约 80 帧 / 秒，其余约 160 帧 / 秒被跳过
- **AND** 截屏角速度按相邻处理帧的呈现时间差（约 12.5 ms）换算，不因跳帧而偏小

#### Scenario: 暂停后恢复
- **GIVEN** 截屏线程在 144 Hz 帧流上取帧后因切出游戏而暂停
- **WHEN** 回到游戏、恢复取帧
- **THEN** 恢复后的第一个新帧被处理（只作为参考帧），之后按上述规则判定

### Requirement: 被跳过的帧
按「处理帧限速」被跳过的帧 SHALL 立即释放，不降采样、不参与任何估计，参考帧与历史帧保持不变；下一处理帧与参考帧的时间差（dt）按呈现时间计算，自然覆盖被跳过的时间。被跳过的帧 SHALL NOT 视为"帧未变"（不计为取帧超时或只有指针更新），但发布值照常按"低置信度时保持与归零"的规则刷新，不因跳帧而变为 0。

#### Scenario: 跳过的帧不归零
- **GIVEN** 240 Hz 帧流，上一处理帧测得 200 °/s
- **WHEN** 接下来的两个新帧被跳过
- **THEN** 这期间发布值保持 200 °/s，超时计数与只有指针更新的计数都不增加

## MODIFIED Requirements

### Requirement: GPU 降采样为灰度网格
每个处理帧（见「处理帧限速」；被跳过的帧不降采样）SHALL 在 GPU 上降采样为 `grid_w × grid_h`（默认 160×90）的单通道 32 位浮点灰度图，灰度 = 0.299R + 0.587G + 0.114B，取值 0..1。每格的值 SHALL 是该格在屏幕上覆盖区域的平均（滤波覆盖整个格子区域，而不是单点采样）。结果 SHALL 读回到 CPU（默认每帧 160×90×4 = 57 600 字节），读回过程中截屏线程 SHALL NOT 忙等 GPU。

#### Scenario: 均匀灰色桌面
- **GIVEN** 整个桌面为 RGB(128,128,128)
- **WHEN** 降采样一帧
- **THEN** 每个格子的值都在 0.502 ± 0.01 内

#### Scenario: 细条纹不混叠
- **GIVEN** 1920×1080 桌面为 1 px 宽的黑白交替竖条纹
- **WHEN** 降采样到 160×90
- **THEN** 所有格子的值都在 0.5 ± 0.05 内

#### Scenario: 被跳过的帧不降采样
- **GIVEN** 240 Hz 帧流
- **WHEN** 一个新帧按「处理帧限速」被跳过
- **THEN** 该帧没有 GPU 降采样与读回，估计器也看不到它

### Requirement: 截屏性能日志
日志级别为 debug（包括 `--verbose`）且截屏线程正在取帧时，系统 SHALL 每 5 s 写一行 `capture perf: frames=<n> timeouts=<n> lowconf=<n> gpu_ms=<x.xxx> cpu_ms=<x.xxx> cpu_pct=<x.xx> speed_avg=<x.x> skipped=<n>`：frames 为这 5 s 内读回并估计的处理帧数（不含被跳过的帧），timeouts 为取帧超时次数，lowconf 为低置信度 / 无纹理的估计数，gpu_ms 为降采样的平均 GPU 耗时（GPU 计时不可用时为 -1），cpu_ms 为每个处理帧读回 + 估计的平均 CPU 耗时，cpu_pct 为截屏线程这 5 s 的单核 CPU 占比，speed_avg 为平均发布值（°/s），skipped 为按「处理帧限速」跳过的新帧数。`skipped` SHALL 是行尾的最后一个字段，其余字段的顺序与写法不变。日志级别高于 debug 时 SHALL NOT 输出该行。

#### Scenario: verbose 模式
- **WHEN** 以 `--verbose` 启动，在已知游戏中停留 12 s
- **THEN** 日志中至少有 2 行以 `capture perf:` 开头，且每行都按上述顺序包含 8 个字段，以 ` skipped=<n>` 结尾

#### Scenario: 60 Hz 显示器
- **WHEN** 在 60 Hz 显示器上的已知游戏中以 `--verbose` 运行
- **THEN** 每行 `capture perf:` 都是 `skipped=0`

#### Scenario: 高刷新率显示器
- **GIVEN** 240 Hz 显示器，游戏以 240 fps 运行
- **WHEN** 查看一行 `capture perf:`
- **THEN** `frames` 约为 400（5 s × 80 帧 / 秒），`skipped` 约为 800，`frames + skipped` 约为这 5 s 取到的新帧数

#### Scenario: info 级别
- **WHEN** `log_level=info` 且未带 `--verbose`
- **THEN** 日志中没有以 `capture perf:` 开头的行

#### Scenario: 暂停时不输出
- **WHEN** 截屏线程处于暂停状态（不在已知游戏里）
- **THEN** 不输出 `capture perf:` 行

### Requirement: 历史帧与比较基线
系统 SHALL 保留最近若干个处理帧（至少 20 帧，被「处理帧限速」跳过的帧不进入历史）带呈现时间戳的灰度网格，并用当前帧与年龄最接近 `[capture] baseline_ms`（默认 66 ms）的历史帧比较；`baseline_ms = 0` 时 SHALL 只与上一处理帧比较。参考帧与当前帧之间的每一对相邻帧都必须有效（间隔 ≤ 250 ms、有纹理、非闪光、帧间估计有效），否则 SHALL 改用更近的、链路完整的帧。重建采集、换屏、暂停后恢复与网格尺寸变化时 SHALL 清空历史。

#### Scenario: 60 Hz 下的基线
- **GIVEN** 60 fps 连续画面，`baseline_ms=66`
- **WHEN** 第 8 帧到达
- **THEN** 参考帧是 4 帧之前（66.7 ms），dt = 4/60 s

#### Scenario: 240 Hz 下的基线
- **GIVEN** 240 fps 连续画面（每 3 帧处理 1 帧，处理间隔 12.5 ms），`baseline_ms=66`
- **WHEN** 第 8 个处理帧到达
- **THEN** 参考帧是 5 个处理帧之前（62.5 ms），dt = 0.0625 s

#### Scenario: 只与上一帧比较
- **GIVEN** `baseline_ms=0`
- **WHEN** 新的处理帧到达
- **THEN** 参考帧是上一处理帧（高刷新率显示器上不是上一个到达的桌面帧）

#### Scenario: 链路中断
- **GIVEN** 3 帧之前有一帧整屏纯白
- **WHEN** 当前帧到达
- **THEN** 参考帧只能是纯白帧之后的帧，不跨过纯白帧比较
