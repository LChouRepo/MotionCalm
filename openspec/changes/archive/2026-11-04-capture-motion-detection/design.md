# Design

## Context

动机见 proposal.md 的 Why，行为契约见 `specs/`。本文只讲实现方式。

本变更建立在变更 1 `core-overlay` 之上，可以直接使用：

- 主线程消息循环 + 固定节拍 `tick()`（采样输入 → 游戏判定 → 强度模型 → 组装 `OverlayFrame` → 渲染）。
- `IntensityInputs.capture_deg_per_s` / `gate_capture` 两个接缝，以及 `GameContext.monitor`、`known_game`、`excluded`。
- `Overlay::follow_monitor()` / `set_capture_excluded()`。overlay 自己的 D3D11 设备只在主线程使用。
- `Config` + INI 模板化保存、热键注册表（id / INI 键 / 中文名 / 默认组合键）、托盘菜单、toast、日志、`--no-capture` / `--verbose` / `--force-game` 参数。

约束：C++20 / MSVC、零第三方依赖、`ComPtr` + RAII、不用异常传递业务错误（初始化失败返回 `bool` 并写日志）、禁止忙等、空闲 CPU ≈ 0%。

## Goals / Non-Goals

**Goals:**
- 从新桌面帧到主线程可读到的速度，延迟不超过 2 帧（60 Hz 下约 33 ms），强度模型的 attack 100 ms 仍是主要延迟。
- 对整体亮度的增益 / 偏移变化（闪光、爆炸、昼夜）不敏感。
- 不在游戏里时零开销：不取帧、不占 GPU，5 s 后连桌面复制对象也释放。
- 截屏路径与 overlay 渲染互不阻塞，一边设备丢失不影响另一边。

**Non-Goals:**
- 不估计旋转（roll）、缩放（向前走产生的放射状运动）、透视形变，只估计全局平移。向前移动由键盘项负责。
- 不做局部运动分割（HUD 与画面分别估计），也不按游戏识别真实 FOV（`assumed_hfov_deg` 是全局常数）。
- 不做 HDR 精确亮度；灰度只用于相关计算，相关系数对线性缩放不敏感。
- 不用 Windows.Graphics.Capture：它要求 WinRT 投影 / C++/WinRT，而且会在被采集窗口周围显示黄框（新系统可关），不符合"零依赖、无感"的取向；DXGI Desktop Duplication 已在 §11 中定下。

## Decisions

### D1. 模块划分

| 文件 | 职责 | 依赖 |
|---|---|---|
| `src/capture/motion_estimator.h/.cpp` | 纯逻辑：投影、去趋势、互相关、亚格峰值、置信度；`SpeedHold`（保持 / 归零）；`shift_to_deg_per_s()` 换算 | 仅标准库，可单元测试 |
| `src/capture/desktop_dup.h/.cpp` | `CaptureService`：截屏线程、显示器 / 适配器匹配、独立 D3D11 设备、Desktop Duplication、GPU 降采样、读回、性能统计；内部调用 `MotionEstimator` | d3d11、dxgi、d3dcompiler |

`CaptureService` 对 app 暴露的接口（主线程调用，全部非阻塞，只有 `stop()` 例外）：

```cpp
enum class CaptureState { Off, Paused, Running, Recovering, Failed };
struct CaptureParams { int grid_w=160, grid_h=90, max_shift=20; double min_correlation=0.60,
                       assumed_hfov_deg=90; int hold_ms=100; };
class CaptureService {
public:
    bool start(const CaptureParams&);              // 创建线程（不建设备）；--no-capture 时 app 根本不调用
    void stop();                                   // 置停止标志 + SetEvent + join（≤ 500 ms）
    void set_target(HMONITOR mon, bool feature_on, bool wanted);   // 每 tick 调用；变化时才 SetEvent
    void update_params(const CaptureParams&);      // 配置变化时调用
    double speed(int64_t now_qpc) const;           // 原子读；过期（> 250 ms）返回 0
    CaptureState state() const;                    // 原子读
    CaptureDebug debug() const;                    // dx / dy / c / 最近速度，调试条用
};
```

`motion_estimator.*` 可能已由并行代理做出原型。实施时先检查文件是否存在：存在就复用并补齐上面的接口与 D5–D7 的规则，不存在才新写。

