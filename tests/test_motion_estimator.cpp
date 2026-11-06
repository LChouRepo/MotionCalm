// test_motion_estimator.cpp — 截屏位移估计的单元测试：确定性合成 160×90 帧（平移 / 亚格 / 噪声 / 亮度突变 / 换图 / 纯色），角速度换算，SpeedHold 保持与归零时间线（绝对时间），耗时。
#include "test_framework.h"

#include "capture/motion_estimator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {

constexpr int kW = 160;
constexpr int kH = 90;
constexpr int kMargin = 48;   // 源图四周比窗口多出的格数：平移 ≤ 48 格时窗口内都是真实纹理
constexpr int kSrcW = kW + 2 * kMargin;
constexpr int kSrcH = kH + 2 * kMargin;
constexpr double kDt = 1.0 / 60.0;
constexpr int kScreenW = 1920;
constexpr int kScreenH = 1080;
constexpr double kHfov = 90.0;
constexpr double kDegPerCellAt60 = 33.75;   // 1 格 = 1920/160 = 12 px → 12·90/1920 = 0.5625° → ÷ (1/60 s)

using Frame = std::vector<float>;

// 32 位 LCG（Numerical Recipes 常数），uniform() ∈ [0,1)。
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : s_(seed * 2654435761u + 0x9E3779B9u) {}
    double uniform() {
        s_ = s_ * 1664525u + 1013904223u;
        return static_cast<double>(s_ >> 8) * (1.0 / 16777216.0);
    }
    double gaussian() {   // Irwin–Hall：12 个均匀数之和 − 6 ≈ N(0, 1)
        double a = 0;
        for (int i = 0; i < 12; ++i) a += uniform();
        return a - 6.0;
    }

private:
    std::uint32_t s_;
};

struct Image {
    int w = 0, h = 0;
    std::vector<double> px;

    double at(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    }
    double bilinear(double x, double y) const {
        const double fx = std::floor(x), fy = std::floor(y);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        const double ax = x - fx, ay = y - fy;
        const double top = (1 - ax) * at(ix, iy) + ax * at(ix + 1, iy);
        const double bot = (1 - ax) * at(ix, iy + 1) + ax * at(ix + 1, iy + 1);
        return (1 - ay) * top + ay * bot;
    }
};

// 源纹理（kSrcW×kSrcH，值域 [0,1]）：均匀随机 → 两遍可分离 [1 2 1]/4 平滑（带限）→ 以 0.5 为中心拉伸对比度 → 叠加 6 个随机矩形 → 截断。
Image make_texture(std::uint32_t seed) {
    Image img;
    img.w = kSrcW;
    img.h = kSrcH;
    img.px.resize(static_cast<std::size_t>(kSrcW) * kSrcH);
    Lcg rng(seed);
    for (double& v : img.px) v = rng.uniform();

    std::vector<double> tmp(img.px.size());
    auto idx = [](int x, int y) { return static_cast<std::size_t>(y) * kSrcW + static_cast<std::size_t>(x); };
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < kSrcH; ++y)
            for (int x = 0; x < kSrcW; ++x)
                tmp[idx(x, y)] = 0.25 * img.at(x - 1, y) + 0.5 * img.at(x, y) + 0.25 * img.at(x + 1, y);
        img.px.swap(tmp);
        for (int y = 0; y < kSrcH; ++y)
            for (int x = 0; x < kSrcW; ++x)
                tmp[idx(x, y)] = 0.25 * img.at(x, y - 1) + 0.5 * img.at(x, y) + 0.25 * img.at(x, y + 1);
        img.px.swap(tmp);
    }
    for (double& v : img.px) v = 0.5 + (v - 0.5) * 2.5;   // 平滑后 σ ≈ 0.08 → ≈ 0.2

    for (int r = 0; r < 6; ++r) {
        const int rw = 8 + static_cast<int>(rng.uniform() * 32);
        const int rh = 6 + static_cast<int>(rng.uniform() * 24);
        const int rx = static_cast<int>(rng.uniform() * (kSrcW - rw));
        const int ry = static_cast<int>(rng.uniform() * (kSrcH - rh));
        const double off = (rng.uniform() - 0.5) * 0.5;
        for (int y = ry; y < ry + rh; ++y)
            for (int x = rx; x < rx + rw; ++x) img.px[idx(x, y)] += off;
    }
    for (double& v : img.px) v = std::clamp(v, 0.0, 1.0);
    return img;
}

