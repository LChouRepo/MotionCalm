// test_replay.cpp — 网格片段回放（GridPipeline）的测试：代码里合成的片段（平移 / 旋转显示器上的平移 / 前进 / 倾斜 / 静止 / 时间间隙 / 文件往返）对照已知运动检查发布的三个角速度；
// 严格周期纹理（混叠）与静态 HUD 的特性记录（只打印，不断言）；MC_REPLAY_DIR 指向的本地真实游戏片段（逐个回放、打印一行汇总，有 .expect 时检查界限；未设置时什么也不做）。
#include "test_framework.h"

#include "capture/grid_pipeline.h"
#include "capture/grid_recording.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr int kW = 160;
constexpr int kH = 90;
constexpr double kFps = 60.0;
constexpr double kHfov = 90.0;   // CaptureParams 默认值
constexpr double kPi = std::numbers::pi;
constexpr double kT0 = 10.0;     // 合成片段首帧的时间戳

using Frame = std::vector<float>;

// ---- 场景纹理（与 test_similarity_estimator.cpp 相同的值噪声 + 圆盘，世界坐标单位 = 格）

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

// 严格周期的"地砖"：周期 kTile 格的灰缝网格 + 砖面明暗（两轴都严格周期）。
constexpr double kTile = 8.0;
double tiles(double x, double y) {
    const double fx = x / kTile - std::floor(x / kTile), fy = y / kTile - std::floor(y / kTile);
    if (fx < 0.15 || fy < 0.15) return 0.2;   // 灰缝
    return 0.6 + 0.15 * std::sin(2 * kPi * fx) * std::cos(2 * kPi * fy);
}

// 内容运动（参考 → 当前，以网格中心为原点、y 向下）：p_cur = s·R(θ)·p_ref + t。
struct Motion {
    double s = 1.0;
    double rot_deg = 0.0;
    double tx = 0.0, ty = 0.0;
};

// 静态矩形（格）：内容不随运动变化（HUD / 武器模型的粗略替身）。
struct Hud {
    int x = 0, y = 0, w = 0, h = 0;
};

// 3×3 超采样盒式平均渲染 160×90。tex(qx, qy) 给出世界坐标处的灰度。
template <class Tex>
Frame render(const Motion& m, Tex&& tex, const Hud* hud = nullptr) {
    Frame f(static_cast<std::size_t>(kW) * kH);
    constexpr int kSs = 3;
    const double c = std::cos(m.rot_deg * kPi / 180.0), sn = std::sin(m.rot_deg * kPi / 180.0);
    for (int j = 0; j < kH; ++j) {
        for (int i = 0; i < kW; ++i) {
            const bool in_hud = hud && i >= hud->x && i < hud->x + hud->w && j >= hud->y && j < hud->y + hud->h;
            double acc = 0;
            for (int v = 0; v < kSs; ++v) {
                for (int u = 0; u < kSs; ++u) {
                    const double px = i + (u + 0.5) / kSs - kW / 2.0;
                    const double py = j + (v + 0.5) / kSs - kH / 2.0;
                    if (in_hud) {
                        acc += 0.15 + 0.7 * scene(px * 0.5 + 900, py * 0.5 + 900, 77);   // 与运动无关
                        continue;
                    }
                    const double ux = (px - m.tx) / m.s, uy = (py - m.ty) / m.s;
                    acc += tex(c * ux + sn * uy, -sn * ux + c * uy);
                }
            }
            f[static_cast<std::size_t>(j) * kW + static_cast<std::size_t>(i)] =
                static_cast<float>(std::clamp(acc / (kSs * kSs), 0.0, 1.0));
        }
    }
    return f;
}

auto scene_tex(std::uint32_t seed) {
    return [seed](double x, double y) { return scene(x, y, seed); };
}

// 1920×1080 横屏、网格 160×90（每格 12×12 像素）。
mc::GridClipInfo info_1080p() {
    mc::GridClipInfo in;
    in.grid_w = kW;
    in.grid_h = kH;
    in.tex_w = 1920;
    in.tex_h = 1080;
    in.screen_w = 1920;
    in.screen_h = 1080;
    in.rotation_deg = 0;
    return in;
}

