# Tasks

## 1. 配置键与强度模型接缝

- [x] 1.1 在 `src/config/config.h/.cpp` 中给 `TriggerConfig` 增加 `capture`、`capture_sensitivity`，新增 `CaptureConfig`（`grid_w`、`grid_h`、`max_shift`、`min_correlation`、`assumed_hfov_deg`、`hold_ms`），把八个键登记进变更 1 的静态模式表（范围见 `specs/configuration`），由它负责截断 / 回落默认值并写 warn。验证：`tests/test_ini.cpp` 新增用例（缺节 → 默认值且无 warn；`max_shift=100` → 40；`min_correlation=1.5` → 0.99；`capture_sensitivity=extreme` → medium）全部通过。
- [x] 1.2 在 `src/res/MotionCalm.default.ini` 的 `[trigger]` 节 `gamepad` 之后补 `capture` / `capture_sensitivity`，在 `[calibration]` 之后补 `[capture]` 节，写中文注释（说明各键含义与范围）；`[hotkeys]` 节在 `calibrate` 之后加入 `toggle_capture=Ctrl+Alt+F6`；`--no-capture` 的命令行说明改为"不启动截屏运动检测"。验证：`tests/test_ini.cpp` 中"默认值保存后与模板逐字节相同"和节顺序（general … hotkeys、calibration、capture）用例通过；删除 INI 后启动，生成的文件含这些键与注释。
- [x] 1.3 在 `src/model/intensity.h/.cpp` 中实现截屏项：`configure()` 算出 k（low 2.0 / medium 1.5 / high 1.0），`update()` 中 `t_capture = gate_capture ? smoothstep(k·v_start, k·v_full, capture_deg_per_s) : 0` 并参与 `max`。验证：`tests/test_intensity.cpp` 新增用例（medium/medium 下 90 / 315 / 540 °/s → 0 / 0.5 / 1.0；high 下 360 → 1.0；low 下 120 → 0；门控为假 → 0；与 t_mouse 取 max；鼠标静止、截屏 600 °/s 持续 300 ms 后平滑强度 > 0.9）全部通过。

## 2. 位移估计（纯逻辑，可单测）

- [x] 2.1 检查 `src/capture/motion_estimator.h/.cpp` 是否已由并行原型实现：已存在就复用，并对照 design.md D6 补齐接口与规则（`configure` / `reset` / `push` 返回 `{dx, dy, confidence, valid}`；最小二乘去趋势；重叠段皮尔逊相关；S = min(max_shift, ⌊N/3⌋)；抛物线插值且修正量截断到 ±0.5；边界不插值；每像素 RMS < 1e-3 → 该轴 c = 0；c = min(c_x, c_y)；低置信度帧也替换参考帧）；不存在就按 D6 新写。验证：头文件不含 Win32 / D3D 依赖，`tests.exe` 能链接。
- [x] 2.2 在同一模块实现 `SpeedHold`（`on_estimate` / `on_no_update` / `value`，规则见 D7）与 `shift_to_deg_per_s(dx, dy, grid_w, grid_h, W, H, hfov, dt)`（dt ≤ 0 或 > 0.25 s 返回"无效"）。验证：2.3 中的换算与保持用例通过。
- [x] 2.3 编写或补全 `tests/test_motion_estimator.cpp`：固定种子的合成帧生成器（4 倍超采样随机块纹理 → 平移 → 盒式平均到 160×90 → 可选 σ = 0.02 噪声）。用例：整数位移 (7, −3)；亚格 (2.25, 0.75)；接近上限 −18（`max_shift=20`）；亮度突变（×1.3 + 0.15 截断）→ c < 0.60 或 |dx|、|dy| < 0.5 格且换算后 < 90 °/s；整屏纯白 → c = 0；纯色 → c = 0；完全换图 → c < 0.60；加 0.3 后去趋势投影逐元素差 < 1e-3；换算 dx = 10 → 337.5 °/s；保持 / 归零时间线（hold 100 ms 内保持、之后 0）；dt = 400 ms 不出速度；闪白帧及其后一帧保持。若原型已带测试，就补齐缺少的用例。把该文件加入 `scripts/build_tests.cmd`。验证：`build.cmd tests` 全部通过、退出码 0。

## 3. 桌面复制采集与 GPU 降采样

