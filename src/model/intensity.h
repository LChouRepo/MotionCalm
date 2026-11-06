// intensity.h — 纯逻辑强度模型（不含 Win32）：阈值 + smoothstep、门控后取最大值、非对称一阶平滑；以及速度窗口与摇杆死区。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc {

enum class MaskLevel { Light = 0, Medium = 1, Strong = 2 };
enum class Sensitivity { Low = 0, Medium = 1, High = 2 };

// 颜色 (R, G, B)，INI 中写作 RRGGBB。
struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

// 一档灵敏度的角速度阈值（°/s）：v_start 开始出现，v_full 满强度。
struct Thresholds {
    double v_start = 0;
    double v_full = 0;
    bool operator==(const Thresholds&) const = default;
};

// [mask] 节 + [general] mask_level。数组按 MaskLevel 下标（Light, Medium, Strong）。
struct MaskConfig {
    MaskLevel level = MaskLevel::Medium;                      // [general] mask_level
    Rgb color{0x00, 0x00, 0x00};                              // color
    std::array<double, 3> max_opacity{0.60, 0.75, 0.85};      // max_opacity_light/medium/strong
    std::array<double, 3> clear_height{0.65, 0.60, 0.55};     // clear_height_*：满强度清晰区直径 / 屏幕高度
    double ellipse_ratio = 1.25;                              // 横向半径 / 纵向半径
    double feather = 0.35;
    int attack_ms = 100;
    int release_ms = 500;
    bool operator==(const MaskConfig&) const = default;
};

// [trigger] 节（除 mouse_lock_rule，它在 Config::mouse_lock_rule）+ [general] sensitivity。
// thresholds 按 Sensitivity 下标（Low, Medium, High）。
struct TriggerConfig {
    Sensitivity sensitivity = Sensitivity::Medium;            // [general] sensitivity
    bool mouse = true;
    bool keyboard = true;
    bool gamepad = true;
    bool capture = true;                                      // 变更 3：截屏运动检测开关
    Sensitivity capture_sensitivity = Sensitivity::Medium;    // 变更 3：截屏项阈值的保守系数（见 capture_threshold_scale）
    double default_counts_per_degree = 18.2;
    std::array<Thresholds, 3> thresholds{{{120, 600}, {60, 360}, {30, 200}}};   // v_start_* / v_full_*
    double move_intensity = 0.35;
    double gamepad_max_deg_per_s = 300;
    double gamepad_deadzone = 0.20;
    int velocity_window_ms = 50;
    double forward_gain = 1.0;                                // 变更 5：INI 在 [capture] forward_gain，前进流速的倍数 [0.5, 3]
    // 变更 5：前进 / 后退与倾斜各自的阈值（INI 在 [capture] zoom_start_* / zoom_full_* / roll_start_* / roll_full_*），
    // 按 sensitivity 选档，不乘 capture_sensitivity 的保守系数。三档比例与鼠标阈值相同：
    // 低 = 中 × 2（开始）/ × 5/3（满），高 = 中 × 0.5 / × 5/9，取整。
    std::array<Thresholds, 3> zoom_thresholds{{{40, 250}, {20, 150}, {10, 83}}};
    std::array<Thresholds, 3> roll_thresholds{{{30, 150}, {15, 90}, {8, 50}}};
    bool operator==(const TriggerConfig&) const = default;
};

// 一个 tick 的模型输入。gate_* 由调用方按 detect/game_detect.h 的 compute_gates 填写。
struct IntensityInputs {
    double mouse_deg_per_s = 0;     // 已经除以 counts_per_degree
    bool moving = false;            // WASD / 方向键任一按下
    double pad_deg_per_s = 0;       // 右摇杆映射后幅值 × gamepad_max_deg_per_s
    double pad_move = 0;            // 左摇杆映射后幅值 0..1
    double capture_deg_per_s = 0;   // 变更 3：截屏运动检测测得的画面角速度
    double capture_zoom_deg_per_s = 0;   // 变更 5：画面扩张 / 收缩换算的水平边缘径向流速（未乘 forward_gain）
    double capture_roll_deg_per_s = 0;   // 变更 5：画面旋转（倾斜）角速度
    bool gate_mouse = false, gate_keys = false, gate_pad = false, gate_capture = false;
};