// 第 k 帧 = frame_at(k)，时间戳 kT0 + k/60。
template <class Fn>
mc::GridClip build_clip(const mc::GridClipInfo& in, int n, Fn&& frame_at) {
    mc::GridClip c;
    c.info = in;
    for (int k = 0; k < n; ++k) c.frames.push_back(mc::GridClipFrame{kT0 + k / kFps, frame_at(k)});
    return c;
}

mc::GridReplayResult replay(const mc::GridClip& c, const mc::CaptureParams& p = {}) {
    mc::GridReplayResult r;
    std::string err;
    if (!CHECK_TRUE(mc::replay_grid_clip(c, p, &r, &err))) std::printf("   replay error: %s\n", err.c_str());
    return r;
}

// frames[from..] 上某字段的均值 / 绝对值最大值。
double mean_of(const std::vector<mc::GridPipelineFrame>& f, std::size_t from, double mc::GridPipelineFrame::*field) {
    double s = 0;
    std::size_t n = 0;
    for (std::size_t k = from; k < f.size(); ++k, ++n) s += f[k].*field;
    return n > 0 ? s / static_cast<double>(n) : 0.0;
}
double max_abs_of(const std::vector<mc::GridPipelineFrame>& f, double mc::GridPipelineFrame::*field,
                  std::size_t from = 0) {
    double m = 0;
    for (std::size_t k = from; k < f.size(); ++k) m = std::max(m, std::fabs(f[k].*field));
    return m;
}

// 发布的前进 / 倾斜在第 kSettled 帧之后才用满 66 ms 基线（之前历史帧不够，基线短、噪声大），"不误报"只在这之后检查。
constexpr std::size_t kSettled = 6;

// 屏幕上每帧平移 (px_x, px_y) 像素 → °/s：deg = |px|·hfov / W（与 shift_to_deg_per_s 独立地按物理意义写出）。
double pan_truth(double px_x, double px_y, int screen_w) { return std::hypot(px_x, px_y) * kHfov / screen_w * kFps; }

// 带说明的检查（框架的 CHECK 只打印表达式）。
void check_msg(bool ok, const std::string& what, int line) {
    ++mctest::ctx().checks;
    if (!ok) mctest::report_failure(__FILE__, line, what);
}

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// 平移片段：每帧 (2, 1) 格（1080p 下 (24, 12) 像素，75.5 °/s）。几个测试共用，只渲染一次。
const mc::GridClip& pan_clip() {
    static const mc::GridClip c =
        build_clip(info_1080p(), 16, [](int k) { return render(Motion{1.0, 0.0, 2.0 * k, 1.0 * k}, scene_tex(5)); });
    return c;
}

}  // namespace

// ---------------------------------------------------------------- 已知运动

TEST(replay_pan_known_speed) {
    const mc::GridClip& c = pan_clip();
    const mc::GridReplayResult r = replay(c);
    const auto& f = r.frames;
    CHECK_EQ(f.size(), c.frames.size());
    if (f.size() < 16) return;
    // 建立后的前 2 帧只更新参考帧（kDiscardFramesAfterBuild），发布 0
    CHECK_TRUE(f[0].discarded);
    CHECK_TRUE(f[1].discarded);
    CHECK_FALSE(f[2].discarded);
    CHECK_EQ(f[0].speed_deg_per_s, 0.0);
    CHECK_EQ(f[1].speed_deg_per_s, 0.0);
    CHECK_EQ(r.summary.discarded, 2u);
    CHECK_EQ(r.summary.estimates, 14u);
    CHECK(r.summary.valid_fraction() > 0.9);
    const double truth = pan_truth(24, 12, 1920);
    const double speed = mean_of(f, 2, &mc::GridPipelineFrame::speed_deg_per_s);
    CHECK_NEAR(speed, truth, 0.08 * truth);
    CHECK_NEAR(mean_of(f, 2, &mc::GridPipelineFrame::dx), 2.0, 0.2);
    CHECK_NEAR(mean_of(f, 2, &mc::GridPipelineFrame::dy), 1.0, 0.2);
    // 纯平移不应误报前进 / 倾斜
    const double zoom_max = max_abs_of(f, &mc::GridPipelineFrame::zoom_deg_per_s, kSettled);
    const double roll_max = max_abs_of(f, &mc::GridPipelineFrame::roll_deg_per_s, kSettled);
    CHECK(zoom_max < 3.0);
    CHECK(roll_max < 3.0);
    std::printf("   pan (2,1) cells/frame: speed %.2f deg/s (truth %.2f), zoom max %.2f (all frames %.2f), roll max %.2f "
                "(all %.2f), valid %.0f%%\n",
                speed, truth, zoom_max, r.summary.zoom_max, roll_max, r.summary.roll_max,
                100.0 * r.summary.valid_fraction());
}

