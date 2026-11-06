// test_similarity_estimator.cpp — 前进 / 倾斜检测（相似变换估计）的单元测试：连续程序纹理按已知缩放 / 旋转 / 平移渲染成 160×90（3×3 超采样盒式平均），
// 验证 ln s 与旋转角误差、静态 HUD、两层深度、纯平移不误报、亮度突变 / 换图 / 纯色无效、换算公式与每帧耗时。
#include "test_framework.h"

#include "capture/similarity_estimator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

constexpr int kW = 160;
constexpr int kH = 90;
constexpr double kFps = 60.0;
constexpr double kHfov = 90.0;
constexpr double kPi = std::numbers::pi;

using Frame = std::vector<float>;

std::uint32_t hash3(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8DA6B343u ^ static_cast<std::uint32_t>(y) * 0xD8163841u ^
                      (seed + 1u) * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0xC2B2AE35u;
    h ^= h >> 15;
    return h;
}
double lattice(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    return static_cast<double>(hash3(x, y, seed) & 0xFFFFFFu) / 16777216.0;
}

// 连续坐标上的值噪声（smoothstep 双线性），cell = 格点间距。
double value_noise(double x, double y, double cell, std::uint32_t seed) {
    const double fx = x / cell, fy = y / cell;
    const double gx = std::floor(fx), gy = std::floor(fy);
    double tx = fx - gx, ty = fy - gy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const auto ix = static_cast<std::int32_t>(gx), iy = static_cast<std::int32_t>(gy);
    const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const double c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    const double top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

// 场景纹理（世界坐标，单位 = 格）：5 个八度的值噪声 + 稀疏的亮 / 暗圆盘，值域约 [0, 1]。
double scene(double x, double y, std::uint32_t seed) {
    static constexpr double kCells[] = {32, 16, 8, 4, 2};
    static constexpr double kAmps[] = {0.34, 0.26, 0.18, 0.12, 0.10};
    double v = 0;
    for (int o = 0; o < 5; ++o) v += kAmps[o] * value_noise(x, y, kCells[o], seed * 7u + static_cast<std::uint32_t>(o));
    const double cs = 20.0;
    const double gx = std::floor(x / cs), gy = std::floor(y / cs);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto cx = static_cast<std::int32_t>(gx) + dx, cy = static_cast<std::int32_t>(gy) + dy;
            const std::uint32_t h = hash3(cx, cy, seed + 101u);
            if ((h & 0xFF) > 70) continue;
            const double px = (cx + 0.2 + 0.6 * ((h >> 8) & 0xFF) / 255.0) * cs;
            const double py = (cy + 0.2 + 0.6 * ((h >> 16) & 0xFF) / 255.0) * cs;
            const double r = 3.0 + 4.0 * ((h >> 24) & 0xFF) / 255.0;
            if ((x - px) * (x - px) + (y - py) * (y - py) < r * r) v += ((h >> 4) & 1) ? 0.35 : -0.35;
        }
    }
    return std::clamp(0.1 + 0.8 * v, 0.0, 1.0);
}

// 内容运动（参考 → 当前）：p_cur = s·R(θ)·p_ref + t，以网格中心为原点、y 向下（屏幕上顺时针为正）。
struct Motion {
    double s = 1.0;
    double rot_deg = 0.0;
    double tx = 0.0, ty = 0.0;
};

// 当前帧在 p 处显示世界坐标 q = R(−θ)(p − t)/s 的内容。
void inverse_map(const Motion& m, double px, double py, double& qx, double& qy) {
    const double c = std::cos(m.rot_deg * kPi / 180.0), sn = std::sin(m.rot_deg * kPi / 180.0);
    const double ux = (px - m.tx) / m.s, uy = (py - m.ty) / m.s;
    qx = c * ux + sn * uy;
    qy = -sn * ux + c * uy;
}

struct Hud {
    int x = 0, y = 0, w = 0, h = 0;   // 静态矩形（格），内容不随运动变化
};

