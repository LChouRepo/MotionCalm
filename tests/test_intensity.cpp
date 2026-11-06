// test_intensity.cpp — model/intensity 的单元测试：motion-trigger spec 中的阈值、移动、门控、max 合成、非对称平滑、速度窗口与死区数值场景。
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "model/intensity.h"

using namespace mc;

namespace {

IntensityModel default_model() {
    IntensityModel m;
    m.configure(TriggerConfig{}, MaskConfig{});
    return m;
}

IntensityInputs mouse_input(double deg_per_s) {
    IntensityInputs in;
    in.mouse_deg_per_s = deg_per_s;
    in.gate_mouse = true;
    return in;
}

// 以固定 dt 推进，最后一步截短，使累计时间恰好为 total。
float run_for(IntensityModel& m, const IntensityInputs& in, double total, double dt) {
    double t = 0;
    float v = m.value();
    while (t < total - 1e-12) {
        const double step = std::min(dt, total - t);
        v = m.update(in, step);
        t += step;
    }
    return v;
}

// 输入强度恒为 1 的输入（鼠标远超 v_full）。
IntensityInputs full_input() { return mouse_input(1000); }

}  // namespace

// ================================================================ 阈值

TEST(intensity_threshold_medium) {
    const Thresholds th = thresholds_for(Sensitivity::Medium, TriggerConfig{});
    CHECK_NEAR(th.v_start, 60, 0);
    CHECK_NEAR(th.v_full, 360, 0);
    CHECK_NEAR(smoothstep(th.v_start, th.v_full, 30), 0.0, 1e-12);
    CHECK_NEAR(smoothstep(th.v_start, th.v_full, 60), 0.0, 1e-12);
    CHECK_NEAR(smoothstep(th.v_start, th.v_full, 210), 0.5, 1e-12);
    CHECK_NEAR(smoothstep(th.v_start, th.v_full, 360), 1.0, 1e-12);
    CHECK_NEAR(smoothstep(th.v_start, th.v_full, 1000), 1.0, 1e-12);
    // 经过模型：鼠标项。
    const IntensityModel m = default_model();
    CHECK_NEAR(m.compute_target(mouse_input(30)), 0.0, 1e-12);
    CHECK_NEAR(m.compute_target(mouse_input(210)), 0.5, 1e-12);
    CHECK_NEAR(m.compute_target(mouse_input(1000)), 1.0, 1e-12);
}

TEST(intensity_threshold_low_high) {
    TriggerConfig trig;
    CHECK_NEAR(smoothstep(thresholds_for(Sensitivity::Low, trig).v_start, thresholds_for(Sensitivity::Low, trig).v_full, 360),
               0.5, 1e-12);
    const Thresholds high = thresholds_for(Sensitivity::High, trig);
    CHECK_NEAR(smoothstep(high.v_start, high.v_full, 115), 0.5, 1e-12);
    CHECK_NEAR(smoothstep(high.v_start, high.v_full, 200), 1.0, 1e-12);

    IntensityModel m;
    trig.sensitivity = Sensitivity::Low;
    m.configure(trig, MaskConfig{});
    CHECK_NEAR(m.compute_target(mouse_input(360)), 0.5, 1e-12);
    trig.sensitivity = Sensitivity::High;
    m.configure(trig, MaskConfig{});
    CHECK_NEAR(m.compute_target(mouse_input(115)), 0.5, 1e-12);
    CHECK_NEAR(m.compute_target(mouse_input(200)), 1.0, 1e-12);
    // 退化阈值不除零。
    CHECK_NEAR(smoothstep(100, 100, 99), 0.0, 0);
    CHECK_NEAR(smoothstep(100, 100, 100), 1.0, 0);
}

TEST(intensity_pad_full_medium) {
    const TriggerConfig trig;
    const double pad = apply_radial_deadzone(32767, 0, trig.gamepad_deadzone) * trig.gamepad_max_deg_per_s;
    CHECK_NEAR(pad, 300, 1e-9);
    IntensityInputs in;
    in.pad_deg_per_s = pad;
    in.gate_pad = true;
    const IntensityModel m = default_model();
    CHECK_NEAR(m.compute_target(in), 0.896, 0.001);
    CHECK_NEAR(smoothstep(60, 360, 300), 0.896, 0.001);
}

// ================================================================ 移动、门控、合成

