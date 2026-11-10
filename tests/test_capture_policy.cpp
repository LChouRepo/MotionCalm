// test_capture_policy.cpp — 截屏采集的纯逻辑：参数收拢、降采样采样布局（1080p 精确盒式平均）、显示器旋转、重建 / 放弃计数、速度过期、处理帧限速（60–240 Hz 帧流）、capture perf: 行格式。
#include "test_framework.h"

#include "capture/capture_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

using namespace mc;

namespace {

constexpr std::int32_t kHrAccessLost = static_cast<std::int32_t>(0x887A0026u);   // DXGI_ERROR_ACCESS_LOST
constexpr std::int32_t kHrUnsupported = static_cast<std::int32_t>(0x887A0004u);  // DXGI_ERROR_UNSUPPORTED

// 样本 i 的源像素坐标（相对格子原点）：(i + 0.5)·step。
double tap_pos(const DownscaleLayout& l, int i) { return (i + 0.5) * l.step_x; }

// 确定性的均匀抖动 [-1, 1)（线性同余），让限速测试可重复。
struct Jitter {
    std::uint32_t s;
    double next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 8388608.0 - 1.0;   // 24 位 → [-1, 1)
    }
};

struct StreamStats {
    int frames = 0;
    int processed = 0;
    double min_interval = 1e9;   // 相邻处理帧的间隔（秒）
    double max_interval = 0;
    double last_t = -1;
};

// 向限速器喂 seconds 秒的 hz 帧流（每个时间戳加 ±jitter_s 的均匀抖动），从 t0 开始。
StreamStats feed(FrameRateLimiter& lim, double hz, double jitter_s, double seconds, double t0 = 100.0,
                 std::uint32_t seed = 12345u) {
    StreamStats st;
    Jitter j{seed};
    const double p = 1.0 / hz;
    const int n = static_cast<int>(seconds * hz + 0.5);
    for (int i = 0; i < n; ++i) {
        const double t = t0 + i * p + j.next() * jitter_s;
        ++st.frames;
        if (lim.on_frame(t)) {
            ++st.processed;
            if (st.last_t >= 0) {
                st.min_interval = std::min(st.min_interval, t - st.last_t);
                st.max_interval = std::max(st.max_interval, t - st.last_t);
            }
            st.last_t = t;
        }
    }
    return st;
}

}  // namespace

// ---------------------------------------------------------------- 参数

TEST(capture_params_defaults_match_spec) {
    const CaptureParams p;
    CHECK_EQ(p.grid_w, 160);
    CHECK_EQ(p.grid_h, 90);
    CHECK_EQ(p.max_shift, 20);
    CHECK_NEAR(p.min_correlation, 0.60, 1e-12);
    CHECK_NEAR(p.assumed_hfov_deg, 90.0, 1e-12);
    CHECK_EQ(p.hold_ms, 100);
    const CaptureParams s = sanitize_capture_params(p);
    CHECK_EQ(s.grid_w, 160);
    CHECK_EQ(s.max_shift, 20);
    CHECK_NEAR(s.min_correlation, 0.60, 1e-12);
}