### D2. 截屏线程使用同一适配器上的独立 D3D11 设备

**决定**：截屏线程在"拥有目标显示器输出的适配器"上调用 `D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, …)` 自建设备与 immediate context。该设备只在截屏线程使用，不与 overlay 共享任何 D3D 对象；两个线程之间只传一个 `double`。

**理由**：
1. **线程安全与互不阻塞**：immediate context 不是线程安全的。共享设备需要 `ID3D11Multithread::SetMultithreadProtected(TRUE)`，此后每次调用都要抢同一把锁；截屏线程的 `Map` / `AcquireNextFrame` 会拖住 overlay 的渲染与 `Present`，正好违背"overlay 帧时间 < 0.2 ms、不抖动"。
2. **适配器必须匹配**：`DuplicateOutput` 要求设备与输出在同一适配器上。overlay 的设备按默认适配器创建，而游戏所在显示器在双显卡笔记本 / 多卡台式机上可能接在另一块卡上；独立设备可以随输出换适配器重建，overlay 不受影响。
3. **故障隔离**：访问丢失、设备移除、驱动重置只需重建截屏设备，不触碰 overlay 的 swap chain / DComp 树。
4. **代价小**：多一个设备约几 MB 显存 / 内存，创建 < 50 ms，只在开始取帧或重建时发生。

**备选**：(a) 共享 overlay 设备 + multithread protect：有锁竞争，且适配器不匹配时无解，否决。(b) 共享设备 + deferred context：deferred context 仍要在 immediate context 上 `ExecuteCommandList`，同样有锁竞争，而且 `Map` 读回仍需 immediate context，否决。

设备创建标志：release 为 0；debug 构建若 SDK Layers 可用则加 `D3D11_CREATE_DEVICE_DEBUG`。特性级别 ≥ 10_0（着色器用 `vs_4_0` / `ps_4_0`）。

### D3. 显示器 → 输出 → 适配器匹配

每次建立采集时（不缓存旧的 factory，显示配置变化后旧 factory 会失效，`IsCurrent()` 为 false）：
`CreateDXGIFactory1` → `EnumAdapters1(i)` → `EnumOutputs(j)` → `GetDesc().Monitor == target`。找到后：建设备 → `QueryInterface<IDXGIOutput1>` → `DuplicateOutput(device)`。若返回 `DXGI_ERROR_UNSUPPORTED`（个别 HDR / FP16 桌面模式），且系统有 `IDXGIOutput5`，再试 `DuplicateOutput1(device, 0, {B8G8R8A8_UNORM, R16G16B16A16_FLOAT})`，降采样着色器对两种格式都按 float 采样，不需要分支。

`DXGI_OUTDUPL_DESC` 给出 `ModeDesc.Width/Height`（即规格里的 W、H，物理像素）与 `Rotation`。旋转为 90° / 270° 时交换 dx / dy，W 取旋转后的桌面宽度；速度只用模长，符号无关紧要。

### D4. GPU 降采样

资源（每次建立采集时创建，网格尺寸变化时重建）：
- 全屏三角形 VS（`SV_VertexID` 生成，无顶点缓冲）+ 降采样 PS，HLSL 以字符串放在 `desktop_dup.cpp`，运行时 `D3DCompile`。不改 overlay 的 `shaders.h`，避免两个变更改同一文件。
- `rt`：`grid_w × grid_h`、`DXGI_FORMAT_R32_FLOAT`、`BIND_RENDER_TARGET`。
- `staging[2]`：同尺寸、`USAGE_STAGING`、`CPU_ACCESS_READ`，组成 2 槽环。
- 线性 / clamp 采样器；常量缓冲 `{src_size, grid_size, taps}`。
- 每帧对取到的桌面纹理创建 SRV（格式取自纹理描述；创建很便宜，同时避免缓存被系统换掉的纹理指针）。

着色器：每个输出格覆盖屏幕上 `cell = (W/grid_w, H/grid_h)` 像素的区域，取 T×T 个双线性样本，T = clamp(⌈max(cell)/2⌉, 2, 8)，样本位置 `origin + (i + 0.5)·cell/T`：

