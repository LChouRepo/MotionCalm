// test_grid_recording.cpp — 网格片段文件格式的单元测试：内存 / 文件往返（含非 ASCII 路径）、16 位量化误差界（含 HDR > 1.0）、零帧、截断 / 损坏 / 荒谬尺寸的输入都报错而不崩溃、编码端的参数校验。
#include "test_framework.h"

#include "capture/grid_recording.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// 32 位 LCG，uniform() ∈ [0,1)。
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : s_(seed * 2654435761u + 0x9E3779B9u) {}
    double uniform() {
        s_ = s_ * 1664525u + 1013904223u;
        return static_cast<double>(s_ >> 8) * (1.0 / 16777216.0);
    }

private:
    std::uint32_t s_;
};

// n 帧 w×h 的随机片段，样本 ∈ [0, max_value)，时间戳从 t0 起每帧 1/60 s。
mc::GridClip make_clip(int n, int w, int h, double max_value, double t0 = 100.0, std::uint32_t seed = 1) {
    mc::GridClip c;
    c.info.grid_w = w;
    c.info.grid_h = h;
    c.info.tex_w = 1920;
    c.info.tex_h = 1080;
    c.info.screen_w = 1920;
    c.info.screen_h = 1080;
    c.info.rotation_deg = 0;
    Lcg rng(seed);
    for (int k = 0; k < n; ++k) {
        mc::GridClipFrame f;
        f.t_s = t0 + k / 60.0;
        f.gray.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (float& v : f.gray) v = static_cast<float>(rng.uniform() * max_value);
        c.frames.push_back(std::move(f));
    }
    return c;
}

std::vector<std::uint8_t> encode(const mc::GridClip& c) {
    std::vector<std::uint8_t> b;
    std::string err;
    CHECK_TRUE(mc::encode_grid_clip(c, &b, &err));
    return b;
}

void set_u32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
void set_f64(std::vector<std::uint8_t>& b, std::size_t off, double v) {
    const std::uint64_t u = std::bit_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(u >> (8 * i));
}

// 解码必须失败且给出说明。
bool decode_fails(const std::vector<std::uint8_t>& b, std::size_t size, const char* label) {
    mc::GridClip out;
    std::string err;
    const bool ok = mc::decode_grid_clip(b.data(), size, &out, &err);
    std::printf("   %-28s -> %s\n", label, ok ? "(accepted!)" : err.c_str());
    return !ok && !err.empty();
}

double max_abs_diff(const mc::GridClip& a, const mc::GridClip& b) {
    double m = 0;
    for (std::size_t k = 0; k < a.frames.size() && k < b.frames.size(); ++k)
        for (std::size_t i = 0; i < a.frames[k].gray.size() && i < b.frames[k].gray.size(); ++i)
            m = std::max(m, std::fabs(static_cast<double>(a.frames[k].gray[i]) - b.frames[k].gray[i]));
    return m;
}

std::filesystem::path temp_file(const wchar_t* name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path();
    return dir / name;
}

}  // namespace

TEST(grid_clip_round_trip_in_memory) {
    const mc::GridClip c = make_clip(5, 160, 90, 1.0, 1234.5);
    const std::vector<std::uint8_t> b = encode(c);
    CHECK_EQ(b.size(), mc::kGridClipHeaderSize + 5 * (8 + 2 * 160 * 90));
    mc::GridClip d;
    std::string err;
    CHECK_TRUE(mc::decode_grid_clip(b.data(), b.size(), &d, &err));
    CHECK_EQ(d.info.grid_w, 160);
    CHECK_EQ(d.info.grid_h, 90);
    CHECK_EQ(d.info.tex_w, 1920);
    CHECK_EQ(d.info.tex_h, 1080);
    CHECK_EQ(d.info.screen_w, 1920);
    CHECK_EQ(d.info.screen_h, 1080);
    CHECK_EQ(d.info.rotation_deg, 0);
    CHECK_NEAR(d.info.source_t0_s, 1234.5, 1e-9);
    CHECK_EQ(d.frames.size(), 5u);
    for (std::size_t k = 0; k < d.frames.size(); ++k) {
        CHECK_NEAR(d.frames[k].t_s, k / 60.0, 1e-9);   // 相对首帧
        CHECK_EQ(d.frames[k].gray.size(), 160u * 90u);
    }
    // 再编码一次：source_t0 保持，时间戳不变，字节完全相同（量化是幂等的）
    const std::vector<std::uint8_t> b2 = encode(d);
    CHECK(b2 == b);
}

