// test_capture_policy.cpp — 截屏采集的纯逻辑：参数收拢、降采样采样布局（1080p 精确盒式平均）、显示器旋转、重建 / 放弃计数、速度过期、capture perf: 行格式。
#include "test_framework.h"

#include "capture/capture_policy.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace mc;

namespace {

constexpr std::int32_t kHrAccessLost = static_cast<std::int32_t>(0x887A0026u);   // DXGI_ERROR_ACCESS_LOST
constexpr std::int32_t kHrUnsupported = static_cast<std::int32_t>(0x887A0004u);  // DXGI_ERROR_UNSUPPORTED

// 样本 i 的源像素坐标（相对格子原点）：(i + 0.5)·step。
double tap_pos(const DownscaleLayout& l, int i) { return (i + 0.5) * l.step_x; }

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
                         "speed_avg=0.0"));
    w.frames = 300;
    w.timeouts = 12;
    w.lowconf = 4;
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
                         "speed_avg=337.5"));
    // spec：每行包含 7 个字段，按顺序
    const std::string line = format_perf_line(w, pct);
    std::size_t pos = 0;
    for (const char* key : {"frames=", "timeouts=", "lowconf=", "gpu_ms=", "cpu_ms=", "cpu_pct=", "speed_avg="}) {
        const std::size_t at = line.find(key, pos);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) pos = at;
    }
    CHECK_EQ(line.rfind("capture perf: ", 0), 0u);
    w.reset(20.0, 2.0);   // 新窗口清零
    CHECK_EQ(w.frames, 0u);
    CHECK_NEAR(w.start_s, 20.0, 1e-12);
    CHECK_EQ(w.cpu_pct(20.0, 3.0), 0.0);   // 墙钟 0 → 0
    CHECK_EQ(w.cpu_pct(21.0, 1.0), 0.0);   // CPU 时间倒退 → 0
}
