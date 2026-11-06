# Design

## Context

动机见 proposal.md，行为契约见 `specs/`。本文只讲实现方式。

本变更建立在 v1.0 的四个已归档变更之上，可以直接使用：

- 截屏线程 `CaptureService`（`src/capture/desktop_dup.*`）：DXGI 桌面复制 → GPU 降采样为 `grid_w × grid_h` 的 `R32_FLOAT` 灰度 → CPU 读回 → `MotionEstimator`（投影互相关平移）→ `SpeedHold`（保持 / 归零）→ 原子 `double` 发布。
- 强度模型 `IntensityModel`（`src/model/intensity.*`）的截屏项：`smoothstep(k·v_start, k·v_full, capture_deg_per_s)`，`gate_capture` 门控，max 合成；`v_start / v_full` 的三档表与载入时的"开始 ≥ 满强度 → 该档恢复默认"规则。
- 模式表驱动的配置（`src/config/config.cpp` 的 `kSchema`）与模板化保存；设置窗口的通用绑定（`add_slider` / `add_check`，范围来自 `config_key_range`）。
- fakegame（`--texture noise`、滚动窗口、闪白）、capturetest、`scripts/verify.ps1 -Capture`。

约束：C++20 / MSVC `/W4` 零警告、零第三方依赖、纯逻辑模块不含 Win32、截屏线程不忙等、主线程不持锁；整进程 CPU 目标 ≤ 单核 2%（60 fps、截屏开启）。

## Goals / Non-Goals

**Goals:**
- 在已有 160×90 网格上测出前进 / 后退（缩放）与倾斜（旋转），单位与现有阈值一致（°/s），走同一套保持规则；阈值各有一张三档表（用户决定，见 D7）。
- 对静态 HUD、局部视差、闪光、换场稳健；纯平移不产生"前进"。
- 平移路径的行为不变（`verify.ps1 -Capture` 的滚动中位数约 400 °/s、闪白不触发）。
- 每帧开销足够小，使整进程在常见情形下仍 ≤ 2%。

**Non-Goals:**
- 不做按游戏的配置档案、不识别真实 FOV、不做局部运动分割或透视 / 仿射模型。
- 不用估计出的平移替代投影互相关的平移结果（两条路径独立，平移项保持 v1.0 行为）。
- 不区分前进与后退、左倾与右倾（只用绝对值）。

## Decisions

### D1. 模块划分

| 文件 | 职责 |
|---|---|
| `src/capture/similarity_estimator.h/.cpp`（新） | 纯逻辑：历史帧环、两层金字塔、稀疏采样点、鲁棒反向组合 LK、置信度、`zoom_to_deg_per_s` / `roll_to_deg_per_s` |
| `src/capture/motion_estimator.*` | `SpeedHold::on_value(optional<double>, t)`：已换算好的测量值走同一保持 / 归零规则 |
| `src/capture/desktop_dup.*` | 截屏线程里在 `MotionEstimator::push` 之后调用 `SimilarityEstimator::push`（传入平移提示），两个新 `SpeedHold`，原子 `zoom` / `roll`，`CaptureDebug` 新字段 |
| `src/capture/capture_policy.*` | `CaptureParams` 加 `detect_roll`、`baseline_ms` 与收拢规则 |
| `src/model/intensity.*` | `IntensityInputs.capture_zoom_deg_per_s / capture_roll_deg_per_s`、`IntensityTerms.capture_zoom / capture_roll`、`TriggerConfig.forward_gain / zoom_thresholds / roll_thresholds`、`thresholds_for(Sensitivity, table)` |
| `src/config/*`、模板 INI | 3 个新键 + 12 个阈值键（模式表 + 模板注释）；阈值顺序校验扩展到三张表 |
| `src/ui/pages/page_trigger.cpp`、`settings_logic.*` | 两个控件 + 两张阈值表；读数类型 `Readout::Gain`（"1.0 倍"） |
| `src/app/app.*` | tick 读取、调试条、frame 行尾、启动日志 |

### D2. 相似变换模型与坐标

内容运动写成复数形式：当前帧坐标 p 映射到参考帧 q = a·p + b（a、b 为复数，a = (1/s)·e^{−iθ}）。坐标以网格中心为原点、y 向下，y 先乘 `pixel_aspect = (tex_h/grid_h)/(tex_w/grid_w)`，使屏幕上的正方形在坐标中也是正方形（1080p → 160×90 时为 1）。对 (a, b) 是线性的四参数模型，组合 / 求逆都是闭式的；结果 s = |1/a|，θ = arg(1/a)，t = −b/a。