// 各分项（门控之后），调试用。
struct IntensityTerms {
    double mouse = 0, keys = 0, pad = 0, pad_move = 0, capture = 0;
    double capture_zoom = 0, capture_roll = 0;   // 变更 5
};

// smoothstep(e0, e1, x) = t²(3 − 2t)，t = clamp((x − e0)/(e1 − e0), 0, 1)；e1 ≤ e0 时退化为阶跃 x ≥ e1。
double smoothstep(double e0, double e1, double x);

// 取当前灵敏度对应的阈值对。
Thresholds thresholds_for(Sensitivity s, const TriggerConfig& trigger);
// 从任一张三档阈值表（鼠标 / 前进 / 倾斜）取 s 那一档。
Thresholds thresholds_for(Sensitivity s, const std::array<Thresholds, 3>& table);

// 截屏项的保守系数 k（motion-trigger「截屏角速度项按截屏灵敏度缩放阈值」）：low 2.0、medium 1.5、high 1.0。
// 截屏项 = smoothstep(k·v_start, k·v_full, capture_deg_per_s)，v_start / v_full 取 [general] sensitivity 那一档。
// 变更 5 的前进项（forward_gain × capture_zoom_deg_per_s）与倾斜项（capture_roll_deg_per_s）不用它：
// 它们有自己的阈值表（TriggerConfig::zoom_thresholds / roll_thresholds），同样按 [general] sensitivity 选档。
double capture_threshold_scale(Sensitivity capture_sensitivity);

// 输入 → 目标强度（门控后取最大）→ 非对称一阶平滑后的强度。
class IntensityModel {
public:
    // 设定阈值、灵敏度、move_intensity、attack/release；不改变当前强度。
    void configure(const TriggerConfig& trigger, const MaskConfig& mask);
    // 推进 dt 秒（截断到 [0, 0.1]），返回平滑后的强度 0..1。|target − value| < 0.001 时直接吸附到 target。
    float update(const IntensityInputs& in, double dt_seconds);
    // 平滑后的强度。
    float value() const { return static_cast<float>(value_); }
    // 平滑前的目标强度（调试条用）。
    float target() const { return static_cast<float>(target_); }
    // 最近一次 update 的各分项（门控之后）。
    const IntensityTerms& terms() const { return terms_; }
    // 把 value 与 target 都置为 v（夹到 0..1），例如重新启用遮罩时清零。
    void reset(double v = 0.0);
    // 只计算目标强度（不改变状态）。
    double compute_target(const IntensityInputs& in, IntensityTerms* terms = nullptr) const;

    static constexpr double kMaxDt = 0.1;      // dt 上限（秒）
    static constexpr double kSnap = 0.001;     // 吸附阈值

private:
    TriggerConfig trigger_{};
    double capture_k_ = 1.5;   // capture_threshold_scale(trigger_.capture_sensitivity)，configure() 时算好
    double attack_s_ = 0.100;
    double release_s_ = 0.500;
    double value_ = 0;
    double target_ = 0;
    IntensityTerms terms_{};
};

// 带时间戳的相对位移窗口（时间单位：秒，来自单调时钟，例如 QPC / 频率）。
// 每条记录是一段位移：点事件（t 时刻发生）或区间事件（在 (t0, t1] 内均匀发生，例如一次批量读取
// 覆盖的那段时间）。求和时区间事件按与窗口重叠的比例计入，所以批量读取的节拍（10 ms、16.7 ms、30 ms、
// 不规则）不会让窗口多算或少算位移。容量上限 kCapacity，超出时丢最旧的。
class VelocityWindow {
public:
    static constexpr std::size_t kCapacity = 8192;