```hlsl
float acc = 0;
for (int j = 0; j < T; ++j)
  for (int i = 0; i < T; ++i) {
    float2 p = cellOrigin + (float2(i, j) + 0.5) * cell / T;      // 像素坐标
    acc += dot(src.SampleLevel(lin, p / srcSize, 0).rgb, float3(0.299, 0.587, 0.114));
  }
return acc / (T * T);
```

1080p 时 cell = 12 px、T = 6、步长 2 px，样本正好落在 2×2 像素的公共角上，一次双线性采样等于 4 像素的平均，6×6 个样本恰好铺满 12×12 的格子，是精确的盒式平均（规格里"细条纹不混叠"的场景依赖这一点）。4K 时 cell = 24、T = 8、步长 3 px，覆盖约 44%，混叠已大幅减弱。总样本数 14 400 × 36 ≈ 52 万次，读一遍 8 MB 桌面纹理，预计 < 0.05 ms。

**备选**：(a) §11 提到的"三步双线性"（逐级 ½ 缩小）：要多张中间 RT、多次 draw，而且 1920 无法连续 ½ 到 160，最后一级仍要非整数缩放，否决。(b) 固定 3×3 样本：1080p 下只覆盖 25% 的格子，细纹理在亚格平移时会闪烁，降低相关峰的平滑度，否决。(c) `GenerateMips`：需要先把整帧复制到带 mip 的纹理，带宽翻倍，否决。

### D5. 读回（不忙等）

每个新帧：`Draw` 到 `rt` → `CopyResource(staging[k], rt)` → 记录本槽的帧时间戳 → `Flush()` → `ReleaseFrame()`（按微软 DDA 示例的顺序：GPU 工作提交后再释放帧）。然后对**上一个**槽调用 `Map(..., D3D11_MAP_FLAG_DO_NOT_WAIT)`：成功就交给估计器；返回 `DXGI_ERROR_WAS_STILL_DRAWING` 就留到下一次循环。取帧超时的循环也会尝试映射未处理的槽，保证画面停下后最后一帧也能及时处理。读回只多 ≤ 1 帧延迟，`Map` 永不阻塞，不依赖驱动是否自旋等待。

### D6. 位移估计的数学

记网格宽 Gw、高 Gh，当前帧灰度 I_t(x, y) ∈ [0, 1]。

1. **投影**：C_t[x] = Σ_{y=0}^{Gh−1} I_t(x, y)，x = 0..Gw−1；R_t[y] = Σ_{x=0}^{Gw−1} I_t(x, y)，y = 0..Gh−1。
2. **去均值 + 去趋势**：对长度为 N 的序列 P[i]，ī = (N−1)/2，
   a = (1/N) Σ P[i]，b = Σ (i − ī) P[i] / Σ (i − ī)²，
   P̂[i] = P[i] − a − b (i − ī)。
   加性亮度变化 I + o 让每个 C[x] 都加 Gh·o，被 a 完全吸收；线性渐变（暗角、天空渐变）被 b 吸收。
3. **纹理能量**：E = √((1/N) Σ P̂[i]²) / M，M 为每个投影元素累加的像素数（列投影 M = Gh，行投影 M = Gw），即"每像素均方根"。E < 1e-3 → 该轴无纹理，c_axis = 0，跳过相关计算。
4. **归一化互相关**：S = min(max_shift, ⌊N/3⌋)（默认 160 / 90 网格下两轴都是 20）。对 s ∈ [−S, S]，取重叠下标 Ω_s = { i | 0 ≤ i < N, 0 ≤ i − s < N }（长度 N − |s|），p_i = P̂_{t−1}[i − s]，q_i = P̂_t[i]，
   ρ(s) = Σ (p_i − p̄)(q_i − q̄) / √( Σ (p_i − p̄)² · Σ (q_i − q̄)² )，分母 < 1e-12 时 ρ = 0。
   在重叠段内再去一次均值（皮尔逊相关），避免截断带来的直流偏差。若 P̂_t[i] = P̂_{t−1}[i − s]，则 ρ(s) = 1，即内容向 +s 方向移动了 s 格。
