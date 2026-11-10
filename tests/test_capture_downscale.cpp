// test_capture_downscale.cpp — 用 WARP 设备跑真实的降采样 HLSL（capture/gpu_downscale）：均匀灰、1 px 条纹不混叠、亮度系数、与 CPU 双线性参考逐格比对、FP16 源、staging 环顺序与丢帧、GPU 降采样 + 估计器端到端位移。
#include "test_framework.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "capture/capture_policy.h"
#include "capture/gpu_downscale.h"
#include "capture/motion_estimator.h"
#include "util/win.h"

using namespace mc;

namespace {

// debug 构建优先带 D3D11 调试层（装了 Graphics Tools 时），最后一个用例检查它没有报任何警告 / 错误。
struct Warp {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> info;
    bool ok = false;
    Warp() {
#ifndef NDEBUG
        ok = SUCCEEDED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr,
                                           0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
        if (ok) dev.As(&info);
#endif
        if (!ok) {
            ok = SUCCEEDED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &dev, nullptr, &ctx));
        }
    }
};

Warp& warp() {
    static Warp w;   // 函数内静态：第一次用到时才创建 WARP 设备
    return w;
}

// 8 位 BGRA 源图（每像素一个 uint32：0xAARRGGBB）。
struct Bgra {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px;
    Bgra(int w_, int h_, std::uint32_t fill = 0xFF000000u) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * h_, fill) {}
    std::uint32_t& at(int x, int y) { return px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]; }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]; }
    // 灰度（0..1），与着色器同一系数。
    double luma(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        const std::uint32_t v = at(x, y);
        const double r = ((v >> 16) & 0xFF) / 255.0, g = ((v >> 8) & 0xFF) / 255.0, b = (v & 0xFF) / 255.0;
        return 0.299 * r + 0.587 * g + 0.114 * b;
    }
};

std::uint32_t gray8(int v) {
    const std::uint32_t u = static_cast<std::uint32_t>(std::clamp(v, 0, 255));
    return 0xFF000000u | (u << 16) | (u << 8) | u;
}

ComPtr<ID3D11Texture2D> make_texture(const Bgra& img, UINT bind = D3D11_BIND_SHADER_RESOURCE) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(img.w);
    td.Height = static_cast<UINT>(img.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = img.px.data();
    sd.SysMemPitch = static_cast<UINT>(img.w) * 4;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(warp().dev->CreateTexture2D(&td, &sd, &t))) return nullptr;
    return t;
}

// float → IEEE half（只处理正规数与 0，足够测试用的值）。
std::uint16_t to_half(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    const std::uint32_t sign = (u >> 16) & 0x8000u;
    const int exp = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    const std::uint32_t mant = (u >> 13) & 0x3FFu;
    if (f == 0.0f || exp <= 0) return static_cast<std::uint16_t>(sign);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | mant);
}

ComPtr<ID3D11Texture2D> make_fp16_texture(int w, int h, float r, float g, float b) {
    std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = to_half(r);
        px[i + 1] = to_half(g);
        px[i + 2] = to_half(b);
        px[i + 3] = to_half(1.0f);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = px.data();
    sd.SysMemPitch = static_cast<UINT>(w) * 8;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(warp().dev->CreateTexture2D(&td, &sd, &t))) return nullptr;
    return t;
}

// 一个降采样器（WARP 上，开 GPU 计时），网格默认 160×90。
struct Rig {
    GpuDownscaler d;
    bool ok = false;
    explicit Rig(int gw = 160, int gh = 90) {
        ok = warp().ok && d.init(warp().dev.Get(), warp().ctx.Get(), true) && d.set_grid(gw, gh);
    }
    // 读回最旧的待读槽（最多等 3 s；DO_NOT_WAIT 期间睡 1 ms）。
    bool read(std::vector<float>* out, double* t = nullptr) {
        out->assign(static_cast<std::size_t>(d.grid_w()) * d.grid_h(), -1.0f);
        double ft = 0;
        for (int i = 0; i < 3000; ++i) {
            const GpuDownscaler::Read r = d.try_read(out->data(), &ft);
            if (r == GpuDownscaler::Read::Ready) {
                if (t != nullptr) *t = ft;
                return true;
            }
            if (r != GpuDownscaler::Read::Pending) return false;
            ::Sleep(1);
        }
        return false;
    }
    std::vector<float> run(ID3D11Texture2D* tex, double t = 0) {
        std::vector<float> g;
        if (!ok || tex == nullptr) return g;
        if (FAILED(d.submit(tex, t))) return g;
        if (!read(&g)) g.clear();
        return g;
    }
};