TEST(intensity_keys_move) {
    IntensityInputs in;
    in.moving = true;
    in.gate_keys = true;
    const IntensityModel m = default_model();
    CHECK_NEAR(m.compute_target(in), 0.35, 1e-12);
    in.moving = false;
    CHECK_NEAR(m.compute_target(in), 0.0, 0);
}

TEST(intensity_pad_move_half) {
    const double left = apply_radial_deadzone(0.6 * 32767, 0, 0.20);
    CHECK_NEAR(left, 0.5, 1e-9);
    IntensityInputs in;
    in.pad_move = left;
    in.gate_pad = true;
    const IntensityModel m = default_model();
    CHECK_NEAR(m.compute_target(in), 0.175, 1e-9);
    in.pad_move = 1.0;   // 左摇杆推满
    CHECK_NEAR(m.compute_target(in), 0.35, 1e-12);
}

TEST(intensity_max_combine) {
    const IntensityModel m = default_model();
    IntensityInputs a = mouse_input(210);   // t_mouse = 0.5
    a.moving = true;
    a.gate_keys = true;                      // t_keys = 0.35
    IntensityTerms terms;
    CHECK_NEAR(m.compute_target(a, &terms), 0.5, 1e-12);
    CHECK_NEAR(terms.mouse, 0.5, 1e-12);
    CHECK_NEAR(terms.keys, 0.35, 1e-12);

    IntensityInputs b;
    b.pad_deg_per_s = 300;   // t_pad ≈ 0.896
    b.gate_pad = true;
    b.moving = true;
    b.gate_keys = true;
    CHECK_NEAR(m.compute_target(b), 0.896, 0.001);
}

TEST(intensity_gate_false_is_zero) {
    const IntensityModel m = default_model();
    IntensityInputs in;
    in.mouse_deg_per_s = 1000;
    in.moving = true;
    in.pad_deg_per_s = 300;
    in.pad_move = 1;
    in.capture_deg_per_s = 1000;
    CHECK_NEAR(m.compute_target(in), 0.0, 0);   // 全部门控为 false

    IntensityInputs only_keys = in;
    only_keys.gate_keys = true;
    CHECK_NEAR(m.compute_target(only_keys), 0.35, 1e-12);   // 鼠标、手柄项都按 0 计

    IntensityInputs no_mouse = in;
    no_mouse.gate_keys = true;
    no_mouse.gate_pad = true;
    IntensityTerms t;
    m.compute_target(no_mouse, &t);
    CHECK_NEAR(t.mouse, 0.0, 0);
    CHECK_NEAR(t.capture, 0.0, 0);
    CHECK_NEAR(t.pad, 0.896, 0.001);
    CHECK_NEAR(t.pad_move, 0.35, 1e-12);

    IntensityModel model = default_model();
    for (int k = 0; k < 60; ++k) model.update(in, 1.0 / 60);
    CHECK_NEAR(model.value(), 0.0, 0);
    CHECK_NEAR(model.target(), 0.0, 0);
}

// ================================================================ 截屏项（变更 3）

namespace {

IntensityInputs capture_input(double deg_per_s) {
    IntensityInputs in;
    in.capture_deg_per_s = deg_per_s;
    in.gate_capture = true;
    return in;
}

IntensityModel capture_model(Sensitivity sensitivity, Sensitivity capture_sensitivity) {
    TriggerConfig trig;
    trig.sensitivity = sensitivity;
    trig.capture_sensitivity = capture_sensitivity;
    IntensityModel m;
    m.configure(trig, MaskConfig{});
    return m;
}

double capture_term(const IntensityModel& m, double deg_per_s) {
    IntensityTerms t;
    m.compute_target(capture_input(deg_per_s), &t);
    return t.capture;
}

}  // namespace

TEST(intensity_capture_scale_factors) {
    CHECK_NEAR(capture_threshold_scale(Sensitivity::Low), 2.0, 0);
    CHECK_NEAR(capture_threshold_scale(Sensitivity::Medium), 1.5, 0);
    CHECK_NEAR(capture_threshold_scale(Sensitivity::High), 1.0, 0);
    // 默认 TriggerConfig：capture=1、capture_sensitivity=medium
    CHECK_TRUE(TriggerConfig{}.capture);
    CHECK_TRUE(TriggerConfig{}.capture_sensitivity == Sensitivity::Medium);
}