// 160×90 窗口，内容相对"原位"平移 (sx, sy) 格：frame(x,y) = src(margin + x − sx, margin + y − sy)。
// 整数平移是精确的（双线性权重为 0/1），小数平移即双线性重采样。
Frame window(const Image& src, double sx, double sy) {
    Frame f(static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            f[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] =
                static_cast<float>(src.bilinear(kMargin + x - sx, kMargin + y - sy));
    return f;
}

// 4 倍超采样：高分辨率图 hi(X,Y) = src 在 ((X+0.5)/4 − 0.5, (Y+0.5)/4 − 0.5) 的双线性值；
// hi 平移 (qx, qy) 个高分辨率像素（= qx/4、qy/4 格）后做 4×4 盒式平均（模拟 GPU 的区域平均降采样）。
Frame window_supersampled(const Image& src, int qx, int qy) {
    Frame f(static_cast<std::size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            double acc = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    const int X = 4 * (kMargin + x) + i - qx;
                    const int Y = 4 * (kMargin + y) + j - qy;
                    acc += src.bilinear((X + 0.5) / 4.0 - 0.5, (Y + 0.5) / 4.0 - 0.5);
                }
            f[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] = static_cast<float>(acc / 16.0);
        }
    return f;
}

Frame with_noise(Frame f, double sigma, std::uint32_t seed) {
    Lcg rng(seed);
    for (float& v : f) v = static_cast<float>(v + sigma * rng.gaussian());
    return f;
}

Frame filled(float v) { return Frame(static_cast<std::size_t>(kW) * kH, v); }

// 新估计器依次 push a（首帧，必须无效）与 b（1/60 s 后），返回 b 的估计。
mc::MotionEstimate estimate_pair(const Frame& a, const Frame& b, const mc::MotionEstimatorParams& p = {}) {
    mc::MotionEstimator est(p);
    const mc::MotionEstimate first = est.push(a.data(), 0.0);
    CHECK_FALSE(first.valid);
    return est.push(b.data(), kDt);
}

void print_estimate(const char* label, const mc::MotionEstimate& e) {
    std::printf("   %-34s dx=%+8.3f dy=%+8.3f c=%.3f valid=%d\n", label, e.dx, e.dy, e.confidence, e.valid ? 1 : 0);
}

// 已知位移：|dx − ex|、|dy − ey| < tol，置信度 > min_conf，有效。
void check_known_shift(const char* label, const Frame& a, const Frame& b, double ex, double ey, double tol,
                       double min_conf) {
    const mc::MotionEstimate e = estimate_pair(a, b);
    print_estimate(label, e);
    CHECK_NEAR(e.dx, ex, tol);
    CHECK_NEAR(e.dy, ey, tol);
    CHECK(e.confidence > min_conf);
    CHECK_TRUE(e.valid);
}

// 默认换算参数：网格 160×90、1920×1080、HFOV 90°、hold 100 ms。
mc::SpeedHoldParams hold_params(double hold_ms = 100.0) {
    mc::SpeedHoldParams p;
    p.grid_w = kW;
    p.grid_h = kH;
    p.screen_w_px = kScreenW;
    p.screen_h_px = kScreenH;
    p.assumed_hfov_deg = kHfov;
    p.hold_ms = hold_ms;
    return p;
}

// 按默认参数（1/60 s）把一次估计换算成发布的截屏角速度。
double published_speed(const mc::MotionEstimate& e) {
    mc::SpeedHold hold;
    hold.configure(hold_params());
    return hold.on_estimate(e, kDt, kDt);
}

// shift_to_deg_per_s 的默认几何简写；无效时返回 -1。
double deg_per_s(double dx, double dy, double dt, double hfov = kHfov) {
    return mc::shift_to_deg_per_s(dx, dy, kW, kH, kScreenW, kScreenH, hfov, dt).value_or(-1.0);
}

mc::MotionEstimate manual_estimate(double dx, double dy, double dt, bool valid) {
    mc::MotionEstimate e;
    e.dx = dx;
    e.dy = dy;
    e.dt_s = dt;
    e.valid = valid;
    e.confidence = valid ? 0.95 : 0.2;
    return e;
}

bool finite(const mc::MotionEstimate& e) {
    return std::isfinite(e.dx) && std::isfinite(e.dy) && std::isfinite(e.confidence) && std::isfinite(e.dt_s);
}

}  // namespace

// ---------------------------------------------------------------- 基本行为