TEST(grid_clip_quantisation_error_bound) {
    // SDR：scale = 1，误差 ≤ 1/131070（半个量化步长）+ float 舍入
    {
        const mc::GridClip c = make_clip(3, 64, 36, 1.0);
        mc::GridClip d;
        std::string err;
        const std::vector<std::uint8_t> b = encode(c);
        CHECK_TRUE(mc::decode_grid_clip(b.data(), b.size(), &d, &err));
        const double e = max_abs_diff(c, d);
        std::printf("   SDR: max error %.3g (bound %.3g)\n", e, 1.0 / 131070.0);
        CHECK(e <= 1.0 / 131070.0 + 1e-7);
    }
    // HDR：样本可到 3.5，scale 跟着变大，不截断
    {
        mc::GridClip c = make_clip(3, 64, 36, 3.0);
        c.frames[1].gray[7] = 3.5f;
        CHECK_NEAR(mc::grid_clip_sample_scale(c), 3.5, 1e-6);
        mc::GridClip d;
        std::string err;
        const std::vector<std::uint8_t> b = encode(c);
        CHECK_TRUE(mc::decode_grid_clip(b.data(), b.size(), &d, &err));
        const double e = max_abs_diff(c, d);
        std::printf("   HDR: max error %.3g (bound %.3g)\n", e, 3.5 / 131070.0);
        CHECK(e <= 3.5 / 131070.0 + 1e-6);
        CHECK_NEAR(d.frames[1].gray[7], 3.5, 1e-6);
    }
    // 负值 / NaN / inf → 0（不能破坏编码）
    {
        mc::GridClip c = make_clip(1, 8, 8, 1.0);
        c.frames[0].gray[0] = -0.25f;
        c.frames[0].gray[1] = std::numeric_limits<float>::quiet_NaN();
        c.frames[0].gray[2] = std::numeric_limits<float>::infinity();
        CHECK_NEAR(mc::grid_clip_sample_scale(c), 1.0, 0.0);   // 非有限值不参与标度
        mc::GridClip d;
        std::string err;
        const std::vector<std::uint8_t> b = encode(c);
        CHECK_TRUE(mc::decode_grid_clip(b.data(), b.size(), &d, &err));
        CHECK_EQ(d.frames[0].gray[0], 0.0f);
        CHECK_EQ(d.frames[0].gray[1], 0.0f);
        CHECK_EQ(d.frames[0].gray[2], 0.0f);
    }
}

TEST(grid_clip_zero_frames) {
    mc::GridClip c = make_clip(0, 160, 90, 1.0);
    const std::vector<std::uint8_t> b = encode(c);
    CHECK_EQ(b.size(), mc::kGridClipHeaderSize);
    mc::GridClip d;
    std::string err;
    CHECK_TRUE(mc::decode_grid_clip(b.data(), b.size(), &d, &err));
    CHECK_EQ(d.frames.size(), 0u);
    CHECK_EQ(d.info.grid_w, 160);
}

TEST(grid_clip_file_round_trip_non_ascii_path) {
    const std::filesystem::path p = temp_file(L"mc_grid_clip_测试_ü.mcgrid");
    const mc::GridClip c = make_clip(4, 96, 54, 1.0, 50.0, 7);
    std::string err;
    CHECK_TRUE(mc::write_grid_clip(p, c, &err));
    std::error_code ec;
    CHECK_TRUE(std::filesystem::exists(p, ec));
    std::filesystem::path tmp = p;
    tmp += L".tmp";
    CHECK_FALSE(std::filesystem::exists(tmp, ec));   // 临时文件已改名
    mc::GridClip d;
    CHECK_TRUE(mc::read_grid_clip(p, &d, &err));
    CHECK_EQ(d.frames.size(), 4u);
    CHECK(max_abs_diff(c, d) <= 1.0 / 131070.0 + 1e-7);
    // 覆盖已有文件
    CHECK_TRUE(mc::write_grid_clip(p, make_clip(2, 96, 54, 1.0), &err));
    CHECK_TRUE(mc::read_grid_clip(p, &d, &err));
    CHECK_EQ(d.frames.size(), 2u);
    std::filesystem::remove(p, ec);
    // 不存在的文件
    mc::GridClip e;
    std::string err2;
    CHECK_FALSE(mc::read_grid_clip(temp_file(L"mc_grid_clip_does_not_exist.mcgrid"), &e, &err2));
    CHECK_FALSE(err2.empty());
}

TEST(grid_clip_truncated_input_is_rejected) {
    const std::vector<std::uint8_t> b = encode(make_clip(3, 16, 9, 1.0));
    CHECK(decode_fails(b, 0, "empty"));
    CHECK(decode_fails(b, 10, "10 bytes"));
    CHECK(decode_fails(b, mc::kGridClipHeaderSize - 1, "header - 1"));
    CHECK(decode_fails(b, mc::kGridClipHeaderSize, "header only (3 frames)"));
    CHECK(decode_fails(b, mc::kGridClipHeaderSize + 5, "mid timestamp"));
    CHECK(decode_fails(b, b.size() - 1, "last byte missing"));
    std::vector<std::uint8_t> longer = b;
    longer.push_back(0);
    CHECK(decode_fails(longer, longer.size(), "one trailing byte"));
    mc::GridClip out;
    std::string err;
    CHECK_FALSE(mc::decode_grid_clip(nullptr, 100, &out, &err));
}