TEST(intensity_capture_default_thresholds) {
    // sensitivity=medium（60 / 360）、capture_sensitivity=medium（k = 1.5）→ 90 / 540
    const IntensityModel m = capture_model(Sensitivity::Medium, Sensitivity::Medium);
    CHECK_NEAR(capture_term(m, 90), 0.0, 0.001);
    CHECK_NEAR(capture_term(m, 315), 0.5, 0.001);
    CHECK_NEAR(capture_term(m, 540), 1.0, 0.001);
    CHECK_NEAR(capture_term(m, 89), 0.0, 0);
    CHECK_NEAR(m.compute_target(capture_input(540)), 1.0, 0.001);
}

TEST(intensity_capture_sensitivity_high_low) {
    const IntensityModel high = capture_model(Sensitivity::Medium, Sensitivity::High);   // k = 1.0 → 60 / 360
    CHECK_NEAR(capture_term(high, 360), 1.0, 0.001);
    CHECK_NEAR(capture_term(high, 210), 0.5, 0.001);
    const IntensityModel low = capture_model(Sensitivity::Medium, Sensitivity::Low);     // k = 2.0 → 120 / 720
    CHECK_NEAR(capture_term(low, 120), 0.0, 0);
    CHECK_NEAR(capture_term(low, 420), 0.5, 0.001);
    // 基准阈值取 [general] sensitivity：high（30 / 200）× 1.5 → 45 / 300
    const IntensityModel base_high = capture_model(Sensitivity::High, Sensitivity::Medium);
    CHECK_NEAR(capture_term(base_high, 45), 0.0, 0);
    CHECK_NEAR(capture_term(base_high, 172.5), 0.5, 0.001);
    CHECK_NEAR(capture_term(base_high, 300), 1.0, 0.001);
}

TEST(intensity_capture_more_conservative_than_mouse) {
    const IntensityModel m = capture_model(Sensitivity::Medium, Sensitivity::Medium);
    IntensityInputs in = capture_input(200);
    in.mouse_deg_per_s = 200;
    in.gate_mouse = true;
    IntensityTerms t;
    m.compute_target(in, &t);
    CHECK(t.capture < t.mouse);
    CHECK(t.capture > 0.0);
}

TEST(intensity_capture_gate_and_max) {
    const IntensityModel m = capture_model(Sensitivity::Medium, Sensitivity::Medium);
    IntensityInputs off = capture_input(1000);
    off.gate_capture = false;
    CHECK_NEAR(m.compute_target(off), 0.0, 0);   // 门控为假 → 0

    // t_mouse ≈ 0.3、t_keys = 0.35、t_capture ≈ 0.8 → target 取截屏项
    IntensityInputs in;
    in.gate_mouse = true;
    in.mouse_deg_per_s = 169;                  // t = 109 / 300 ≈ 0.363 → smoothstep ≈ 0.300
    in.gate_keys = true;
    in.moving = true;
    in.gate_capture = true;
    in.capture_deg_per_s = 90 + 0.7 * 450;     // t = 0.7 → 0.784
    IntensityTerms t;
    const double target = m.compute_target(in, &t);
    CHECK_NEAR(t.keys, 0.35, 1e-12);
    CHECK(t.mouse > 0.29 && t.mouse < 0.31);
    CHECK_NEAR(t.capture, 0.784, 0.001);
    CHECK_NEAR(target, t.capture, 0);
}

TEST(intensity_capture_600_rises_within_300ms) {
    // 鼠标、键盘无输入，画面以 600 °/s 平移 300 ms（默认灵敏度）→ 平滑强度 > 0.9
    IntensityModel m = capture_model(Sensitivity::Medium, Sensitivity::Medium);
    const float v = run_for(m, capture_input(600), 0.300, 1.0 / 60);
    CHECK(v > 0.9f);
}

// ================================================================ 平滑

TEST(intensity_attack_100ms) {
    IntensityModel m = default_model();
    const float v50 = run_for(m, full_input(), 0.050, 0.001);
    CHECK(v50 >= 0.77f && v50 <= 0.78f);
    const float v100 = run_for(m, full_input(), 0.050, 0.001);
    CHECK(v100 >= 0.945f && v100 <= 0.955f);
    CHECK_NEAR(v100, 1.0 - std::exp(-3.0), 1e-5);
    CHECK_NEAR(m.target(), 1.0, 0);
}