// 两层深度：中心半径 radius 格内用 inner 运动，外面用 outer 运动。
struct Layered {
    double radius = 0;
    Motion inner;
};

Frame render(const Motion& m, std::uint32_t seed = 1, const Hud* hud = nullptr, const Layered* layer = nullptr,
             double gain = 1.0, double bias = 0.0) {
    Frame f(static_cast<std::size_t>(kW) * kH);
    constexpr int kSs = 3;
    for (int j = 0; j < kH; ++j) {
        for (int i = 0; i < kW; ++i) {
            double acc = 0;
            for (int v = 0; v < kSs; ++v) {
                for (int u = 0; u < kSs; ++u) {
                    const double px = i + (u + 0.5) / kSs - kW / 2.0;
                    const double py = j + (v + 0.5) / kSs - kH / 2.0;
                    double qx, qy;
                    if (hud && i >= hud->x && i < hud->x + hud->w && j >= hud->y && j < hud->y + hud->h) {
                        acc += scene(px * 0.7 + 500, py * 0.7 + 500, seed + 55u);   // HUD：与运动无关的静态纹理
                        continue;
                    }
                    const Motion* mm = &m;
                    if (layer && px * px + py * py < layer->radius * layer->radius) mm = &layer->inner;
                    inverse_map(*mm, px, py, qx, qy);
                    acc += scene(qx, qy, seed);
                }
            }
            const double g = acc / (kSs * kSs) * gain + bias;
            f[static_cast<std::size_t>(j) * kW + static_cast<std::size_t>(i)] = static_cast<float>(std::clamp(g, 0.0, 1.0));
        }
    }
    return f;
}

// 以 60 fps 推入 n 帧：第 k 帧的累计运动 = 每帧运动的 k 次幂（缩放、旋转按指数 / 线性累计，平移线性）。返回最后一次估计。
mc::SimilarityEstimate run_sequence(mc::SimilarityEstimator& est, int n, double ln_s_per_frame, double rot_per_frame,
                                    double tx_per_frame, double ty_per_frame, const Hud* hud = nullptr,
                                    std::uint32_t seed = 1, double t0 = 100.0) {
    mc::SimilarityEstimate e;
    for (int k = 0; k < n; ++k) {
        Motion m{std::exp(ln_s_per_frame * k), rot_per_frame * k, tx_per_frame * k, ty_per_frame * k};
        const Frame f = render(m, seed, hud);
        mc::TranslationHint hint{tx_per_frame, ty_per_frame};
        e = est.push(f.data(), t0 + k / kFps, &hint);
    }
    return e;
}

// 基线 66 ms 时 60 fps 下应选 4 帧前（66.7 ms）。
constexpr int kBackAt60 = 4;

}  // namespace

// ---------------------------------------------------------------- 换算

TEST(similarity_zoom_conversion) {
    // s = 1.02、dt = 66 ms、HFOV 90° → ln 1.02 / 0.066 × 45 = 13.502 °/s
    const auto z = mc::zoom_to_deg_per_s(1.02, 0.066, 90.0);
    CHECK(z.has_value());
    CHECK_NEAR(*z, std::log(1.02) / 0.066 * 45.0, 1e-9);
    CHECK_NEAR(*z, 13.502, 0.001);
    CHECK_NEAR(*mc::zoom_to_deg_per_s(1.0 / 1.02, 0.066, 90.0), *z, 1e-9);   // 后退取绝对值
    CHECK_NEAR(*mc::zoom_to_deg_per_s(1.02, 0.066, 120.0), *z * 120.0 / 90.0, 1e-9);
    CHECK_FALSE(mc::zoom_to_deg_per_s(1.02, 0.0, 90.0).has_value());
    CHECK_FALSE(mc::zoom_to_deg_per_s(1.02, 0.4, 90.0).has_value());
    CHECK_FALSE(mc::zoom_to_deg_per_s(0.0, 0.066, 90.0).has_value());
    CHECK_FALSE(mc::zoom_to_deg_per_s(std::nan(""), 0.066, 90.0).has_value());
    // 倾斜：2° / 66 ms = 30.30 °/s
    CHECK_NEAR(*mc::roll_to_deg_per_s(2.0, 0.066), 30.303, 0.001);
    CHECK_NEAR(*mc::roll_to_deg_per_s(-2.0, 0.066), 30.303, 0.001);
    CHECK_FALSE(mc::roll_to_deg_per_s(2.0, -0.01).has_value());
}

