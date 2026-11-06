// intensity.cpp — model/intensity 的实现（纯逻辑，不含 Win32）。
#include "model/intensity.h"

#include <algorithm>
#include <cmath>

namespace mc {

double smoothstep(double e0, double e1, double x) {
    if (!(e1 > e0)) return x >= e1 ? 1.0 : 0.0;
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

Thresholds thresholds_for(Sensitivity s, const std::array<Thresholds, 3>& table) {
    const auto i = static_cast<std::size_t>(s);
    return i < table.size() ? table[i] : table[1];
}

Thresholds thresholds_for(Sensitivity s, const TriggerConfig& trigger) { return thresholds_for(s, trigger.thresholds); }

double capture_threshold_scale(Sensitivity capture_sensitivity) {
    switch (capture_sensitivity) {
        case Sensitivity::Low: return 2.0;
        case Sensitivity::Medium: return 1.5;
        case Sensitivity::High: return 1.0;
    }
    return 1.5;
}

// ---------------------------------------------------------------- IntensityModel

void IntensityModel::configure(const TriggerConfig& trigger, const MaskConfig& mask) {
    trigger_ = trigger;
    capture_k_ = capture_threshold_scale(trigger.capture_sensitivity);
    attack_s_ = std::max(1, mask.attack_ms) / 1000.0;
    release_s_ = std::max(1, mask.release_ms) / 1000.0;
}

double IntensityModel::compute_target(const IntensityInputs& in, IntensityTerms* terms) const {
    const Thresholds th = thresholds_for(trigger_.sensitivity, trigger_);
    const double move = std::clamp(trigger_.move_intensity, 0.0, 1.0);
    IntensityTerms t;
    if (in.gate_mouse) t.mouse = smoothstep(th.v_start, th.v_full, in.mouse_deg_per_s);
    if (in.gate_keys && in.moving) t.keys = move;
    if (in.gate_pad) {
        t.pad = smoothstep(th.v_start, th.v_full, in.pad_deg_per_s);
        t.pad_move = std::clamp(in.pad_move, 0.0, 1.0) * move;
    }
    // 截屏项：阈值与鼠标同档，再乘 capture_sensitivity 的保守系数 k。
    // 变更 5：前进 / 后退（乘 forward_gain）与倾斜用各自的阈值表（同样按 sensitivity 选档，不乘 k），同样受 gate_capture 门控。
    if (in.gate_capture) {
        t.capture = smoothstep(capture_k_ * th.v_start, capture_k_ * th.v_full, in.capture_deg_per_s);
        const Thresholds zt = thresholds_for(trigger_.sensitivity, trigger_.zoom_thresholds);
        const Thresholds rt = thresholds_for(trigger_.sensitivity, trigger_.roll_thresholds);
        t.capture_zoom = smoothstep(zt.v_start, zt.v_full,
                                    std::clamp(trigger_.forward_gain, 0.0, 10.0) * in.capture_zoom_deg_per_s);
        t.capture_roll = smoothstep(rt.v_start, rt.v_full, in.capture_roll_deg_per_s);
    }
    if (terms) *terms = t;
    const double target = std::max({t.mouse, t.keys, t.pad, t.pad_move, t.capture, t.capture_zoom, t.capture_roll});
    return std::clamp(target, 0.0, 1.0);
}

float IntensityModel::update(const IntensityInputs& in, double dt_seconds) {
    target_ = compute_target(in, &terms_);
    double dt = dt_seconds;
    if (!(dt > 0.0)) dt = 0.0;   // 负数与 NaN 都按 0
    if (dt > kMaxDt) dt = kMaxDt;
    if (dt > 0.0) {
        const double tau = (target_ > value_ ? attack_s_ : release_s_) / 3.0;
        value_ += (target_ - value_) * (1.0 - std::exp(-dt / tau));
    }
    if (std::fabs(target_ - value_) < kSnap) value_ = target_;
    value_ = std::clamp(value_, 0.0, 1.0);
    return static_cast<float>(value_);
}

void IntensityModel::reset(double v) {
    value_ = std::clamp(v, 0.0, 1.0);
    target_ = value_;
    terms_ = IntensityTerms{};
}

// ---------------------------------------------------------------- VelocityWindow

void VelocityWindow::set_window(double window_s) { window_s_ = window_s > 0.0 ? window_s : 0.001; }

void VelocityWindow::add_span(double t0_s, double t1_s, double dx, double dy, double path, std::uint32_t events) {
    if (ring_.empty()) ring_.resize(kCapacity);
    // 淘汰已经不可能与任何窗口重叠的旧记录（t1 ≤ t1_s − window；求和窗口的末端不早于最新记录的 t1）。
    while (count_ > 0 && ring_[head_].t1 <= t1_s - window_s_) {
        head_ = (head_ + 1) % kCapacity;
        --count_;
    }
    if (count_ == kCapacity) {   // 满了：丢最旧的
        head_ = (head_ + 1) % kCapacity;
        --count_;
    }
    if (!(t0_s < t1_s)) t0_s = t1_s;   // 点事件
    if (path < 0.0) path = std::sqrt(dx * dx + dy * dy);
    ring_[(head_ + count_) % kCapacity] = Event{t0_s, t1_s, dx, dy, path, events};
    ++count_;
}

double VelocityWindow::overlap(const Event& e, double lo, double hi) {
    if (!(e.t0 < e.t1)) return (e.t1 > lo && e.t1 <= hi) ? 1.0 : 0.0;
    const double a = std::max(e.t0, lo);
    const double b = std::min(e.t1, hi);
    // 1 ns 以下的重叠是 (now − window) 的舍入误差：停止 50 ms 后速度必须恰好为 0。
    return b - a > 1e-9 ? (b - a) / (e.t1 - e.t0) : 0.0;
}

template <class F>
void VelocityWindow::for_each_overlapping(double lo, double hi, F&& f) const {
    for (std::size_t k = 0; k < count_; ++k) {
        const Event& e = ring_[(head_ + k) % kCapacity];
        const double w = overlap(e, lo, hi);
        if (w > 0.0) f(e, w);
    }
}

double VelocityWindow::counts_per_s(double now_s) const {
    double end = now_s;
    if (max_hold_s_ > 0.0 && count_ > 0) {
        // 数据地平线：最新一段之后的下一批还没到期时，窗口停在最新一段的末尾（见头文件）。
        // 1.5 × 段长：容忍上报间隔的抖动（例如 8 ms 一包偶尔 8.5 ms 才到）。
        const Event& last = ring_[(head_ + count_ - 1) % kCapacity];
        const double hold = std::min(max_hold_s_, 1.5 * (last.t1 - last.t0));
        if (now_s > last.t1 && now_s - last.t1 <= hold) end = last.t1;
    }
    double sx = 0, sy = 0;
    for_each_overlapping(end - window_s_, end, [&](const Event& e, double w) {
        sx += e.dx * w;
        sy += e.dy * w;
    });
    return std::sqrt(sx * sx + sy * sy) / window_s_;
}

double VelocityWindow::path_length(double now_s) const {
    double sum = 0;
    for_each_overlapping(now_s - window_s_, now_s, [&](const Event& e, double w) { sum += e.path * w; });
    return sum;
}

std::uint32_t VelocityWindow::event_count(double now_s) const {
    const double lo = now_s - window_s_;
    std::uint32_t n = 0;
    for (std::size_t k = 0; k < count_; ++k) {
        const Event& e = ring_[(head_ + k) % kCapacity];
        if (e.t1 > lo && e.t1 <= now_s) n += e.events;
    }
    return n;
}

void VelocityWindow::clear() {
    head_ = 0;
    count_ = 0;
}

// ---------------------------------------------------------------- BatchTimeline

void BatchTimeline::reset(double now_s) {
    last_read_ = now_s;
    last_motion_ = -1e300;
}

double BatchTimeline::on_read(double now_s, bool has_motion) {
    double start = last_read_;
    if (has_motion && now_s - last_motion_ <= kContinuousGapS) start = last_motion_;
    start = std::clamp(start, now_s - kMaxSpanS, now_s);
    if (now_s > last_read_) last_read_ = now_s;
    if (has_motion) last_motion_ = now_s;
    return start;
}

// ---------------------------------------------------------------- deadzone

double apply_radial_deadzone(double x, double y, double dz) {
    const double m = std::min(1.0, std::sqrt(x * x + y * y) / 32767.0);
    dz = std::clamp(dz, 0.0, 0.99);
    if (m < dz) return 0.0;
    return std::clamp((m - dz) / (1.0 - dz), 0.0, 1.0);
}

}  // namespace mc