TEST(motion_first_frame_invalid) {
    const Frame a = window(make_texture(1), 0, 0);
    mc::MotionEstimator est;
    const mc::MotionEstimate e = est.push(a.data(), 5.0);
    CHECK_FALSE(e.valid);
    CHECK_EQ(e.confidence, 0.0);
    CHECK_EQ(e.dx, 0.0);
    CHECK_EQ(e.dy, 0.0);
    CHECK_EQ(e.dt_s, 0.0);
}

TEST(motion_dt_from_timestamps_and_reset) {
    const Image src = make_texture(2);
    const Frame a = window(src, 0, 0), b = window(src, 3, 0);
    mc::MotionEstimator est;
    est.push(a.data(), 1.0);
    const mc::MotionEstimate e = est.push(b.data(), 1.025);
    CHECK_TRUE(e.valid);
    CHECK_NEAR(e.dt_s, 0.025, 1e-12);
    est.reset();   // 忘掉参考帧 → 下一帧当首帧
    const mc::MotionEstimate after = est.push(a.data(), 2.0);
    CHECK_FALSE(after.valid);
    CHECK_EQ(after.dt_s, 0.0);
}

TEST(motion_null_frame_ignored) {
    const Image src = make_texture(3);
    const Frame a = window(src, 0, 0), b = window(src, 5, 0);
    mc::MotionEstimator est;
    est.push(a.data(), 0.0);
    const mc::MotionEstimate n = est.push(nullptr, kDt);
    CHECK_FALSE(n.valid);
    const mc::MotionEstimate e = est.push(b.data(), 2 * kDt);   // 仍与 a 比较
    CHECK_TRUE(e.valid);
    CHECK_NEAR(e.dx, 5.0, 0.5);
    CHECK_NEAR(e.dt_s, 2 * kDt, 1e-12);
}

TEST(motion_configure_changes_grid_and_resets) {
    mc::MotionEstimator est;
    est.push(filled(0.5f).data(), 0.0);
    mc::MotionEstimatorParams p;
    p.width = 80;
    p.height = 45;
    p.max_shift = 0;   // 非法 → 收拢为 1
    est.configure(p);
    CHECK_EQ(est.params().width, 80);
    CHECK_EQ(est.params().height, 45);
    CHECK_EQ(est.params().max_shift, 1);
    std::vector<float> small(80 * 45, 0.25f);
    CHECK_FALSE(est.push(small.data(), 1.0).valid);   // configure 后是首帧
}

// ---------------------------------------------------------------- 已知位移

TEST(motion_shift_horizontal_plus3) {
    const Image src = make_texture(11);
    check_known_shift("shift (+3, 0)", window(src, 0, 0), window(src, 3, 0), 3, 0, 0.5, 0.8);
}

TEST(motion_shift_horizontal_minus7) {
    const Image src = make_texture(12);
    check_known_shift("shift (-7, 0)", window(src, 0, 0), window(src, -7, 0), -7, 0, 0.5, 0.8);
}

TEST(motion_shift_horizontal_plus12) {
    const Image src = make_texture(13);
    check_known_shift("shift (+12, 0)", window(src, 0, 0), window(src, 12, 0), 12, 0, 0.5, 0.8);
}

TEST(motion_shift_vertical_minus4) {
    const Image src = make_texture(14);
    check_known_shift("shift (0, -4)", window(src, 0, 0), window(src, 0, -4), 0, -4, 0.5, 0.8);
}

TEST(motion_shift_diagonal_5_2) {
    const Image src = make_texture(15);
    check_known_shift("shift (+5, +2)", window(src, 0, 0), window(src, 5, 2), 5, 2, 0.5, 0.8);
}

TEST(motion_shift_subpixel_bilinear_2p5) {
    const Image src = make_texture(16);
    check_known_shift("shift (+2.5, 0) bilinear", window(src, 0, 0), window(src, 2.5, 0), 2.5, 0, 0.5, 0.8);
}

TEST(motion_shift_subpixel_supersampled_2p25_0p75) {   // spec：4 倍超采样平移后盒式平均
    const Image src = make_texture(17);
    check_known_shift("shift (+2.25, +0.75) 4x box", window_supersampled(src, 0, 0), window_supersampled(src, 9, 3),
                      2.25, 0.75, 0.5, 0.8);
}

TEST(motion_shift_7_minus3_noise_sigma002) {   // spec：σ = 0.02 噪声
    const Image src = make_texture(18);
    check_known_shift("shift (+7, -3) noise 0.02", with_noise(window(src, 0, 0), 0.02, 101),
                      with_noise(window(src, 7, -3), 0.02, 102), 7, -3, 0.5, 0.8);
}