TEST(intensity_release_500ms) {
    IntensityModel m = default_model();
    m.reset(1.0);
    const IntensityInputs idle;
    const float v500 = run_for(m, idle, 0.500, 1.0 / 60);
    CHECK(v500 >= 0.045f && v500 <= 0.055f);
    CHECK_NEAR(v500, std::exp(-3.0), 1e-5);
    const float v1200 = run_for(m, idle, 0.700, 1.0 / 60);
    CHECK_EQ(v1200, 0.0f);   // 1.2 s 时恰好为 0
}

TEST(intensity_snaps_to_zero) {
    IntensityModel m = default_model();
    m.reset(1.0);
    const IntensityInputs idle;
    double t = 0;
    double first_zero = -1;
    while (t < 1.2 - 1e-9) {
        m.update(idle, 0.004);
        t += 0.004;
        if (first_zero < 0 && m.value() == 0.0f) first_zero = t;
    }
    CHECK(first_zero > 1.1 && first_zero < 1.2);   // 理论值：e^(−t/τ) < 0.001 时，t ≈ 1.151 s
    CHECK_EQ(m.value(), 0.0f);
    // 上升方向同样吸附到 target。
    IntensityModel up = default_model();
    for (int k = 0; k < 100; ++k) up.update(full_input(), 0.01);
    CHECK_EQ(up.value(), 1.0f);
}

TEST(intensity_dt_independent) {
    const double dts[] = {1.0 / 60, 1.0 / 144, 0.004};
    float results[3] = {};
    for (int k = 0; k < 3; ++k) {
        IntensityModel m = default_model();
        results[k] = run_for(m, full_input(), 0.100, dts[k]);
    }
    CHECK_NEAR(results[0], results[1], 0.005);
    CHECK_NEAR(results[1], results[2], 0.005);
    CHECK_NEAR(results[0], results[2], 0.005);
}

TEST(intensity_reverse_uses_release) {
    TriggerConfig trig;
    trig.move_intensity = 0.2;   // 让 target = 0.2
    IntensityModel m;
    m.configure(trig, MaskConfig{});
    m.reset(0.6);
    IntensityInputs in;
    in.moving = true;
    in.gate_keys = true;
    const float v = m.update(in, 0.010);
    CHECK_NEAR(m.target(), 0.2, 1e-6);
    const double tau_release = 0.5 / 3.0;   // ≈ 166.7 ms
    CHECK_NEAR(v, 0.2 + 0.4 * std::exp(-0.010 / tau_release), 1e-6);
    const double attack_like = 0.2 + 0.4 * std::exp(-0.010 / (0.1 / 3.0));
    CHECK(std::fabs(v - attack_like) > 0.05);
}

TEST(intensity_dt_clamped) {
    IntensityModel a = default_model();
    IntensityModel b = default_model();
    const float big = a.update(full_input(), 5.0);   // 截断到 0.1 s
    const float ref = b.update(full_input(), 0.1);
    CHECK_EQ(big, ref);
    CHECK_NEAR(big, 1.0 - std::exp(-3.0), 1e-5);

    IntensityModel c = default_model();
    CHECK_EQ(c.update(full_input(), -1.0), 0.0f);   // 负 dt 按 0
    CHECK_EQ(c.update(full_input(), std::numeric_limits<double>::quiet_NaN()), 0.0f);
    CHECK_NEAR(c.target(), 1.0, 0);
}

// ================================================================ 速度窗口

TEST(intensity_velocity_8000) {
    VelocityWindow w(0.050);
    for (int k = 0; k <= 1000; ++k) w.add(k * 0.001, 8, 0);   // 每 1 ms dx=+8
    // 查询时刻落在两个事件之间（真实 tick 的情形）：窗口 (now − 50 ms, now] 里恰好 50 个事件。
    for (const double now : {1.0002, 1.0005, 1.0009}) CHECK_NEAR(w.counts_per_s(now), 8000, 160);
    const double between = w.counts_per_s(1.0005);
    const double deg = between / TriggerConfig{}.default_counts_per_degree;
    CHECK_NEAR(deg, 439.6, 439.6 * 0.02);
    CHECK_NEAR(8000 / 18.2, 439.56, 0.01);
}

TEST(intensity_velocity_cancel) {
    VelocityWindow w(0.050);
    w.add(0.010, +100, 0);
    w.add(0.030, -100, 0);
    CHECK_NEAR(w.counts_per_s(0.040), 0.0, 1e-12);
    CHECK_NEAR(w.path_length(0.040), 200.0, 1e-12);
}

