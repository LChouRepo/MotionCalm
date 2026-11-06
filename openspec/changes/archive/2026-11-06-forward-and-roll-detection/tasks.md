# Tasks

## 1. 相似变换估计（纯逻辑，可单测）

- [x] 1.1 新建 `src/capture/similarity_estimator.h/.cpp`（不含 Win32）：`SimilarityParams`（尺寸、`pixel_aspect`、`baseline_s`、`min_confidence`、`min_texture`、`max_brightness_jump`、省算力门槛）、`SimilarityEstimate`（valid、scale、rotation_deg、tx、ty、dt_s、confidence、inlier_fraction、frames_back、iterations、refined）、`TranslationHint`、`SimilarityEstimator::push/reset/configure`，以及 `zoom_to_deg_per_s` / `roll_to_deg_per_s`（design D2–D6）。验证：`tests.exe` 链接通过；头文件只依赖标准库。
- [x] 1.2 实现鲁棒反向组合 LK：两层金字塔、稀疏采样点（粗层约 840、细层约 1740）、|残差| 直方图中位数 → σ、Huber（粗层）/ Tukey（细层）、4×4 Cholesky、W ∘ W(Δ)⁻¹、SSE2 法方程累加（design D3）。验证：1.4 的精度用例通过。
- [x] 1.3 实现 20 帧历史环、短基线链（候选初值：平移提示 / 上一帧运动 / 恒等，残差打分）、取最接近 `baseline_s` 的参考帧、链组合初值、小运动直接用链结果、长基线精化与退回上一帧；闪光 / 换场 / 无纹理 / 低置信度判无效（design D4、D5）。验证：1.4 的无效情形与基线用例通过。
- [x] 1.4 编写 `tests/test_similarity_estimator.cpp`（连续程序纹理 3×3 超采样渲染 160×90）：换算公式；缩放 1.02 / 0.98（序列与两帧）；旋转 ±2°；平移 + 缩放 + 旋转；20% 静态 HUD；两层深度；纯平移不误报；静止有效且为 0；fakegame 速度（每帧 ×e^{6.5/60}、6°）；亮度突变 / 纯白 / 闪白后恢复；换图；纯色与一维条纹无 NaN；400 ms 间隔与 reset；`baseline_s=0`；非正方形格子；小运动走链结果；每帧耗时（release 断言 < 0.5 ms，静止 / 平移 < 0.15 ms）。验证：`build.cmd tests release` 全部通过。
  - 实测（release）：ln s 误差 1.02 → −1.5%、0.98 → −1.3%；旋转 +2° → −0.2%、−2° → +0.5%；组合：s 1.02995（真值 1.03）、θ 1.486°（1.5°）、t (7.75, −4.05)；HUD：缩放 −3.7%、旋转 −13.0%（≤ 15%）；两层 1.01 / 1.03 → 1.02987；纯平移 6 / −12 格每帧 → 前进 0.10 / 0.24 °/s、倾斜 0.32 / 0.39 °/s；6.5/s → 6.361/s，360 °/s → 359.8 °/s；换图置信度 0.069；每帧 0.099 ms（持续运动）、0.030 ms（静止 / 平移）。

## 2. 截屏线程接入

- [x] 2.1 `SpeedHold::on_value(optional<double>, t)`（与 `on_estimate` 同一规则，负值截到 0、非有限值视为无效）。验证：`tests/test_motion_estimator.cpp` 的 `motion_hold_on_value_rule` 通过。
- [x] 2.2 `CaptureParams` 加 `detect_roll`（默认 1）与 `baseline_ms`（默认 66，收拢到 [0, 150]）。验证：`capture_params_sanitize_clamps_to_spec_ranges` 通过。
- [x] 2.3 `CaptureService` 截屏线程：`MotionEstimator::push` 之后调用 `SimilarityEstimator::push`（平移提示在交换坐标轴之前传入）；`pixel_aspect` 按纹理尺寸与网格算出；两个新 `SpeedHold`；`publish()` 同时写 `speed` / `zoom` / `roll` 与同一时刻；`detect_roll=0` 时 roll 为 0；暂停 / 重建 / 换屏 / 网格或基线变化时清空历史；省算力门槛按 HFOV 设为 8 °/s / 20 °/s；`zoom_speed(now)` / `roll_speed(now)` 与 `speed(now)` 同一过期规则；`CaptureDebug` 加 scale、rotation_deg、sim_confidence、sim_valid、sim_dt_s、zoom / roll 发布值、sim_ms_sum、sim_valid_count（design D6）。验证：`build.cmd all debug/release` 0 警告；capturetest 每秒行显示 `sim_ms simok zoom roll`。（门槛已由 7.4 改为 4 / 4 °/s）
- [x] 2.4 capturetest 打印估计器每帧耗时与有效帧数。验证：`out\release\capturetest.exe --seconds 10` 汇总行有 `sim_ms=`。
  - 实测（本机 1920×1080、60 Hz 桌面，部分区域在动；同一时段先跑 v1.0 HEAD 的 capturetest 作对照）：v1.0 `cpu_ms=0.032`、截屏线程 0.91%、进程 1.01%（最后 1 s）；本变更（稀疏采样前的版本）`sim_ms=0.061`、`cpu_ms=0.086`；最终版本在单元测试与自检中静止 / 平移 0.03 ms、运动 0.09–0.12 ms。按 60 fps 折算新增约 0.2–0.7% 单核，整进程约 1.2–1.7%。最后一次 capturetest 时桌面完全静止（1 fps，帧间隔 > 250 ms 全部无效，`sim_ms=0.022`）。