TEST(motion_shift_near_limit_minus18) {   // spec：max_shift = 20，向左 18 格
    const Image src = make_texture(19);
    check_known_shift("shift (-18, 0)", window(src, 0, 0), window(src, -18, 0), -18, 0, 0.5, 0.6);
}

TEST(motion_noise_sigma5pct_shift4) {   // 值域 [0,1] 的 5%：σ = 0.05
    const Image src = make_texture(20);
    check_known_shift("shift (+4, 0) noise 0.05", with_noise(window(src, 0, 0), 0.05, 201),
                      with_noise(window(src, 4, 0), 0.05, 202), 4, 0, 0.5, 0.8);
}

// 超出搜索范围（+35 > max_shift 20）：真实峰值不在窗口内，窗口内最高的只是伪相关峰。
// 约定的行为：置信度 < min_correlation（0.60）→ valid = false（两者同时成立），dx 无意义（不作断言）。
// 用 8 个种子验证这不是碰巧。
TEST(motion_shift_beyond_max_rejected) {
    double worst = 0;
    for (std::uint32_t seed = 21; seed < 29; ++seed) {
        const Image src = make_texture(seed);
        const mc::MotionEstimate e = estimate_pair(window(src, 0, 0), window(src, 35, 0));
        worst = std::max(worst, e.confidence);
        CHECK_FALSE(e.valid);
        CHECK(e.confidence < 0.6);
        CHECK(finite(e));
    }
    std::printf("   shift (+35, 0) beyond max_shift: max c over 8 seeds = %.3f\n", worst);
}

// ---------------------------------------------------------------- 不应被当成运动的变化

TEST(motion_brightness_gain_30pct_no_motion) {   // 整体 ×1.3（不截断），无平移
    const Frame a = window(make_texture(31), 0, 0);
    Frame b = a;
    for (float& v : b) v *= 1.3f;
    const mc::MotionEstimate e = estimate_pair(a, b);
    print_estimate("gain x1.3", e);
    CHECK_NEAR(e.dx, 0.0, 0.3);
    CHECK_NEAR(e.dy, 0.0, 0.3);
    const double speed = published_speed(e);
    std::printf("   gain x1.3 published speed = %.3f deg/s\n", speed);
    CHECK(speed < 10.0);   // 0.3 格 ≈ 10 °/s；实际应接近 0
}

TEST(motion_brightness_gain_offset_clamped) {   // spec：×1.3 再 +0.15，截断到 [0,1]
    const Frame a = window(make_texture(32), 0, 0);
    Frame b = a;
    for (float& v : b) v = std::clamp(v * 1.3f + 0.15f, 0.0f, 1.0f);
    const mc::MotionEstimate e = estimate_pair(a, b);
    print_estimate("gain x1.3 +0.15 clamped", e);
    CHECK(e.confidence < 0.6 || (std::fabs(e.dx) < 0.5 && std::fabs(e.dy) < 0.5));
    CHECK(published_speed(e) < 90.0);
}

TEST(motion_brightness_offset_detrended_projections_equal) {   // spec：每像素 +0.3 后去趋势投影逐元素差 < 1e-3
    const Frame a = window(make_texture(33), 0, 0);
    Frame b = a;
    for (float& v : b) v += 0.3f;
    std::vector<double> ca(kW), ra(kH), cb(kW), rb(kH);
    mc::project_and_detrend(a.data(), kW, kH, ca.data(), ra.data());
    mc::project_and_detrend(b.data(), kW, kH, cb.data(), rb.data());
    double worst = 0;
    for (int i = 0; i < kW; ++i) worst = std::max(worst, std::fabs(ca[i] - cb[i]));
    for (int i = 0; i < kH; ++i) worst = std::max(worst, std::fabs(ra[i] - rb[i]));
    CHECK(worst < 1e-3);
    // 线性渐变（暗角 / 天空渐变）同样被去掉
    Frame g = a;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) g[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] += 0.002f * x;
    mc::project_and_detrend(g.data(), kW, kH, cb.data(), rb.data());
    worst = 0;
    for (int i = 0; i < kW; ++i) worst = std::max(worst, std::fabs(ca[i] - cb[i]));
    CHECK(worst < 1e-3);
}

TEST(motion_scene_replacement_low_confidence) {   // 两张独立随机纹理：8 个种子都必须 c < 0.60
    double worst = 0;
    for (std::uint32_t seed = 41; seed < 49; ++seed) {
        const mc::MotionEstimate e =
            estimate_pair(window(make_texture(seed), 0, 0), window(make_texture(seed + 1000), 0, 0));
        worst = std::max(worst, e.confidence);
        CHECK(e.confidence < 0.6);
        CHECK_FALSE(e.valid);
    }
    std::printf("   scene replacement: max c over 8 seeds = %.3f\n", worst);
}