// ---------------------------------------------------------------- 已知缩放 / 旋转

TEST(similarity_scale_expand_and_contract) {
    for (const double s : {1.02, 0.98}) {
        mc::SimilarityEstimator est;
        const double lnf = std::log(s) / kBackAt60;   // 66.7 ms 内累计到 s
        const mc::SimilarityEstimate e = run_sequence(est, 8, lnf, 0, 0, 0);
        CHECK(e.valid);
        CHECK_EQ(e.frames_back, kBackAt60);
        CHECK_NEAR(e.dt_s, kBackAt60 / kFps, 1e-9);
        CHECK_NEAR(std::log(e.scale), std::log(s), 0.1 * std::fabs(std::log(s)));
        CHECK(std::fabs(e.rotation_deg) < 0.1);
        std::printf("   s=%.2f: est s=%.5f (ln err %.1f%%) rot=%.3f conf=%.3f inl=%.2f iters=%d\n", s, e.scale,
                    100.0 * (std::log(e.scale) / std::log(s) - 1.0), e.rotation_deg, e.confidence, e.inlier_fraction,
                    e.iterations);
    }
}

TEST(similarity_scale_two_frames) {   // 只有两帧：参考帧就是上一帧，dt = 66 ms
    mc::SimilarityEstimator est;
    const Frame a = render(Motion{});
    const Frame b = render(Motion{1.02});
    CHECK_FALSE(est.push(a.data(), 5.0).valid);
    const mc::SimilarityEstimate e = est.push(b.data(), 5.066);
    CHECK(e.valid);
    CHECK_EQ(e.frames_back, 1);
    CHECK_NEAR(std::log(e.scale), std::log(1.02), 0.1 * std::log(1.02));
    CHECK_NEAR(*mc::zoom_to_deg_per_s(e.scale, e.dt_s, kHfov), 13.502, 1.36);
}

TEST(similarity_rotation_plus_minus_2deg) {
    for (const double deg : {2.0, -2.0}) {
        mc::SimilarityEstimator est;
        const mc::SimilarityEstimate e = run_sequence(est, 8, 0, deg / kBackAt60, 0, 0);
        CHECK(e.valid);
        CHECK_NEAR(e.rotation_deg, deg, 0.1 * std::fabs(deg));
        CHECK(std::fabs(std::log(e.scale)) < 0.002);
        std::printf("   rot=%+.1f: est %.4f deg (err %.1f%%) s=%.5f conf=%.3f\n", deg, e.rotation_deg,
                    100.0 * (e.rotation_deg / deg - 1.0), e.scale, e.confidence);
    }
}

TEST(similarity_combined_translation_scale_rotation) {
    mc::SimilarityEstimator est;
    // 66.7 ms 内：平移 (8, −4) 格、缩放 1.03、旋转 1.5°
    const mc::SimilarityEstimate e = run_sequence(est, 8, std::log(1.03) / 4, 1.5 / 4, 2.0, -1.0);
    CHECK(e.valid);
    CHECK_NEAR(std::log(e.scale), std::log(1.03), 0.1 * std::log(1.03));
    CHECK_NEAR(e.rotation_deg, 1.5, 0.15);
    CHECK_NEAR(e.tx, 8.0, 0.5);
    CHECK_NEAR(e.ty, -4.0, 0.5);
    std::printf("   est s=%.5f rot=%.3f t=(%.2f, %.2f) conf=%.3f\n", e.scale, e.rotation_deg, e.tx, e.ty, e.confidence);
}

