// capture_policy.h — 截屏采集的纯逻辑（不含 Win32 / D3D，可单元测试）：状态与参数、降采样采样布局、显示器旋转、重建 / 放弃策略、速度过期、处理帧限速、性能统计行。
#pragma once

#include <cstdint>
#include <string>

namespace mc {

// 截屏服务的状态（openspec capture-motion-detection design D1）。
//   Off        截屏检测未开启（feature_on = false），不持有任何桌面复制对象
//   Paused     开启但不需要取帧（wanted = false 或没有目标显示器）；5 s 后释放桌面复制对象
//   Running    正在取帧
//   Recovering 采集中断 / 建立失败，等待重建（每 2 s 重试）
//   Failed     连续 10 次非 E_ACCESSDENIED 失败，本次运行不再重试（直到 feature_on 关 → 开）
enum class CaptureState { Off, Paused, Running, Recovering, Failed };

// "off" / "paused" / "running" / "recovering" / "failed"。
const char* capture_state_name(CaptureState s);

// [capture] 节中截屏服务使用的键（默认值与 configuration spec 一致）。forward_gain 也在 [capture] 节，
// 但由强度模型使用，放在 TriggerConfig::forward_gain。
struct CaptureParams {
    int grid_w = 160;                // 降采样网格宽（格）[64, 320]
    int grid_h = 90;                 // 降采样网格高（格）[36, 180]
    int max_shift = 20;              // 每帧最大搜索位移（格）[2, 40]
    double min_correlation = 0.60;   // 最低置信度 [0.10, 0.99]
    double assumed_hfov_deg = 90;    // 假定水平视场角（°）[30, 170]
    int hold_ms = 100;               // 低置信度 / 无新帧时沿用上一速度的最长时间（ms）[0, 1000]
    bool detect_roll = true;         // 变更 5：是否发布倾斜角速度（画面旋转）
    int baseline_ms = 66;            // 变更 5：前进 / 倾斜估计与多久之前的帧比较（ms）[0, 150]；0 = 上一帧
    bool operator==(const CaptureParams&) const = default;   // Config 的整体比较要用
};

// 把参数收拢到 configuration spec 的合法范围（越界截断，NaN → 默认值）。配置模块已经校验过，这里只是防御。
CaptureParams sanitize_capture_params(const CaptureParams& p);

// 网格尺寸是否相同（不同时须重建降采样资源并丢弃参考帧）。
inline bool same_grid(const CaptureParams& a, const CaptureParams& b) {
    return a.grid_w == b.grid_w && a.grid_h == b.grid_h;
}

// ---------------------------------------------------------------- 常量（design Open Questions：代码常量，不进 INI）

inline constexpr unsigned kAcquireTimeoutMs = 16;       // AcquireNextFrame 等待
inline constexpr double kRetryIntervalS = 2.0;          // 重建失败后的重试间隔
inline constexpr int kMaxConsecutiveFailures = 10;      // 非 E_ACCESSDENIED 连续失败到此次数 → Failed
inline constexpr double kReleaseAfterPauseS = 5.0;      // 暂停这么久后释放桌面复制对象
inline constexpr double kSpeedMaxAgeS = 0.25;           // 发布值超过这么久没刷新 → 主线程按 0 处理
inline constexpr int kDiscardFramesAfterBuild = 2;      // 建立 / 重建后前 2 帧只更新参考帧
inline constexpr double kPerfLogIntervalS = 5.0;        // capture perf: 行的间隔
inline constexpr double kTargetFrameIntervalS = 0.01375;   // 处理帧的目标间隔（FrameRateLimiter，高刷新率显示器上抽帧）

// 发布于 published_s 的速度在 now_s 时是否仍可用（≤ 250 ms）。两者同为 qpc_now_s() 基准。
inline bool speed_is_fresh(double published_s, double now_s) {
    return now_s - published_s <= kSpeedMaxAgeS;
}

// ---------------------------------------------------------------- 处理帧限速（高刷新率显示器）

// 决定一个新桌面帧（只看非指针更新的帧，按其 DDA LastPresentTime）是否送去降采样 + 估计；不处理的帧取到后直接释放。
// 规则（"离目标间隔最近的那一帧"）：G = 本帧距上一处理帧的时间，g = 本帧距上一到达帧的时间；
//   G + g/2 ≥ target → 处理。即本帧比 target 早到的量不大于"等下一帧（约 g 之后）会晚到的量"时就处理本帧。
// 对周期 p 的均匀帧流：1.5·p ≥ target（target 13.75 ms 时 p ≥ 9.17 ms，即 ≤ 109 Hz）时每帧都处理；
//   否则处理间隔 = n·p ∈ [target − p/2, target + p/2)，处理帧率在 55–109 Hz 之间：
//   60 / 75 / 90 / 100 Hz → 全部；120 → 60；144 → 72；165 → 82.5；170 → 85；180 → 90；200 → 66.7；240 → 80；360 → 72。
// 13.75 ms 的取法：100 Hz（1.5·p = 15 ms）与 120 Hz（单帧 12.5 ms）之间居中，两边各留 1.25 ms 抖动余量；
//   60 / 75 Hz 每个时间戳可容忍约 ±3.7 / ±2.0 ms 抖动而不丢帧。抖动越过边界时只是在相邻两种抽帧比之间切换（dt 来自时间戳，估计不受影响）。
// 首帧、时间戳非有限或 ≤ 0、时间戳不递增（≤ 上一到达帧）时一律处理并以本帧重新锚定（与限速之前的行为相同）。
// 会话建立 / 关闭、暂停后恢复时调用 reset()。纯逻辑，非线程安全。
class FrameRateLimiter {
public:
    explicit FrameRateLimiter(double target_interval_s = kTargetFrameIntervalS) : target_(target_interval_s) {}