TEST(capture_params_sanitize_clamps_to_spec_ranges) {
    CaptureParams p;
    p.grid_w = 10;
    p.grid_h = 1000;
    p.max_shift = 100;            // spec：→ 40
    p.min_correlation = 1.5;      // spec：→ 0.99
    p.assumed_hfov_deg = std::nan("");
    p.hold_ms = -5;
    CaptureParams s = sanitize_capture_params(p);
    CHECK_EQ(s.grid_w, 64);
    CHECK_EQ(s.grid_h, 180);
    CHECK_EQ(s.max_shift, 40);
    CHECK_NEAR(s.min_correlation, 0.99, 1e-12);
    CHECK_NEAR(s.assumed_hfov_deg, 90.0, 1e-12);
    CHECK_EQ(s.hold_ms, 0);
    // 变更 5
    CaptureParams q;
    q.baseline_ms = 400;
    q.detect_roll = false;
    CHECK_EQ(sanitize_capture_params(q).baseline_ms, 150);
    CHECK_FALSE(sanitize_capture_params(q).detect_roll);
    q.baseline_ms = -1;
    CHECK_EQ(sanitize_capture_params(q).baseline_ms, 0);
    CHECK_EQ(CaptureParams{}.baseline_ms, 66);
    CHECK_TRUE(CaptureParams{}.detect_roll);
    p.grid_w = 1000;
    p.grid_h = 1;
    p.max_shift = 0;
    p.min_correlation = 0.0;
    p.assumed_hfov_deg = 500;
    p.hold_ms = 5000;
    s = sanitize_capture_params(p);
    CHECK_EQ(s.grid_w, 320);
    CHECK_EQ(s.grid_h, 36);
    CHECK_EQ(s.max_shift, 2);
    CHECK_NEAR(s.min_correlation, 0.10, 1e-12);
    CHECK_NEAR(s.assumed_hfov_deg, 170.0, 1e-12);
    CHECK_EQ(s.hold_ms, 1000);
    CaptureParams a, b;
    b.max_shift = 7;
    CHECK_TRUE(same_grid(a, b));
    b.grid_h = 108;
    CHECK_FALSE(same_grid(a, b));
}

TEST(capture_state_names) {
    CHECK_EQ(std::string(capture_state_name(CaptureState::Off)), std::string("off"));
    CHECK_EQ(std::string(capture_state_name(CaptureState::Paused)), std::string("paused"));
    CHECK_EQ(std::string(capture_state_name(CaptureState::Running)), std::string("running"));
    CHECK_EQ(std::string(capture_state_name(CaptureState::Recovering)), std::string("recovering"));
    CHECK_EQ(std::string(capture_state_name(CaptureState::Failed)), std::string("failed"));
}

// ---------------------------------------------------------------- 降采样布局

TEST(capture_layout_1080p_exact_box) {   // design D4：cell 12 px、6×6 样本、步长 2 px，样本落在像素公共角上
    const DownscaleLayout l = downscale_layout(1920, 1080, 160, 90);
    CHECK_NEAR(l.cell_w, 12.0, 1e-12);
    CHECK_NEAR(l.cell_h, 12.0, 1e-12);
    CHECK_EQ(l.taps_x, 6);
    CHECK_EQ(l.taps_y, 6);
    CHECK_NEAR(l.step_x, 2.0, 1e-12);
    CHECK_NEAR(l.step_y, 2.0, 1e-12);
    // 像素 k 的中心在 k + 0.5；样本在奇数整数坐标 → 正好是两个像素的分界 → 双线性 = 2×2 像素平均；6 个样本铺满 12 px。
    for (int i = 0; i < l.taps_x; ++i) {
        const double p = tap_pos(l, i);
        CHECK_NEAR(p, 1.0 + 2.0 * i, 1e-12);
    }
}

TEST(capture_layout_common_resolutions_cover_cells) {
    struct Case {
        int w, h, gw, gh, tx, ty;
    };
    const Case cases[] = {
        {3840, 2160, 160, 90, 12, 12},   // 4K：cell 24 → 12 样本、步长 2（仍是精确盒式平均）
        {2560, 1440, 160, 90, 8, 8},     // cell 16
        {1280, 720, 160, 90, 4, 4},      // cell 8
        {1366, 768, 160, 90, 5, 5},      // cell 8.54 × 8.53：步长 ≈ 1.71
        {3440, 1440, 160, 90, 11, 8},    // 超宽屏：cell 21.5 × 16
        {1920, 1200, 160, 90, 6, 7},     // cell 12 × 13.33
        {1920, 1080, 192, 108, 5, 5},    // grid 改为 192×108：cell 10
        {1920, 1080, 64, 36, 15, 15},    // 最小网格：cell 30
    };
    for (const Case& c : cases) {
        const DownscaleLayout l = downscale_layout(c.w, c.h, c.gw, c.gh);
        CHECK_EQ(l.taps_x, c.tx);
        CHECK_EQ(l.taps_y, c.ty);
        CHECK(l.step_x <= 2.0 + 1e-9);   // 每个双线性样本覆盖 2 px → 样本间无空隙
        CHECK(l.step_y <= 2.0 + 1e-9);
        CHECK_NEAR(l.step_x * l.taps_x, l.cell_w, 1e-9);
        CHECK_NEAR(l.step_y * l.taps_y, l.cell_h, 1e-9);
    }
}

