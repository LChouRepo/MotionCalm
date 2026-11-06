// motion_estimator.cpp — 投影互相关位移估计、角速度换算与保持 / 归零的实现（纯标准库）。
#include "capture/motion_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace mc {

namespace {

// 原地减去最小二乘直线 a + b·(i − ī)。
void detrend(double* p, int n) {
    if (n <= 0) return;
    double mean = 0;
    for (int i = 0; i < n; ++i) mean += p[i];
    mean /= n;
    if (n < 2) {
        p[0] = 0;
        return;
    }
    const double c = 0.5 * (n - 1);
    double num = 0;
    for (int i = 0; i < n; ++i) num += (i - c) * (p[i] - mean);
    const double den = static_cast<double>(n) * (static_cast<double>(n) * n - 1.0) / 12.0;   // Σ(i − ī)²
    const double b = num / den;
    for (int i = 0; i < n; ++i) p[i] -= mean + b * (i - c);
}

}  // namespace

void project_and_detrend(const float* gray, int width, int height, double* col_out, double* row_out) {
    for (int x = 0; x < width; ++x) col_out[x] = 0;
    for (int y = 0; y < height; ++y) {
        const float* r = gray + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        double s = 0;
        for (int x = 0; x < width; ++x) {
            const double v = r[x];
            col_out[x] += v;
            s += v;
        }
        row_out[y] = s;
    }
    detrend(col_out, width);
    detrend(row_out, height);
}

double shift_to_deg(double dx_cells, double dy_cells, int grid_w, int grid_h, int screen_w_px, int screen_h_px,
                    double assumed_hfov_deg) {
    if (grid_w <= 0 || grid_h <= 0 || screen_w_px <= 0 || screen_h_px <= 0) return 0;
    const double px_x = dx_cells * static_cast<double>(screen_w_px) / grid_w;
    const double px_y = dy_cells * static_cast<double>(screen_h_px) / grid_h;
    return std::hypot(px_x, px_y) * assumed_hfov_deg / screen_w_px;
}

std::optional<double> shift_to_deg_per_s(double dx_cells, double dy_cells, int grid_w, int grid_h, int screen_w_px,
                                         int screen_h_px, double assumed_hfov_deg, double dt_s) {
    if (!(dt_s > 0) || !(dt_s <= kMaxFrameIntervalS)) return std::nullopt;   // 也挡掉 NaN
    if (grid_w <= 0 || grid_h <= 0 || screen_w_px <= 0 || screen_h_px <= 0) return std::nullopt;
    if (!std::isfinite(dx_cells) || !std::isfinite(dy_cells)) return std::nullopt;
    return shift_to_deg(dx_cells, dy_cells, grid_w, grid_h, screen_w_px, screen_h_px, assumed_hfov_deg) / dt_s;
}

// ---------------------------------------------------------------- MotionEstimator

MotionEstimator::MotionEstimator(MotionEstimatorParams p) { configure(p); }

void MotionEstimator::configure(const MotionEstimatorParams& p) {
    params_ = p;
    params_.width = std::max(1, params_.width);
    params_.height = std::max(1, params_.height);
    params_.max_shift = std::max(1, params_.max_shift);
    init_axis(col_, params_.width, params_.height);
    init_axis(row_, params_.height, params_.width);
    reset();
}

void MotionEstimator::init_axis(Axis& a, int n, int pixels) const {
    a.n = n;
    a.pixels = pixels;
    a.max_shift = std::min(params_.max_shift, n / 3);
    a.cur.assign(static_cast<std::size_t>(n), 0.0);
    a.prev.assign(static_cast<std::size_t>(n), 0.0);
    a.rho.assign(static_cast<std::size_t>(2 * std::max(a.max_shift, 0) + 1), 0.0);
}

void MotionEstimator::reset() {
    has_prev_ = false;
    prev_t_ = 0;
    for (Axis* a : {&col_, &row_}) {
        a->prev_energy = a->cur_energy = 0;
        a->prev_textured = a->cur_textured = false;
    }
}

void MotionEstimator::finish_axis(Axis& a) const {
    double e = 0;
    for (const double v : a.cur) e += v * v;
    a.cur_energy = e;
    const double rms_per_px = std::sqrt(e / a.n) / a.pixels;
    a.cur_textured = e > 0 && rms_per_px >= params_.min_texture;
}