TEST(similarity_static_hud_20_percent) {
    // 左下角 64×45 = 2880 格 = 20% 的静态 HUD（与运动无关的纹理），缩放 1.02 + 旋转 2°
    const Hud hud{0, 45, 64, 45};
    {
        mc::SimilarityEstimator est;
        const mc::SimilarityEstimate e = run_sequence(est, 8, std::log(1.02) / 4, 0, 0, 0, &hud);
        CHECK(e.valid);
        CHECK_NEAR(std::log(e.scale), std::log(1.02), 0.15 * std::log(1.02));
        std::printf("   HUD + zoom: s=%.5f (ln err %.1f%%) conf=%.3f inl=%.2f\n", e.scale,
                    100.0 * (std::log(e.scale) / std::log(1.02) - 1.0), e.confidence, e.inlier_fraction);
    }
    {
        mc::SimilarityEstimator est;
        const mc::SimilarityEstimate e = run_sequence(est, 8, 0, 2.0 / 4, 0, 0, &hud);
        CHECK(e.valid);
        CHECK_NEAR(e.rotation_deg, 2.0, 0.15 * 2.0);
        std::printf("   HUD + roll: rot=%.4f (err %.1f%%) conf=%.3f inl=%.2f\n", e.rotation_deg,
                    100.0 * (e.rotation_deg / 2.0 - 1.0), e.confidence, e.inlier_fraction);
    }
}

TEST(similarity_two_depth_layers) {
    // 远景（中心半径 30 格）放大 1.01，近景（外圈）放大 1.03（66 ms 两帧）：结果为正的扩张，落在两者之间
    mc::SimilarityEstimator est;
    Layered layer{30.0, Motion{1.01}};
    const Frame a = render(Motion{}, 3);
    const Frame b = render(Motion{1.03}, 3, nullptr, &layer);
    est.push(a.data(), 1.0);
    const mc::SimilarityEstimate e = est.push(b.data(), 1.066);
    CHECK(e.valid);
    const double ln = std::log(e.scale);
    CHECK(ln > 0.9 * std::log(1.01));
    CHECK(ln < 1.1 * std::log(1.03));
    std::printf("   layers 1.01 / 1.03: est s=%.5f conf=%.3f inl=%.2f\n", e.scale, e.confidence, e.inlier_fraction);
}

TEST(similarity_pure_pan_no_false_forward) {
    // 每帧横向 6 格（1080p 下约 200 °/s）、纵向 1 格：缩放 / 旋转应接近 0
    for (const double dx : {6.0, -12.0}) {
        mc::SimilarityEstimator est;
        const mc::SimilarityEstimate e = run_sequence(est, 10, 0, 0, dx, 1.0, nullptr, 5);
        CHECK(e.valid);
        const double zoom = *mc::zoom_to_deg_per_s(e.scale, e.dt_s, kHfov);
        const double roll = *mc::roll_to_deg_per_s(e.rotation_deg, e.dt_s);
        CHECK(zoom < 3.0);
        CHECK(roll < 3.0);
        CHECK_NEAR(e.tx, dx * e.frames_back, 0.5);
        std::printf("   pan %.0f cells/frame: zoom=%.2f deg/s roll=%.2f deg/s t=(%.2f, %.2f) back=%d conf=%.3f\n", dx,
                    zoom, roll, e.tx, e.ty, e.frames_back, e.confidence);
    }
}

TEST(similarity_static_scene_is_valid_zero) {
    mc::SimilarityEstimator est;
    const mc::SimilarityEstimate e = run_sequence(est, 6, 0, 0, 0, 0);
    CHECK(e.valid);
    CHECK(std::fabs(std::log(e.scale)) < 1e-4);
    CHECK(std::fabs(e.rotation_deg) < 0.01);
    CHECK(e.confidence > 0.95);
}

// ---------------------------------------------------------------- 大运动（fakegame 的验证速度）