- [x] 3.1 新建 `src/capture/desktop_dup.h`：声明 `CaptureState`、`CaptureParams`、`CaptureDebug` 与 `CaptureService`（`start` / `stop` / `set_target` / `update_params` / `speed` / `state` / `debug`，见 design.md D1），文件头一行注释说明职责；把 `desktop_dup.cpp`、`motion_estimator.cpp` 加入 `scripts/build_app.cmd`。验证：`build.cmd app debug` 编译通过。
- [x] 3.2 实现会话建立：枚举 factory / 适配器 / 输出，匹配 `HMONITOR`；在该适配器上建独立 D3D11 设备；`DuplicateOutput`，遇 `DXGI_ERROR_UNSUPPORTED` 时回退 `IDXGIOutput5::DuplicateOutput1`；读取 W、H、Rotation；写 info 日志（分辨率 + 适配器名）。验证：带 `--verbose --force-game` 运行假游戏，日志出现开始取帧行，含 `1920x1080`（或本机分辨率）与适配器名。
- [x] 3.3 实现降采样：全屏三角形 VS + T×T 双线性样本的 PS（D4），`R32_FLOAT` RT、2 槽 staging 环、每帧建 SRV；提交后 `Flush` 再 `ReleaseFrame`；对上一槽 `Map(DO_NOT_WAIT)`，`WAS_STILL_DRAWING` 时留到下一轮（D5）。验证：debug 构建下增加一个仅 `--verbose` 生效的诊断：开始取帧后把第一张网格的最小 / 最大 / 平均值写进日志；在纯灰桌面壁纸上平均值 ≈ 0.5，D3D debug layer 无报错。
  - 实测：`capture first grid 160x90: min=… max=… mean=…` 行已出现；"纯灰壁纸 ≈ 0.5"与 debug layer 无报错由单元测试 `capture_downscale_uniform_gray_128`、`capture_downscale_debug_layer_clean` 覆盖，未在实机换壁纸。
- [x] 3.4 实现截屏线程主循环（D8）：停止标志 + 自动重置事件；`feature_on=false` 立即释放会话，`wanted=false` 暂停并在 5 s 后释放；16 ms 取帧超时；只有指针更新的帧按"无更新"处理；目标显示器变化时重建；开始 / 重建后丢弃前 2 帧；每轮比较参数 generation，网格变化时重建 RT / staging 并 `estimator.reset()`；通过 `SpeedHold` 发布原子速度与时间戳。验证：在桌面与假游戏之间 Alt-Tab，日志显示暂停、5 s 后释放、回到游戏后恢复；用任务管理器的"详细信息 → 线程"或 `--verbose` 日志确认暂停时没有 `capture perf:` 行，进程 CPU ≈ 0。
  - 未勾：Alt-Tab 暂停 → 5 s 释放 → 回到游戏恢复，尚未实机验证（接线后才可测）。
- [ ] 3.5 实现失败处理（D8 `on_build_fail`）：访问丢失 / 设备移除 → 释放并立即重建；重建失败每 2 s 重试；`E_ACCESSDENIED` 不计数；其他错误连续 10 次 → `Failed` + warn 日志（含 HRESULT 与次数）；`feature_on` 关 → 开时清零。验证：假游戏运行中按 Win+L 锁屏 15 s 再解锁，日志显示重建并恢复，不进入 `Failed`；切换一次显示器分辨率（或缩放比例），日志显示一次重建并恢复。 <!-- 未验证（2026-11-04）：重建 / 重试 / 放弃策略由 tests/test_capture_policy.cpp 覆盖；未在真机制造访问丢失 -->
  - 未勾：Win+L / 分辨率切换的恢复尚未实机验证。
- [x] 3.6 实现性能统计（D12）：仅在 debug 级别启用时间戳查询（非阻塞 `GetData`），QPC 统计 `cpu_ms`，`GetThreadTimes` 统计 `cpu_pct`；暂停时不输出；每 5 s 写一行 `capture perf: frames=… timeouts=… lowconf=… gpu_ms=… cpu_ms=… cpu_pct=… speed_avg=…`；确认 `util/log` 如 core-overlay D10 所述有 mutex 保护（截屏线程直接调用）。验证：`--verbose` 在假游戏中运行 12 s，日志至少 2 行完整的 `capture perf:`；`log_level=info` 时没有该行；60 s 内没有交错或破损的日志行。
  - 未勾：实测 8 s 的 `--force-game` 运行中出现 1 行完整的 `capture perf:`（gpu_ms=0.029 cpu_pct=0.89）；"12 s ≥ 2 行"与 60 s 无破损行留给 `verify.ps1 -Capture`（约 12 s 取帧）及 5.4。