## 3. 强度模型与配置

- [x] 3.1 `IntensityInputs.capture_zoom_deg_per_s / capture_roll_deg_per_s`、`IntensityTerms.capture_zoom / capture_roll`、`TriggerConfig.forward_gain=1.5`；`gate_capture` 时 t_zoom = smoothstep(k·v_start, k·v_full, gain × zoom)、t_roll = smoothstep(…, roll)，max 合成（design D7）。验证：`tests/test_intensity.cpp` 的 `intensity_forward_term_mapping`（1.02 / 66 ms / 90° → 13.502 °/s → 0；210 → 0.5；gain 3.0 / 0.5；6.5×45 → 0.877）、`intensity_roll_term_and_thresholds`、`intensity_forward_roll_gated_and_max_combined`（门控、与 WASD 取 max、截屏关时 WASD 仍 0.35）通过。（阈值与默认倍数已由 7.1 修订）
- [x] 3.2 模式表加 `[capture] forward_gain`（0.5–3.0，一位小数）、`detect_roll`、`baseline_ms`（0–150）；模板在 `hold_ms` 之后加三行带中文注释的键（UTF-8 BOM + CRLF）。验证：`ini_template_matches_defaults`（[capture] 九个键、每键前有注释）、`ini_forward_and_roll_keys`（旧文件无 warn、截断、非法值、写回往返）、`config_access_get_matches_template_for_every_key`（71 个键）、`config_access_numeric_range_and_clamp_every_key`（数值键 36 个）、`config_access_forward_and_roll_keys`（get / set / 截断 / 恢复默认）通过；默认值保存与模板逐字节相同。

## 4. 设置窗口、调试条、帧日志

- [x] 4.1 触发页"截屏检测细节"加滑块"前进 / 后退的灵敏度(&R)"（步长 0.1、读数 `Readout::Gain` "1.5 倍"）与"画面倾斜(&T)：识别画面倾斜"复选框，各带辅助说明；`resource.h` 新 ID 1356–1358。验证：`settings_slider_spec_ranges_and_steps`（0.5–3.0 / 0.1 → 25 个位置）、`format_readout(…, Gain)` 用例通过；`settingsdemo.exe --selftest` 中新增 6 项（默认位置 10 / "1.5 倍"、拖到最右 3.0、最左 0.5、默认勾选、取消 → 0、再勾选 → 1）全部 PASS。（默认读数已由 7.3 改为"1.0 倍"）
  - `settingsdemo --selftest` 另有 2 项快捷键冲突检查 FAIL（"toggle 行状态：已被其他程序占用"），原因是测试机上另有一个正在运行的 v1.0.0 实例占用了 Ctrl+Alt+F9，与本变更无关。
- [x] 4.2 调试条截屏字段在 Running 时追加 ` · 前进 N°/s · 倾斜 M°/s`；frame 行末尾追加 ` zoom=<1 位小数> roll=<1 位小数>`（在可选的 preview 之后）；启动日志打印 forward_gain / detect_roll / baseline_ms。验证：`capture_debug_field_text` 新用例通过；verify.ps1 / verify_calibration.ps1 现有正则（`exe=(?<exe>\S*)`）不受影响（代码审阅）。

## 5. 测试工具与验证脚本

- [x] 5.1 fakegame `--zoom/--zoom-start/--zoom-seconds`、`--roll/--roll-start/--roll-seconds` 场景模式（Shepard 式滚动八度值噪声 + 软边圆斑，1/6 分辨率逐像素渲染后 `StretchDIBits` 拉伸）；打印 `zoom start/stop`、`roll start/stop`（带 `wall=`）与平均渲染耗时；`scripts/build_tools.cmd` 给 fakegame 链接两个估计器。验证：`fakegame.exe --help` 列出新参数。
- [x] 5.2 fakegame `--selfcheck`（不开窗，离线渲染 → 2×2 盒式平均成 160×90 → 真实估计器，门槛与截屏线程相同）。验证：`out\release\fakegame.exe --selfcheck` 退出码 0。
  - 实测：放大段 `zoom` 中位数 291.6 °/s（指令 292.5），倾斜 1.4；旋转段 `roll` 中位数 359.8 °/s（指令 360），前进 0.2；静止段全部 0.0；三段有效帧 80/80；渲染 3.4–3.9 ms / 帧（320×180）；估计器 0.115 / 0.092 / 0.033 ms / 帧。投影互相关在放大段给出约 74.5 °/s、旋转段约 57.8 °/s 的假平移（低于平移项起点 90 °/s）。
