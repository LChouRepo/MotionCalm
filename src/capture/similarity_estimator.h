// similarity_estimator.h — 截屏前进 / 倾斜检测的纯逻辑（不含 Win32 / D3D）：在灰度网格上用鲁棒参数化 Lucas–Kanade（反向组合、两层金字塔、Huber / Tukey 权重）估计全局相似变换（平移 + 缩放 + 旋转），与约 66 ms 前的历史帧比较；以及缩放 / 旋转 → °/s 的换算。
#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace mc {

// 相似变换估计参数（openspec forward-and-roll-detection design D2–D5）。
struct SimilarityParams {
    int width = 160;                  // 网格宽（格）
    int height = 90;                  // 网格高（格）
    double pixel_aspect = 1.0;        // 每格在屏幕上的高 / 宽（1920×1080 → 160×90 时为 1）；≤ 0 按 1
    double baseline_s = 0.066;        // 目标基线：与多久之前的帧比较（秒）；0 = 总是与上一帧比较
    double min_confidence = 0.60;     // confidence ≥ 该值才算有效（与 [capture] min_correlation 同一个值）
    double min_texture = 0.005;       // 灰度标准差低于它 → 无纹理（输入灰度单位，默认对应 0..1 灰度）
    double max_brightness_jump = 0.08;   // 相邻两帧灰度均值之差超过它 → 视为闪光 / 换场（输入灰度单位）
    // 省算力：短基线链组合出的长基线运动低于这两个速率时，不再做长基线精化，直接用链的结果（误差约 ±25%）。
    // CaptureService 按 HFOV 设为前进流速 4 °/s、倾斜 4 °/s：约为前进 / 倾斜独立阈值最低一档起点
    // （高灵敏度：前进 10 °/s、倾斜 8 °/s）的一半，精化与否基本不影响强度。
    double refine_min_log_rate = 0.1;    // |ln s| / dt（1/s）
    double refine_min_deg_rate = 4.0;    // |θ| / dt（°/s）
};

// 内容运动（参考帧 → 当前帧），以网格中心为原点、y 向下、y 坐标先乘 pixel_aspect（即屏幕上的正方形坐标）：
//   p_cur = scale · R(rotation) · p_ref + (tx, ty·pixel_aspect)
struct SimilarityEstimate {
    bool valid = false;          // 首帧、无纹理、闪光 / 换场、拟合失败、置信度低时为 false
    double scale = 1.0;          // 内容放大倍数 s（> 1：画面向外扩张 = 向前；< 1：向后）
    double rotation_deg = 0.0;   // 内容旋转角（屏幕上顺时针为正）
    double tx = 0, ty = 0;       // 内容平移（格）
    double dt_s = 0;             // 当前帧与所用参考帧的时间差（秒）；没有参考帧时为 0
    double confidence = 0;       // 0..1：鲁棒加权的归一化互相关（重叠不足时按比例打折）
    double inlier_fraction = 0;  // 重叠像素中权重 > 0 的比例
    int frames_back = 0;         // 参考帧是几帧之前（1 = 上一帧）；没有参考帧时为 0
    int iterations = 0;          // 本次 push 的 Gauss–Newton 迭代总数（各层、各候选之和）
    bool refined = false;        // 做了细层精化（false = 运动很小，直接用短基线链的组合）
};

// 来自 MotionEstimator（投影互相关）的平移提示：上一帧 → 本帧的内容平移（格，与 gray 同一方向）。
struct TranslationHint {
    double dx = 0, dy = 0;
};

// 全局相似变换估计。每帧：归一化 → 两层金字塔 + 稀疏采样点的梯度 → 与上一帧在粗层做短基线估计（候选初值：平移提示 /
// 上一帧的运动 / 恒等）→ 沿短基线链组合出到约 baseline_s 之前那一帧的初值 → 运动明显时粗层 + 细层精化 → 置信度。
// configure() 之后 push() 不再分配堆内存。非线程安全。
class SimilarityEstimator {
public:
    static constexpr int kRingCapacity = 20;   // 保留的历史帧数（含当前帧）