TEST(capture_layout_extremes) {
    DownscaleLayout l = downscale_layout(7680, 4320, 64, 36);   // cell 120 → 封顶 16 样本
    CHECK_EQ(l.taps_x, kMaxDownscaleTaps);
    CHECK_EQ(l.taps_y, kMaxDownscaleTaps);
    CHECK_NEAR(l.step_x, 7.5, 1e-12);
    l = downscale_layout(160, 90, 160, 90);   // 1:1
    CHECK_EQ(l.taps_x, 1);
    CHECK_NEAR(l.step_x, 1.0, 1e-12);
    CHECK_NEAR(tap_pos(l, 0), 0.5, 1e-12);   // 像素中心 = 点采样
    l = downscale_layout(0, 1080, 160, 90);
    CHECK_EQ(l.taps_x, 1);
    CHECK_EQ(l.taps_y, 1);
    CHECK_EQ(l.cell_w, 0.0);
    l = downscale_layout(1920, 1080, 0, 90);
    CHECK_EQ(l.cell_h, 0.0);
}

// ---------------------------------------------------------------- 旋转

TEST(capture_output_geometry_rotation) {
    OutputGeometry g = output_geometry(0, 1920, 1080);
    CHECK_EQ(g.screen_w, 1920);
    CHECK_EQ(g.screen_h, 1080);
    CHECK_FALSE(g.swap_axes);
    g = output_geometry(180, 1920, 1080);
    CHECK_EQ(g.screen_w, 1920);
    CHECK_FALSE(g.swap_axes);
    g = output_geometry(90, 1920, 1080);   // 竖屏：桌面 1080 宽
    CHECK_EQ(g.screen_w, 1080);
    CHECK_EQ(g.screen_h, 1920);
    CHECK_TRUE(g.swap_axes);
    g = output_geometry(270, 2560, 1440);
    CHECK_EQ(g.screen_w, 1440);
    CHECK_TRUE(g.swap_axes);
    g = output_geometry(45, 1920, 1080);   // 未知值按 0
    CHECK_EQ(g.screen_w, 1920);
    CHECK_FALSE(g.swap_axes);
}

// ---------------------------------------------------------------- 重建 / 放弃

TEST(capture_rebuild_policy_gives_up_after_10) {   // spec：连续 10 次非 E_ACCESSDENIED 失败 → 放弃
    RebuildPolicy p;
    for (int i = 1; i <= 9; ++i) {
        CHECK_FALSE(p.on_failure(kHrUnsupported));
        CHECK_EQ(p.count(), i);
    }
    CHECK_TRUE(p.on_failure(kHrAccessLost));
    CHECK_EQ(p.count(), 10);
    CHECK_EQ(p.last_hr(), kHrAccessLost);
    p.reset();   // feature_on 关 → 开
    CHECK_EQ(p.count(), 0);
    CHECK_EQ(p.last_hr(), 0);
    CHECK_FALSE(p.on_failure(kHrUnsupported));
}