5. **峰值与亚格插值**：s* = argmax ρ(s)，ρ₀ = ρ(s*)。若 −S < s* < S，令 ρ₋ = ρ(s*−1)，ρ₊ = ρ(s*+1)，den = ρ₋ − 2ρ₀ + ρ₊；den < 0 时 δ = ½ (ρ₋ − ρ₊) / den，否则 δ = 0；δ 截断到 [−0.5, 0.5]，位移 = s* + δ。s* 落在 ±S 时不插值（真实位移可能超出范围，用 ±S 作为下界，置信度照常；若内容确实已移出范围，相关通常本来就低）。
6. **置信度**：c_x = max(0, ρ₀,x)，c_y = max(0, ρ₀,y)，c = min(c_x, c_y)。
7. **换算**：shift_px = √((dx·W/Gw)² + (dy·H/Gh)²)，deg = shift_px · hfov / W，ω = deg / dt。

增益变化 g·I 会让 P̂ 整体乘 g，皮尔逊相关不受影响，所以闪光 / 爆炸若只是变亮变暗，峰值仍在 s = 0；整屏纯白则落到第 3 步，c = 0。

计算量：投影 14 400 次加法；相关 41 × (160 + 90) ≈ 1 万次乘加；全部用 `double` 累加，每帧 < 30 µs。

**为什么用投影而不用二维相位相关 / 块匹配**：相机偏航 / 俯仰在画面上近似全局平移，投影把二维问题降成两个一维问题，O(N) 且无需 FFT（零依赖），对局部动画（粒子、角色）有天然的平均作用。§11 已定此方案，这里只补全公式。

### D7. 保持与归零（`SpeedHold`，纯逻辑）

状态：`last_value`、`last_valid_t`。事件：
- `on_estimate(est, dt, t)`：若 `est.c ≥ min_correlation` 且 0 < dt ≤ 0.25 s → `last_value = ω`、`last_valid_t = t`；否则视同无更新。
- `on_no_update(t)`：超时、只有指针更新、重建中、dt 无效时调用。
- `value(t)`：`t − last_valid_t ≤ hold_ms` ? `last_value` : 0。

每个事件之后发布 `value(t)`。低置信度帧照样成为新参考帧（`MotionEstimator::push` 总是保存当前投影），取帧超时则参考帧不变。归零是阶跃，可见的淡出交给强度模型的 release（默认 500 ms）；**备选**是在截屏侧再做指数衰减，但那样等于两级平滑，退出时间变长且难以推算，所以不采用。

重建或首次开始后丢弃前 2 帧：只更新参考帧，不产生速度。这样可以盖住两个瞬态：显示器切换后第一帧的内容跳变，以及刚设置截屏排除后可能仍含 overlay 的 1–2 帧合成结果。

### D8. 线程模型与交接

```
主线程（tick）                                截屏线程
──────────────                                ─────────────────────────────────────────
set_target(mon, feature_on, wanted) ──原子──▶ target_mon / feature_on / wanted（变化时 SetEvent）
update_params(p) ──mutex + generation──────▶ 每轮循环比较 generation，变了就复制参数
speed(now) ◀──── atomic<double> speed_ ──────  发布 value(t)
              ◀── atomic<int64_t> speed_qpc_
state()    ◀──── atomic<CaptureState> ───────  状态机
```

- 发布用 `memory_order_release`，读取用 `acquire`；`static_assert(std::atomic<double>::is_always_lock_free)`。
- 参数用 `std::mutex` 保护，只在复制一个小结构体时持锁；配置很少变化，主线程不会因此阻塞。
- 唤醒用一个自动重置事件 `CreateEventW(nullptr, FALSE, FALSE, nullptr)`。

截屏线程循环（伪代码）：

