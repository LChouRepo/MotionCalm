# Proposal

## Why

v1.0 的截屏检测只估计全局**平移**（列 / 行投影互相关），只能发现转视角、横向平移一类的画面运动。用户反馈：朋友在 Subnautica 里主动游动时仍然头晕——向前 / 向后游产生的是从画面中心向外（或向内）的放射状"扩张"光流，投影互相关看不到它；WASD 只给固定的 0.35；Subnautica 在水中侧移时还会让镜头左右倾斜（roll），画面整体旋转，同样不会触发。本变更（变更 5，v1.1）在已有的桌面复制截屏上增加**前进 / 后退（缩放）与倾斜（旋转）**检测，换算成与现有阈值同一量纲的 °/s 交给强度模型。

## What Changes

- 新增纯逻辑的全局相似变换估计（平移 + 缩放 + 旋转）：在现有 160×90 灰度网格上做鲁棒参数化 Lucas–Kanade（反向组合 Gauss–Newton、两层金字塔 160×90 + 80×45、Huber / Tukey 权重，让静态 HUD 与视差成为离群点），初值来自现有投影互相关的平移结果。
- 与约 66 ms 之前的历史帧比较（保留带时间戳的历史网格环，取年龄最接近基线的一帧），每帧约 0.3% 的缩放也能测出；置信度来自鲁棒加权的归一化互相关、重叠比例与内点比例；低置信度、闪光 / 换场 → 无效 → 与平移相同的保持 / 归零规则。
- 换算：前进 / 后退 = 画面水平边缘的径向流速 `zoom_deg_per_s = |ln s| / Δt × (assumed_hfov_deg / 2)`；倾斜 `roll_deg_per_s = |Δθ| / Δt`。截屏线程以无锁原子量发布两者（与平移角速度同一发布时刻、同一过期规则）。
- 强度模型：新增前进项 `smoothstep(zoom_start, zoom_full, forward_gain × zoom)` 与倾斜项 `smoothstep(roll_start, roll_full, roll)`。两项有**自己的阈值**（不用鼠标 / 截屏阈值，也不乘 `capture_sensitivity` 的保守系数），按 `[general] sensitivity` 选档：前进中档 20 / 150 °/s、倾斜中档 15 / 90 °/s；低 / 高档按鼠标阈值三档的比例（开始 ×2 / ×0.5、满强度 ×5/3 / ×5/9，取整）：前进 40 / 250、10 / 83，倾斜 30 / 150、8 / 50。两项受 `gate_capture` 门控，与所有项取 max。WASD 项（0.35）保留；截屏关闭时 WASD 照常有效。
- 配置 `[capture]` 新增 `forward_gain=1.0`（0.5–3.0）、`detect_roll=1`、`baseline_ms=66`（0–150，0 = 只与上一帧比较），以及 12 个阈值键 `zoom_start_low/medium/high`、`zoom_full_*`、`roll_start_*`、`roll_full_*`（1–5000 °/s，每档开始 < 满强度，规则同 `[trigger] v_start_* / v_full_*`）。
- 设置窗口"触发"页高级区"截屏检测细节"新增滑块"前进 / 后退的灵敏度"（0.5–3.0 倍，步长 0.1，默认"1.0 倍"）、复选框"识别画面倾斜"，以及前进 / 倾斜两张"3 档 × 开始 / 完全变暗"阈值表。
- 调试条截屏字段在取帧时追加 ` · 前进 N°/s · 倾斜 M°/s`；`frame` 日志行末尾追加 ` zoom=<°/s> roll=<°/s>`（验证脚本的现有解析不受影响）。
- 测试工具：fakegame 新增 `--zoom` / `--roll` 场景模式（随缩放滚动的多八度程序化场景，1/6 分辨率逐像素渲染后拉伸到全屏）与不开窗的 `--selfcheck`；capturetest 打印估计器的每帧耗时；`scripts/verify.ps1 -Forward` 新场景。
- 版本号 1.1.0（资源版本信息、`scripts/package.ps1` 默认版本）。

不在范围内：按游戏的配置档案（per-game profiles）、真实 FOV 识别、局部运动分割、透视 / 仿射形变。

## Capabilities

### New Capabilities

（无）

### Modified Capabilities

- `capture-motion-detection`: ADDED 相似变换估计（前进 / 倾斜）、历史帧与比较基线、前进 / 倾斜的有效性、前进流速与倾斜角速度换算、保持与归零、交给主线程、`detect_roll`、单元测试、性能预算。
- `motion-trigger`: ADDED 前进项与倾斜项（各自的阈值、`forward_gain`、`gate_capture`）；MODIFIED「目标强度合成」（max 合成加入两项）。
- `configuration`: MODIFIED「[capture] 节」（3 个新键 + 12 个阈值键）、「截屏配置值的校验」（新键范围、阈值顺序）、「调试帧日志」（行尾 `zoom=` / `roll=`）。
- `settings-window`: MODIFIED「触发页截屏检测高级参数」（两项新控件 + 两张阈值表）。
- `overlay-rendering`: MODIFIED「调试条」（截屏字段追加前进 / 倾斜）。