    explicit VelocityWindow(double window_s = 0.050) : window_s_(window_s) {}
    // 修改窗口长度（秒，≤0 时按 0.001）。
    void set_window(double window_s);
    double window() const { return window_s_; }
    // 数据地平线保持的上限（秒，默认 0 = 不保持）。见 counts_per_s。
    void set_max_hold(double hold_s) { max_hold_s_ = hold_s > 0.0 ? hold_s : 0.0; }
    // 追加一次点事件（t_s 应单调不减）；顺带淘汰已不可能落进窗口的事件。
    void add(double t_s, double dx, double dy) { add_span(t_s, t_s, dx, dy, -1.0, 1); }
    // 追加一段在 (t0_s, t1_s] 内均匀发生的位移（t1_s 应单调不减；t0_s ≥ t1_s 时按点事件）。
    // path = 这段的路径长度 Σ√(dx²+dy²)（< 0 时取 |(dx, dy)|）；events = 它包含的原始事件数。
    void add_span(double t0_s, double t1_s, double dx, double dy, double path = -1.0, std::uint32_t events = 1);
    // |Σ(dx, dy)| / window，求和范围是 (end − window, end]，区间事件按重叠比例计入。
    // end 通常等于 now；设置了 max_hold 时，如果最新一段的结束时刻 t1 距 now 不超过
    // min(max_hold, 该段长度)（下一批位移还没到期），就用 end = t1：输入被系统合并成 ~8 ms 一包
    // 上报时，窗口末尾那段"已经发生、还没上报"的时间不算作静止。
    double counts_per_s(double now_s) const;
    // Σ √(dx² + dy²)，范围 (now − window, now]，同样按重叠比例（路径长度，单位：计数；不做地平线保持）。
    double path_length(double now_s) const;
    // (now − window, now] 内结束的记录所含的原始事件数。
    std::uint32_t event_count(double now_s) const;
    // 清空全部事件。
    void clear();
    // 当前保存的记录数（含可能已过期但尚未淘汰的）。
    std::size_t size() const { return count_; }

private:
    struct Event {
        double t0, t1, dx, dy, path;
        std::uint32_t events;
    };
    // 记录 e 落在 (lo, hi] 内的比例（点事件为 0 或 1）。
    static double overlap(const Event& e, double lo, double hi);
    template <class F>
    void for_each_overlapping(double lo, double hi, F&& f) const;

    double window_s_;
    double max_hold_s_ = 0.0;
    std::vector<Event> ring_;   // 首次 add 时分配 kCapacity
    std::size_t head_ = 0;      // 最旧事件的下标
    std::size_t count_ = 0;
};

// 批量读取的时间归属（纯逻辑，design D3）：一次读取拿到的相对位移视为在 (start, now] 内均匀发生。
// - 连续移动（上一批位移距今 ≤ kContinuousGapS）：start = 上一批位移的读取时刻。中间没读到位移的读取
//   （例如 tick 时的空读取）不切断它——被系统合并上报的一包位移覆盖的是"上一包到这一包"的整段时间。
// - 停顿之后的第一批：start = 上一次读取（不论有没有位移）——位移只可能在那之后到达。
// - 最长铺开 kMaxSpanS（模态循环、调试器暂停之后）。
class BatchTimeline {
public:
    static constexpr double kContinuousGapS = 0.040;
    static constexpr double kMaxSpanS = 0.250;

    // 起点（例如注册原始输入的时刻）；之前的位移都不再关心。
    void reset(double now_s);
    // 记一次读取；返回本批位移的起点 start（≤ now_s）。has_motion = false 时返回值无意义。
    double on_read(double now_s, bool has_motion);

private:
    double last_read_ = 0.0;
    double last_motion_ = -1e300;
};

// 径向死区：m = min(1, √(x²+y²) / 32767)；m < dz → 0，否则 (m − dz)/(1 − dz)，结果 ∈ [0, 1]。
// x、y 为 XInput 原始摇杆值（−32768..32767）。
double apply_radial_deadzone(double x, double y, double dz);

}  // namespace mc