- [x] 5.3 `scripts/verify.ps1 -Forward`（`-ForwardRate` 默认 6.5、`-RollDegPerS` 默认 360）：新 INI、无模拟输入，判定见 design D11；`-Capture` 与基础场景不变。验证：PowerShell 解析无错误。（默认速度已由 7.5 改为 3 / 75）
- [ ] 5.4 实机运行 `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Forward`：放大段强度 > 0.5 且 mouse=0.0、`zoom=` 中位数在 135 ±25%；旋转段强度 > 0.5、`roll=` 中位数在 75 ±25%；静止段 ≤ 0.05（7.5 修订后的默认速度）。 <!-- 未验证：需要用户在场，由主代理运行；运行前须先退出正在运行的 v1.0.0 实例（单实例互斥量会让测试用的 MotionCalm.exe 立即退出） -->
- [ ] 5.5 实机回归 `verify.ps1`（基础）、`verify.ps1 -Capture`（滚动中位数约 400 °/s、闪白不触发）、`verify_calibration.ps1` 不变。 <!-- 未验证：同上，由主代理运行 -->

## 6. 版本与构建

- [x] 6.1 版本号 1.1.0（`resource.h` 的 `MC_VERSION_*`、"MotionCalm v1.1"）；`scripts/package.ps1` 默认 `-Version 1.1.0`。验证：`build.cmd all release` 后资源编译通过。
- [x] 6.2 `build.cmd all debug` 与 `build.cmd all release`：0 警告（/W4），273 个单元测试全部通过。

## 7. 修订：前进 / 倾斜各自的阈值（用户决定，2026-11-06）

- [x] 7.1 `TriggerConfig.zoom_thresholds`（40 / 250、20 / 150、10 / 83）与 `roll_thresholds`（30 / 150、15 / 90、8 / 50），`forward_gain` 默认 1.0；`compute_target` 的前进 / 倾斜项按 `sensitivity` 选档、不乘 k（design D7）。验证：`intensity_forward_term_mapping`（13.5 → 0；60 → 0.2258；85 → 0.5；90 → 0.5576；截屏灵敏度不影响；gain 2.0 / 0.5；低 / 高档中点；135 → 0.963，偏低 25% 仍 > 0.5）、`intensity_roll_term_and_thresholds`（52.5 → 0.5；30 → 0.104；75 → 0.896；选档；自定义表）、`intensity_forward_roll_gated_and_max_combined` 通过。
- [x] 7.2 `[capture]` 12 个阈值键（模式表 1–5000、0 位小数；模板在 `baseline_ms` 之后，带注释）；载入时某档开始 ≥ 满强度 → 该档恢复默认 + warn；`config_thresholds_ordered` 检查三张表（design D8）。验证：`ini_template_matches_defaults`（[capture] 21 个键）、`ini_forward_and_roll_thresholds`（默认、旧文件无 warn、截断 / 非法、顺序错误恢复、往返）、`config_access_get_matches_template_for_every_key`（83 个键）、`config_access_numeric_range_and_clamp_every_key`（数值键 48 个）、`config_access_thresholds_ordered`、`config_access_forward_and_roll_keys` 通过。
- [x] 7.3 触发页"截屏检测细节"加前进 / 倾斜两张阈值表（ID 1360–1371，访问键 &4–&9），校验开始 < 完全；滑块默认读数"1.0 倍"。验证：`settingsdemo --selftest` 新增 5 项（默认 20 / 150、开始改 200 被拒、改 30 生效、倾斜高档满强度默认 50、改 5 被拒）与"默认在位置 5，读数 1.0 倍"PASS。
- [x] 7.4 省算力门槛 8 / 20 °/s → 4 / 4 °/s（CaptureService、fakegame 自检、`similarity_small_motion_uses_chain_estimate` 改为 2.25 °/s 前进、2 °/s 倾斜）；启动日志打印当前档的前进 / 倾斜阈值。验证：`build.cmd tests release` 全部通过。
- [x] 7.5 fakegame `--selfcheck` 默认跑 `--zoom 2`（90 °/s）/ `--roll 60`、再跑 6.5 / 360 与静止段，没有指令的一项须 < 5 °/s；`verify.ps1 -Forward` 默认 `-ForwardRate 3`、`-RollDegPerS 75`，旋转段改为最大强度 > 0.5（design D11）。验证：`fakegame --selfcheck` 退出码 0（zoom 90.0 / 90，roll 60.0 / 60，291.6 / 292.5，359.8 / 360，静止 0.0）；`--selfcheck --zoom 3 --roll 75` → 134.9 / 135、74.9 / 75。
- [x] 7.6 README："前进 / 后退的灵敏度"默认 1.0 倍，"觉得往前游还是晕，就调到 1.5–2 倍"。
- [x] 7.7 实机 `verify.ps1 -Forward`（新默认速度）。 <!-- 未验证：同 5.4，需要先退出用户的 v1.0.0 实例，由主代理运行 -->

## Workflow follow-up

- 主代理请用户退出 v1.0.0 实例后跑 5.4 / 5.5 / 7.7，勾选并记录结果（5.4 的速度已改为 7.5 的新默认值）。
- 实机验证通过后归档：`openspec archive forward-and-roll-detection`。