TEST(motion_flat_frames_invalid_no_nan) {   // 纯灰 / 纯白 / 纯黑：c = 0、无效、无 NaN；纯色之后的第一张纹理帧也无效
    mc::MotionEstimator est;
    double t = 0;
    for (const float v : {0.5f, 0.5f, 1.0f, 0.0f, 0.3f}) {
        const Frame f = filled(v);
        const mc::MotionEstimate e = est.push(f.data(), t);
        t += kDt;
        CHECK_FALSE(e.valid);
        CHECK_EQ(e.confidence, 0.0);
        CHECK(finite(e));
    }
    const Frame tex = window(make_texture(51), 0, 0);
    const mc::MotionEstimate e1 = est.push(tex.data(), t);
    CHECK_FALSE(e1.valid);   // 参考帧无纹理
    CHECK_EQ(e1.confidence, 0.0);
    const mc::MotionEstimate e2 = est.push(tex.data(), t + kDt);
    CHECK_TRUE(e2.valid);
    CHECK_NEAR(e2.dx, 0.0, 0.1);
    CHECK_NEAR(e2.dy, 0.0, 0.1);
}

TEST(motion_single_axis_texture_invalid) {   // 只有竖条纹（行投影为常数）→ 该轴无纹理 → c = 0
    const Image src = make_texture(52);
    auto stripes = [&](int sx) {
        Frame f(static_cast<std::size_t>(kW) * kH);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
                f[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)] =
                    static_cast<float>(src.at(kMargin + x - sx, kMargin));
        return f;
    };
    const mc::MotionEstimate e = estimate_pair(stripes(0), stripes(3));
    CHECK_FALSE(e.valid);
    CHECK_EQ(e.confidence, 0.0);
    CHECK(finite(e));
}

// ---------------------------------------------------------------- 估计器 + 发布值联动（spec 场景）

TEST(motion_flash_white_frame_holds_then_recovers) {
    // 60 fps，内容每帧 +4 格（≈ 135 °/s）；中间一帧整屏纯白。
    const Image src = make_texture(61);
    mc::MotionEstimator est;
    mc::SpeedHold hold;
    hold.configure(hold_params());
    double t = 0;
    auto feed = [&](const Frame& f, mc::MotionEstimate* out) {
        *out = est.push(f.data(), t);
        const double v = hold.on_estimate(*out, out->dt_s, t);
        t += kDt;
        return v;
    };
    mc::MotionEstimate e;
    feed(window(src, -20, 0), &e);
    feed(window(src, -16, 0), &e);
    const double v_moving = feed(window(src, -12, 0), &e);
    CHECK_TRUE(e.valid);
    CHECK_NEAR(v_moving, 4 * kDegPerCellAt60, 0.5 * kDegPerCellAt60);

    const double v_white = feed(filled(1.0f), &e);   // 纯白：低置信度，保持
    CHECK_FALSE(e.valid);
    CHECK_NEAR(v_white, v_moving, 1e-9);
    const double v_after = feed(window(src, -4, 0), &e);   // 与纯白比较：仍低置信度，保持（距上次有效 33 ms）
    CHECK_FALSE(e.valid);
    CHECK_NEAR(v_after, v_moving, 1e-9);
    const double v_back = feed(window(src, 0, 0), &e);   // 第三帧恢复测量
    CHECK_TRUE(e.valid);
    CHECK_NEAR(v_back, 4 * kDegPerCellAt60, 0.5 * kDegPerCellAt60);
}

TEST(motion_scene_cut_then_still) {
    const Image a = make_texture(62), b = make_texture(63);
    mc::MotionEstimator est;
    mc::SpeedHold hold;
    hold.configure(hold_params());
    double t = 0;
    auto feed = [&](const Frame& f, mc::MotionEstimate* out) {
        *out = est.push(f.data(), t);
        const double v = hold.on_estimate(*out, out->dt_s, t);
        t += kDt;
        return v;
    };
    mc::MotionEstimate e;
    feed(window(a, 0, 0), &e);
    const double v_fast = feed(window(a, 9, 0), &e);   // ≈ 304 °/s
    CHECK_TRUE(e.valid);
    CHECK_NEAR(v_fast, 9 * kDegPerCellAt60, 0.5 * kDegPerCellAt60);
    const double v_cut = feed(window(b, 0, 0), &e);   // 换场景：低置信度，保持
    CHECK_FALSE(e.valid);
    CHECK(e.confidence < 0.6);
    CHECK_NEAR(v_cut, v_fast, 1e-9);
    const double v_still = feed(window(b, 0, 0), &e);   // 新场景静止：高置信度、≈ 0
    CHECK_TRUE(e.valid);
    CHECK(v_still < 20.0);
}