TEST(intensity_velocity_diagonal) {
    VelocityWindow w(0.050);
    w.add(0.010, 10, 20);
    w.add(0.030, 20, 20);   // 累计 (30, 40)
    CHECK_NEAR(w.counts_per_s(0.040), 1000.0, 1e-9);
}

TEST(intensity_velocity_zero_after_window) {
    VelocityWindow w(0.050);
    for (int k = 0; k <= 200; ++k) w.add(k * 0.001, 8, 0);
    const double t_last = 200 * 0.001;
    CHECK(w.counts_per_s(t_last + 0.049) > 0.0);
    CHECK_NEAR(w.counts_per_s(t_last + 0.050 + 0.001), 0.0, 0);   // 停止后 50 ms 加一个 tick 之内归零
    CHECK_NEAR(w.counts_per_s(t_last + 0.100), 0.0, 0);
    w.clear();
    CHECK_EQ(w.size(), 0);
}

TEST(intensity_velocity_path_and_capacity) {
    VelocityWindow path(0.100);
    for (int k = 0; k < 20; ++k) path.add(1.0 + k * 0.004, (k % 2) ? 1 : -1, 0);   // 来回抖动：位移抵消，路径累加
    CHECK_NEAR(path.path_length(1.080), 20.0, 1e-9);
    CHECK_NEAR(path.counts_per_s(1.080), 0.0, 1e-9);

    VelocityWindow w(0.050);
    for (int k = 0; k < 10000; ++k) w.add(5.0, 1, 0);   // 同一时刻 10000 个事件：超出上限时丢最旧的
    CHECK_EQ(w.size(), VelocityWindow::kCapacity);
    CHECK_NEAR(w.counts_per_s(5.0), VelocityWindow::kCapacity / 0.050, 1e-6);
}

// ================================================================ 批量读取（BatchTimeline + 区间事件）

namespace {

// 与 RawInput::drain / add_batch 相同的组装：一次读取 → BatchTimeline 给出时间段 → 两个窗口。
struct BatchSim {
    BatchTimeline timeline;
    VelocityWindow velocity{0.050};
    VelocityWindow path{0.100};
    explicit BatchSim(double t0, double max_hold = 0.025) {
        timeline.reset(t0);
        velocity.set_max_hold(max_hold);
    }
    void read(double now, double dx, std::uint32_t n) {
        const double start = timeline.on_read(now, n > 0);
        if (n == 0) return;
        velocity.add_span(start, now, dx, 0, std::fabs(dx), n);
        path.add_span(start, now, dx, 0, std::fabs(dx), n);
    }
};

}  // namespace

TEST(intensity_velocity_irregular_batches_constant_rate) {
    // 恒定 8000 counts/s；读取间隔按 10, 14, 16.7, 30 ms 循环（tick / 空闲轮询 / 卡顿混在一起），
    // 每批的计数 = 速率 × 距上次读取的时间。稳态下每次读取后的速度都应在 ±2% 内（区间按重叠比例积分后其实是精确的）。
    const double rate = 8000.0;
    const double gaps[] = {0.010, 0.014, 0.0167, 0.030};
    BatchSim sim(0.0);
    double t = 0.0;
    int checked = 0;
    double worst = 0.0;
    for (int k = 0; k < 200; ++k) {
        const double gap = gaps[k % 4];
        t += gap;
        sim.read(t, rate * gap, 3);
        if (t < 0.2) continue;   // 窗口填满之后才算稳态
        for (const double probe : {0.0, 0.003}) {   // 读取时刻，以及读取后 3 ms（下一批还没到期，地平线保持）
            const double v = sim.velocity.counts_per_s(t + probe);
            worst = std::max(worst, std::fabs(v / rate - 1.0));
            ++checked;
        }
    }
    CHECK_TRUE(checked > 300);
    CHECK_TRUE(worst < 0.02);
    CHECK_NEAR(sim.velocity.counts_per_s(t) / 18.2, 439.6, 439.6 * 0.02);
}