// CPU 参考：按同一布局在每格取 taps_x × taps_y 个双线性样本（纹素空间 = 像素坐标 − 0.5，边缘 clamp）。
std::vector<double> cpu_reference(const Bgra& img, int gw, int gh) {
    const DownscaleLayout l = downscale_layout(img.w, img.h, gw, gh);
    std::vector<double> out(static_cast<std::size_t>(gw) * gh);
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
            double acc = 0;
            for (int j = 0; j < l.taps_y; ++j)
                for (int i = 0; i < l.taps_x; ++i) {
                    const double u = gx * l.cell_w + (i + 0.5) * l.step_x - 0.5;
                    const double v = gy * l.cell_h + (j + 0.5) * l.step_y - 0.5;
                    const double fu = std::floor(u), fv = std::floor(v);
                    const int x0 = static_cast<int>(fu), y0 = static_cast<int>(fv);
                    const double ax = u - fu, ay = v - fv;
                    const double top = (1 - ax) * img.luma(x0, y0) + ax * img.luma(x0 + 1, y0);
                    const double bot = (1 - ax) * img.luma(x0, y0 + 1) + ax * img.luma(x0 + 1, y0 + 1);
                    acc += (1 - ay) * top + ay * bot;
                }
            out[static_cast<std::size_t>(gy) * gw + static_cast<std::size_t>(gx)] = acc / (l.taps_x * l.taps_y);
        }
    return out;
}

// CPU 精确盒式平均（每格覆盖的整数像素块）。
std::vector<double> cpu_box(const Bgra& img, int gw, int gh) {
    const int cw = img.w / gw, ch = img.h / gh;
    std::vector<double> out(static_cast<std::size_t>(gw) * gh);
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
            double acc = 0;
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) acc += img.luma(gx * cw + x, gy * ch + y);
            out[static_cast<std::size_t>(gy) * gw + static_cast<std::size_t>(gx)] = acc / (cw * ch);
        }
    return out;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

void min_max(const std::vector<float>& g, float* lo, float* hi) {
    *lo = 1e9f;
    *hi = -1e9f;
    for (const float v : g) {
        *lo = std::min(*lo, v);
        *hi = std::max(*hi, v);
    }
}

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed * 2654435761u + 12345u) {}
    std::uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
};

// 随机灰度块纹理（块大小 block px，非周期），可整体平移 (ox, oy) px：img(x, y) = base(x − ox, y − oy)。
Bgra block_texture(int w, int h, int block, std::uint32_t seed, int ox = 0, int oy = 0) {
    const int margin = 256;
    const int bw = (w + 2 * margin) / block + 2, bh = (h + 2 * margin) / block + 2;
    std::vector<int> levels(static_cast<std::size_t>(bw) * bh);
    Lcg rng(seed);
    for (int& v : levels) v = static_cast<int>(rng.next() % 256);
    Bgra img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox + margin, sy = y - oy + margin;
            img.at(x, y) = gray8(levels[static_cast<std::size_t>(sy / block) * bw + static_cast<std::size_t>(sx / block)]);
        }
    return img;
}

}  // namespace

// ---------------------------------------------------------------- 基本

// 着色器在构建时由 fxc /WX 编译（有警告就构建失败）；这里确认预编译的字节码能在 WARP 设备上创建，管线可用。
TEST(capture_downscale_shader_init) {
    Rig rig;
    CHECK_TRUE(warp().ok);
    CHECK_TRUE(rig.ok);
    CHECK_EQ(rig.d.error(), std::string());
    CHECK_EQ(rig.d.compile_messages(), std::string());   // 接口保留给 desktop_dup.cpp，恒为空
    CHECK_EQ(rig.d.grid_w(), 160);
    CHECK_EQ(rig.d.grid_h(), 90);
}

TEST(capture_downscale_uniform_gray_128) {   // spec：整个桌面 RGB(128,128,128) → 每格 0.502 ± 0.01
    Rig rig;
    const Bgra img(1920, 1080, gray8(128));
    const std::vector<float> g = rig.run(make_texture(img).Get());
    CHECK_EQ(g.size(), static_cast<std::size_t>(160 * 90));
    float lo = 0, hi = 0;
    min_max(g, &lo, &hi);
    std::printf("   uniform 128: min=%.5f max=%.5f\n", lo, hi);
    CHECK_NEAR(lo, 0.502, 0.01);
    CHECK_NEAR(hi, 0.502, 0.01);
    CHECK_EQ(rig.d.layout().taps_x, 6);
}