```
while (!stop):
  if params_generation changed: copy params; if grid changed → rebuild downscale resources, estimator.reset()
  if !feature_on:          release session now; state=Off;     WaitForSingleObject(evt, INFINITE); continue
  if !wanted:              state=Paused; wait(evt, linger_left); if linger(5 s) expired → release session; continue
  if target_mon != session.mon: release session
  if !session:
      if now < next_retry: wait(evt, next_retry-now); continue
      if !build(target_mon): on_build_fail(hr); next_retry = now + 2 s; continue
      fail_count = 0; discard = 2; state=Running; log info
  hr = AcquireNextFrame(16, &info, &res)
  if hr == WAIT_TIMEOUT:    try_map_pending(); hold.on_no_update(now); publish; continue
  if FAILED(hr):            release session; state=Recovering; next_retry = now; continue   // 立即重建一次
  if info.LastPresentTime == 0 || info.AccumulatedFrames == 0:
                            ReleaseFrame(); try_map_pending(); hold.on_no_update(now); publish; continue
  downscale + copy to staging[k] + Flush + ReleaseFrame
  try_map_pending()  → estimator.push(gray) → (discard>0 ? discard-- : hold.on_estimate(...)) → publish
```

`on_build_fail(hr)`：`E_ACCESSDENIED` 不计数；其他 `fail_count++`，到 10 → `state=Failed`、写 warn、线程进入只等 `feature_on` 变化的等待。主线程下次 tick 读到 `Failed`，更新截屏排除与托盘文字；用户关→开（`feature_on` 先 false 后 true）时清零计数。

关闭（`feature_on=false`）立即释放会话；"不在游戏里"（`wanted=false`）则保留 5 s，避免 Alt-Tab 抖动时反复建会话（`DuplicateOutput` 一次要 10–100 ms）。

线程优先级保持 `THREAD_PRIORITY_NORMAL`：每帧 CPU < 0.3 ms，被游戏抢占也只会让速度"过期"变 0，属于安全方向的失败。

### D9. 接入 `tick()`

```cpp
// app.cpp tick()，位于"更新游戏判定"之后、"更新强度模型"之前
const bool cap_on = !cli_.no_capture && cfg_.trigger.capture && capture_.state() != CaptureState::Failed;
apply_capture_exclusion(cap_on);                    // auto/always/never → overlay_.set_capture_excluded()，仅在结果变化时调用
capture_.set_target(overlay_.monitor(), cap_on, cap_on && gates.capture);   // 排除先于 wanted=true
in.capture_deg_per_s = cap_on ? capture_.speed(now_qpc) : 0.0;
in.gate_capture      = gates.capture;               // enabled && !excluded && known_game
```

- `overlay_.monitor()` 是 overlay 当前跟随的显示器。若变更 1 的 `Overlay` 没有这个访问器，就补一个，或在 app 里记下最近一次 `follow_monitor()` 的参数。
- `--no-capture` 时 app 不调用 `capture_.start()`；`cap_on` 恒为 false，排除按 auto 规则为 false（集成验证脚本依赖这一点截到遮罩）。
- `hide_from_capture=never` 且 `cap_on` 时，每次从关到开写一条 warn。
- 调试条：`debug_text` 里原先固定的"截屏 0"改为按 `capture_.state()` / `capture_.debug()` 生成：`Running` → `{°/s 取整}°/s c{0.00}`，`Paused` → `暂停`，`Off`（含 `--no-capture`）→ `关`，`Failed` → `失败`（见 `specs/overlay-rendering` 的「调试条」）。`Recovering` 显示为 `暂停`。

### D10. 配置与模型

- `TriggerConfig` 增加 `bool capture = true; Sensitivity capture_sensitivity = Sensitivity::Medium;`，新增 `CaptureConfig`（六个键，范围见 `specs/configuration`）。这些键登记进变更 1 的静态模式表（core-overlay design D9：节、键、类型、范围、最少小数位），解析、校验、写回自动生效，变更 4 的设置窗口也从同一张表取范围。
- `IntensityModel::configure()` 预先算出 `k = {2.0, 1.5, 1.0}[capture_sensitivity]`，`update()` 中 `t_capture = gate_capture ? smoothstep(k*v_start, k*v_full, capture_deg_per_s) : 0`，然后与其他项取 `max`。
- 配置变化（重载、托盘、热键）后 app 调用 `capture_.update_params()`；`capture` 键变化通过下一次 tick 的 `set_target` 生效。

### D11. 托盘与热键