### D3. 鲁棒反向组合 Lucas–Kanade

- 模板 = 当前帧（归一化为零均值、单位标准差，整体增益 / 偏移不影响残差），参考帧在映射位置双线性采样。最速下降向量 sd = ∇T·∂W/∂(da, db, dtx, dty) = [gx·x + gy·y, −gx·y + gy·x, gx, gy] 每帧只在采样点上算一次；每次迭代：残差 → |残差| 直方图中位数 → σ = max(1.4826·MAD, 0.02) → 权重（粗层 Huber k = 1.345σ，细层 Tukey c = 4.685σ）→ 4×4 法方程（Cholesky，主元过小 = 退化）→ W ← W ∘ W(Δ)⁻¹。
- 两层金字塔：细层 160×90，粗层 2×2 盒式平均 80×45；两层都用细层格宽为坐标单位，参数无需换算。
- 稀疏采样（省算力，精度几乎不变）：粗层取奇数行上的棋盘格（约 840 点），细层取奇数行奇数列子格上的棋盘格（约 1740 点）；法方程第 2 遍用 SSE2 一次 4 点、float 累加每 256 点并入 double。
- 落在参考帧外的点不参与；重叠比例 < 15% 或有效点 < 64 时放弃。

### D4. 历史环、短基线链与长基线

- 环保存 20 帧（归一化细层 + 粗层 + 时间戳 + 均值 / 标准差 + 到上一帧的"短基线映射"）。
- 每帧先在粗层估计当前帧 → 上一帧的短基线映射（≤ 3 次迭代）。候选初值：投影互相关的平移提示（a = 1，b = −(dx, dy·asp)）、上一帧的短基线映射（匀速假设）、没有提示时的恒等；两候选几乎相同时只留一个，否则只做残差打分取 |残差| 中位数最小者。
- 沿有效的短基线链往回走，取年龄最接近 `baseline_ms` 的帧 R（年龄 ≤ 250 ms、链上每一环有效），初值 = 链上映射的组合（复数乘法）。
- 若链组合出的运动很小（前进流速 < 4 °/s 且倾斜 < 4 °/s，按当前 HFOV 换成 ln s 速率；约为前进 / 倾斜最低一档起点的一半），直接发布链的结果（不精化）；否则粗层 1 次 Huber → 细层 ≤ 3 次 Tukey 精化；与 R 精化失败时退回上一帧（细层）。
- 这样每帧约 0.3% 的缩放在 66 ms 基线上累计到约 1.2%，细层可测；大幅运动（每帧 ×1.11 或 6°）也能靠链初值收敛。

### D5. 有效性与置信度

- 无纹理（灰度标准差 < 0.005）、与上一帧灰度均值差 > 0.08 或标准差之比超出 [0.67, 1.5]（闪光、换场、亮度突变）→ 当帧无效，且链在此断开。
- 置信度 = 鲁棒加权 NCC × min(1, 重叠/0.35) × min(1, 内点/0.5)；< `min_correlation`（与平移共用）→ 无效。换图时 MAD 尺度变大、几乎所有点都被纳入，NCC ≈ 0.07 → 无效。
- 无效结果的 s、θ、置信度都是有限值。

### D6. 换算、保持与发布

- `zoom_to_deg_per_s(s, dt, hfov) = |ln s| / dt × hfov / 2`，`roll_to_deg_per_s(θ, dt) = |θ| / dt`；dt ∉ (0, 0.25] → nullopt。
- 截屏线程每个新帧：`zoom_hold.on_value(valid ? zoom : nullopt, t)`、`roll_hold.on_value(...)`；`publish()` 同时写 `speed` / `zoom` / `roll` 与同一个 `speed_t`；`detect_roll=0` 时 roll 发布 0。建立后丢弃的 2 帧不更新保持值；`enter_idle` / 重建 / 换屏 / 网格变化时清空历史环与保持值。
- 主线程 `zoom_speed(now)` / `roll_speed(now)` 与 `speed(now)` 同一过期规则（状态非 Running / Recovering 或 > 250 ms → 0）。

### D7. 强度模型