MotionEstimator::AxisResult MotionEstimator::estimate_axis(Axis& a) {
    AxisResult r;
    const int n = a.n;
    const int S = a.max_shift;
    if (S < 1 || !a.cur_textured || !a.prev_textured) return r;

    const double* prev = a.prev.data();
    const double* cur = a.cur.data();
    double* rho = a.rho.data();
    // 重叠段方差低于整条投影能量的 1e-12 视为平坦，ρ 记 0（与输入标度无关）。
    const double eps_p = 1e-12 * a.prev_energy;
    const double eps_q = 1e-12 * a.cur_energy;

    // ρ(s)：cur[i] 与 prev[i − s] 在重叠段 Ω_s = {i | 0 ≤ i < n, 0 ≤ i − s < n} 上的皮尔逊相关。
    // ρ(s) = 1 ⇔ cur[i] = prev[i − s]，即内容向 +s 方向移动了 s 格。
    for (int s = -S; s <= S; ++s) {
        const int i0 = s > 0 ? s : 0;
        const int i1 = s < 0 ? n + s : n;
        double sp = 0, sq = 0, spp = 0, sqq = 0, spq = 0;
        for (int i = i0; i < i1; ++i) {
            const double p = prev[i - s];
            const double q = cur[i];
            sp += p;
            sq += q;
            spp += p * p;
            sqq += q * q;
            spq += p * q;
        }
        const double inv_m = 1.0 / (i1 - i0);
        const double vp = spp - sp * sp * inv_m;
        const double vq = sqq - sq * sq * inv_m;
        double v = 0;
        if (vp > eps_p && vq > eps_q) v = std::clamp((spq - sp * sq * inv_m) / std::sqrt(vp * vq), -1.0, 1.0);
        rho[s + S] = v;
    }

    // 峰值；完全并列时取 |s| 较小者（按 0, −1, +1, −2, +2 … 的顺序找严格更大的值）。
    // 注意：严格周期的纹理（网格、地砖）会有多个 ρ ≈ 1 的混叠峰，这里不做歧义检测，可能锁到错误的混叠位移。
    int best = 0;
    double best_rho = rho[S];
    for (int k = 1; k <= S; ++k) {
        if (rho[S - k] > best_rho) {
            best_rho = rho[S - k];
            best = -k;
        }
        if (rho[S + k] > best_rho) {
            best_rho = rho[S + k];
            best = k;
        }
    }

    // 抛物线亚格插值（峰在 ±S 边界时不插值：真实位移可能在范围外）。
    double shift = best;
    if (best > -S && best < S) {
        const double rm = rho[best - 1 + S];
        const double rp = rho[best + 1 + S];
        const double den = rm - 2.0 * best_rho + rp;
        if (den < 0) shift += std::clamp(0.5 * (rm - rp) / den, -0.5, 0.5);
    }
    r.shift = shift;
    r.confidence = std::clamp(best_rho, 0.0, 1.0);
    return r;
}

void MotionEstimator::commit_axis(Axis& a) {
    std::swap(a.cur, a.prev);   // 只交换指针，不分配
    a.prev_energy = a.cur_energy;
    a.prev_textured = a.cur_textured;
}

MotionEstimate MotionEstimator::push(const float* gray, double timestamp_s) {
    MotionEstimate e;
    if (gray == nullptr) return e;

    project_and_detrend(gray, params_.width, params_.height, col_.cur.data(), row_.cur.data());
    finish_axis(col_);
    finish_axis(row_);

    if (has_prev_) {
        e.dt_s = timestamp_s - prev_t_;
        const AxisResult rx = estimate_axis(col_);
        const AxisResult ry = estimate_axis(row_);
        e.dx = rx.shift;
        e.dy = ry.shift;
        e.confidence = std::min(rx.confidence, ry.confidence);
        const bool textured = col_.cur_textured && col_.prev_textured && row_.cur_textured && row_.prev_textured;
        e.valid = textured && e.confidence >= params_.min_correlation;
    }

    commit_axis(col_);
    commit_axis(row_);
    prev_t_ = timestamp_s;
    has_prev_ = true;
    return e;
}

// ---------------------------------------------------------------- SpeedHold

void SpeedHold::configure(const SpeedHoldParams& p) {
    const SpeedHoldParams def;
    params_ = p;
    if (params_.grid_w <= 0) params_.grid_w = def.grid_w;
    if (params_.grid_h <= 0) params_.grid_h = def.grid_h;
    if (params_.screen_w_px <= 0) params_.screen_w_px = def.screen_w_px;
    if (params_.screen_h_px <= 0) params_.screen_h_px = def.screen_h_px;
    if (!(params_.assumed_hfov_deg > 0)) params_.assumed_hfov_deg = def.assumed_hfov_deg;
    if (!(params_.hold_ms >= 0)) params_.hold_ms = def.hold_ms;
}

double SpeedHold::on_estimate(const MotionEstimate& est, double dt_s, double t_s) {
    if (est.valid) {
        const std::optional<double> w =
            shift_to_deg_per_s(est.dx, est.dy, params_.grid_w, params_.grid_h, params_.screen_w_px,
                               params_.screen_h_px, params_.assumed_hfov_deg, dt_s);
        return on_value(w, t_s);
    }
    return value(t_s);
}

double SpeedHold::on_value(std::optional<double> v, double t_s) {
    if (v && std::isfinite(*v)) {
        last_value_ = std::max(0.0, *v);
        last_valid_t_ = t_s;
        has_valid_ = true;
    }
    return value(t_s);
}

double SpeedHold::value(double t_s) const {
    if (!has_valid_) return 0.0;
    // 1e-9 s 容差：让 6 × (1/60 s) 这类累加恰好落在 hold 边界上时仍算"未超过"。
    return (t_s - last_valid_t_ <= params_.hold_ms / 1000.0 + 1e-9) ? last_value_ : 0.0;
}

void SpeedHold::reset() {
    last_value_ = 0;
    last_valid_t_ = 0;
    has_valid_ = false;
}

}  // namespace mc
