// similarity_estimator.cpp — 鲁棒参数化 Lucas–Kanade 相似变换估计与缩放 / 旋转换算的实现（纯标准库）。
#include "capture/similarity_estimator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <bit>
#include <numbers>

#include <emmintrin.h>   // SSE2（x64 基线指令集）

namespace mc {

namespace {

constexpr double kMaxIntervalS = 0.25;      // 与 kMaxFrameIntervalS 相同：更长的间隔不比较
constexpr double kMinStdRatio = 0.67;       // 相邻两帧灰度标准差之比超出 [0.67, 1.5] → 闪光 / 换场
constexpr int kHistBins = 256;              // |残差| 直方图（归一化灰度单位），用于求中位数
constexpr float kHistMax = 4.0f;
constexpr double kSigmaFloor = 0.02;        // 鲁棒尺度下限（归一化灰度单位）：完美匹配时不至于把量化误差当离群
constexpr double kHuberK = 1.345;
constexpr double kTukeyC = 4.685;
constexpr double kMinOverlapForFull = 0.35; // 重叠比例低于它时置信度按比例打折
constexpr double kMinInlierForFull = 0.50;  // 内点比例低于它时置信度按比例打折
// 每次 push 的迭代上限：短基线（粗层）、长基线粗层、细层。
constexpr int kShortIters = 3;
constexpr int kLongCoarseIters = 1;
constexpr int kFineIters = 3;

using Cplx = SimilarityEstimator::Cplx;
using Warp = SimilarityEstimator::Warp;

// q = outer(inner(p))
Warp compose(const Warp& outer, const Warp& inner) { return Warp{outer.a * inner.a, outer.a * inner.b + outer.b}; }

// 4×4 对称正定方程组 H·x = g（Cholesky）。主元相对其对角元过小 → false（退化：无纹理或只有一维纹理）。
bool solve4(const std::array<double, 16>& H, const std::array<double, 4>& g, std::array<double, 4>& x) {
    double L[4][4] = {};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = H[static_cast<std::size_t>(i * 4 + j)];
            for (int k = 0; k < j; ++k) s -= L[i][k] * L[j][k];
            if (i == j) {
                const double d = H[static_cast<std::size_t>(i * 4 + i)];
                if (!(d > 0) || !(s > 1e-6 * d)) return false;
                L[i][i] = std::sqrt(s);
            } else {
                L[i][j] = s / L[j][j];
            }
        }
    }
    double y[4];
    for (int i = 0; i < 4; ++i) {
        double s = g[static_cast<std::size_t>(i)];
        for (int k = 0; k < i; ++k) s -= L[i][k] * y[k];
        y[i] = s / L[i][i];
    }
    for (int i = 3; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < 4; ++k) s -= L[k][i] * x[static_cast<std::size_t>(k)];
        x[static_cast<std::size_t>(i)] = s / L[i][i];
    }
    for (const double v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------- 换算

std::optional<double> zoom_to_deg_per_s(double scale, double dt_s, double assumed_hfov_deg) {
    if (!(dt_s > 0) || !(dt_s <= kMaxIntervalS)) return std::nullopt;
    if (!(scale > 0) || !std::isfinite(scale) || !std::isfinite(assumed_hfov_deg)) return std::nullopt;
    return std::fabs(std::log(scale)) / dt_s * (assumed_hfov_deg / 2.0);
}

std::optional<double> roll_to_deg_per_s(double rotation_deg, double dt_s) {
    if (!(dt_s > 0) || !(dt_s <= kMaxIntervalS)) return std::nullopt;
    if (!std::isfinite(rotation_deg)) return std::nullopt;
    return std::fabs(rotation_deg) / dt_s;
}

// ---------------------------------------------------------------- SimilarityEstimator

SimilarityEstimator::SimilarityEstimator(SimilarityParams p) { configure(p); }

void SimilarityEstimator::init_samples(Samples& s, int w, int h, double spacing, bool checkerboard) {
    s.w = w;
    s.h = h;
    s.spacing = spacing;
    s.idx.clear();
    s.fi.clear();
    s.fj.clear();
    for (int j = 1; j < h - 1; ++j) {
        for (int i = 1; i < w - 1; ++i) {
            // 稀疏采样（省算力）：粗层取奇数行上的棋盘格；细层取奇数行奇数列构成的子格上的棋盘格。
            const bool take = checkerboard ? ((j & 1) == 1 && ((i + j) & 1) == 0)
                                           : ((i & 1) == 1 && (j & 1) == 1 && (((i >> 1) + (j >> 1)) & 1) == 0);
            if (!take) continue;
            s.idx.push_back(j * w + i);
            s.fi.push_back(static_cast<float>(i));
            s.fj.push_back(static_cast<float>(j));
        }
    }
    // 补齐到 4 的倍数（SSE 一次处理 4 个样本）：补出来的样本残差恒为 NaN（不参与），其余量为 0。
    const std::size_t n_pad = (s.idx.size() + 3) & ~static_cast<std::size_t>(3);
    for (std::vector<float>* v : {&s.t, &s.sd0, &s.sd1, &s.gx, &s.gy}) v->assign(n_pad, 0.0f);
    s.e.assign(n_pad, std::numeric_limits<float>::quiet_NaN());
}

void SimilarityEstimator::configure(const SimilarityParams& p) {
    params_ = p;
    params_.width = std::max(8, params_.width);
    params_.height = std::max(8, params_.height);
    if (!(params_.pixel_aspect > 0) || !std::isfinite(params_.pixel_aspect)) params_.pixel_aspect = 1.0;
    if (!(params_.baseline_s >= 0)) params_.baseline_s = 0.0;
    params_.baseline_s = std::min(params_.baseline_s, kMaxIntervalS);
    w0_ = params_.width;
    h0_ = params_.height;
    w1_ = w0_ / 2;
    h1_ = h0_ / 2;
    const std::size_t n0 = static_cast<std::size_t>(w0_) * static_cast<std::size_t>(h0_);
    const std::size_t n1 = static_cast<std::size_t>(w1_) * static_cast<std::size_t>(h1_);
    ring_.assign(kRingCapacity, Entry{});
    for (Entry& e : ring_) {
        e.l0.assign(n0, 0.0f);
        e.l1.assign(n1, 0.0f);
    }
    init_samples(s0_, w0_, h0_, 1.0, false);   // 细层：奇数行奇数列上再取棋盘格（约 1/8 的像素，160×90 时 1738 个）
    init_samples(s1_, w1_, h1_, 2.0, true);    // 粗层：奇数行上的棋盘格（约 1/4 的像素，80×45 时 840 个）
    reset();
}

void SimilarityEstimator::reset() {
    head_ = -1;
    count_ = 0;
    for (Entry& e : ring_) {
        e.link_valid = false;
        e.textured = false;
    }
}

void SimilarityEstimator::build_template(Entry& e, const float* gray) {
    const std::size_t n0 = static_cast<std::size_t>(w0_) * static_cast<std::size_t>(h0_);
    // 逐行用 float 累加（可向量化），行和再进 double。
    double sum = 0, sum2 = 0;
    for (int j = 0; j < h0_; ++j) {
        const float* row = gray + static_cast<std::size_t>(j) * static_cast<std::size_t>(w0_);
        float rs = 0, rs2 = 0;
        for (int i = 0; i < w0_; ++i) {
            rs += row[i];
            rs2 += row[i] * row[i];
        }
        sum += rs;
        sum2 += rs2;
    }
    const double mean = sum / static_cast<double>(n0);
    const double var = std::max(0.0, sum2 / static_cast<double>(n0) - mean * mean);
    e.mean = std::isfinite(mean) ? mean : 0.0;
    e.stddev = std::isfinite(var) ? std::sqrt(var) : 0.0;
    e.textured = e.stddev >= params_.min_texture && e.stddev > 0;
    const float m = static_cast<float>(e.mean);
    const float inv = e.textured ? static_cast<float>(1.0 / e.stddev) : 0.0f;
    for (std::size_t k = 0; k < n0; ++k) e.l0[k] = (gray[k] - m) * inv;
    for (int j = 0; j < h1_; ++j) {
        const float* r0 = e.l0.data() + static_cast<std::size_t>(2 * j) * static_cast<std::size_t>(w0_);
        const float* r1 = r0 + w0_;
        float* o = e.l1.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(w1_);
        for (int i = 0; i < w1_; ++i) o[i] = 0.25f * (r0[2 * i] + r0[2 * i + 1] + r1[2 * i] + r1[2 * i + 1]);
    }
    // 采样点上的模板值、梯度（中心差分，单位：每格宽；y 方向除以 pixel_aspect）与最速下降向量
    // sd = [gx·x + gy·y, −gx·y + gy·x, gx, gy]，(x, y) 为以网格中心为原点的正方形坐标。
    const double asp = params_.pixel_aspect;
    auto fill = [&](Samples& s, const float* img) {
        const double kx = 0.5 / s.spacing, ky = 0.5 / s.spacing / asp;
        const double ox = 0.5 * s.spacing - 0.5 * w0_, oy = 0.5 * s.spacing - 0.5 * h0_;
        const std::size_t n = s.idx.size();
        for (std::size_t q = 0; q < n; ++q) {
            const std::size_t k = static_cast<std::size_t>(s.idx[q]);
            const double gx = (img[k + 1] - img[k - 1]) * kx;
            const double gy = (img[k + static_cast<std::size_t>(s.w)] - img[k - static_cast<std::size_t>(s.w)]) * ky;
            const double x = s.spacing * s.fi[q] + ox;
            const double y = (s.spacing * s.fj[q] + oy) * asp;
            s.t[q] = img[k];
            s.gx[q] = static_cast<float>(gx);
            s.gy[q] = static_cast<float>(gy);
            s.sd0[q] = static_cast<float>(gx * x + gy * y);
            s.sd1[q] = static_cast<float>(-gx * y + gy * x);
        }
    };
    fill(s0_, e.l0.data());
    fill(s1_, e.l1.data());
}

SimilarityEstimator::Fit SimilarityEstimator::residuals(const Samples& s, const float* I, const Warp& wp) const {
    // 参考帧中的采样位置（该层像素下标）是 (i, j) 的仿射函数：
    //   fi = ar·i − ai·asp·j + ci，fj = (ai/asp)·i + ar·j + cj
    Fit f;
    f.w = wp;
    const double asp = params_.pixel_aspect;
    const double sp = s.spacing;
    const double ox = 0.5 * sp - 0.5 * w0_, oy = 0.5 * sp - 0.5 * h0_;
    const double ar = wp.a.real(), ai = wp.a.imag(), br = wp.b.real(), bi = wp.b.imag();
    const float a_ii = static_cast<float>(ar), a_ij = static_cast<float>(-ai * asp);
    const float a_ji = static_cast<float>(ai / asp), a_jj = static_cast<float>(ar);
    const float ci = static_cast<float>((ar * ox - ai * oy * asp + br - ox) / sp);
    const float cj = static_cast<float>((ai * ox / asp + ar * oy + bi / asp - oy) / sp);
    const int w = s.w, h = s.h;
    const float fw_max = static_cast<float>(w - 1), fh_max = static_cast<float>(h - 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float hist_k = static_cast<float>(kHistBins) / kHistMax;
    // 4 份交错的直方图：残差集中在少数几个桶里时，++hist[b] 不会一直等同一个地址的上一次写入。
    std::array<std::array<int, kHistBins + 1>, 4> hist4{};
    const std::size_t n = s.idx.size();
    int n_in = 0;
    float* E = s.e.data();
    for (std::size_t q = 0; q < n; ++q) {
        const float fi = a_ii * s.fi[q] + a_ij * s.fj[q] + ci;
        const float fj = a_ji * s.fi[q] + a_jj * s.fj[q] + cj;
        if (!(fi >= 0.0f && fj >= 0.0f && fi <= fw_max && fj <= fh_max)) {
            E[q] = nan;
            continue;
        }
        int ix = static_cast<int>(fi), iy = static_cast<int>(fj);
        if (ix > w - 2) ix = w - 2;
        if (iy > h - 2) iy = h - 2;
        const float ax = fi - static_cast<float>(ix), ay = fj - static_cast<float>(iy);
        const float* p0 = I + static_cast<std::size_t>(iy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ix);
        const float* p1 = p0 + w;
        const float top = p0[0] + (p0[1] - p0[0]) * ax;
        const float bot = p1[0] + (p1[1] - p1[0]) * ax;
        const float e = top + (bot - top) * ay - s.t[q];
        E[q] = e;
        ++n_in;
        int b = static_cast<int>(std::fabs(e) * hist_k);
        if (b > kHistBins) b = kHistBins;
        ++hist4[q & 3][static_cast<std::size_t>(b)];
    }
    f.overlap = n > 0 ? static_cast<double>(n_in) / static_cast<double>(n) : 0.0;
    f.inlier = n_in;   // 暂存样本数，fit() 里换成比例
    if (n_in == 0) {
        f.med_abs = kHistMax;
        return f;
    }
    const int half = (n_in + 1) / 2;
    int cum = 0;
    f.med_abs = kHistMax;
    for (int b = 0; b <= kHistBins; ++b) {
        const std::size_t bb = static_cast<std::size_t>(b);
        const int c = hist4[0][bb] + hist4[1][bb] + hist4[2][bb] + hist4[3][bb];
        if (cum + c >= half) {
            const double frac = c > 0 ? (half - cum - 0.5) / c : 0.5;
            f.med_abs = (b + std::clamp(frac, 0.0, 1.0)) * (kHistMax / kHistBins);
            break;
        }
        cum += c;
    }
    f.ok = true;
    return f;
}

SimilarityEstimator::Fit SimilarityEstimator::fit(int level, const Entry& ref, const Warp& init, int max_iters,
                                                  Robust kind) const {
    Fit f;
    f.w = init;
    const Samples& s = level == 1 ? s1_ : s0_;
    const float* I = level == 1 ? ref.l1.data() : ref.l0.data();
    const std::size_t n = s.idx.size();
    if (n < 64) return f;
    int iters = 0;
    for (int it = 0; it < max_iters; ++it) {
        // ---- 第 1 遍：按当前映射采样参考帧，残差 e = I(W(p)) − T(p)，|e| 的中位数 → 鲁棒尺度 σ
        Fit r = residuals(s, I, f.w);
        const int n_in = static_cast<int>(r.inlier);
        if (!r.ok || n_in < 64 || r.overlap < 0.15) {
            f.ok = false;
            f.overlap = r.overlap;
            f.iters = iters;
            return f;
        }
        f.overlap = r.overlap;
        f.med_abs = r.med_abs;
        const double sigma = std::max(1.4826 * r.med_abs, kSigmaFloor);
        const double cut = (kind == Robust::Tukey ? kTukeyC : kHuberK) * sigma;
        const float cutf = static_cast<float>(cut);
        const float inv_cut2 = static_cast<float>(1.0 / (cut * cut));

        // ---- 第 2 遍：权重 + 法方程（反向组合：sd 每帧只算一次），以及鲁棒加权 NCC
        std::array<double, 16> H{};
        std::array<double, 4> g{};
        double sw = 0, swi = 0, swt = 0, swii = 0, swtt = 0, swit = 0;
        int n_pos = 0;
        {
            // SSE2：每次 4 个样本，float 累加器每 256 个样本并入 double。
            enum { kH00, kH01, kH02, kH03, kH11, kH12, kH13, kH22, kH23, kH33, kG0, kG1, kG2, kG3,
                   kSw, kSwi, kSwt, kSwii, kSwtt, kSwit, kAcc };
            __m128 acc[kAcc];
            double tot[kAcc] = {};
            const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
            const __m128 vk = _mm_set1_ps(inv_cut2), vcut = _mm_set1_ps(cutf);
            const __m128 absmask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
            const bool tukey = kind == Robust::Tukey;
            const std::size_t n_pad = s.e.size();
            const float* E = s.e.data();
            std::size_t q = 0;
            while (q < n_pad) {
                for (__m128& a : acc) a = zero;
                const std::size_t q_end = std::min(n_pad, q + 256);
                for (; q < q_end; q += 4) {
                    __m128 e = _mm_loadu_ps(E + q);
                    const __m128 valid = _mm_cmpord_ps(e, e);   // NaN：不在参考帧内
                    e = _mm_and_ps(e, valid);
                    __m128 w;
                    if (tukey) {
                        const __m128 u = _mm_mul_ps(_mm_mul_ps(e, e), vk);
                        w = _mm_max_ps(_mm_sub_ps(one, u), zero);
                        w = _mm_mul_ps(w, w);
                    } else {
                        const __m128 ae = _mm_and_ps(e, absmask);
                        w = _mm_min_ps(one, _mm_div_ps(vcut, ae));   // ae = 0 → +inf → 1
                    }
                    w = _mm_and_ps(w, valid);
                    n_pos += std::popcount(static_cast<unsigned>(_mm_movemask_ps(_mm_cmpgt_ps(w, zero))));
                    const __m128 s0 = _mm_loadu_ps(s.sd0.data() + q), s1 = _mm_loadu_ps(s.sd1.data() + q);
                    const __m128 s2 = _mm_loadu_ps(s.gx.data() + q), s3 = _mm_loadu_ps(s.gy.data() + q);
                    const __m128 w0 = _mm_mul_ps(w, s0), w1 = _mm_mul_ps(w, s1);
                    const __m128 w2 = _mm_mul_ps(w, s2), w3 = _mm_mul_ps(w, s3);
                    acc[kH00] = _mm_add_ps(acc[kH00], _mm_mul_ps(w0, s0));
                    acc[kH01] = _mm_add_ps(acc[kH01], _mm_mul_ps(w0, s1));
                    acc[kH02] = _mm_add_ps(acc[kH02], _mm_mul_ps(w0, s2));
                    acc[kH03] = _mm_add_ps(acc[kH03], _mm_mul_ps(w0, s3));
                    acc[kH11] = _mm_add_ps(acc[kH11], _mm_mul_ps(w1, s1));
                    acc[kH12] = _mm_add_ps(acc[kH12], _mm_mul_ps(w1, s2));
                    acc[kH13] = _mm_add_ps(acc[kH13], _mm_mul_ps(w1, s3));
                    acc[kH22] = _mm_add_ps(acc[kH22], _mm_mul_ps(w2, s2));
                    acc[kH23] = _mm_add_ps(acc[kH23], _mm_mul_ps(w2, s3));
                    acc[kH33] = _mm_add_ps(acc[kH33], _mm_mul_ps(w3, s3));
                    acc[kG0] = _mm_add_ps(acc[kG0], _mm_mul_ps(w0, e));
                    acc[kG1] = _mm_add_ps(acc[kG1], _mm_mul_ps(w1, e));
                    acc[kG2] = _mm_add_ps(acc[kG2], _mm_mul_ps(w2, e));
                    acc[kG3] = _mm_add_ps(acc[kG3], _mm_mul_ps(w3, e));
                    const __m128 t = _mm_loadu_ps(s.t.data() + q);
                    const __m128 iv = _mm_add_ps(t, e);
                    const __m128 wi = _mm_mul_ps(w, iv), wt = _mm_mul_ps(w, t);
                    acc[kSw] = _mm_add_ps(acc[kSw], w);
                    acc[kSwi] = _mm_add_ps(acc[kSwi], wi);
                    acc[kSwt] = _mm_add_ps(acc[kSwt], wt);
                    acc[kSwii] = _mm_add_ps(acc[kSwii], _mm_mul_ps(wi, iv));
                    acc[kSwtt] = _mm_add_ps(acc[kSwtt], _mm_mul_ps(wt, t));
                    acc[kSwit] = _mm_add_ps(acc[kSwit], _mm_mul_ps(wi, t));
                }
                for (int k = 0; k < kAcc; ++k) {
                    alignas(16) float lanes[4];
                    _mm_store_ps(lanes, acc[k]);
                    tot[k] += (static_cast<double>(lanes[0]) + lanes[1]) + (static_cast<double>(lanes[2]) + lanes[3]);
                }
            }
            H[0] = tot[kH00];
            H[1] = tot[kH01];
            H[2] = tot[kH02];
            H[3] = tot[kH03];
            H[5] = tot[kH11];
            H[6] = tot[kH12];
            H[7] = tot[kH13];
            H[10] = tot[kH22];
            H[11] = tot[kH23];
            H[15] = tot[kH33];
            g = {tot[kG0], tot[kG1], tot[kG2], tot[kG3]};
            sw = tot[kSw];
            swi = tot[kSwi];
            swt = tot[kSwt];
            swii = tot[kSwii];
            swtt = tot[kSwtt];
            swit = tot[kSwit];
        }
        H[4] = H[1];
        H[8] = H[2];
        H[9] = H[6];
        H[12] = H[3];
        H[13] = H[7];
        H[14] = H[11];
        f.inlier = static_cast<double>(n_pos) / n_in;
        if (sw > 0) {
            const double vi = swii - swi * swi / sw;
            const double vt = swtt - swt * swt / sw;
            const double cv = swit - swi * swt / sw;
            f.ncc = (vi > 1e-12 && vt > 1e-12) ? std::clamp(cv / std::sqrt(vi * vt), -1.0, 1.0) : 0.0;
        } else {
            f.ncc = 0.0;
        }
        std::array<double, 4> d{};
        if (n_pos < 32 || !solve4(H, g, d)) {
            f.ok = false;
            f.iters = iters;
            return f;
        }
        // W ← W ∘ W(Δ)⁻¹，W(Δ)(p) = (1 + da + i·db)·p + (dtx + i·dty)
        const Cplx da(1.0 + d[0], d[1]);
        const Cplx db(d[2], d[3]);
        if (std::abs(da) < 0.5) {   // 发散
            f.ok = false;
            f.iters = iters;
            return f;
        }
        f.w = Warp{f.w.a / da, f.w.b - f.w.a * db / da};
        f.ok = true;
        ++iters;
        const double sa = std::abs(f.w.a);
        if (!std::isfinite(sa) || sa < 0.25 || sa > 4.0 || !std::isfinite(std::abs(f.w.b))) {
            f.ok = false;
            f.iters = iters;
            return f;
        }
        if (std::fabs(d[0]) < 3e-4 && std::fabs(d[1]) < 3e-4 && std::fabs(d[2]) < 0.01 && std::fabs(d[3]) < 0.01) break;
    }
    f.iters = iters;
    return f;
}

double SimilarityEstimator::confidence_of(const Fit& f) const {
    if (!f.ok) return 0.0;
    double c = std::max(0.0, f.ncc);
    c *= std::min(1.0, f.overlap / kMinOverlapForFull);
    c *= std::min(1.0, f.inlier / kMinInlierForFull);
    return std::clamp(c, 0.0, 1.0);
}

SimilarityEstimate SimilarityEstimator::push(const float* gray, double timestamp_s, const TranslationHint* hint) {
    SimilarityEstimate r;
    if (gray == nullptr) return r;
    const int cap = kRingCapacity;
    const int prev = head_;
    const int prev_count = count_;
    head_ = (head_ + 1) % cap;
    count_ = std::min(count_ + 1, cap);
    Entry& cur = ring_[static_cast<std::size_t>(head_)];
    cur.t = timestamp_s;
    cur.link_valid = false;
    build_template(cur, gray);
    if (prev_count == 0 || prev < 0) return r;

    const Entry& P = ring_[static_cast<std::size_t>(prev)];
    const double dt1 = timestamp_s - P.t;
    if (!(dt1 > 0) || !(dt1 <= kMaxIntervalS)) return r;
    if (!cur.textured || !P.textured) return r;
    // 闪光 / 换场：整体亮度或对比度突变的帧不比较（链在这里断开）
    if (std::fabs(cur.mean - P.mean) > params_.max_brightness_jump) return r;
    const double ratio = cur.stddev / P.stddev;
    if (!(ratio >= kMinStdRatio) || !(ratio <= 1.0 / kMinStdRatio)) return r;

    const double asp = params_.pixel_aspect;
    // ---- 短基线（上一帧，粗层）：候选初值 = 平移提示 / 上一帧的帧间运动（匀速假设）/ 恒等，只做残差打分，取中位数最小者
    Warp cand[3];
    int nc = 0;
    if (hint != nullptr && std::isfinite(hint->dx) && std::isfinite(hint->dy)) {
        cand[nc++] = Warp{Cplx(1.0, 0.0), -Cplx(hint->dx, hint->dy * asp)};
    }
    if (P.link_valid) cand[nc++] = P.link;
    if (hint == nullptr) cand[nc++] = Warp{};
    Warp start = cand[0];
    // 候选几乎相同（静止或匀速时提示与上一帧运动一致）就不必逐个打分
    const auto same = [](const Warp& x, const Warp& y) {
        return std::abs(x.a - y.a) < 1e-3 && std::abs(x.b - y.b) < 0.05;
    };
    if (nc == 2 && same(cand[0], cand[1])) nc = 1;
    if (nc > 1) {
        double best_med = std::numeric_limits<double>::infinity();
        for (int c = 0; c < nc; ++c) {
            const Fit sc = residuals(s1_, P.l1.data(), cand[c]);
            if (sc.ok && sc.overlap >= 0.3 && sc.med_abs < best_med) {
                best_med = sc.med_abs;
                start = cand[c];
            }
        }
    }
    const Fit s1 = fit(1, P, start, kShortIters, Robust::Huber);
    r.iterations += s1.iters;
    cur.link = s1.w;
    cur.link_valid = s1.ok && confidence_of(s1) >= params_.min_confidence;
    if (!cur.link_valid) {
        r.confidence = confidence_of(s1);
        return r;
    }

    // ---- 选参考帧：沿有效的短基线链往回走，取年龄最接近 baseline_s 的那一帧，初值 = 链上映射的组合
    Warp chain = cur.link;   // cur → 环中 idx 那一帧
    int idx = prev;
    int best_idx = prev, best_back = 1;
    Warp best_chain = chain;
    double best_err = std::fabs(dt1 - params_.baseline_s);
    for (int k = 2; k < count_; ++k) {
        const Entry& e = ring_[static_cast<std::size_t>(idx)];
        if (!e.link_valid) break;
        const int older = (idx - 1 + cap) % cap;
        const Entry& o = ring_[static_cast<std::size_t>(older)];
        const double age = timestamp_s - o.t;
        if (!(age > 0) || age > kMaxIntervalS || !o.textured) break;
        chain = compose(e.link, chain);
        idx = older;
        const double err = std::fabs(age - params_.baseline_s);
        if (err < best_err) {
            best_err = err;
            best_idx = idx;
            best_back = k;
            best_chain = chain;
        } else if (age > params_.baseline_s) {
            break;
        }
    }

    // ---- 运动很小：直接用链的组合（置信度取短基线），省掉长基线精化
    {
        const double age = timestamp_s - ring_[static_cast<std::size_t>(best_idx)].t;
        const Cplx inv = 1.0 / best_chain.a;
        const double log_rate = std::fabs(std::log(std::abs(inv))) / age;
        const double deg_rate = std::fabs(std::arg(inv)) * (180.0 / std::numbers::pi) / age;
        if (log_rate < params_.refine_min_log_rate && deg_rate < params_.refine_min_deg_rate) {
            const Cplx t = -best_chain.b * inv;
            r.confidence = confidence_of(s1);
            r.inlier_fraction = s1.inlier;
            r.frames_back = best_back;
            r.dt_s = age;
            r.scale = std::abs(inv);
            r.rotation_deg = std::arg(inv) * (180.0 / std::numbers::pi);
            r.tx = t.real();
            r.ty = t.imag() / asp;
            r.valid = std::isfinite(r.scale) && std::isfinite(r.rotation_deg) && r.scale > 0;
            return r;
        }
    }

    // ---- 长基线精化：粗层（链初值的误差会累积）→ 细层 Tukey；失败时退回上一帧
    r.refined = true;
    Fit fin;
    const Entry* ref = &P;
    int used_back = 1;
    if (best_back > 1) {
        const Entry& R = ring_[static_cast<std::size_t>(best_idx)];
        const Fit c1 = fit(1, R, best_chain, kLongCoarseIters, Robust::Huber);
        r.iterations += c1.iters;
        fin = fit(0, R, c1.ok ? c1.w : best_chain, kFineIters, Robust::Tukey);
        r.iterations += fin.iters;
        ref = &R;
        used_back = best_back;
        if (!fin.ok || confidence_of(fin) < params_.min_confidence) {
            ref = &P;
            used_back = 1;
        }
    }
    if (used_back == 1) {
        fin = fit(0, P, cur.link, kFineIters, Robust::Tukey);
        r.iterations += fin.iters;
    }
    r.confidence = confidence_of(fin);
    r.inlier_fraction = fin.inlier;
    r.frames_back = used_back;
    r.dt_s = timestamp_s - ref->t;
    if (!fin.ok || r.confidence < params_.min_confidence) return r;

    // cur → ref 的映射 q = a·p + b 取逆，得到内容运动 p = a⁻¹·q − a⁻¹·b
    const Cplx inv = 1.0 / fin.w.a;
    const Cplx t = -fin.w.b * inv;
    r.scale = std::abs(inv);
    r.rotation_deg = std::arg(inv) * (180.0 / std::numbers::pi);
    r.tx = t.real();
    r.ty = t.imag() / asp;
    r.valid = std::isfinite(r.scale) && std::isfinite(r.rotation_deg) && r.scale > 0;
    return r;
}

}  // namespace mc