// ---------------------------------------------------------------- 角速度换算

TEST(motion_shift_to_deg_values) {
    // 1 格 = 1920/160 = 12 px；HFOV 90° → 0.5625°
    CHECK_NEAR(mc::shift_to_deg(1, 0, kW, kH, kScreenW, kScreenH, kHfov), 0.5625, 1e-12);
    CHECK_NEAR(mc::shift_to_deg(0, -1, kW, kH, kScreenW, kScreenH, kHfov), 0.5625, 1e-12);   // 1080/90 = 12 px
    CHECK_NEAR(mc::shift_to_deg(3, 4, kW, kH, kScreenW, kScreenH, kHfov), 5 * 0.5625, 1e-12);   // 模长
    CHECK_EQ(mc::shift_to_deg(1, 0, 0, kH, kScreenW, kScreenH, kHfov), 0.0);
    CHECK_EQ(mc::shift_to_deg(1, 0, kW, kH, kScreenW, 0, kHfov), 0.0);
}

TEST(motion_shift_to_deg_per_s_values) {
    // ÷ (1/60 s) → 33.75 °/s
    CHECK_NEAR(deg_per_s(1, 0, kDt), 33.75, 0.01);
    CHECK_NEAR(deg_per_s(-1, 0, kDt), 33.75, 0.01);
    CHECK_NEAR(deg_per_s(0, 1, kDt), 33.75, 0.01);
    // spec：W = 1920、H = 1080、160×90、HFOV 90、dt = 1/60 s、dx = 10 → 120 px → 5.625° → 337.5 °/s（相对误差 < 0.1%）
    CHECK_NEAR(deg_per_s(10, 0, kDt), 337.5, 0.3375);
    CHECK_NEAR(deg_per_s(3, 4, kDt), 5 * 33.75, 1e-9);
    CHECK_NEAR(deg_per_s(1, 0, kDt, 120.0), 45.0, 1e-9);
    CHECK_NEAR(deg_per_s(1, 0, 0.25), 0.5625 / 0.25, 1e-9);   // 0.25 s 仍有效（闭区间）
    // 无效：dt ≤ 0、dt > 0.25 s（spec：两帧相隔 400 ms）、NaN、非法尺寸
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, kScreenW, kScreenH, kHfov, 0.0).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, kScreenW, kScreenH, kHfov, -kDt).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, kScreenW, kScreenH, kHfov, 0.4).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, kScreenW, kScreenH, kHfov, 0.2500001).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, kScreenW, kScreenH, kHfov, std::nan("")).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(std::nan(""), 0, kW, kH, kScreenW, kScreenH, kHfov, kDt).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, 0, kH, kScreenW, kScreenH, kHfov, kDt).has_value());
    CHECK_FALSE(mc::shift_to_deg_per_s(1, 0, kW, kH, 0, kScreenH, kHfov, kDt).has_value());
}

// ---------------------------------------------------------------- 保持与归零（SpeedHold，绝对时间）

// 有效 100 °/s 之后连续无效帧（60 fps）：距上次有效 ≤ 100 ms 保持 100，之后阶跃到 0（无指数尾巴）。
TEST(motion_hold_holds_then_steps_to_zero) {
    mc::SpeedHold hold;
    hold.configure(hold_params());
    const double dx = 100.0 / kDegPerCellAt60;
    const double t0 = 1000.0;   // 绝对时间（QPC 秒）
    CHECK_NEAR(hold.on_estimate(manual_estimate(dx, 0, kDt, true), kDt, t0), 100.0, 1e-9);
    CHECK_NEAR(hold.last_measurement(), 100.0, 1e-9);
    int held_frames = 0;
    for (int i = 1; i <= 24; ++i) {   // 再 400 ms
        const double v = hold.on_estimate(manual_estimate(0, 0, kDt, false), kDt, t0 + i * kDt);
        if (i * kDt <= 0.100 + 1e-9) {
            CHECK_NEAR(v, 100.0, 1e-9);
            ++held_frames;
        } else {
            CHECK_EQ(v, 0.0);
        }
    }
    CHECK_EQ(held_frames, 6);   // 16.7 … 100 ms
    CHECK_EQ(hold.value(t0 + 0.5), 0.0);
    CHECK_NEAR(hold.last_measurement(), 100.0, 1e-9);   // 测量值本身还在（调试用）
}