TEST(capture_rebuild_policy_access_denied_not_counted) {   // spec：UAC 30 s 期间每次都是 E_ACCESSDENIED → 不放弃
    RebuildPolicy p;
    for (int i = 0; i < 100; ++i) CHECK_FALSE(p.on_failure(RebuildPolicy::kAccessDenied));
    CHECK_EQ(p.count(), 0);
    CHECK_EQ(p.last_hr(), RebuildPolicy::kAccessDenied);
    // 交错：E_ACCESSDENIED 既不计数也不清零
    for (int i = 0; i < 5; ++i) p.on_failure(kHrUnsupported);
    p.on_failure(RebuildPolicy::kAccessDenied);
    for (int i = 0; i < 4; ++i) CHECK_FALSE(p.on_failure(kHrUnsupported));
    CHECK_EQ(p.count(), 9);
    p.on_success();   // 成功建立并确认可用 → 清零
    CHECK_EQ(p.count(), 0);
    for (int i = 0; i < 9; ++i) CHECK_FALSE(p.on_failure(kHrUnsupported));
    CHECK_TRUE(p.on_failure(kHrUnsupported));
}

TEST(capture_constants_match_spec) {
    CHECK_EQ(kAcquireTimeoutMs, 16u);
    CHECK_NEAR(kRetryIntervalS, 2.0, 1e-12);
    CHECK_EQ(kMaxConsecutiveFailures, 10);
    CHECK_NEAR(kReleaseAfterPauseS, 5.0, 1e-12);
    CHECK_NEAR(kSpeedMaxAgeS, 0.25, 1e-12);
    CHECK_EQ(kDiscardFramesAfterBuild, 2);
    CHECK_NEAR(kPerfLogIntervalS, 5.0, 1e-12);
    CHECK_NEAR(kTargetFrameIntervalS, 0.01375, 1e-12);
    CHECK_NEAR(FrameRateLimiter{}.target(), kTargetFrameIntervalS, 1e-15);
}

// ---------------------------------------------------------------- 处理帧限速

TEST(capture_limiter_60_75_100hz_never_decimated) {   // 60 / 75 Hz（以及 90 / 100 Hz）带抖动也一帧不丢
    {
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 60.0, 0.003, 10.0);   // 每个时间戳 ±3 ms
        CHECK_EQ(st.frames, 600);
        CHECK_EQ(st.processed, st.frames);
    }
    {
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 75.0, 0.0019, 10.0);  // ±1.9 ms（理论容限约 ±2.08 ms）
        CHECK_EQ(st.frames, 750);
        CHECK_EQ(st.processed, st.frames);
    }
    {
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 90.0, 0.0005, 5.0);
        CHECK_EQ(st.processed, st.frames);
    }
    {
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 100.0, 0.0003, 5.0);
        CHECK_EQ(st.processed, st.frames);
    }
    {   // 60 fps 的游戏在 144 Hz 显示器上（无 VRR）：帧间隔在 2 / 3 个刷新周期（13.9 / 20.8 ms）之间交替 → 全部处理
        FrameRateLimiter lim;
        int processed = 0;
        for (int i = 0; i < 600; ++i) {
            if (lim.on_frame(100.0 + std::floor(i * 2.4) / 144.0)) ++processed;
        }
        CHECK_EQ(processed, 600);
    }
}

TEST(capture_limiter_high_refresh_rates) {
    const double T = kTargetFrameIntervalS;
    {   // 120 Hz ±0.25 ms：每隔一帧 → 60 Hz
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 120.0, 0.00025, 2.0);
        CHECK_EQ(st.frames, 240);
        CHECK_EQ(st.processed, 120);
        CHECK(st.min_interval >= 2.0 / 120.0 - 0.0005 - 1e-9);
        CHECK(st.max_interval <= 2.0 / 120.0 + 0.0005 + 1e-9);
    }
    {   // 144 Hz ±0.5 ms → 72 Hz
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 144.0, 0.0005, 2.0);
        CHECK_EQ(st.frames, 288);
        CHECK_EQ(st.processed, 144);
    }
    {   // 165 Hz ±0.3 ms → 82.5 Hz
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 165.0, 0.0003, 2.0);
        CHECK_EQ(st.frames, 330);
        CHECK_EQ(st.processed, 165);
    }
    {   // 240 Hz ±0.1 ms：每 3 帧处理 1 帧 → 80 Hz
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 240.0, 0.0001, 2.0);
        CHECK_EQ(st.frames, 480);
        CHECK_EQ(st.processed, 160);
        CHECK(st.min_interval >= 3.0 / 240.0 - 0.0002 - 1e-9);
        CHECK(st.max_interval <= 3.0 / 240.0 + 0.0002 + 1e-9);
    }
    {   // 240 Hz 抖动大到越过边界（±0.4 ms）：在每 3 帧 / 每 4 帧之间切换，处理帧率仍在 60–80 Hz
        FrameRateLimiter lim;
        const StreamStats st = feed(lim, 240.0, 0.0004, 2.0);
        CHECK(st.processed >= 119);
        CHECK(st.processed <= 161);
        CHECK(st.min_interval >= 2.0 * T / 3.0 - 1e-9);   // 硬下限：G ≥ target − g/2 且 g ≤ G
    }
}