TEST(intensity_velocity_batch_after_idle_tick_spread_over_30ms) {
    BatchSim sim(1.0, 0.0);   // 不做地平线保持，单看铺开的区间
    // 两次空闲轮询（30 ms 一次，没有位移），然后一批 300 counts 到达。
    sim.read(1.030, 0, 0);
    sim.read(1.060, 0, 0);
    BatchTimeline probe_tl;
    probe_tl.reset(1.030);
    CHECK_NEAR(probe_tl.on_read(1.060, true), 1.030, 1e-12);   // 起点 = 上一次（空）读取
    sim.read(1.090, 300, 1);
    // 这批铺在 (1.060, 1.090] 上：整段都在窗口里时全额计入……
    CHECK_NEAR(sim.velocity.counts_per_s(1.090), 300 / 0.050, 1e-6);
    // ……窗口 (1.050, 1.100] 也全覆盖；窗口 (1.070, 1.120] 只覆盖最后 20 ms = 2/3；(1.080, 1.130] 只覆盖 1/3。
    CHECK_NEAR(sim.velocity.counts_per_s(1.100), 300 / 0.050, 1e-6);
    CHECK_NEAR(sim.velocity.counts_per_s(1.120), 200 / 0.050, 1e-6);
    CHECK_NEAR(sim.velocity.counts_per_s(1.130), 100 / 0.050, 1e-6);
    CHECK_NEAR(sim.velocity.counts_per_s(1.140), 0.0, 0);
    CHECK_NEAR(sim.path.path_length(1.140), 300.0, 1e-6);   // 路径窗口 (1.040, 1.140] 覆盖全段
    CHECK_EQ(sim.path.event_count(1.140), 1u);

    // 连续移动：上一批位移之后只有一次空读取（tick），新的一批从上一批位移处开始，不被空读取切断。
    BatchTimeline tl;
    tl.reset(0.0);
    CHECK_NEAR(tl.on_read(0.008, true), 0.0, 1e-12);
    tl.on_read(0.012, false);   // 空读取（tick）
    CHECK_NEAR(tl.on_read(0.016, true), 0.008, 1e-12);
    // 停顿超过 40 ms：从上一次读取开始（位移只可能在那之后到达）。
    tl.on_read(0.050, false);
    CHECK_NEAR(tl.on_read(0.070, true), 0.050, 1e-12);
    // 模态循环之后：最多往回铺 250 ms。
    CHECK_NEAR(tl.on_read(2.0, true), 2.0 - BatchTimeline::kMaxSpanS, 1e-12);
}

TEST(intensity_velocity_coalesced_125hz_with_event_wakes) {
    // 现场情形：Windows 把后台收到的鼠标输入合并成 8 ms 一包（8000 counts/s → 每包 64 counts），到达时刻有
    // 0–0.4 ms 的抖动；主循环为每包醒来读取一次，另外 60 Hz 的 tick 也会读取（多数时候是空的），速度在 tick 时计算。
    const double rate = 8000.0;
    BatchSim sim(0.0);
    double worst = 0.0;
    int checked = 0;
    int chunk = 0;
    double next_chunk = 0.008 + 0.0001;
    double next_tick = 0.0031;
    double last_arrival = 0.0;
    while (next_tick < 3.0) {
        if (next_chunk + 0.00005 <= next_tick) {
            sim.read(next_chunk + 0.00005, rate * 0.008, 1);   // 醒来读取（比到达晚 50 µs）
            last_arrival = next_chunk;
            ++chunk;
            next_chunk = 0.008 * (chunk + 1) + 0.0004 * ((chunk * 7) % 5) / 4.0;   // 0–0.4 ms 抖动
        } else {
            sim.read(next_tick, 0, 0);   // tick 时的读取：这次没有新包
            if (next_tick > 0.2) {
                const double v = sim.velocity.counts_per_s(next_tick);
                worst = std::max(worst, std::fabs(v / rate - 1.0));
                ++checked;
            }
            next_tick += 1.0 / 60.0;
        }
    }
    CHECK_TRUE(checked > 150);
    CHECK_TRUE(worst < 0.02);
    // 停止：最后一包之后 50 ms 加一个 tick 之内归零（地平线保持不会拖住它）。
    CHECK_TRUE(sim.velocity.counts_per_s(last_arrival + 0.010) > 0.0);
    CHECK_NEAR(sim.velocity.counts_per_s(last_arrival + 0.050 + 1.0 / 60.0), 0.0, 0);
}

TEST(intensity_velocity_hold_does_not_extend_points) {
    // 点事件（add）没有区间长度，地平线保持对它不起作用：与原来的语义完全一致。
    VelocityWindow w(0.050);
    w.set_max_hold(0.025);
    for (int k = 0; k <= 200; ++k) w.add(k * 0.001, 8, 0);
    const double t_last = 0.200;
    CHECK_NEAR(w.counts_per_s(t_last + 0.0005), 8000, 160);
    CHECK_NEAR(w.counts_per_s(t_last + 0.050 + 0.001), 0.0, 0);
}