    // 一个新桌面帧到达（t_s = 呈现时刻，秒）：true → 处理；false → 跳过（直接 ReleaseFrame）。
    bool on_frame(double t_s);
    // 忘掉历史：下一帧视为首帧。
    void reset() { has_last_ = false; }
    double target() const { return target_; }

private:
    double target_;
    double last_processed_ = 0;   // 上一处理帧的时间
    double last_seen_ = 0;        // 上一到达帧（含跳过的帧）的时间
    bool has_last_ = false;
};

// ---------------------------------------------------------------- 降采样布局（design D4）

// 每个输出格覆盖源图 cell = (src_w/grid_w, src_h/grid_h) 像素；每轴取 taps = clamp(⌈cell/2⌉, 1, 16) 个双线性样本，
// 位置 origin + (i + 0.5)·step，step = cell/taps ≤ 2 px。cell 为偶数整数时（1080p→160×90：12 px，6×6 样本、步长 2 px），
// 每个样本恰好落在 2×2 像素的公共角上，一次双线性采样 = 4 像素平均，整个格子是精确的盒式平均（细条纹不混叠）。
// cell > 32 px 时样本数封顶 16，步长 > 2 px（只在 8K 或极小网格上出现，覆盖不完整）。
inline constexpr int kMaxDownscaleTaps = 16;

struct DownscaleLayout {
    double cell_w = 0, cell_h = 0;   // 每格覆盖的源像素
    int taps_x = 1, taps_y = 1;      // 每格每轴的双线性样本数
    double step_x = 0, step_y = 0;   // 样本间距（源像素）= cell / taps
};

// 任一尺寸 ≤ 0 时返回全 0 的布局（taps = 1）。
DownscaleLayout downscale_layout(int src_w, int src_h, int grid_w, int grid_h);

// ---------------------------------------------------------------- 显示器旋转（design D3）

// 桌面复制给出的纹理总是扫描输出（未旋转）方向。旋转 90° / 270° 时纹理的 x 对应桌面的 y：
// 速度只用位移模长，所以只需交换 dx / dy（以及网格宽高），并用桌面方向的宽度 W 做角度换算。
struct OutputGeometry {
    int screen_w = 0;        // 桌面方向的像素宽（角度换算用的 W）
    int screen_h = 0;        // 桌面方向的像素高
    bool swap_axes = false;  // 纹理 x ↔ 桌面 y
};

// rotation_deg ∈ {0, 90, 180, 270}（其他值按 0）；tex_w / tex_h = 复制得到的纹理尺寸。
OutputGeometry output_geometry(int rotation_deg, int tex_w, int tex_h);

// ---------------------------------------------------------------- 重建 / 放弃（design D8 on_build_fail）

// 连续失败计数：E_ACCESSDENIED（UAC 安全桌面、锁屏）不计数；其他错误累计到 kMaxConsecutiveFailures 次 → 放弃。
class RebuildPolicy {
public:
    static constexpr std::int32_t kAccessDenied = static_cast<std::int32_t>(0x80070005u);   // E_ACCESSDENIED