TEST(similarity_fast_zoom_and_roll_like_fakegame) {
    // fakegame --zoom 6.5：每帧 ln s = 6.5/60；--roll 360：每帧 6°。
    // 测试纹理的细节有限（最细一个八度 2 格），只放大到 e^(6.5·12/60) ≈ 3.7 倍；fakegame 用随缩放滚动的八度，没有这个限制。
    {
        mc::SimilarityEstimator est;
        int valid = 0;
        double sum = 0;
        for (int k = 0; k < 13; ++k) {
            const Frame f = render(Motion{std::exp(6.5 / kFps * k)}, 9);
            const mc::SimilarityEstimate e = est.push(f.data(), 10.0 + k / kFps);
            if (e.valid && k >= 6) {
                ++valid;
                sum += std::log(e.scale) / e.dt_s;
            }
        }
        CHECK_EQ(valid, 7);
        const double rate = valid > 0 ? sum / valid : 0;
        CHECK_NEAR(rate, 6.5, 0.65);
        std::printf("   zoom 6.5/s: est %.3f/s (%d valid of 7)\n", rate, valid);
    }
    {
        mc::SimilarityEstimator est;
        int valid = 0;
        double sum = 0;
        for (int k = 0; k < 20; ++k) {
            const Frame f = render(Motion{1.0, 6.0 * k}, 9);
            const mc::SimilarityEstimate e = est.push(f.data(), 10.0 + k / kFps);
            if (e.valid && k >= 6) {
                ++valid;
                sum += e.rotation_deg / e.dt_s;
            }
        }
        CHECK_EQ(valid, 14);
        const double rate = valid > 0 ? sum / valid : 0;
        CHECK_NEAR(rate, 360.0, 36.0);
        std::printf("   roll 360 deg/s: est %.1f deg/s (%d valid of 14)\n", rate, valid);
    }
}

// ---------------------------------------------------------------- 无效情形

TEST(similarity_brightness_jump_is_invalid) {
    mc::SimilarityEstimator est;
    const Frame a = render(Motion{});
    const Frame b = render(Motion{}, 1, nullptr, nullptr, 1.3, 0.15);   // ×1.3 + 0.15 截断到 [0,1]
    est.push(a.data(), 1.0);
    const mc::SimilarityEstimate e = est.push(b.data(), 1.0 + 1 / kFps);
    CHECK_FALSE(e.valid);
    // 整屏纯白
    const Frame white(static_cast<std::size_t>(kW) * kH, 1.0f);
    const mc::SimilarityEstimate w = est.push(white.data(), 1.0 + 2 / kFps);
    CHECK_FALSE(w.valid);
    // 闪白之后的第一帧（与纯白帧比较）也无效，再下一帧恢复
    const mc::SimilarityEstimate after = est.push(a.data(), 1.0 + 3 / kFps);
    CHECK_FALSE(after.valid);
    const mc::SimilarityEstimate again = est.push(a.data(), 1.0 + 4 / kFps);
    CHECK(again.valid);
}

TEST(similarity_scene_replacement_is_invalid) {
    mc::SimilarityEstimator est;
    const Frame a = render(Motion{}, 11);
    const Frame b = render(Motion{}, 12);   // 另一张独立纹理
    est.push(a.data(), 1.0);
    const mc::SimilarityEstimate e = est.push(b.data(), 1.0 + 1 / kFps);
    CHECK_FALSE(e.valid);
    CHECK(e.confidence < 0.60);
    std::printf("   replacement: conf=%.3f\n", e.confidence);
}