`compute_target` 中 `gate_capture` 为真时：`t.capture_zoom = smoothstep(zs, zf, forward_gain × zoom)`、`t.capture_roll = smoothstep(rs, rf, roll)`，其中 `(zs, zf) = zoom_thresholds[sensitivity]`、`(rs, rf) = roll_thresholds[sensitivity]`，**不乘** `capture_sensitivity` 的保守系数 k（k 只作用于平移项）；max 合成加入两项。`forward_gain`（默认 1.0）与两张表存在 `TriggerConfig`（模型的配置结构），INI 键在 `[capture]`。

默认三档（°/s）：

| 灵敏度 | 前进 开始 / 满强度 | 倾斜 开始 / 满强度 |
|---|---|---|
| 低 | 40 / 250 | 30 / 150 |
| 中（默认） | 20 / 150 | 15 / 90 |
| 高 | 10 / 83 | 8 / 50 |

中档由用户定；低 / 高档沿用鼠标阈值三档之间的比例（鼠标 120 / 600、60 / 360、30 / 200：开始 ×2 / ×0.5，满强度 ×5/3 / ×5/9），取整到整数。

### D8. 配置

模式表追加 `MC_DBL("capture","forward_gain",trigger.forward_gain,0.5,3.0,1)`、`MC_BOOL("capture","detect_roll",capture.detect_roll)`、`MC_INT("capture","baseline_ms",capture.baseline_ms,0,150)`，以及 12 行 `MC_DBL("capture","zoom_start_<档>",trigger.zoom_thresholds[i].v_start,1,5000,0)` 等（`zoom_full_*`、`roll_start_*`、`roll_full_*`）；模板在 `hold_ms` 之后加三行带注释的键，`baseline_ms` 之后加两行说明注释与 12 个带注释的阈值键（UTF-8 BOM + CRLF 不变）。`load_config` 末尾的"某档开始 ≥ 满强度 → 该档两个值恢复默认并记一条 warn"扩展到三张表；`config_thresholds_ordered` 同样检查三张表（设置窗口写入时兜底）。`detect_roll` / `baseline_ms` 经 `update_params` 进截屏线程（`baseline_ms` 或网格、`min_correlation` 变化时重新配置估计器并清空历史；HFOV 变化只更新省算力门槛）。

### D9. 界面与诊断

- 触发页"截屏检测细节"开头加滑块"前进 / 后退的灵敏度(&R)"（步长 0.1、一位小数、`Readout::Gain` → "1.0 倍"）与"画面倾斜(&T)：识别画面倾斜"复选框，各带一行辅助说明（docs/settings-ui.md 的文案规则）；其后是前进 / 倾斜两张阈值表（与"开始变暗 / 完全变暗的转速"同一版式：表头一行 + 低 / 中 / 高三行，每行两个数值框，最多一位小数，开始 < 完全，访问键 &4–&9，控件 ID 1360–1371）和一行辅助说明。
- 调试条：`format_capture_debug_field(..., zoom, roll)` 在 Running 时追加 ` · 前进 N°/s · 倾斜 M°/s`。
- frame 行：在 exe（及可选的 preview）之后追加 ` zoom=%.1f roll=%.1f`。verify.ps1 现有正则 `exe=(?<exe>\S*)` 只取到空格，不受影响。
- capturetest：每秒行追加 `sim_ms simok zoom roll`，汇总行追加 `sim_ms=` 与有效帧数；`CaptureDebug` 新增 `scale / rotation_deg / sim_confidence / sim_valid / sim_dt_s / zoom_deg_per_s / roll_deg_per_s / sim_ms_sum / sim_valid_count`。

### D10. fakegame 场景模式与离线自检