TEST(capture_limiter_rate_sweep_bounds) {   // 51–499 Hz 的均匀帧流：≤ 109 Hz 全部处理；否则处理间隔 ∈ [T − p/2, T + p/2]
    const double T = kTargetFrameIntervalS;
    for (int hz = 51; hz < 500; hz += 7) {
        FrameRateLimiter lim;
        const double p = 1.0 / hz;
        const StreamStats st = feed(lim, static_cast<double>(hz), 0.0, 1.0);
        if (1.5 * p >= T) {
            CHECK_EQ(st.processed, st.frames);
        } else {
            CHECK(st.processed < st.frames);
            CHECK(st.min_interval >= T - p / 2 - 1e-9);
            CHECK(st.max_interval <= T + p / 2 + 1e-9);
            CHECK(st.max_interval <= 4.0 * T / 3.0 + 1e-9);   // 处理帧率不低于约 54.5 Hz（max_shift 的可测速度范围不低于 60 Hz 显示器太多）
            CHECK(st.min_interval >= 2.0 * T / 3.0 - 1e-9);   // 不高于约 109 Hz
            CHECK(st.processed >= 54);
        }
    }
}

TEST(capture_limiter_first_frame_and_bad_timestamps) {
    FrameRateLimiter lim;
    CHECK_TRUE(lim.on_frame(5.0));      // 首帧
    CHECK_FALSE(lim.on_frame(5.004));   // 4 ms 后：6 ms < 13.75 ms
    CHECK_FALSE(lim.on_frame(5.008));
    CHECK_TRUE(lim.on_frame(5.012));    // 12 + 2 ≥ 13.75
    // 时间戳为 0 / 负数 / 非有限：照常处理，并且下一帧重新开始
    CHECK_TRUE(lim.on_frame(0.0));
    CHECK_TRUE(lim.on_frame(5.014));    // reset 之后的首帧
    CHECK_FALSE(lim.on_frame(5.016));
    CHECK_TRUE(lim.on_frame(-1.0));
    CHECK_TRUE(lim.on_frame(std::numeric_limits<double>::quiet_NaN()));
    CHECK_TRUE(lim.on_frame(std::numeric_limits<double>::infinity()));
    CHECK_TRUE(lim.on_frame(5.018));
    CHECK_FALSE(lim.on_frame(5.020));
    // 时间戳倒退 / 重复：处理并以它重新锚定（与限速之前的行为相同；估计器自己把 dt ≤ 0 当无效）
    CHECK_TRUE(lim.on_frame(4.0));
    CHECK_FALSE(lim.on_frame(4.004));
    CHECK_TRUE(lim.on_frame(4.004));    // 与上一到达帧相同
    CHECK_FALSE(lim.on_frame(4.006));
    // 任意 target：0 → 不限速
    FrameRateLimiter none(0.0);
    for (int i = 0; i < 10; ++i) CHECK_TRUE(none.on_frame(1.0 + i * 0.001));
}