TEST(similarity_flat_frame_is_invalid_without_nan) {
    mc::SimilarityEstimator est;
    const Frame gray(static_cast<std::size_t>(kW) * kH, 0.5f);
    for (int k = 0; k < 4; ++k) {
        const mc::SimilarityEstimate e = est.push(gray.data(), 2.0 + k / kFps);
        CHECK_FALSE(e.valid);
        CHECK(std::isfinite(e.scale));
        CHECK(std::isfinite(e.rotation_deg));
        CHECK(std::isfinite(e.confidence));
        CHECK_EQ(e.confidence, 0.0);
    }
    // 只有一维纹理（横条纹）：缩放 / 旋转不可解 → 无效，同样没有 NaN
    Frame stripes(static_cast<std::size_t>(kW) * kH);
    for (int j = 0; j < kH; ++j)
        for (int i = 0; i < kW; ++i) stripes[static_cast<std::size_t>(j) * kW + i] = static_cast<float>(0.5 + 0.3 * std::sin(j * 0.4));
    mc::SimilarityEstimator est2;
    est2.push(stripes.data(), 3.0);
    const mc::SimilarityEstimate s = est2.push(stripes.data(), 3.0 + 1 / kFps);
    CHECK_FALSE(s.valid);
    CHECK(std::isfinite(s.scale));
    CHECK(std::isfinite(s.confidence));
    CHECK_EQ(est.push(nullptr, 9.0).valid, false);
}

TEST(similarity_interval_too_long_and_reset) {
    mc::SimilarityEstimator est;
    const Frame a = render(Motion{});
    const Frame b = render(Motion{1.02});
    est.push(a.data(), 1.0);
    CHECK_FALSE(est.push(b.data(), 1.4).valid);   // 400 ms
    CHECK(est.push(b.data(), 1.4 + 1 / kFps).valid);
    est.reset();
    CHECK_FALSE(est.push(a.data(), 2.0).valid);   // reset 后是首帧
    CHECK(est.push(a.data(), 2.0 + 1 / kFps).valid);
}

TEST(similarity_baseline_zero_uses_previous_frame) {
    mc::SimilarityParams p;
    p.baseline_s = 0.0;
    mc::SimilarityEstimator est(p);
    const mc::SimilarityEstimate e = run_sequence(est, 6, std::log(1.01), 0, 0, 0);
    CHECK(e.valid);
    CHECK_EQ(e.frames_back, 1);
    CHECK_NEAR(std::log(e.scale), std::log(1.01), 0.1 * std::log(1.01));
}

TEST(similarity_non_square_cells) {
    // 1920×1200 显示器降到 160×90：每格 12 × 13.33 像素（aspect 1.111）。内容在屏幕上旋转 2°。
    const double asp = 1200.0 / 90.0 / (1920.0 / 160.0);
    mc::SimilarityParams p;
    p.pixel_aspect = asp;
    mc::SimilarityEstimator est(p);
    mc::SimilarityEstimate e;
    for (int k = 0; k < 8; ++k) {
        const double th = 0.5 * k * kPi / 180.0;
        Frame f(static_cast<std::size_t>(kW) * kH);
        for (int j = 0; j < kH; ++j)
            for (int i = 0; i < kW; ++i) {
                double acc = 0;
                for (int v = 0; v < 3; ++v)
                    for (int u = 0; u < 3; ++u) {
                        const double px = i + (u + 0.5) / 3 - kW / 2.0;
                        const double py = (j + (v + 0.5) / 3 - kH / 2.0) * asp;   // 屏幕上的正方形坐标
                        const double qx = std::cos(th) * px + std::sin(th) * py;
                        const double qy = -std::sin(th) * px + std::cos(th) * py;
                        acc += scene(qx, qy, 4);
                    }
                f[static_cast<std::size_t>(j) * kW + i] = static_cast<float>(acc / 9);
            }
        e = est.push(f.data(), 1.0 + k / kFps);
    }
    CHECK(e.valid);
    CHECK_NEAR(e.rotation_deg, 2.0, 0.2);
    CHECK(std::fabs(std::log(e.scale)) < 0.002);
}

// ---------------------------------------------------------------- 耗时