- 托盘菜单在"边缘参照点"后插入"截屏运动检测"。文字规则：`--no-capture` → "截屏运动检测（已由 --no-capture 禁用）" + `MF_GRAYED`；`Failed` → "截屏运动检测（采集失败）"；否则为原文 + `\t` + 热键文字。
- 热键动作 `toggle_capture`（动作表第 6 项，中文名"截屏运动检测开关"）与托盘点击共用 `App::toggle_capture()`：`--no-capture` 时只弹 toast；否则翻转 `cfg_.trigger.capture` → 立即保存 → toast "截屏运动检测：开|关"（与变更 1 的"中心参照点：开|关"同一风格）。

### D12. 性能统计

- GPU：只在日志级别为 debug 时启用 `D3D11_QUERY_TIMESTAMP_DISJOINT` + 两个 `TIMESTAMP` 查询（4 组环形），用 `GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)` 非阻塞取结果，取不到就跳过；整段时间都 disjoint 时 `gpu_ms=-1`。
- CPU：每帧用 QPC 计 `Map` + 估计耗时得 `cpu_ms`；每 5 s 用 `GetThreadTimes`（内核 + 用户）差值 ÷ 墙钟时间得 `cpu_pct`。
- 每 5 s 由截屏线程直接写日志行。变更 1 的 `util/log` 已是 `std::mutex` 保护的单例（core-overlay design D10 为此预留），截屏线程可以直接调用；行前缀 `capture perf:` 与变更 1 overlay 的 `gpu frames=` / `perf ticks=` 行互不冲突。

## Risks / Trade-offs

- [静态 HUD、准星、小地图在投影里占比大，把峰值拉向 0] → 动态内容通常占画面大部分，峰值仍在真实位移处，只是 ρ 略降；方向上偏保守，与"截屏检测要保守"的产品取向一致。
- [`assumed_hfov_deg` 与游戏实际 FOV 不符，加上透视，角速度误差可达 ±30%] → 有保守系数 k，用户可在 INI / 设置窗口调整 HFOV；README 说明。
- [周期纹理（栅栏、地砖、fakegame 的网格）让相关出现多个峰，可能锁错周期] → `max_shift` 限制搜索范围，偶发错估会被强度模型的 attack 平滑掉；fakegame 的滚动模式必须用非周期随机纹理，否则验证本身不可靠。
- [向前走 / 缩放 / 翻滚不是平移] → 相关低或位移小，不触发；向前走由键盘项覆盖。
- [游戏帧率低于刷新率、偶发卡顿] → 超时与低置信度共用 `hold_ms` 保持，dt > 250 ms 不算速度。
- [WDA_EXCLUDEFROMCAPTURE 不可用（Win10 2004 之前）或 `never`：遮罩进入自己的截屏] → 静止的暗角不产生位移，主要是降低边缘纹理对比、让 c 略降；写 warn 日志，不停用功能。
- [刚设置排除的 1–2 帧仍含 overlay] → 开始 / 重建后丢弃前 2 帧。
- [其他程序占用桌面复制（OBS、远程桌面），返回 `NOT_CURRENTLY_AVAILABLE` / `SESSION_DISCONNECTED`] → 计入失败，10 次后放弃并在 warn 日志里说明"可能有其他程序在采集屏幕"；用户关 → 开可重试。
- [UAC / 锁屏期间长时间 `E_ACCESSDENIED`] → 不计数，持续每 2 s 重试；不在游戏里时根本不取帧。
- [双显卡：游戏在独显渲染、显示器接在核显，系统内部跨卡拷贝] → 由系统处理，代价计入性能日志；超出预算时用户可关闭截屏检测。
- [反作弊误判] → Desktop Duplication 是 OBS、Discord 等通用的系统 API，不注入、不读内存，风险与 OBS 相同。
- [截屏线程写日志与主线程并发] → 变更 1 的日志模块已加锁（D12）。

## Migration Plan

- 无数据迁移：旧 INI 缺少截屏键时用默认值，下次保存按模板补齐。
- 回滚 / 规避：用户可通过托盘 / 热键关闭（`capture=0`），或用 `--no-capture` 启动；代码层面截屏服务与其他模块之间只有 `set_target` / `speed` 两个接缝，必要时可在 app 里整段不调用。

## Open Questions

- 放弃前的失败次数（10）、释放会话的延时（5 s）、无纹理阈值（1e-3）、速度过期时间（250 ms）是代码常量，可在实测后微调，不进 INI。
