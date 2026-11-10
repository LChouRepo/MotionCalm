// capture_policy.cpp — capture_policy.h 的实现（纯标准库）。
#include "capture/capture_policy.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace mc {

namespace {

int clamp_int(int v, int lo, int hi) { return std::clamp(v, lo, hi); }

double clamp_real(double v, double lo, double hi, double def) {
    if (!std::isfinite(v)) return def;
    return std::clamp(v, lo, hi);
}

int taps_for(double cell) {
    if (!(cell > 0)) return 1;
    // 1e-9：cell 恰为偶数整数（12.000000001 之类的浮点误差）时不多取一个样本。
    const int t = static_cast<int>(std::ceil(cell / 2.0 - 1e-9));
    return std::clamp(t, 1, kMaxDownscaleTaps);
}

}  // namespace

const char* capture_state_name(CaptureState s) {
    switch (s) {
        case CaptureState::Off: return "off";
        case CaptureState::Paused: return "paused";
        case CaptureState::Running: return "running";
        case CaptureState::Recovering: return "recovering";
        case CaptureState::Failed: return "failed";
    }
    return "?";
}

CaptureParams sanitize_capture_params(const CaptureParams& p) {
    const CaptureParams def;
    CaptureParams r;
    r.grid_w = clamp_int(p.grid_w, 64, 320);
    r.grid_h = clamp_int(p.grid_h, 36, 180);
    r.max_shift = clamp_int(p.max_shift, 2, 40);
    r.min_correlation = clamp_real(p.min_correlation, 0.10, 0.99, def.min_correlation);
    r.assumed_hfov_deg = clamp_real(p.assumed_hfov_deg, 30.0, 170.0, def.assumed_hfov_deg);
    r.hold_ms = clamp_int(p.hold_ms, 0, 1000);
    r.detect_roll = p.detect_roll;
    r.baseline_ms = clamp_int(p.baseline_ms, 0, 150);
    return r;
}

DownscaleLayout downscale_layout(int src_w, int src_h, int grid_w, int grid_h) {
    DownscaleLayout l;
    if (src_w <= 0 || src_h <= 0 || grid_w <= 0 || grid_h <= 0) return l;
    l.cell_w = static_cast<double>(src_w) / grid_w;
    l.cell_h = static_cast<double>(src_h) / grid_h;
    l.taps_x = taps_for(l.cell_w);
    l.taps_y = taps_for(l.cell_h);
    l.step_x = l.cell_w / l.taps_x;
    l.step_y = l.cell_h / l.taps_y;
    return l;
}

OutputGeometry output_geometry(int rotation_deg, int tex_w, int tex_h) {
    OutputGeometry g;
    const bool swap = rotation_deg == 90 || rotation_deg == 270;
    g.swap_axes = swap;
    g.screen_w = swap ? tex_h : tex_w;
    g.screen_h = swap ? tex_w : tex_h;
    return g;
}

bool RebuildPolicy::on_failure(std::int32_t hr) {
    last_hr_ = hr;
    if (hr == kAccessDenied) return false;
    ++count_;
    return count_ >= kMaxConsecutiveFailures;
}

bool FrameRateLimiter::on_frame(double t_s) {
    if (!std::isfinite(t_s) || t_s <= 0) {   // 时间戳不可用：不限速，下一帧重新开始
        has_last_ = false;
        return true;
    }
    if (!has_last_ || t_s <= last_seen_) {   // 首帧 / 时间戳不递增：处理并重新锚定
        has_last_ = true;
        last_processed_ = t_s;
        last_seen_ = t_s;
        return true;
    }
    const double since_processed = t_s - last_processed_;
    const double gap = t_s - last_seen_;
    last_seen_ = t_s;
    if (since_processed + 0.5 * gap >= target_) {
        last_processed_ = t_s;
        return true;
    }
    return false;
}

void PerfWindow::reset(double now_s, double thread_cpu_s) {
    *this = PerfWindow{};
    start_s = now_s;
    thread_cpu_start_s = thread_cpu_s;
}

double PerfWindow::cpu_pct(double now_s, double thread_cpu_s) const {
    const double wall = now_s - start_s;
    if (!(wall > 0)) return 0.0;
    return std::max(0.0, thread_cpu_s - thread_cpu_start_s) / wall * 100.0;
}

std::string format_perf_line(const PerfWindow& w, double cpu_pct) {
    return std::format("capture perf: frames={} timeouts={} lowconf={} gpu_ms={:.3f} cpu_ms={:.3f} cpu_pct={:.2f} "
                       "speed_avg={:.1f} skipped={}",
                       w.frames, w.timeouts, w.lowconf, w.gpu_ms_avg(), w.cpu_ms_avg(), cpu_pct, w.speed_avg(),
                       w.skipped);
}

}  // namespace mc