TEST(capture_limiter_resume_after_pause) {
    FrameRateLimiter lim;
    StreamStats st = feed(lim, 240.0, 0.0, 1.0, 100.0);
    CHECK_EQ(st.processed, 80);
    // 画面停了 3 s（不 reset）：恢复后的第一帧一定处理，随后回到每 3 帧 1 帧
    st = feed(lim, 240.0, 0.0, 1.0, 104.0);
    CHECK_EQ(st.processed, 80);
    // 暂停（reset）后恢复：第一帧处理，即使时间戳离 reset 前的处理帧很近
    FrameRateLimiter lim2;
    CHECK_TRUE(lim2.on_frame(10.0));
    lim2.reset();
    CHECK_TRUE(lim2.on_frame(10.001));
    CHECK_FALSE(lim2.on_frame(10.005));
    // reset 之后时间戳比 reset 之前还早（另一个会话 / 显示器）：照样处理
    lim2.reset();
    CHECK_TRUE(lim2.on_frame(9.0));
}

TEST(capture_speed_freshness) {   // spec：发布时刻早于当前 250 ms 以上 → 主线程用 0
    CHECK_TRUE(speed_is_fresh(100.0, 100.0));
    CHECK_TRUE(speed_is_fresh(100.0, 100.25));
    CHECK_FALSE(speed_is_fresh(100.0, 100.2501));
    CHECK_FALSE(speed_is_fresh(100.0, 100.4));   // 截屏线程被阻塞 400 ms
    CHECK_FALSE(speed_is_fresh(-1e300, 5.0));    // 从未发布
}

// ---------------------------------------------------------------- 性能统计

TEST(capture_perf_line_format) {
    PerfWindow w;
    w.reset(10.0, 1.0);
    CHECK_EQ(w.gpu_ms_avg(), -1.0);   // 没有 GPU 样本
    CHECK_EQ(w.cpu_ms_avg(), 0.0);
    CHECK_EQ(w.speed_avg(), 0.0);
    CHECK_EQ(format_perf_line(w, 0.0),
             std::string("capture perf: frames=0 timeouts=0 lowconf=0 gpu_ms=-1.000 cpu_ms=0.000 cpu_pct=0.00 "
                         "speed_avg=0.0 skipped=0"));
    w.frames = 300;
    w.timeouts = 12;
    w.lowconf = 4;
    w.skipped = 900;   // 240 Hz：每 3 帧处理 1 帧
    w.gpu_ms_sum = 15.0;
    w.gpu_samples = 300;
    w.cpu_ms_sum = 36.0;
    w.speed_sum = 3375.0;
    w.speed_samples = 10;
    // 5 s 墙钟内线程用了 0.05 s CPU → 1.00 %
    const double pct = w.cpu_pct(15.0, 1.05);
    CHECK_NEAR(pct, 1.0, 1e-9);
    CHECK_EQ(format_perf_line(w, pct),
             std::string("capture perf: frames=300 timeouts=12 lowconf=4 gpu_ms=0.050 cpu_ms=0.120 cpu_pct=1.00 "
                         "speed_avg=337.5 skipped=900"));
    // spec：每行包含 7 个字段，按顺序；后加的 skipped 追加在末尾（旧的按前缀 / 字段名解析不受影响）
    const std::string line = format_perf_line(w, pct);
    std::size_t pos = 0;
    for (const char* key : {"frames=", "timeouts=", "lowconf=", "gpu_ms=", "cpu_ms=", "cpu_pct=", "speed_avg=",
                            "skipped="}) {
        const std::size_t at = line.find(key, pos);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) pos = at;
    }
    CHECK_EQ(line.rfind("capture perf: ", 0), 0u);
    w.reset(20.0, 2.0);   // 新窗口清零
    CHECK_EQ(w.frames, 0u);
    CHECK_EQ(w.skipped, 0u);
    CHECK_NEAR(w.start_s, 20.0, 1e-12);
    CHECK_EQ(w.cpu_pct(20.0, 3.0), 0.0);   // 墙钟 0 → 0
    CHECK_EQ(w.cpu_pct(21.0, 1.0), 0.0);   // CPU 时间倒退 → 0
}