TEST(motion_hold_valid_zero_resets_immediately) {
    mc::SpeedHold hold;
    hold.configure(hold_params());
    CHECK_NEAR(hold.on_estimate(manual_estimate(100.0 / kDegPerCellAt60, 0, kDt, true), kDt, 5.0), 100.0, 1e-9);
    CHECK_EQ(hold.on_estimate(manual_estimate(0, 0, kDt, true), kDt, 5.0 + kDt), 0.0);
    CHECK_EQ(hold.value(5.0 + kDt), 0.0);
}

TEST(motion_hold_timeouts_hold_then_zero) {   // spec：200 °/s 之后 300 ms 每次取帧都超时
    mc::SpeedHold hold;
    hold.configure(hold_params());
    const double t0 = 42.0;
    CHECK_NEAR(hold.on_estimate(manual_estimate(200.0 / kDegPerCellAt60, 0, kDt, true), kDt, t0), 200.0, 1e-9);
    for (int i = 1; i <= 18; ++i) {   // 18 × 16 ms = 288 ms
        const double v = hold.on_no_update(t0 + i * 0.016);
        if (i * 16 <= 100) {
            CHECK_NEAR(v, 200.0, 1e-9);
        } else {
            CHECK_EQ(v, 0.0);
        }
    }
}

TEST(motion_hold_40fps_on_60hz_never_zero) {   // spec：新帧间隔 25 ms，中间夹一次 16 ms 超时
    mc::SpeedHold hold;
    hold.configure(hold_params());
    const double expect = deg_per_s(6, 0, 0.025);   // 135 °/s
    CHECK_NEAR(expect, 135.0, 1e-9);
    double t = 7.0;
    CHECK_NEAR(hold.on_estimate(manual_estimate(6, 0, 0.025, true), 0.025, t), expect, 1e-9);
    for (int i = 0; i < 10; ++i) {
        CHECK_NEAR(hold.on_no_update(t + 0.016), expect, 1e-9);
        t += 0.025;
        CHECK_NEAR(hold.on_estimate(manual_estimate(6, 0, 0.025, true), 0.025, t), expect, 1e-9);
    }
    // 超时之后来了一张低置信度帧：仍按绝对时间计 hold（距上次有效 25 ms）。
    CHECK_NEAR(hold.on_no_update(t + 0.016), expect, 1e-9);
    CHECK_NEAR(hold.on_estimate(manual_estimate(0, 0, 0.025, false), 0.025, t + 0.025), expect, 1e-9);
    CHECK_NEAR(hold.on_no_update(t + 0.099), expect, 1e-9);   // 距上次有效 99 ms
    CHECK_EQ(hold.on_no_update(t + 0.101), 0.0);              // 101 ms
}

TEST(motion_hold_frame_interval_out_of_range) {   // spec：dt ≤ 0 或 > 250 ms 不产生速度
    mc::SpeedHold hold;
    hold.configure(hold_params());
    CHECK_EQ(hold.on_estimate(manual_estimate(10, 0, 0.4, true), 0.4, 1.0), 0.0);
    CHECK_EQ(hold.on_estimate(manual_estimate(10, 0, 0.0, true), 0.0, 1.0), 0.0);
    CHECK_EQ(hold.on_estimate(manual_estimate(10, 0, -kDt, true), -kDt, 1.0), 0.0);
    CHECK_FALSE(hold.has_measurement());
    // 已有有效值时，400 ms 的间隔本身就超过 hold → 0
    CHECK_NEAR(hold.on_estimate(manual_estimate(10, 0, kDt, true), kDt, 2.0), 337.5, 1e-9);
    CHECK_EQ(hold.on_estimate(manual_estimate(10, 0, 0.4, true), 0.4, 2.4), 0.0);
    // reset 后回到 0
    CHECK_NEAR(hold.on_estimate(manual_estimate(10, 0, kDt, true), kDt, 3.0), 337.5, 1e-9);
    hold.reset();
    CHECK_FALSE(hold.has_measurement());
    CHECK_EQ(hold.value(3.0), 0.0);
}