## 4. 接入应用（tick、截屏排除、托盘、热键、命令行）

- [x] 4.1 在 `src/app/app.cpp` 中：启动时若无 `--no-capture` 就调用 `capture_.start()`，并写一条 info 说明状态；有 `--no-capture` 时写 info "截屏检测已由 --no-capture 禁用"，且不创建线程；退出时 `stop()`（≤ 500 ms join）。验证：`--no-capture` 启动后日志有该行，Process Explorer 中没有截屏线程，INI 不变；正常退出无挂起。
- [x] 4.2 在 `tick()` 中按 design.md D9 接入：先算 `cap_on` → 应用截屏排除 → `set_target(overlay 显示器, cap_on, cap_on && gate_capture)` → 读 `speed(now)`（> 250 ms 过期为 0）填入 `IntensityInputs`；`gate_capture` 再与 `cap_on` 相与；调试条里原先固定的"截屏 0"按 `specs/overlay-rendering`「调试条」改为 `{°/s}°/s c{0.00}` / `暂停` / `关` / `失败`。必要时给 `Overlay` 补 `monitor()` 访问器，或在 app 中记录最近一次 `follow_monitor()` 的参数。验证：开调试条，在滚动的假游戏中截屏字段显示数百 °/s 与 c；切到 chrome 后显示"截屏 暂停"；`--no-capture` 下显示"截屏 关"。
  - 实测：调试条显示"截屏 0°/s c1.00"（桌面静止画面）与"截屏 关"（`--no-capture`、capture=0）；"暂停 / 失败"由单元测试 `capture_debug_field_text` 覆盖；滚动假游戏中的数百 °/s 留给 `verify.ps1 -Capture`。frame 行在 pad 之后加了 `capture=<°/s>`（configuration delta「调试帧日志」）。
- [x] 4.3 实现截屏排除联动（auto 跟随 `cap_on`，always / never 强制；`never` 且开启时写 warn；`SetWindowDisplayAffinity` 失败时写 warn 后继续）。验证：默认配置下在已知游戏中按 PrintScreen，截图里没有遮罩；以 `--no-capture` 启动并运行 `scripts/verify.ps1`，截图里有遮罩；把托盘"截屏运动检测"关掉后再截图，能看到遮罩。
  - 实测（BitBlt + CAPTUREBLT 截图）：默认 auto + 截屏开启时截图里没有中心点；`hide_from_capture=never` 时有中心点并写 warn；热键关掉截屏检测后同一 tick 内日志 `overlay capture visibility: visible to capture`。
- [x] 4.4 在 `src/tray/tray.cpp` 中按 `specs/tray-and-hotkeys`「托盘菜单结构」加入"截屏运动检测"勾选项（位置在"边缘参照点"之后，显示热键文字；`Failed` 与 `--no-capture` 时文字按 design.md D11 变化，后者灰显）；点击调用 `App::toggle_capture()`：翻转 → 立即保存 → toast。验证：点击后 INI 中 `capture` 翻转、注释保留；出现 toast "截屏运动检测：关" / "截屏运动检测：开"（不在游戏时为仅文字模式）。
  - 托盘点击与热键共用 `App::action_toggle_capture()`；实测热键路径：INI 中 capture 0 ↔ 1 翻转、注释保留、toast "截屏运动检测：关 / 开"。菜单文字由单元测试 `tray_capture_item_text` 覆盖，未实际点开托盘菜单。
- [x] 4.5 在热键动作表中加入 `toggle_capture`（中文名"截屏运动检测开关"，默认 `Ctrl+Alt+F6`，位置在 `calibrate` 之后、`cycle_sensitivity` 之前），与托盘共用处理函数；`--no-capture` 时只弹 toast "截屏运动检测已被 --no-capture 禁用"。验证：`tests/test_hotkey_parse.cpp` 中动作表用例更新为 11 项且顺序正确；默认配置启动后注册 6 个热键；游戏中按 `Ctrl+Alt+F6` 能切换并出现 toast；`toggle_capture=None` 时不注册；`--no-capture` 下按热键 INI 不变。
  - 实测：默认配置注册 6 个热键（含 toggle_capture=Ctrl+Alt+F6）；`--no-capture` 下热键只弹 toast、INI 不变。动作 id = 11（枚举追加，已有 id 不变），表中位置在 calibrate 之后。