TEST(replay_rotated_output_swaps_axes) {
    // 竖屏（旋转 90°）：纹理 1080×1920（扫描输出方向），桌面 1920×1080；网格 160×90 在纹理方向，每格 6.75×21.33 像素。
    mc::GridClipInfo in = info_1080p();
    in.tex_w = 1080;
    in.tex_h = 1920;
    in.rotation_deg = 90;
    const mc::GridPipeline probe(mc::CaptureParams{}, in);
    CHECK_TRUE(probe.geometry().swap_axes);
    CHECK_EQ(probe.geometry().screen_w, 1920);
    CHECK_EQ(probe.geometry().screen_h, 1080);
    CHECK_EQ(probe.hold_params().grid_w, kH);   // SpeedHold 用桌面方向的网格
    CHECK_EQ(probe.hold_params().grid_h, kW);
    CHECK_NEAR(probe.similarity_params().pixel_aspect, (1920.0 / kH) / (1080.0 / kW), 1e-9);

    // 纹理 x 方向每帧 2 格 = 13.5 纹理像素 = 桌面上竖直 13.5 像素
    const mc::GridClip c = build_clip(in, 12, [](int k) { return render(Motion{1.0, 0.0, 2.0 * k, 0.0}, scene_tex(6)); });
    const mc::GridReplayResult r = replay(c);
    const double truth = pan_truth(0, 2.0 * 1080 / kW, 1920);
    const double speed = mean_of(r.frames, 2, &mc::GridPipelineFrame::speed_deg_per_s);
    CHECK_NEAR(speed, truth, 0.08 * truth);
    // 发布的 dx / dy 是桌面方向：位移在 dy 上
    CHECK_NEAR(std::fabs(mean_of(r.frames, 2, &mc::GridPipelineFrame::dy)), 2.0, 0.2);
    CHECK(std::fabs(mean_of(r.frames, 2, &mc::GridPipelineFrame::dx)) < 0.2);
    std::printf("   rotated 90: speed %.2f deg/s (truth %.2f), zoom max %.2f\n", speed, truth, r.summary.zoom_max);
}

TEST(replay_zoom_known_rate) {
    // 每帧 ln s = ln(1.02)/4（66.7 ms 放大 2%）：径向流速 = ln 速率 × hfov/2 = 13.37 °/s
    const double lnf = std::log(1.02) / 4;
    const mc::GridClip c = build_clip(info_1080p(), 14, [&](int k) { return render(Motion{std::exp(lnf * k)}, scene_tex(9)); });
    const mc::GridReplayResult r = replay(c);
    const double truth = lnf * kFps * kHfov / 2;
    const double zoom = mean_of(r.frames, kSettled, &mc::GridPipelineFrame::zoom_deg_per_s);
    CHECK_NEAR(zoom, truth, 0.15 * truth);
    const double roll_max = max_abs_of(r.frames, &mc::GridPipelineFrame::roll_deg_per_s, kSettled);
    CHECK(roll_max < 3.0);
    std::printf("   zoom: %.2f deg/s (truth %.2f), roll max %.2f, pan speed mean %.2f, sim valid %.0f%%\n", zoom, truth,
                roll_max, mean_of(r.frames, kSettled, &mc::GridPipelineFrame::speed_deg_per_s),
                100.0 * r.summary.sim_valid_fraction());
}

