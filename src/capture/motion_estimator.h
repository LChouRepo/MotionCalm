// motion_estimator.h — 截屏运动检测的纯逻辑（不含 Win32 / D3D）：灰度网格两帧投影互相关 → 全局位移与置信度；位移 → 角速度；低置信度时的保持 / 归零（SpeedHold）。
#pragma once

#include <optional>
#include <vector>

namespace mc {

// 位移估计参数（DESIGN §11，默认值同 [capture] 节）。
struct MotionEstimatorParams {
    int width = 160;                 // 网格宽（格）
    int height = 90;                 // 网格高（格）
    int max_shift = 20;              // 搜索 ±max_shift 格；每轴实际范围 S = min(max_shift, ⌊N/3⌋)
    double min_correlation = 0.60;   // confidence ≥ 该值才算有效
    double min_texture = 1e-3;       // 去趋势投影的"每像素均方根"低于它 → 该轴无纹理（输入灰度单位；默认值对应 0..1 灰度）
};

// 一次估计的结果。
struct MotionEstimate {
    double dx = 0, dy = 0;     // 本帧内容相对上一帧的位移（格，亚格精度；+x 向右、+y 向下）。只有 valid 时可信
    double confidence = 0;     // min(c_x, c_y)；c_axis = 该轴归一化互相关峰值，截断到 [0,1]；无纹理的轴为 0
    bool valid = false;        // 首帧、低置信度、任一轴无纹理（含纯色帧）时为 false
    double dt_s = 0;           // 与上一帧的时间戳差（秒）；首帧为 0
};

// 全局平移估计：列 / 行投影 → 去均值 + 去线性趋势 → 与上一帧投影在重叠段上做皮尔逊相关 → 峰值 + 抛物线亚格插值。
// 每帧 O(W·H + (W+H)·S)，构造（或 configure）之后 push() 不再分配堆内存。非线程安全。
class MotionEstimator {
public:
    explicit MotionEstimator(MotionEstimatorParams p = {});

    // 更换参数（重新分配缓冲）并 reset()。非法值会被收拢：尺寸 ≥ 1，max_shift ≥ 1。
    void configure(const MotionEstimatorParams& p);

    // 输入一帧行主序 width×height 灰度（任意线性标度），返回它相对上一帧的位移。
    // 不论结果是否有效，本帧都会成为下一次比较的参考帧。gray 为 nullptr 时返回无效结果且不改变状态。
    MotionEstimate push(const float* gray, double timestamp_s);

    // 丢弃参考帧：下一次 push() 视为首帧。
    void reset();

    const MotionEstimatorParams& params() const { return params_; }

private:
    struct Axis {
        int n = 0;                     // 投影长度
        int pixels = 0;                // 每个投影元素累加的像素数（列投影 = height，行投影 = width）
        int max_shift = 0;             // S
        std::vector<double> cur, prev; // 去趋势后的投影
        std::vector<double> rho;       // ρ(s)，下标 s + S
        double cur_energy = 0, prev_energy = 0;   // Σ P̂²
        bool cur_textured = false, prev_textured = false;
    };
    struct AxisResult {
        double shift = 0;
        double confidence = 0;
    };

    void init_axis(Axis& a, int n, int pixels) const;
    void finish_axis(Axis& a) const;               // 计算本帧能量与"有纹理"标志
    static AxisResult estimate_axis(Axis& a);
    static void commit_axis(Axis& a);              // 本帧 → 参考帧

    MotionEstimatorParams params_;
    Axis col_, row_;
    bool has_prev_ = false;
    double prev_t_ = 0;
};

// 计算列投影 col_out[x] = Σ_y I(x,y)（width 个）与行投影 row_out[y] = Σ_x I(x,y)（height 个），
// 并各自减去最小二乘直线（同时去均值、去线性趋势）。整体加性亮度变化不影响结果。
void project_and_detrend(const float* gray, int width, int height, double* col_out, double* row_out);

// 网格位移 → 相机转过的角度（°）：shift_px = √((dx·W/grid_w)² + (dy·H/grid_h)²)，deg = shift_px·hfov/W。
// W、H 为被复制显示器（按桌面方向）的像素宽高。任一尺寸 ≤ 0 时返回 0。
double shift_to_deg(double dx_cells, double dy_cells, int grid_w, int grid_h, int screen_w_px, int screen_h_px,
                    double assumed_hfov_deg);

// 同上再除以两帧图像的呈现时间差 dt（秒），得到截屏角速度（°/s）。
// dt ≤ 0、dt > kMaxFrameIntervalS（0.25 s）、非有限值或任一尺寸 ≤ 0 → "无效"（nullopt）：后一帧只作参考帧，不产生速度。
inline constexpr double kMaxFrameIntervalS = 0.25;
std::optional<double> shift_to_deg_per_s(double dx_cells, double dy_cells, int grid_w, int grid_h, int screen_w_px,
                                         int screen_h_px, double assumed_hfov_deg, double dt_s);

// SpeedHold 的换算参数：网格尺寸、被复制显示器（桌面方向）像素尺寸、假定 HFOV、保持时长。
struct SpeedHoldParams {
    int grid_w = 160;
    int grid_h = 90;
    int screen_w_px = 1920;
    int screen_h_px = 1080;
    double assumed_hfov_deg = 90.0;
    double hold_ms = 100.0;
};

// 发布的截屏角速度：保持与归零（openspec capture-motion-detection design D7）。时间 t 为绝对时间（秒，QPC 基准）。
//   on_estimate(est, dt, t)：est.valid（即 c ≥ min_correlation 且两轴有纹理）且 0 < dt ≤ 0.25 s
//                            → 记下 last_value = ω、last_valid_t = t；否则视同无更新。
//   on_no_update(t)        ：取帧超时、只有指针更新、重建中时调用（只为对称与可读性；不改变状态）。
//   value(t)               ：t − last_valid_t ≤ hold_ms ? last_value : 0。
// 归零是阶跃（没有指数尾巴）：可见的淡出由强度模型的 release 负责，避免两级平滑。
// 三个事件都返回 value(t)，调用方据此发布。非线程安全。
class SpeedHold {
public:
    // 更换换算参数与保持时长；不清除已有测量（运行中改配置不打断发布值）。非法值收拢到默认值。
    void configure(const SpeedHoldParams& p);
    const SpeedHoldParams& params() const { return params_; }

    // 一个新帧的估计。dt_s = 被比较的两帧图像的时间差（通常就是 est.dt_s），t_s = 这一帧的测量（呈现）时刻。
    double on_estimate(const MotionEstimate& est, double dt_s, double t_s);
    // 已经换算好的测量值（变更 5 的前进 / 倾斜流速用）：有值 → 记为有效测量；nullopt → 视同低置信度。
    double on_value(std::optional<double> v, double t_s);
    // 没有新帧可比较。
    double on_no_update(double t_s) const { return value(t_s); }
    // t_s 时刻应发布的值。
    double value(double t_s) const;

    // 回到初始状态（无有效测量，发布 0）。
    void reset();

    bool has_measurement() const { return has_valid_; }
    double last_measurement() const { return has_valid_ ? last_value_ : 0.0; }   // 最近一次有效测量（°/s）
    double last_valid_t() const { return last_valid_t_; }

private:
    SpeedHoldParams params_;
    double last_value_ = 0;
    double last_valid_t_ = 0;
    bool has_valid_ = false;
};

}  // namespace mc