    // 记录一次建立失败；返回 true 表示应进入 Failed。
    bool on_failure(std::int32_t hr);
    // 建立成功并确认可用：清零。
    void on_success() { count_ = 0; }
    // feature_on 关 → 开：清零（含最后一次 HRESULT）。
    void reset() {
        count_ = 0;
        last_hr_ = 0;
    }
    int count() const { return count_; }
    std::int32_t last_hr() const { return last_hr_; }

private:
    int count_ = 0;
    std::int32_t last_hr_ = 0;
};

// ---------------------------------------------------------------- 性能统计（design D12）

// 一个 5 s 统计窗口。暂停时不累计也不输出；恢复取帧时 reset。
struct PerfWindow {
    double start_s = 0;
    double thread_cpu_start_s = 0;   // 截屏线程（内核 + 用户）CPU 时间
    std::uint64_t frames = 0;        // 读回并估计的新桌面帧
    std::uint64_t timeouts = 0;      // AcquireNextFrame 超时
    std::uint64_t lowconf = 0;       // 有前一帧可比、但低置信度 / 无纹理的估计
    std::uint64_t skipped = 0;       // 取到但被 FrameRateLimiter 跳过（未处理）的新桌面帧
    double gpu_ms_sum = 0;           // 降采样 GPU 耗时（时间戳查询）
    std::uint64_t gpu_samples = 0;
    double cpu_ms_sum = 0;           // 每帧读回 + 估计的 CPU 耗时
    double speed_sum = 0;            // 每次发布值之和
    std::uint64_t speed_samples = 0;

    void reset(double now_s, double thread_cpu_s);
    double gpu_ms_avg() const { return gpu_samples > 0 ? gpu_ms_sum / static_cast<double>(gpu_samples) : -1.0; }
    double cpu_ms_avg() const { return frames > 0 ? cpu_ms_sum / static_cast<double>(frames) : 0.0; }
    double speed_avg() const { return speed_samples > 0 ? speed_sum / static_cast<double>(speed_samples) : 0.0; }
    // 本窗口内截屏线程的单核 CPU 占比（%）。
    double cpu_pct(double now_s, double thread_cpu_s) const;
};

// "capture perf: frames=<n> timeouts=<n> lowconf=<n> gpu_ms=<x.xxx> cpu_ms=<x.xxx> cpu_pct=<x.xx> speed_avg=<x.x> skipped=<n>"
// （spec「截屏性能日志」；GPU 计时不可用时 gpu_ms=-1.000）。skipped 为后加字段，追加在末尾以保持旧解析方式可用；
// frames 只数处理过的帧，所以高刷新率显示器上 frames 约为 5 s × 处理帧率，frames + skipped 才是取到的新帧数。
std::string format_perf_line(const PerfWindow& w, double cpu_pct);

}  // namespace mc