TEST(replay_roll_known_rate_and_detect_roll_off) {
    // 每帧 0.5°：30 °/s
    const mc::GridClip c = build_clip(info_1080p(), 14, [](int k) { return render(Motion{1.0, 0.5 * k}, scene_tex(10)); });
    const mc::GridReplayResult r = replay(c);
    const double truth = 0.5 * kFps;
    const double roll = mean_of(r.frames, kSettled, &mc::GridPipelineFrame::roll_deg_per_s);
    CHECK_NEAR(roll, truth, 0.12 * truth);
    const double zoom_max = max_abs_of(r.frames, &mc::GridPipelineFrame::zoom_deg_per_s, kSettled);
    CHECK(zoom_max < 3.0);
    std::printf("   roll: %.2f deg/s (truth %.2f), zoom max %.2f, pan speed mean %.2f\n", roll, truth, zoom_max,
                mean_of(r.frames, kSettled, &mc::GridPipelineFrame::speed_deg_per_s));
    // detect_roll = 0：倾斜照样估计，但发布 0（与 CaptureService::Impl::publish 相同）
    mc::CaptureParams p;
    p.detect_roll = false;
    const mc::GridReplayResult off = replay(c, p);
    CHECK_EQ(off.summary.roll_max, 0.0);
    CHECK_NEAR(max_abs_of(off.frames, &mc::GridPipelineFrame::rotation_deg), max_abs_of(r.frames, &mc::GridPipelineFrame::rotation_deg), 1e-12);
}

TEST(replay_static_scene_publishes_zero) {
    const Frame still = render(Motion{}, scene_tex(11));
    const mc::GridClip c = build_clip(info_1080p(), 8, [&](int) { return still; });
    const mc::GridReplayResult r = replay(c);
    CHECK_NEAR(r.summary.valid_fraction(), 1.0, 1e-12);
    CHECK(r.summary.sim_valid_fraction() > 0.99);
    CHECK(r.summary.speed_max < 0.5);
    CHECK(r.summary.zoom_max < 0.5);
    CHECK(r.summary.roll_max < 0.5);
}

TEST(replay_time_gap_resets_published_speed) {
    // 第 8 帧之前插入 0.5 s 间隙：dt > 0.25 s 的估计不产生速度，保持 100 ms 早已过期 → 发布 0；下一帧恢复
    mc::GridClip c = pan_clip();
    for (std::size_t k = 8; k < c.frames.size(); ++k) c.frames[k].t_s += 0.5;
    const mc::GridReplayResult r = replay(c);
    if (r.frames.size() < 10) return;
    CHECK(r.frames[7].speed_deg_per_s > 0);
    CHECK_EQ(r.frames[8].speed_deg_per_s, 0.0);
    CHECK(r.frames[8].dt_s > 0.25);
    CHECK(r.frames[9].speed_deg_per_s > 0);
}

TEST(replay_after_file_round_trip_matches) {
    const mc::GridClip& c = pan_clip();
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path();
    const std::filesystem::path p = dir / L"mc_replay_往返.mcgrid";
    std::string err;
    CHECK_TRUE(mc::write_grid_clip(p, c, &err));
    mc::GridClip d;
    CHECK_TRUE(mc::read_grid_clip(p, &d, &err));
    std::filesystem::remove(p, ec);
    const mc::GridReplayResult a = replay(c);
    const mc::GridReplayResult b = replay(d);
    CHECK_EQ(a.frames.size(), b.frames.size());
    const double tol = 0.01 * pan_truth(24, 12, 1920);
    for (std::size_t k = 0; k < a.frames.size() && k < b.frames.size(); ++k) {
        CHECK_EQ(a.frames[k].valid, b.frames[k].valid);
        CHECK_NEAR(a.frames[k].speed_deg_per_s, b.frames[k].speed_deg_per_s, tol);
        CHECK_NEAR(a.frames[k].t_s - a.frames[0].t_s, b.frames[k].t_s, 1e-9);   // 读回的时间戳相对首帧
    }
}