// ================================================================ 死区

TEST(intensity_deadzone_rescale) {
    const double dz = 0.20;
    CHECK_NEAR(apply_radial_deadzone(0.15 * 32767, 0, dz), 0.0, 0);
    CHECK_NEAR(apply_radial_deadzone(0, 0.6 * 32767, dz), 0.5, 1e-9);
    CHECK_NEAR(apply_radial_deadzone(32767, 0, dz), 1.0, 1e-12);
    CHECK_NEAR(apply_radial_deadzone(32767, 32767, dz), 1.0, 0);    // 角上幅值 > 1：截断为 1
    CHECK_NEAR(apply_radial_deadzone(-32768, 0, dz), 1.0, 0);
    CHECK_NEAR(apply_radial_deadzone(0, 0, dz), 0.0, 0);
    // 半推：(0.6 − 0.2) / 0.8 = 0.5 → 300 °/s × 0.5 = 150。
    CHECK_NEAR(apply_radial_deadzone(0.6 * 32767, 0, dz) * 300, 150, 1e-6);
}

// ================================================================ 变更 5：前进 / 倾斜项

namespace {
mc::IntensityInputs zoom_roll_input(double zoom, double roll, bool gate = true) {
    mc::IntensityInputs in;
    in.capture_zoom_deg_per_s = zoom;
    in.capture_roll_deg_per_s = roll;
    in.gate_capture = gate;
    return in;
}
}  // namespace

TEST(intensity_forward_term_mapping) {
    // 前进 / 后退有自己的阈值（中档 20 / 150 °/s），不乘截屏灵敏度的保守系数；forward_gain 默认 1.0。
    // s = 1.02 在 66 ms 内、HFOV 90° → ln 1.02 / 0.066 × 45 = 13.50 °/s < 20 → 0
    const double z102 = std::log(1.02) / 0.066 * 45.0;
    CHECK_NEAR(z102, 13.502, 0.001);
    mc::TriggerConfig tc;
    CHECK_NEAR(tc.forward_gain, 1.0, 0);
    mc::MaskConfig mc_;
    mc::IntensityModel m;
    m.configure(tc, mc_);
    mc::IntensityTerms t;
    CHECK_EQ(m.compute_target(zoom_roll_input(z102, 0), &t), 0.0);
    CHECK_EQ(t.capture_zoom, 0.0);
    // 日常游动：60 °/s → smoothstep(20, 150, 60) = 0.2258；90 °/s → 0.5576（> 0.5）；45 °/s → 0.0967
    CHECK_NEAR(m.compute_target(zoom_roll_input(60.0, 0), &t), 0.2258, 0.0001);
    CHECK_NEAR(t.capture_zoom, mc::smoothstep(20, 150, 60), 1e-12);
    CHECK_NEAR(m.compute_target(zoom_roll_input(90.0, 0)), 0.5576, 0.0001);
    CHECK_NEAR(m.compute_target(zoom_roll_input(45.0, 0)), 0.0967, 0.0001);
    // 中点 85 °/s → 0.5；20 → 0；150 → 1
    CHECK_NEAR(m.compute_target(zoom_roll_input(85.0, 0)), 0.5, 1e-9);
    CHECK_EQ(m.compute_target(zoom_roll_input(20.0, 0)), 0.0);
    CHECK_NEAR(m.compute_target(zoom_roll_input(150.0, 0)), 1.0, 1e-12);
    // 截屏灵敏度（平移项的保守系数）不影响前进项
    tc.capture_sensitivity = mc::Sensitivity::Low;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(85.0, 0)), 0.5, 1e-9);
    tc.capture_sensitivity = mc::Sensitivity::Medium;
    // forward_gain 2.0：42.5 × 2 = 85 → 0.5；0.5：170 × 0.5 = 85 → 0.5
    tc.forward_gain = 2.0;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(42.5, 0)), 0.5, 1e-9);
    tc.forward_gain = 0.5;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(170.0, 0)), 0.5, 1e-9);
    // 按 [general] sensitivity 选档：低 40 / 250（中点 145），高 10 / 83（中点 46.5）
    tc.forward_gain = 1.0;
    tc.sensitivity = mc::Sensitivity::Low;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(145.0, 0)), 0.5, 1e-9);
    CHECK_EQ(m.compute_target(zoom_roll_input(40.0, 0)), 0.0);
    tc.sensitivity = mc::Sensitivity::High;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(46.5, 0)), 0.5, 1e-9);
    // verify.ps1 -Forward 的速度：--zoom 3 → 135 °/s（中档）→ smoothstep(20, 150, 135) ≈ 0.963
    tc.sensitivity = mc::Sensitivity::Medium;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(3.0 * 45.0, 0)), mc::smoothstep(20, 150, 135), 1e-12);
    CHECK(m.compute_target(zoom_roll_input(3.0 * 45.0, 0)) > 0.95);
    // 估计偏低 25%（101 °/s）也 > 0.5
    CHECK(m.compute_target(zoom_roll_input(0.75 * 135.0, 0)) > 0.5);
}