TEST(capture_downscale_1px_stripes_no_alias) {   // spec：1 px 黑白交替竖条纹 → 所有格 0.5 ± 0.05
    Rig rig;
    Bgra v(1920, 1080), hz(1920, 1080), ck(1920, 1080);
    for (int y = 0; y < 1080; ++y)
        for (int x = 0; x < 1920; ++x) {
            v.at(x, y) = gray8((x & 1) ? 255 : 0);
            hz.at(x, y) = gray8((y & 1) ? 255 : 0);
            ck.at(x, y) = gray8(((x ^ y) & 1) ? 255 : 0);
        }
    const char* names[] = {"vertical", "horizontal", "checker"};
    const Bgra* imgs[] = {&v, &hz, &ck};
    for (int k = 0; k < 3; ++k) {
        const std::vector<float> g = rig.run(make_texture(*imgs[k]).Get());
        CHECK_EQ(g.size(), static_cast<std::size_t>(160 * 90));
        float lo = 0, hi = 0;
        min_max(g, &lo, &hi);
        std::printf("   1px %-10s stripes: min=%.5f max=%.5f\n", names[k], lo, hi);
        CHECK_NEAR(lo, 0.5, 0.05);
        CHECK_NEAR(hi, 0.5, 0.05);
    }
}

TEST(capture_downscale_luma_weights) {   // 灰度 = 0.299R + 0.587G + 0.114B
    Rig rig;
    const std::uint32_t colors[] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu, 0xFF000000u};
    const double expect[] = {0.299, 0.587, 0.114, 1.0, 0.0};
    for (int k = 0; k < 5; ++k) {
        const std::vector<float> g = rig.run(make_texture(Bgra(320, 180, colors[k])).Get());
        float lo = 0, hi = 0;
        min_max(g, &lo, &hi);
        CHECK_NEAR(lo, expect[k], 2e-3);
        CHECK_NEAR(hi, expect[k], 2e-3);
    }
}

TEST(capture_downscale_matches_box_average_1080p) {   // 1080p：GPU 结果 = 每格 12×12 像素的精确平均
    Rig rig;
    Bgra img(1920, 1080);
    Lcg rng(7);
    for (std::uint32_t& p : img.px) p = 0xFF000000u | (rng.next() & 0xFFFFFFu);   // 每像素独立随机 RGB（最坏情况）
    const std::vector<float> g = rig.run(make_texture(img).Get());
    const double d = max_abs_diff(g, cpu_box(img, 160, 90));
    std::printf("   1080p random RGB vs exact box: max |diff| = %.5f\n", d);
    CHECK(d < 3e-3);
}

TEST(capture_downscale_matches_bilinear_reference_odd_sizes) {   // 非整数格（1366×768、3440×1440）与 CPU 双线性参考一致
    struct Case {
        int w, h, gw, gh;
    };
    const Case cases[] = {{1366, 768, 160, 90}, {3440, 1440, 160, 90}, {1920, 1080, 192, 108}};
    for (const Case& c : cases) {
        Rig rig(c.gw, c.gh);
        const Bgra img = block_texture(c.w, c.h, 3, 11);
        const std::vector<float> g = rig.run(make_texture(img).Get());
        const double d = max_abs_diff(g, cpu_reference(img, c.gw, c.gh));
        std::printf("   %dx%d -> %dx%d vs CPU bilinear: max |diff| = %.5f\n", c.w, c.h, c.gw, c.gh, d);
        CHECK(d < 0.01);
    }
}

TEST(capture_downscale_fp16_source) {   // DuplicateOutput1 的 R16G16B16A16_FLOAT 桌面：按 float 采样，HDR 值 > 1 原样保留
    Rig rig;
    std::vector<float> g = rig.run(make_fp16_texture(640, 360, 0.25f, 0.25f, 0.25f).Get());
    float lo = 0, hi = 0;
    min_max(g, &lo, &hi);
    CHECK_NEAR(lo, 0.25, 1e-3);
    CHECK_NEAR(hi, 0.25, 1e-3);
    g = rig.run(make_fp16_texture(640, 360, 2.0f, 2.0f, 2.0f).Get());
    min_max(g, &lo, &hi);
    CHECK_NEAR(lo, 2.0, 1e-3);
    CHECK_NEAR(hi, 2.0, 1e-3);
}