TEST(replay_csv_and_reset) {
    const mc::GridReplayResult r = replay(pan_clip());
    const std::string h = mc::grid_replay_csv_header();
    const std::string row = r.frames.empty() ? std::string{} : mc::grid_replay_csv_row(0, r.frames[0]);
    CHECK_EQ(std::count(h.begin(), h.end(), ','), std::count(row.begin(), row.end(), ','));
    CHECK(row.find('\n') == std::string::npos);
    // reset()：重新丢弃 2 帧
    const mc::GridClip& c = pan_clip();
    mc::GridPipeline pipe(mc::CaptureParams{}, c.info);
    for (int k = 0; k < 4; ++k) pipe.push(c.frames[static_cast<std::size_t>(k)].gray.data(), c.frames[static_cast<std::size_t>(k)].t_s);
    pipe.reset();
    CHECK_TRUE(pipe.push(c.frames[4].gray.data(), c.frames[4].t_s).discarded);
    CHECK_TRUE(pipe.push(c.frames[5].gray.data(), c.frames[5].t_s).discarded);
    const mc::GridPipelineFrame f6 = pipe.push(c.frames[6].gray.data(), c.frames[6].t_s);
    CHECK_FALSE(f6.discarded);
    CHECK(f6.speed_deg_per_s > 0);
    CHECK_FALSE(pipe.push(nullptr, 1.0).valid);
}

// ---------------------------------------------------------------- 回放预期（.expect）

TEST(replay_expectations_parse_and_check) {
    mc::ReplayExpectations e;
    std::string err;
    const char* text =
        "\xEF\xBB\xBF# comment\r\n"
        "hfov = 100   # trailing comment\r\n"
        "max_shift=30\n"
        "detect_roll=0\n"
        "\n"
        "valid_fraction_min=0.8\n"
        "mean_speed_max=+50\n"
        "frames_min=10\n";
    CHECK_TRUE(mc::parse_replay_expectations(text, &e, &err));
    CHECK_NEAR(e.hfov.value_or(0), 100.0, 0);
    CHECK_EQ(e.max_shift.value_or(0), 30);
    CHECK_FALSE(e.detect_roll.value_or(true));
    CHECK_FALSE(e.hold_ms.has_value());
    CHECK_EQ(e.bounds.size(), 3u);
    mc::CaptureParams p;
    e.apply_to(&p);
    CHECK_NEAR(p.assumed_hfov_deg, 100.0, 0);
    CHECK_EQ(p.max_shift, 30);
    CHECK_FALSE(p.detect_roll);
    CHECK_EQ(p.hold_ms, mc::CaptureParams{}.hold_ms);

    mc::GridPipelineSummary s;
    s.frames = 20;
    s.estimates = 10;
    s.valid = 9;
    s.speed_sum = 20 * 40.0;
    CHECK(mc::check_replay_expectations(e, s).empty());
    s.valid = 5;              // 50% < 80%
    s.speed_sum = 20 * 60.0;  // 60 > 50
    const std::vector<std::string> fails = mc::check_replay_expectations(e, s);
    CHECK_EQ(fails.size(), 2u);
    for (const std::string& f : fails) std::printf("   expected failure: %s\n", f.c_str());

    for (const char* bad : {"valid_fractoin_min=0.5", "hfov=abc", "hfov=1\nhfov=2", "just text", "detect_roll=yes",
                            "mean_speed=3", "max_shift=1.5"}) {
        mc::ReplayExpectations x;
        std::string why;
        CHECK_FALSE(mc::parse_replay_expectations(bad, &x, &why));
        CHECK_FALSE(why.empty());
    }
    CHECK(mc::replay_expectations_path(L"C:\\clips\\de_dust.mcgrid") == std::filesystem::path(L"C:\\clips\\de_dust.expect"));
}

// ---------------------------------------------------------------- 特性记录（只打印，不断言估计值）

// 严格周期纹理：投影互相关在 s ≡ 真实位移 (mod 周期) 处有多个 ρ ≈ 1 的混叠峰。估计器取离上一次有效位移最近的峰
// （没有先验时取 |s| 最小的），所以每帧位移小于半个周期的平移必须报对，不能锁到大位移。
TEST(replay_periodic_texture_slow_pan_is_not_aliased) {
    for (int v = 1; v <= 3; ++v) {   // 周期 8 格：1–3 格 / 帧都小于半个周期
        const mc::GridClip c =
            build_clip(info_1080p(), 8, [&](int k) { return render(Motion{1.0, 0.0, static_cast<double>(v) * k, 0.0}, tiles); });
        const mc::GridReplayResult r = replay(c);
        const double truth = pan_truth(v * 12.0, 0, 1920);
        const double rep = mean_of(r.frames, 2, &mc::GridPipelineFrame::speed_deg_per_s);
        std::printf("   tiles pan %d cells/frame: truth %.1f deg/s, reported %.1f, dx_mean %+.2f, dy_mean %+.2f\n", v, truth, rep,
                    mean_of(r.frames, 2, &mc::GridPipelineFrame::dx), mean_of(r.frames, 2, &mc::GridPipelineFrame::dy));
        CHECK_NEAR(rep, truth, 0.10 * truth);
        CHECK_NEAR(mean_of(r.frames, 2, &mc::GridPipelineFrame::dy), 0.0, 0.2);
    }
}