TEST(grid_clip_corrupt_header_is_rejected) {
    const std::vector<std::uint8_t> good = encode(make_clip(2, 16, 9, 1.0));
    auto mutate = [&](auto fn) {
        std::vector<std::uint8_t> b = good;
        fn(b);
        return b;
    };
    std::vector<std::uint8_t> b;
    b = mutate([](auto& v) { v[0] = 'X'; });
    CHECK(decode_fails(b, b.size(), "bad magic"));
    b = mutate([](auto& v) { set_u32(v, 8, 2); });
    CHECK(decode_fails(b, b.size(), "version 2"));
    b = mutate([](auto& v) { set_u32(v, 12, 16); });
    CHECK(decode_fails(b, b.size(), "header size 16"));
    b = mutate([](auto& v) { set_u32(v, 12, 0xFFFFFFF0u); });
    CHECK(decode_fails(b, b.size(), "header size huge"));
    b = mutate([](auto& v) { set_u32(v, 16, 0); });
    CHECK(decode_fails(b, b.size(), "grid width 0"));
    b = mutate([](auto& v) { set_u32(v, 16, 0xFFFFFFFFu); });
    CHECK(decode_fails(b, b.size(), "grid width 2^32-1"));
    b = mutate([](auto& v) { set_u32(v, 20, 100000); });
    CHECK(decode_fails(b, b.size(), "grid height 100000"));
    b = mutate([](auto& v) { set_u32(v, 24, 0); });
    CHECK(decode_fails(b, b.size(), "texture width 0"));
    b = mutate([](auto& v) { set_u32(v, 36, 0x7FFFFFFFu); });
    CHECK(decode_fails(b, b.size(), "screen height huge"));
    b = mutate([](auto& v) { set_u32(v, 40, 45); });
    CHECK(decode_fails(b, b.size(), "rotation 45"));
    b = mutate([](auto& v) { set_u32(v, 44, 0xFFFFFFFFu); });
    CHECK(decode_fails(b, b.size(), "frame count 2^32-1"));
    b = mutate([](auto& v) { set_u32(v, 44, 3); });
    CHECK(decode_fails(b, b.size(), "frame count 3 (has 2)"));
    b = mutate([](auto& v) { set_f64(v, 48, 0.0); });
    CHECK(decode_fails(b, b.size(), "scale 0"));
    b = mutate([](auto& v) { set_f64(v, 48, std::numeric_limits<double>::quiet_NaN()); });
    CHECK(decode_fails(b, b.size(), "scale NaN"));
    b = mutate([](auto& v) { set_f64(v, 48, 1e300); });
    CHECK(decode_fails(b, b.size(), "scale 1e300"));
    // 第 2 帧的时间戳：倒退 / NaN
    const std::size_t t1 = mc::kGridClipHeaderSize + (8 + 2 * 16 * 9);
    b = mutate([&](auto& v) { set_f64(v, t1, -1.0); });
    CHECK(decode_fails(b, b.size(), "timestamp goes backwards"));
    b = mutate([&](auto& v) { set_f64(v, t1, std::numeric_limits<double>::quiet_NaN()); });
    CHECK(decode_fails(b, b.size(), "timestamp NaN"));
    // 未改动的仍然合法
    mc::GridClip out;
    std::string err;
    CHECK_TRUE(mc::decode_grid_clip(good.data(), good.size(), &out, &err));
}

TEST(grid_clip_encoder_validates_input) {
    std::vector<std::uint8_t> b;
    std::string err;
    mc::GridClip c = make_clip(2, 16, 9, 1.0);
    c.frames[1].gray.pop_back();
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    CHECK_FALSE(err.empty());
    c = make_clip(2, 16, 9, 1.0);
    c.info.rotation_deg = 45;
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    c = make_clip(2, 16, 9, 1.0);
    c.info.tex_w = 0;
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    c = make_clip(2, 4, 4, 1.0);   // 网格太小
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    c = make_clip(2, 16, 9, 1.0);
    c.frames[1].t_s = c.frames[0].t_s - 0.01;
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    c = make_clip(1, 16, 9, 1.0);
    c.frames[0].gray[0] = 1e7f;   // 超过 kGridClipMaxScale
    CHECK_FALSE(mc::encode_grid_clip(c, &b, &err));
    // 写入失败（目录不存在）不留下文件、给出说明
    std::string err2;
    CHECK_FALSE(mc::write_grid_clip(temp_file(L"mc_no_such_dir_8f3a") / L"x.mcgrid", make_clip(1, 16, 9, 1.0), &err2));
    CHECK_FALSE(err2.empty());
}