TEST(capture_downscale_source_without_srv_bind) {   // 源纹理没有 SHADER_RESOURCE 绑定：先复制到内部纹理
    Rig rig;
    const std::vector<float> g = rig.run(make_texture(Bgra(1920, 1080, gray8(64)), D3D11_BIND_RENDER_TARGET).Get());
    CHECK_EQ(g.size(), static_cast<std::size_t>(160 * 90));
    float lo = 0, hi = 0;
    min_max(g, &lo, &hi);
    CHECK_NEAR(lo, 64 / 255.0, 2e-3);
    CHECK_NEAR(hi, 64 / 255.0, 2e-3);
}

// ---------------------------------------------------------------- staging 环

TEST(capture_downscale_ring_order_and_drop) {
    Rig rig;
    CHECK_TRUE(rig.ok);
    const ComPtr<ID3D11Texture2D> t1 = make_texture(Bgra(320, 180, gray8(51)));
    const ComPtr<ID3D11Texture2D> t2 = make_texture(Bgra(320, 180, gray8(102)));
    const ComPtr<ID3D11Texture2D> t3 = make_texture(Bgra(320, 180, gray8(153)));
    const ComPtr<ID3D11Texture2D> t4 = make_texture(Bgra(320, 180, gray8(204)));
    std::vector<float> g;
    double t = 0;
    std::vector<float> scratch(static_cast<std::size_t>(160 * 90));
    CHECK_TRUE(rig.d.try_read(scratch.data(), &t) == GpuDownscaler::Read::None);   // 空环
    CHECK(SUCCEEDED(rig.d.submit(t1.Get(), 1.0)));
    CHECK(SUCCEEDED(rig.d.submit(t2.Get(), 2.0)));
    CHECK(SUCCEEDED(rig.d.submit(t3.Get(), 3.0)));
    CHECK_EQ(rig.d.pending(), 3);
    CHECK_EQ(rig.d.dropped(), 0u);
    // 先进先出
    const double expect_t[] = {1.0, 2.0, 3.0};
    const double expect_v[] = {51 / 255.0, 102 / 255.0, 153 / 255.0};
    for (int k = 0; k < 3; ++k) {
        CHECK_TRUE(rig.read(&g, &t));
        CHECK_NEAR(t, expect_t[k], 1e-12);
        CHECK_NEAR(g[0], expect_v[k], 2e-3);
        CHECK_NEAR(g.back(), expect_v[k], 2e-3);
    }
    CHECK_EQ(rig.d.pending(), 0);
    // 4 帧都不读：最旧的被丢弃
    CHECK(SUCCEEDED(rig.d.submit(t1.Get(), 11.0)));
    CHECK(SUCCEEDED(rig.d.submit(t2.Get(), 12.0)));
    CHECK(SUCCEEDED(rig.d.submit(t3.Get(), 13.0)));
    CHECK(SUCCEEDED(rig.d.submit(t4.Get(), 14.0)));
    CHECK_EQ(rig.d.pending(), 3);
    CHECK_EQ(rig.d.dropped(), 1u);
    CHECK_TRUE(rig.read(&g, &t));
    CHECK_NEAR(t, 12.0, 1e-12);
    rig.d.drop_pending();
    CHECK_EQ(rig.d.pending(), 0);
    CHECK_TRUE(rig.d.try_read(scratch.data(), &t) == GpuDownscaler::Read::None);
}

TEST(capture_downscale_regrid_and_release_keep_bytecode) {
    Rig rig;
    CHECK_TRUE(rig.d.set_grid(192, 108));
    std::vector<float> g = rig.run(make_texture(Bgra(1920, 1080, gray8(128))).Get());
    CHECK_EQ(g.size(), static_cast<std::size_t>(192 * 108));
    CHECK_EQ(rig.d.layout().taps_x, 5);
    rig.d.release();
    CHECK_FALSE(rig.d.ready());
    CHECK_TRUE(FAILED(rig.d.submit(make_texture(Bgra(64, 36)).Get(), 0)));
    // 重新 init（字节码已缓存）
    CHECK_TRUE(rig.d.init(warp().dev.Get(), warp().ctx.Get(), false));
    CHECK_TRUE(rig.d.set_grid(160, 90));
    CHECK_FALSE(rig.d.gpu_timing());
    g = rig.run(make_texture(Bgra(1920, 1080, gray8(128))).Get());
    CHECK_EQ(g.size(), static_cast<std::size_t>(160 * 90));
}