// 周期纹理静止不动：不能因为混叠峰而报出运动。
TEST(replay_periodic_texture_static_reports_zero) {
    const mc::GridClip c = build_clip(info_1080p(), 8, [&](int) { return render(Motion{1.0, 0.0, 0.0, 0.0}, tiles); });
    const mc::GridReplayResult r = replay(c);
    CHECK(r.summary.speed_max < 0.5);
}

// 从静止逐帧加速到超过半个周期（每帧位移 0,1,…,6 格）：靠"离上一次位移最近"的先验一路跟住真实位移。
TEST(replay_periodic_texture_tracks_acceleration_past_half_period) {
    const int n = 8;
    const mc::GridClip c = build_clip(info_1080p(), n, [&](int k) {
        const int step = k < 7 ? k : 6;                      // 第 k 帧相对上一帧的位移
        double pos = 0;
        for (int j = 1; j <= k; ++j) pos += (j < 7 ? j : 6);
        (void)step;
        return render(Motion{1.0, 0.0, pos, 0.0}, tiles);
    });
    const mc::GridReplayResult r = replay(c);
    for (int k = 2; k < n; ++k) {
        const double want = k < 7 ? k : 6;
        std::printf("   frame %d: dx %+.2f (truth %+.0f)\n", k, r.frames[static_cast<std::size_t>(k)].dx, want);
        CHECK_NEAR(r.frames[static_cast<std::size_t>(k)].dx, want, 0.3);
    }
}

// 特性记录（只打印）：从静止直接以 ≥ 半个周期的速度起步时没有先验，单看两帧无法区分 v 与 v − 周期，
// 估计器取 |s| 最小的那个（低估而不是高估）。这是信息上的极限，不是缺陷。
TEST(replay_characterise_periodic_texture_aliasing) {
    std::printf("   characterisation only (not asserted): tiles with period %.0f cells, pan starting at >= half a period\n", kTile);
    std::printf("   cells/frame  truth deg/s  reported deg/s   dx_mean  valid\n");
    for (int v = 4; v <= 7; ++v) {
        const mc::GridClip c =
            build_clip(info_1080p(), 8, [&](int k) { return render(Motion{1.0, 0.0, static_cast<double>(v) * k, 0.0}, tiles); });
        const mc::GridReplayResult r = replay(c);
        const double rep = mean_of(r.frames, 2, &mc::GridPipelineFrame::speed_deg_per_s);
        CHECK(std::isfinite(rep));
        std::printf("   %11d  %11.1f  %14.1f  %+8.2f  %4.0f%%\n", v, pan_truth(v * 12.0, 0, 1920), rep,
                    mean_of(r.frames, 2, &mc::GridPipelineFrame::dx), 100.0 * r.summary.valid_fraction());
    }
}

// 静态 HUD（底部一条 + 右下角"武器"块，约占画面 22%）叠在平移上：记录 HUD 对平移 / 前进 / 倾斜估计的影响。
TEST(replay_characterise_static_hud_over_pan) {
    const Hud bar{0, 78, kW, 12};
    const Hud weapon{110, 50, 50, 28};
    std::printf("   characterisation only (not asserted): pan 3 cells/frame with static HUD\n");
    for (const Hud* hud : {static_cast<const Hud*>(nullptr), &bar, &weapon}) {
        const mc::GridClip c = build_clip(info_1080p(), 10, [&](int k) {
            Frame f = render(Motion{1.0, 0.0, 3.0 * k, 0.0}, scene_tex(12), hud);
            if (hud == &weapon) {   // 武器块之外再加底部 HUD 条
                const Frame b = render(Motion{1.0, 0.0, 3.0 * k, 0.0}, scene_tex(12), &bar);
                for (int j = bar.y; j < bar.y + bar.h; ++j)
                    for (int i = 0; i < kW; ++i) f[static_cast<std::size_t>(j) * kW + i] = b[static_cast<std::size_t>(j) * kW + i];
            }
            return f;
        });
        const mc::GridReplayResult r = replay(c);
        const double truth = pan_truth(36, 0, 1920);
        const double rep = mean_of(r.frames, 2, &mc::GridPipelineFrame::speed_deg_per_s);
        CHECK(std::isfinite(rep));
        std::printf("   %-22s speed %6.1f deg/s (truth %.1f)  dx_mean %+.2f  c_avg %.3f  valid %3.0f%%  zoom max %.2f  roll max %.2f\n",
                    hud == nullptr ? "no HUD" : (hud == &bar ? "bottom bar (13%)" : "bar + weapon (22%)"), rep, truth,
                    mean_of(r.frames, 2, &mc::GridPipelineFrame::dx), r.summary.confidence_avg(),
                    100.0 * r.summary.valid_fraction(), r.summary.zoom_max, r.summary.roll_max);
    }
}