- `--zoom r`（d(ln s)/dt，1/s）、`--zoom-start/--zoom-seconds`、`--roll deg/s`、`--roll-start/--roll-seconds`；任一给出即进入场景模式（忽略 `--texture` 的底图与 `--scroll`）。
- 场景：Shepard 式滚动八度——八度 k 的屏幕周期 = 48·S/2^k（S = 当前放大倍数，低分辨率像素），权重按 u = k − log2 S 取高斯，只算 |u − 0.5| ≤ 2.6 的约 5 个八度；每个八度 = 查表值噪声 + 每格至多一个软边圆斑。屏幕上的频谱与时间无关（无限放大不会"用完"细节），每个八度本身严格按相似变换运动。旋转绕屏幕中心。
- 1/6 分辨率（1080p → 320×180）逐像素计算，`StretchDIBits`（COLORONCOLOR）拉伸到全屏；1080p 下约 3.5–4 ms / 帧。
- 时间窗开始 / 结束打印 `zoom start/stop`、`roll start/stop` 行（带 `wall=`）。
- `--selfcheck`：不开窗，以 60 fps 时间戳离线渲染放大段、旋转段、静止段各 90 帧，2×2 盒式平均成 160×90 灰度，依次交给真实的 `MotionEstimator` 与 `SimilarityEstimator`（门槛与 CaptureService 相同），打印估计 / 指令中位数与耗时，都在 ±25% 内（没有指令的一项 < 5 °/s）退出码 0。不给 `--zoom` / `--roll` 时依次跑日常游动量级 `--zoom 2`（90 °/s）、`--roll 60`，再跑大幅的 `--zoom 6.5`、`--roll 360` 与静止段。

### D11. verify.ps1 -Forward

新 INI（默认配置）+ `--verbose --force-game --exit-after 17`；fakegame `--zoom 3`（2–5 s）后 `--roll 75`（7–10 s），不发模拟输入。判定：放大前强度 ≤ 0.05；放大段最大强度 > 0.5、mouse=0.0、`zoom=` 中位数在 3×45 = 135 的 ±25%；放大结束 1 s 后到旋转开始 ≤ 0.05；旋转段最大强度 > 0.5、`roll=` 中位数在 75 的 ±25%；打印两段的 `capture=`（平移）与交叉项。速度取值：135 °/s 是快速游动 / 冲刺量级，中档目标强度约 0.96（估计偏低 25% 时约 0.68）；75 °/s 倾斜约 0.90（偏低 25% 时约 0.57）。

### D12. 性能

| 情形 | 估计器每帧（release，单元测试 / 自检） |
|---|---|
| 静止 / 纯平移（走链结果） | ≈ 0.03 ms |
| 持续缩放 + 旋转 + 平移（每帧精化） | ≈ 0.10 ms |
| fakegame 自检放大 / 旋转段 | 见 tasks.md 实测 |

60 fps 下精化路径约 +0.6% 单核，非精化路径约 +0.2%。

## Risks / Trade-offs

- [独立阈值偏低带来的误触发] 前进 / 倾斜的起点（中档 20 / 15 °/s）远低于平移项（90 °/s），估计器的串扰会更容易越过起点：自检中纯平移的前进 / 倾斜串扰约 0.1–0.4 °/s，放大段的倾斜串扰约 0.4–1.4 °/s、旋转段的前进串扰约 0.1–0.2 °/s，都远低于起点；但真实游戏里的局部运动（见下）可能产生十几 °/s 的小值，在高灵敏度（前进 10、倾斜 8 °/s 起）下会出现轻微变暗。
- [大面积平滑画面（雾、水体）] 纹理不足时 LK 倾向于低估缩放（偏向"没动"），方向上是保守的（少触发而不是误触发）。
- [局部运动] 画面只有一部分在动（例如桌面上滚动的终端）时，估计介于各部分之间，可能出现十几 °/s 的小值；低于中档起点或刚越过起点（强度 < 0.05）。
- [投影互相关在缩放 / 旋转时给出假平移] 自检中放大段平移中位数约 75 °/s、旋转段约 58 °/s（仍低于平移项起点 90 °/s）；平移路径本身未改，按非目标保留。
- [精化门槛] 链结果在门槛（4 °/s）以下误差约 ±25%；只有用户把阈值调到 5 °/s 附近时才会影响强度。门槛从 8 / 20 °/s 降到 4 / 4 °/s 后，轻微前进 / 倾斜时精化更频繁，估计器每帧开销在这些帧上从约 0.03 ms 升到约 0.1 ms。
- [CPU] 持续剧烈前进 / 倾斜时估计器约 0.1–0.2 ms / 帧；如实测整进程超过 2%，可改为隔帧精化（Open Questions）。

## Migration Plan

旧 INI 缺少新键（3 个 + 12 个阈值键）→ 默认值、不记 warn；下次保存按模板补写。v1.1 初版已经写出 `forward_gain=1.5` 的 INI 保留用户值 1.5（不自动改成新默认 1.0）。版本号改为 1.1.0。

## Open Questions

- 持续前进时整进程 CPU 若超过 2%，是否接受"精化隔帧"（输出在精化值与链值之间交替，精度略降）。