TEST(capture_downscale_gpu_timing_nonblocking) {
    Rig rig;
    if (!rig.d.gpu_timing()) {
        std::printf("   WARP: timestamp queries unavailable (gpu_ms would be -1)\n");
        return;
    }
    const ComPtr<ID3D11Texture2D> t = make_texture(Bgra(1920, 1080, gray8(100)));
    double sum = 0;
    int n = 0;
    for (int k = 0; k < 8; ++k) {
        rig.run(t.Get(), k);
        n += rig.d.poll_gpu_ms(&sum);
    }
    for (int i = 0; i < 200 && n == 0; ++i) {
        ::Sleep(1);
        n += rig.d.poll_gpu_ms(&sum);
    }
    std::printf("   WARP gpu timing: %d samples, avg %.3f ms\n", n, n > 0 ? sum / n : -1.0);
    CHECK(n >= 0);
    CHECK(sum >= 0);
}

// ---------------------------------------------------------------- 端到端：GPU 降采样 + 位移估计

TEST(capture_downscale_end_to_end_shift) {
    Rig rig;
    MotionEstimator est;
    // 1080p 随机块纹理（块 9 px，非周期），第二帧内容右移 36 px（3 格）、上移 24 px（−2 格）
    const Bgra a = block_texture(1920, 1080, 9, 21, 0, 0);
    const Bgra b = block_texture(1920, 1080, 9, 21, 36, -24);
    const Bgra c = block_texture(1920, 1080, 9, 21, 42, -24);   // 再右移 6 px（半格）
    const std::vector<float> ga = rig.run(make_texture(a).Get());
    const std::vector<float> gb = rig.run(make_texture(b).Get());
    const std::vector<float> gc = rig.run(make_texture(c).Get());
    CHECK_EQ(gb.size(), static_cast<std::size_t>(160 * 90));
    if (gb.size() != static_cast<std::size_t>(160 * 90) || gc.size() != gb.size()) return;
    CHECK_FALSE(est.push(ga.data(), 10.0).valid);
    const MotionEstimate e1 = est.push(gb.data(), 10.0 + 1.0 / 60);
    std::printf("   GPU+estimator (+36,-24) px: dx=%+.3f dy=%+.3f c=%.3f\n", e1.dx, e1.dy, e1.confidence);
    CHECK_TRUE(e1.valid);
    CHECK_NEAR(e1.dx, 3.0, 0.25);
    CHECK_NEAR(e1.dy, -2.0, 0.25);
    const MotionEstimate e2 = est.push(gc.data(), 10.0 + 2.0 / 60);
    std::printf("   GPU+estimator (+6, 0) px:   dx=%+.3f dy=%+.3f c=%.3f\n", e2.dx, e2.dy, e2.confidence);
    CHECK_TRUE(e2.valid);
    CHECK_NEAR(e2.dx, 0.5, 0.25);
    CHECK_NEAR(e2.dy, 0.0, 0.25);
    // 换算：3 格、2 格 → √(36² + 24²) px = 43.27 px → 2.028° → ×60 = 121.7 °/s
    const std::optional<double> w =
        shift_to_deg_per_s(e1.dx, e1.dy, 160, 90, 1920, 1080, 90.0, e1.dt_s);
    CHECK_TRUE(w.has_value());
    CHECK_NEAR(w.value_or(0), std::hypot(36.0, 24.0) * 90.0 / 1920.0 * 60.0, 10.0);
}

// ---------------------------------------------------------------- D3D11 调试层（放在本文件最后：检查上面所有用例）

TEST(capture_downscale_debug_layer_clean) {
    Warp& w = warp();
    if (!w.info) {
        std::printf("   D3D11 debug layer not active (release build or SDK layers not installed)\n");
        return;
    }
    int bad = 0;
    const UINT64 n = w.info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        w.info->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(w.info->GetMessage(i, m, &len))) continue;
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            ++bad;
            std::printf("   d3d11 debug: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
        }
    }
    std::printf("   D3D11 debug layer: %llu stored messages, %d warnings/errors\n", static_cast<unsigned long long>(n),
                bad);
    CHECK_EQ(bad, 0);
    // 探针：确认调试层真的在验证（对 DEFAULT 资源 Map 是非法调用，必须产生一条错误消息）。
    const ComPtr<ID3D11Texture2D> t = make_texture(Bgra(16, 16));
    D3D11_MAPPED_SUBRESOURCE m{};
    CHECK_TRUE(FAILED(w.ctx->Map(t.Get(), 0, D3D11_MAP_READ, 0, &m)));
    CHECK(w.info->GetNumStoredMessages() > n);
    w.info->ClearStoredMessages();
}