// ---------------------------------------------------------------- 本地真实游戏片段（MC_REPLAY_DIR）

// MC_REPLAY_DIR 指向一个目录时回放其中每个 *.mcgrid，打印一行汇总；有同名 .expect 时检查其中的界限。
// 未设置时只打印一行说明（CI 不受影响）。真实录像不进 git，见 tests/fixtures/README.md。
TEST(replay_local_clips_from_env) {
    wchar_t* env = nullptr;
    std::size_t len = 0;
    std::filesystem::path dir;
    if (_wdupenv_s(&env, &len, L"MC_REPLAY_DIR") == 0 && env != nullptr) {
        dir = env;
    }
    std::free(env);
    if (dir.empty()) {
        std::printf("   MC_REPLAY_DIR not set: local clip replay skipped\n");
        return;
    }
    std::vector<std::filesystem::path> clips;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2) && it->path().extension() == L".mcgrid") clips.push_back(it->path());
    }
    check_msg(!ec, "MC_REPLAY_DIR cannot be listed: " + u8(dir) + " (" + ec.message() + ")", __LINE__);
    std::sort(clips.begin(), clips.end());
    std::printf("   MC_REPLAY_DIR=%s: %zu clip(s)\n", u8(dir).c_str(), clips.size());
    for (const std::filesystem::path& path : clips) {
        const std::string name = u8(path.filename());
        mc::GridClip clip;
        std::string err;
        if (!mc::read_grid_clip(path, &clip, &err)) {
            check_msg(false, name + ": " + err, __LINE__);
            continue;
        }
        mc::CaptureParams params;
        mc::ReplayExpectations expect;
        const std::filesystem::path expect_path = mc::replay_expectations_path(path);
        std::error_code ec3;
        const bool has_expect = std::filesystem::exists(expect_path, ec3);
        if (has_expect) {
            if (!mc::read_replay_expectations(expect_path, &expect, &err)) {
                check_msg(false, name + ": bad .expect: " + err, __LINE__);
                continue;
            }
            expect.apply_to(&params);
        }
        mc::GridReplayResult r;
        if (!mc::replay_grid_clip(clip, params, &r, &err)) {
            check_msg(false, name + ": " + err, __LINE__);
            continue;
        }
        const mc::GridPipelineSummary& s = r.summary;
        const std::vector<std::string> fails = has_expect ? mc::check_replay_expectations(expect, s) : std::vector<std::string>{};
        std::printf("   %s: %llu frames %.1f s, valid %.0f%% lowconf %.0f%% sim %.0f%%, speed %.1f/%.1f zoom %.1f/%.1f "
                    "roll %.1f/%.1f deg/s (mean/max)%s\n",
                    name.c_str(), static_cast<unsigned long long>(s.frames), s.duration_s(), 100.0 * s.valid_fraction(),
                    100.0 * s.lowconf_fraction(), 100.0 * s.sim_valid_fraction(), s.mean_speed(), s.speed_max,
                    s.mean_zoom(), s.zoom_max, s.mean_roll(), s.roll_max,
                    !has_expect ? "  (no .expect)" : (fails.empty() ? "  expect PASS" : "  expect FAIL"));
        for (const std::string& f : fails) check_msg(false, name + ": " + f, __LINE__);
    }
}