    explicit SimilarityEstimator(SimilarityParams p = {});

    // 更换参数（重新分配缓冲）并 reset()。非法值会被收拢：尺寸 ≥ 8，pixel_aspect > 0，baseline_s ∈ [0, 0.25]。
    void configure(const SimilarityParams& p);
    const SimilarityParams& params() const { return params_; }
    // 只改省算力的两个门槛（不丢弃历史帧）。
    void set_refine_thresholds(double min_log_rate, double min_deg_rate) {
        params_.refine_min_log_rate = min_log_rate;
        params_.refine_min_deg_rate = min_deg_rate;
    }

    // 输入一帧行主序 width×height 灰度（任意线性标度），返回它相对历史帧的相似变换。
    // 不论结果是否有效，本帧都进入历史环。gray 为 nullptr 时返回无效结果且不改变状态。
    SimilarityEstimate push(const float* gray, double timestamp_s, const TranslationHint* hint = nullptr);

    // 丢弃全部历史帧：下一次 push() 视为首帧。
    void reset();

    using Cplx = std::complex<double>;
    // 映射 cur → ref：q = a·p + b（p、q 为 SimilarityEstimate 注释里的正方形坐标，单位 = 格宽）。
    struct Warp {
        Cplx a{1.0, 0.0};
        Cplx b{0.0, 0.0};
    };

private:
    struct Entry {
        double t = 0;
        double mean = 0, stddev = 0;   // 原始灰度的均值 / 标准差
        bool textured = false;
        bool link_valid = false;       // link：本帧 → 上一帧（环中前一项）的短基线映射
        Warp link;
        std::vector<float> l0, l1;     // 归一化后的细层 / 粗层
    };
    struct Fit {
        Warp w;
        bool ok = false;
        double ncc = 0;        // 鲁棒加权 NCC
        double overlap = 0;    // 落在参考帧内的样本比例
        double inlier = 0;     // 其中权重 > 0 的比例
        double med_abs = 0;    // |残差| 的中位数（归一化灰度单位）
        int iters = 0;
    };
    enum class Robust { Huber, Tukey };
    // 一层的采样点（细层约 1/8、粗层约 1/4 的像素，见 configure），位置在 configure 时定好，模板值与最速下降向量每帧更新。
    struct Samples {
        int w = 0, h = 0;                  // 该层图像尺寸
        double spacing = 1;                // 该层一个像素 = 多少格宽
        std::vector<int> idx;              // 像素下标 j·w + i
        std::vector<float> fi, fj;         // (i, j)
        std::vector<float> t;              // 模板值
        std::vector<float> sd0, sd1, gx, gy;   // ∇T·∂W/∂(da, db, dtx, dty)
        mutable std::vector<float> e;      // 残差；NaN = 不在参考帧内
    };

    // 只做第 1 遍（采样 + 残差 + 中位数），给候选初值打分。
    Fit residuals(const Samples& smp, const float* ref, const Warp& w) const;
    Fit fit(int level, const Entry& ref, const Warp& init, int max_iters, Robust kind) const;
    double confidence_of(const Fit& f) const;
    void build_template(Entry& e, const float* gray);
    void init_samples(Samples& s, int w, int h, double spacing, bool checkerboard);

    SimilarityParams params_;
    int w0_ = 0, h0_ = 0, w1_ = 0, h1_ = 0;
    std::vector<Entry> ring_;    // kRingCapacity 项
    int head_ = -1;              // 最新一项的下标
    int count_ = 0;
    Samples s0_, s1_;            // 当前帧（模板）在细层 / 粗层的采样点
};

// 缩放 → 画面水平边缘的径向流速（°/s）：|ln s| / dt × hfov / 2。
// dt ≤ 0、dt > 0.25 s、s ≤ 0、非有限值 → nullopt。
std::optional<double> zoom_to_deg_per_s(double scale, double dt_s, double assumed_hfov_deg);
// 旋转 → 倾斜角速度（°/s）：|θ| / dt。dt 的规则同上。
std::optional<double> roll_to_deg_per_s(double rotation_deg, double dt_s);

}  // namespace mc