TEST(motion_hold_configure_keeps_state_and_converts) {
    mc::SpeedHold hold;
    hold.configure(hold_params());
    CHECK_NEAR(hold.on_estimate(manual_estimate(1, 0, kDt, true), kDt, 1.0), 33.75, 1e-9);
    mc::SpeedHoldParams p = hold_params(250.0);
    p.assumed_hfov_deg = 120.0;
    hold.configure(p);   // 不清除已有测量；新 hold 立即生效
    CHECK_NEAR(hold.value(1.2), 33.75, 1e-9);
    CHECK_NEAR(hold.on_estimate(manual_estimate(1, 0, kDt, true), kDt, 1.3), 45.0, 1e-9);   // 新 HFOV
    CHECK_EQ(hold.value(1.3 + 0.251), 0.0);
    // hold_ms = 0：只在测量时刻本身发布
    hold.configure(hold_params(0.0));
    CHECK_NEAR(hold.on_estimate(manual_estimate(2, 0, kDt, true), kDt, 2.0), 67.5, 1e-9);
    CHECK_EQ(hold.value(2.001), 0.0);
    // 非法参数收拢到默认值
    mc::SpeedHoldParams bad;
    bad.grid_w = 0;
    bad.screen_h_px = -5;
    bad.assumed_hfov_deg = std::nan("");
    bad.hold_ms = -1;
    hold.configure(bad);
    CHECK_EQ(hold.params().grid_w, 160);
    CHECK_EQ(hold.params().screen_h_px, 1080);
    CHECK_EQ(hold.params().assumed_hfov_deg, 90.0);
    CHECK_EQ(hold.params().hold_ms, 100.0);
}

// spec「画面完全静止」+「一帧闪白」的组合：估计器 + SpeedHold，帧时间戳为大的绝对 QPC 秒。
TEST(motion_hold_flash_frame_with_absolute_timestamps) {
    const Image src = make_texture(64);
    mc::MotionEstimator est;
    mc::SpeedHold hold;
    hold.configure(hold_params());
    double t = 123456.789;
    double v = 0;
    for (int k = 0; k < 4; ++k) {   // 每帧 +5 格 ≈ 168.75 °/s
        const Frame f = window(src, -20 + 5 * k, 0);
        const mc::MotionEstimate e = est.push(f.data(), t);
        v = hold.on_estimate(e, e.dt_s, t);
        t += kDt;
    }
    CHECK_NEAR(v, 5 * kDegPerCellAt60, 0.5 * kDegPerCellAt60);
    const mc::MotionEstimate w = est.push(filled(1.0f).data(), t);
    CHECK_FALSE(w.valid);
    CHECK_NEAR(w.dt_s, kDt, 1e-6);
    CHECK_NEAR(hold.on_estimate(w, w.dt_s, t), v, 1e-9);
    CHECK_NEAR(hold.on_no_update(t + 0.05), v, 1e-9);
    CHECK_EQ(hold.on_no_update(t + 0.2), 0.0);
}

// ---------------------------------------------------------------- 耗时

TEST(motion_timing_1000_pushes) {
    const Image src = make_texture(71);
    const Frame a = window(src, 0, 0), b = window(src, 3, 1);
    mc::MotionEstimator est;
    int valid = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        const mc::MotionEstimate e = est.push((i & 1) ? b.data() : a.data(), i * kDt);
        valid += e.valid ? 1 : 0;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
#ifdef NDEBUG
    std::printf("   1000 pushes of %dx%d: %.2f ms (%.1f us/frame, release, limit 200 ms)\n", kW, kH, ms, ms);
    CHECK(ms < 200.0);
#else
    std::printf("   1000 pushes of %dx%d: %.2f ms (%.1f us/frame, debug build: not asserted)\n", kW, kH, ms, ms);
#endif
    CHECK_EQ(valid, 999);
}

// 变更 5：SpeedHold::on_value（前进 / 倾斜流速复用同一条保持 / 归零规则）
TEST(motion_hold_on_value_rule) {
    mc::SpeedHold hold;
    hold.configure(hold_params());
    CHECK_EQ(hold.on_value(std::nullopt, 1.0), 0.0);
    CHECK_FALSE(hold.has_measurement());
    CHECK_NEAR(hold.on_value(120.0, 2.0), 120.0, 1e-12);
    CHECK_NEAR(hold.on_value(std::nullopt, 2.05), 120.0, 1e-12);   // 低置信度：保持
    CHECK_NEAR(hold.on_value(std::nullopt, 2.1), 120.0, 1e-12);    // 恰好 100 ms
    CHECK_EQ(hold.on_value(std::nullopt, 2.101), 0.0);             // 超过 hold → 0
    CHECK_EQ(hold.on_value(std::nan(""), 3.0), 0.0);                // 非有限值视同无效
    CHECK_NEAR(hold.on_value(-5.0, 4.0), 0.0, 0);                    // 负值截到 0
}