TEST(intensity_roll_term_and_thresholds) {
    mc::TriggerConfig tc;
    mc::MaskConfig mc_;
    mc::IntensityModel m;
    m.configure(tc, mc_);
    mc::IntensityTerms t;
    // 倾斜：中档 15 / 90 °/s，中点 52.5 → 0.5；不乘 forward_gain
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 52.5), &t), 0.5, 1e-9);
    CHECK_NEAR(t.capture_roll, 0.5, 1e-9);
    CHECK_EQ(m.compute_target(zoom_roll_input(0, 15.0)), 0.0);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 90.0)), 1.0, 1e-12);
    // 游动侧倾 30 °/s → smoothstep(15, 90, 30) = 0.104
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 30.0)), 0.104, 0.001);
    // verify.ps1 -Forward 的 --roll 75 → smoothstep(15, 90, 75) = 0.896；偏低 25% 也 > 0.5
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 75.0)), 0.896, 0.001);
    CHECK(m.compute_target(zoom_roll_input(0, 0.75 * 75.0)) > 0.5);
    tc.forward_gain = 3.0;   // 不影响倾斜
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 52.5)), 0.5, 1e-9);
    // 截屏灵敏度不影响倾斜；触发灵敏度高 → 8 / 50（中点 29），低 → 30 / 150（中点 90）
    tc.forward_gain = 1.0;
    tc.capture_sensitivity = mc::Sensitivity::High;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 52.5)), 0.5, 1e-9);
    tc.sensitivity = mc::Sensitivity::High;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 29.0)), 0.5, 1e-9);
    tc.sensitivity = mc::Sensitivity::Low;
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 90.0)), 0.5, 1e-9);
    // 用户改了阈值表
    tc.sensitivity = mc::Sensitivity::Medium;
    tc.roll_thresholds[1] = {10, 30};
    m.configure(tc, mc_);
    CHECK_NEAR(m.compute_target(zoom_roll_input(0, 20.0)), 0.5, 1e-9);
}

TEST(intensity_forward_roll_gated_and_max_combined) {
    mc::TriggerConfig tc;
    mc::MaskConfig mc_;
    mc::IntensityModel m;
    m.configure(tc, mc_);
    // gate_capture 为假 → 两项都为 0
    mc::IntensityTerms t;
    CHECK_EQ(m.compute_target(zoom_roll_input(1000, 1000, false), &t), 0.0);
    CHECK_EQ(t.capture_zoom, 0.0);
    CHECK_EQ(t.capture_roll, 0.0);
    // 与 WASD（0.35）取最大：前进项小时 WASD 决定，前进项大时前进项决定
    mc::IntensityInputs in = zoom_roll_input(45.0, 0);   // 慢游 45 °/s → ≈ 0.095
    in.moving = true;
    in.gate_keys = true;
    CHECK_NEAR(m.compute_target(in), 0.35, 1e-12);
    in.capture_zoom_deg_per_s = 90.0;   // 快游 → 0.558
    CHECK_NEAR(m.compute_target(in), mc::smoothstep(20, 150, 90.0), 1e-12);
    // 截屏关闭时 WASD 仍然有效
    in.gate_capture = false;
    CHECK_NEAR(m.compute_target(in), 0.35, 1e-12);
    // 平移、前进、倾斜三项取最大
    mc::IntensityInputs all = zoom_roll_input(85.0, 70.0);   // 前进 0.5，倾斜 smoothstep(15,90,70)
    all.capture_deg_per_s = 200.0;                           // 平移 smoothstep(90,540,200)
    CHECK_NEAR(m.compute_target(all, &t), mc::smoothstep(15, 90, 70.0), 1e-12);
    CHECK(t.capture < t.capture_roll);
    CHECK_NEAR(t.capture_zoom, 0.5, 1e-9);
}