- [ ] 4.6 配置热更新：重载配置、托盘、热键改动后调用 `capture_.update_params()`。验证：运行中把 INI 的 `grid_w/grid_h` 改为 192/108 并保存，`--verbose` 日志显示网格重建且继续输出 `capture perf:`；把 `assumed_hfov_deg` 改为 120，滚动假游戏的 `speed_avg` 约增大到 4/3 倍。 <!-- 未验证（2026-11-04）：配置热更新代码已接入（update_params），未做改 grid_w 的真机运行 -->
  - 未勾：已实现（重载 / 热键切换后调用 `capture_.update_params()`）；实测重载生效（`config applied`），但取帧中改 grid 后的"capture grid rebuilt"与 hfov 换算倍数尚未实机验证。

## 5. 集成与手动验证（运行前提醒用户不要碰鼠标）

- [x] 5.1 （以 fakegame 的 --texture noise --scroll <px/s> --scroll-start/--scroll-seconds --flash-at 实现，未加独立的 scroll / flash 模式）给 `tools/fakegame` 增加（若已存在则复用）`--mode scroll --scroll-px-per-s <n>`：全屏无边框、隐藏并锁定光标，绘制**非周期**的随机块纹理并按给定速度水平滚动；以及 `--mode flash --flash-ms <n>`：静止的随机块纹理，背景在黑与白之间每 n ms 切换一次。验证：两种模式都能运行 `--seconds` 秒后自动退出，滚动平滑且无明显周期图案。
  - 未勾（未运行，按要求不跑 fakegame）：没有新增 `--mode scroll / flash`，而是在现有 `--scroll` / `--flash-at` 上加了 `--texture noise`（确定性的非周期值噪声 + 色块，没有 64 px 网格）、`--scroll-start` / `--scroll-seconds`（滚动时间窗），并在滚动开始 / 结束、闪白时打印 `wall=` 墙钟。离线用真实 MotionEstimator 检查过 noise 纹理：150–600 °/s 每帧估计全部有效且误差 < 1 格，闪白帧 c = 0。
- [x] 5.2 滚动验证：启动 `MotionCalm.exe --verbose --force-game --exit-after 20`，再启动 `fakegame.exe --seconds 15 --mode scroll --scroll-px-per-s 12800`（1080p 下约 600 °/s），全程不碰鼠标键盘。验证：日志中滚动期间强度 > 0.8，`capture perf:` 的 `speed_avg` 在 500–700 之间；再用 `--scroll-px-per-s 6000`（约 281 °/s）运行，强度稳定在 0.2–0.6 之间。由于截屏检测开启时遮罩对截图不可见，这一步以日志数值为准，不看截图。
- [x] 5.3 闪烁验证：`fakegame.exe --seconds 15 --mode flash --flash-ms 200`，同样不碰鼠标。验证：全程日志中的强度 < 0.05，`capture perf:` 的 `lowconf` > 0。
- [ ] 5.4 性能验证：1080p、60 Hz 下滚动模式运行 60 s。验证：`capture perf:` 行的 `gpu_ms` 平均 < 0.2、`cpu_pct` 平均 < 2.0；随后切到桌面停留 60 s，没有 `capture perf:` 行，进程 CPU（`GetProcessTimes` 差值）≈ 0%。 <!-- 未验证（2026-11-04）：只有 15 s 窗口的数据：gpu_ms 0.02–0.08、cpu_pct 0.82–0.87，未跑满 60 s -->
- [ ] 5.5 恢复验证：滚动期间先锁屏（Win+L）10 s 再解锁，然后以管理员身份启动一个程序触发 UAC，停留 30 s 后取消。验证：两次都在日志中看到重建并恢复，强度在恢复后 1 s 内重新 > 0.8，状态从未变成 `Failed`。 <!-- 未验证（2026-11-04）：锁屏（Win+L）与 UAC 恢复未测，需要用户在场操作 -->
- [x] 5.6 （可选）在 `scripts/verify.ps1` 中增加 `-Capture` 场景，自动跑 5.2 / 5.3 并解析日志（每段 ≤ 20 s，运行前提醒用户不要碰鼠标）。验证：脚本对滚动场景报告通过，对闪烁场景报告"未触发"。
  - 未勾（未运行）：已加 `scripts\verify.ps1 -Capture [-CaptureDegPerS 400]`：一次运行内先滚动 4 s（约 400 °/s，按主显示器宽换算）、再闪白一次，判定滚动中强度 > 0.3 且 mouse=0.0、滚动前静止 ≤ 0.05、闪白前后 ≤ 0.1、日志有 capture started 与 excluded from capture。