## Impact

- 新文件：`src/capture/similarity_estimator.h/.cpp`、`tests/test_similarity_estimator.cpp`。
- 修改：`src/capture/desktop_dup.*`（截屏线程接入、原子发布、`CaptureDebug`）、`src/capture/motion_estimator.*`（`SpeedHold::on_value`）、`src/capture/capture_policy.*`（`detect_roll`、`baseline_ms`）、`src/model/intensity.*`、`src/config/config.*`、`src/res/MotionCalm.default.ini`、`src/res/resource.h`、`src/ui/pages/page_trigger.cpp`、`src/ui/settings_logic.*`、`src/app/app.*`、`tools/fakegame`、`tools/capturetest`、`tools/settingsdemo`、`scripts/build_tools.cmd`、`scripts/verify.ps1`、`scripts/package.ps1` 与相关单元测试。
- 依赖：无新增（SSE2 为 x64 基线指令集）。
- 性能：估计器在 160×90 上每帧约 0.03–0.05 ms（静止 / 平移）到约 0.1–0.2 ms（明显的前进 / 倾斜）。

## 用户决定（2026-11-06，已定）

- **前进与倾斜用各自独立的阈值**：初版按"阈值同截屏项"实现后，默认设置下日常游动（前进流速约 45–90 °/s）几乎不触发前进项。用户决定：前进 / 倾斜不再用截屏阈值、也不乘 `capture_sensitivity` 的保守系数，而是各有一张三档阈值表——前进中档 20 / 150 °/s、倾斜中档 15 / 90 °/s，低 / 高档按鼠标阈值三档之间的比例换算（开始 ×2 / ×0.5、满强度 ×5/3 / ×5/9，取整），放在 `[capture]` 的 `zoom_start_* / zoom_full_* / roll_start_* / roll_full_*`；`forward_gain` 默认改为 1.0（范围 0.5–3.0 不变）。默认配置下：前进 45 °/s → 0.10、60 °/s → 0.23、90 °/s → 0.56、135 °/s → 0.96；倾斜 30 °/s → 0.10、60 °/s → 0.65。
- **选档用 `[general] sensitivity`**（与鼠标、手柄同一个"触发灵敏度"），`capture_sensitivity` 只作用于平移项。

## 待确认

- **fakegame / verify.ps1 的验证速度**：`--zoom` 的含义是对数速率 d(ln s)/dt（1/s）。`verify.ps1 -Forward` 默认 `--zoom 3`（135 °/s，相当于快速游动 / 冲刺，中档目标强度约 0.96，估计偏低 25% 时仍约 0.68）与 `--roll 75`（中档约 0.90，偏低 25% 时约 0.57），两段都要求最大强度 > 0.5；可用 `-ForwardRate` / `-RollDegPerS` 改。`fakegame --selfcheck` 默认跑日常游动量级 `--zoom 2`（90 °/s）/ `--roll 60`，再跑 v1.1 初版的大幅速度 `--zoom 6.5` / `--roll 360`。
- **新增 `baseline_ms` 键**：需求说"可配置的基线窗口"，本变更放进 `[capture] baseline_ms=66`（0–150 ms，0 = 只与上一帧比较），不放进设置窗口（属于"一般不用改"的内部参数，与 `max_shift` 同类但更少用）。
- **`forward_gain` 的存放位置**：INI 在 `[capture]`，内部归强度模型使用（`TriggerConfig::forward_gain`）；"恢复默认设置"会把它连同 `[capture]` 一起恢复。
- **复用 `min_correlation` 作为相似变换估计的最低置信度**：不新增置信度键；"画面匹配要求"滑块同时作用于平移与前进 / 倾斜。
- **闪光 / 换场判定**：相邻两帧灰度均值之差 > 0.08 或标准差之比超出 [0.67, 1.5] 时，前进 / 倾斜这一帧直接判为无效（比"亮度突变后仍给出 0"更保守，与平移路径"闪白不触发"一致）。
- **省算力的门槛**：短基线链组合出的运动低于前进流速 4 °/s、倾斜 4 °/s 时不做长基线精化，直接发布链的结果（误差约 ±25%，约为前进 / 倾斜最低一档起点 10 / 8 °/s 的一半）；只有在用户把阈值调到这附近时才会影响精度。
- **CPU 预算**：实测见 tasks.md；平移 / 静止时估计器每帧约 0.03–0.05 ms，强烈前进 / 倾斜时约 0.1–0.2 ms（60 fps 下约 +0.6–1.2% 单核）。若"整进程 ≤ 2%"在持续前进时超出，可再改为"精化隔帧进行"。
- **调试条与 `frame` 行的 `zoom=` 显示未乘 `forward_gain` 的原始流速**（与 `capture=` 不乘 `capture_sensitivity` 系数一致），`forward_gain` 只影响强度。
- **旋转显示器**：相似变换在扫描输出方向的网格上估计；|ln s| 与 |θ| 与方向无关，只用于强度，不区分旋转方向的正负。