TEST(similarity_timing_per_frame) {
    // 预先渲染一段"缩放 + 旋转 + 平移"的 24 帧循环（每帧 ln s = 0.005、0.3°、1 格），重复推入 600 帧
    std::vector<Frame> frames;
    for (int k = 0; k < 24; ++k) frames.push_back(render(Motion{std::exp(0.005 * k), 0.3 * k, 1.0 * k, 0.0}, 21));
    mc::SimilarityEstimator est;
    int valid = 0;
    long long iters = 0;
    const int n = 600;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        const int k = i % 24;
        if (k == 0) est.reset();   // 循环接缝处不比较
        mc::TranslationHint hint{1.0, 0.0};
        const mc::SimilarityEstimate e = est.push(frames[static_cast<std::size_t>(k)].data(), i / kFps, &hint);
        valid += e.valid ? 1 : 0;
        iters += e.iterations;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
#ifdef NDEBUG
    std::printf("   %dx%d: %.3f ms/frame, %.1f GN iterations/frame (release, limit 0.5 ms)\n", kW, kH, ms,
                static_cast<double>(iters) / n);
    CHECK(ms < 0.5);
#else
    std::printf("   %dx%d: %.3f ms/frame, %.1f GN iterations/frame (debug build: not asserted)\n", kW, kH, ms,
                static_cast<double>(iters) / n);
#endif
    CHECK(valid >= n * 22 / 24 - 2);
}

TEST(similarity_timing_static_and_pan_fast_path) {
    // 静止画面与纯平移：链的组合已经说明"几乎没有缩放 / 旋转"，跳过长基线精化
    std::vector<Frame> frames;
    for (int k = 0; k < 24; ++k) frames.push_back(render(Motion{1.0, 0.0, 2.0 * k, 0.0}, 22));
    for (const bool pan : {false, true}) {
        mc::SimilarityEstimator est;
        int valid = 0, refined = 0;
        const int n = 600;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            const int k = pan ? i % 24 : 0;
            if (pan && k == 0) est.reset();
            mc::TranslationHint hint{pan ? 2.0 : 0.0, 0.0};
            const mc::SimilarityEstimate e = est.push(frames[static_cast<std::size_t>(k)].data(), i / kFps, &hint);
            valid += e.valid ? 1 : 0;
            refined += e.refined ? 1 : 0;
            if (e.valid) CHECK(*mc::zoom_to_deg_per_s(e.scale, e.dt_s, kHfov) < 3.0);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("   %s: %.3f ms/frame, refined %d of %d valid\n", pan ? "pan 2 cells/frame" : "static", ms, refined, valid);
        CHECK_EQ(refined, 0);
        CHECK(valid >= (pan ? n * 22 / 24 - 2 : n - 1));
#ifdef NDEBUG
        CHECK(ms < 0.15);
#endif
    }
}

TEST(similarity_small_motion_uses_chain_estimate) {
    // CaptureService 的门槛：前进流速 4 °/s（HFOV 90° → ln 速率 0.0889/s）、倾斜 4 °/s。
    // 2.25 °/s 的前进与 2 °/s 的倾斜都低于门槛：不精化，链的结果仍在 ±35% 内
    mc::SimilarityEstimator est;
    est.set_refine_thresholds(4.0 / 45.0, 4.0);
    const double ln_rate = 0.05, deg_rate = 2.0;
    const mc::SimilarityEstimate e = run_sequence(est, 10, ln_rate / kFps, deg_rate / kFps, 0, 0, nullptr, 31);
    CHECK(e.valid);
    CHECK_FALSE(e.refined);
    CHECK_NEAR(std::log(e.scale) / e.dt_s, ln_rate, 0.35 * ln_rate);
    CHECK_NEAR(e.rotation_deg / e.dt_s, deg_rate, 0.35 * deg_rate);
    std::printf("   chain only: ln rate %.3f/s (cmd %.3f), roll %.2f deg/s (cmd %.1f)\n", std::log(e.scale) / e.dt_s,
                ln_rate, e.rotation_deg / e.dt_s, deg_rate);
    // 超过门槛（前进 13.5 °/s）：精化
    mc::SimilarityEstimator est2;
    est2.set_refine_thresholds(4.0 / 45.0, 4.0);
    const mc::SimilarityEstimate f = run_sequence(est2, 10, 0.3 / kFps, 0, 0, 0, nullptr, 31);
    CHECK(f.valid);
    CHECK_TRUE(f.refined);
    CHECK_NEAR(std::log(f.scale) / f.dt_s, 0.3, 0.03);
}
