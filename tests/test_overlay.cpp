// test_overlay.cpp — overlay 外观：make_params 的几何 / DPI / toast 数值，以及用 WARP 设备离屏渲染真实着色器（overlay/overlay.hlsl 构建时编译出的、app 也在用的字节码）后读回像素，核对 overlay-rendering spec 的数值场景。
#include "test_framework.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "config/config.h"
#include "overlay/overlay.h"
#include "util/win.h"

// overlay.hlsl 的字节码：与 renderer.cpp 引用的是同一份 scripts\build_shaders.cmd 输出（%MC_OBJ%\shaders\）。
#include "overlay_ps.h"
#include "overlay_vs.h"

using namespace mc;

namespace {

// 离屏渲染器：WARP 设备 + 与 app 相同的着色器字节码，渲染到 B8G8R8A8 纹理再读回（预乘 BGRA，每像素一个 uint32）。
struct Offscreen {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;
    bool ok = false;

    Offscreen() {
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            return;
        }
        if (FAILED(dev->CreateVertexShader(kOverlayVS, sizeof(kOverlayVS), nullptr, &vs))) return;
        if (FAILED(dev->CreatePixelShader(kOverlayPS, sizeof(kOverlayPS), nullptr, &ps))) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(OverlayConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rs))) return;
        ok = true;
    }

    std::vector<std::uint32_t> render(const OverlayConstants& c, UINT w, UINT h) {
        std::vector<std::uint32_t> out;
        if (!ok) return out;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> rt, staging;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt))) return out;
        if (FAILED(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv))) return out;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return out;
        // 先填一个非零值，确认着色器真的覆盖了每个像素（不需要 Clear）。
        const float junk[4] = {0.3f, 0.6f, 0.9f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), junk);
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, &c, 0, 0);
        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs.Get());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {cb.Get()};
        ctx->PSSetConstantBuffers(0, 1, cbs);
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), rt.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
        out.resize(static_cast<std::size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            std::memcpy(&out[static_cast<std::size_t>(y) * w], static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch,
                        w * 4);
        }
        ctx->Unmap(staging.Get(), 0);
        return out;
    }
};

Offscreen& gpu() {
    static Offscreen o;   // 函数内静态：第一次用到时才创建 WARP 设备
    return o;
}

struct Image {
    std::vector<std::uint32_t> px;
    UINT w = 0, h = 0;
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)]; }
    double a(int x, int y) const { return static_cast<double>((at(x, y) >> 24) & 0xFF) / 255.0; }
    double r(int x, int y) const { return static_cast<double>((at(x, y) >> 16) & 0xFF) / 255.0; }
    double g(int x, int y) const { return static_cast<double>((at(x, y) >> 8) & 0xFF) / 255.0; }
    double b(int x, int y) const { return static_cast<double>(at(x, y) & 0xFF) / 255.0; }
};

OverlayFrame scene_frame(MaskLevel level, float i, bool center = false, bool edges = false) {
    OverlayFrame f;
    f.scene = true;
    f.level = level;
    f.intensity = i;
    f.show_center_dot = center;
    f.show_edge_dots = edges;
    return f;
}

Image draw(const OverlayFrame& f, const MaskStyle& mask, const DotStyle& dots, UINT w = 1920, UINT h = 1080,
           UINT dpi = 96) {
    Image img;
    img.w = w;
    img.h = h;
    img.px = gpu().render(Overlay::make_params(f, mask, dots, w, h, dpi).constants, w, h);
    return img;
}

constexpr double kTol = 0.006;   // 8 位量化（1/255 ≈ 0.004）加浮点误差

}  // namespace

// ---------------------------------------------------------------- make_params（纯 CPU）

TEST(overlay_params_strong_geometry_1080p) {
    const auto p = Overlay::make_params(scene_frame(MaskLevel::Strong, 1.0f), MaskConfig{}, DotConfig{}, 1920, 1080, 96);
    CHECK_NEAR(p.constants.size_radii[2], 371.25, 1e-3);   // rx
    CHECK_NEAR(p.constants.size_radii[3], 297.0, 1e-3);    // ry
    CHECK_NEAR(p.constants.mask[0], 3.161, 0.001);         // R
    CHECK_NEAR(p.constants.mask[1], 0.35, 1e-6);           // feather
    CHECK_NEAR(p.constants.mask[3], 0.85, 1e-6);           // max_opacity
}

TEST(overlay_params_dpi_144_scales_pixels) {
    OverlayFrame f = scene_frame(MaskLevel::Medium, 0.0f, true, true);
    const auto p = Overlay::make_params(f, MaskConfig{}, DotConfig{}, 1920, 1080, 144);
    CHECK_NEAR(p.constants.center[0], 4.5, 1e-6);   // 中心点半径
    CHECK_NEAR(p.constants.center[1], 1.5, 1e-6);   // 描边宽
    CHECK_NEAR(p.constants.edge[1], 36.0, 1e-6);    // 边缘点内缩
    CHECK_NEAR(p.dpi_scale, 1.5, 1e-6);
}

TEST(overlay_params_edge_alpha_and_switches) {
    for (const auto& [i, want] : {std::pair{0.0f, 0.15}, std::pair{0.5f, 0.45}, std::pair{1.0f, 0.75}}) {
        const auto p = Overlay::make_params(scene_frame(MaskLevel::Medium, i, true, true), MaskConfig{}, DotConfig{},
                                            1920, 1080, 96);
        CHECK_NEAR(p.constants.edge[3], want, 1e-6);
    }
    OverlayFrame text_only = scene_frame(MaskLevel::Strong, 1.0f, true, true);
    text_only.scene = false;
    const auto p = Overlay::make_params(text_only, MaskConfig{}, DotConfig{}, 1920, 1080, 96);
    CHECK_EQ(p.constants.mask[2], 0.0f);     // 仅文字模式：强度按 0
    CHECK_EQ(p.constants.center[2], 0.0f);   // 不画参照点
    CHECK_EQ(p.constants.edge[2], 0.0f);
}

TEST(overlay_toast_opacity_timeline) {
    CHECK_NEAR(Overlay::toast_opacity(0.0), 1.0, 1e-9);
    CHECK_NEAR(Overlay::toast_opacity(1.2), 1.0, 1e-9);
    CHECK_NEAR(Overlay::toast_opacity(1.35), 0.5, 1e-6);
    CHECK_NEAR(Overlay::toast_opacity(1.5), 0.0, 1e-9);
    CHECK_NEAR(Overlay::toast_opacity(3.0), 0.0, 1e-9);
    OverlayFrame f;
    f.toast = L"遮罩：强";
    f.toast_age_s = 1.49;
    CHECK_TRUE(f.has_text());
    f.toast_age_s = 1.5;
    CHECK_FALSE(f.has_text());
}

// ---------------------------------------------------------------- 真实 HLSL（WARP 离屏渲染）

// 着色器在构建时由 fxc /WX 编译（有警告就构建失败）；这里确认字节码是 DXBC 容器，并且能在设备上创建。
TEST(overlay_shader_bytecode_creates) {
    CHECK_TRUE(std::memcmp(kOverlayVS, "DXBC", 4) == 0);
    CHECK_TRUE(std::memcmp(kOverlayPS, "DXBC", 4) == 0);
    CHECK_TRUE(gpu().ok);
}

TEST(overlay_shader_zero_intensity_fully_transparent) {
    const Image img = draw(scene_frame(MaskLevel::Strong, 0.0f), MaskConfig{}, DotConfig{});
    CHECK_EQ(img.px.size(), static_cast<std::size_t>(1920 * 1080));
    std::size_t nonzero = 0;
    for (std::uint32_t v : img.px) nonzero += v != 0 ? 1u : 0u;
    CHECK_EQ(nonzero, static_cast<std::size_t>(0));   // 也说明着色器覆盖了 Clear 的垃圾值
}

TEST(overlay_shader_strong_full_intensity_values) {
    const Image img = draw(scene_frame(MaskLevel::Strong, 1.0f), MaskConfig{}, DotConfig{});
    CHECK_NEAR(img.a(960, 540), 0.0, kTol);          // 屏幕中心
    CHECK_NEAR(img.a(960, 540 - 297), 0.0, kTol);    // 中心正上方 297 px（d = 1）
    CHECK_NEAR(img.a(960, 540 - 401), 0.85, kTol);   // ≥ 401 px（d ≥ 1.35）
    CHECK_NEAR(img.a(960, 0), 0.85, kTol);           // 顶边中点
    CHECK_NEAR(img.a(0, 0), 0.85, kTol);             // 四个角
    CHECK_NEAR(img.a(1919, 0), 0.85, kTol);
    CHECK_NEAR(img.a(0, 1079), 0.85, kTol);
    CHECK_NEAR(img.a(1919, 1079), 0.85, kTol);
    CHECK_NEAR(img.r(0, 0) + img.g(0, 0) + img.b(0, 0), 0.0, 1e-9);   // 纯黑
}

TEST(overlay_shader_medium_light_full_intensity) {
    const Image med = draw(scene_frame(MaskLevel::Medium, 1.0f), MaskConfig{}, DotConfig{});
    CHECK_NEAR(med.a(960, 0), 0.75, kTol);
    CHECK_NEAR(med.a(960, 540 - 323), 0.0, kTol);   // 324 px 以内为 0
    const Image light = draw(scene_frame(MaskLevel::Light, 1.0f), MaskConfig{}, DotConfig{});
    CHECK_NEAR(light.a(960, 0), 0.60, kTol);
    CHECK_NEAR(light.a(960, 540 - 350), 0.0, kTol);   // 351 px 以内为 0
}

TEST(overlay_shader_half_intensity_only_corners) {
    const Image img = draw(scene_frame(MaskLevel::Strong, 0.5f), MaskConfig{}, DotConfig{});
    CHECK_NEAR(img.a(0, 0), 0.425, kTol);
    CHECK_NEAR(img.a(1919, 1079), 0.425, kTol);
    CHECK_NEAR(img.a(960, 0), 0.0, kTol);   // d ≈ 1.818 < r_in ≈ 2.256
}

TEST(overlay_shader_mask_color_premultiplied) {
    MaskConfig mask;
    mask.color = Rgb{0x20, 0x00, 0x00};
    // 强档、i = 1、四角 a = 0.85：预乘红 = 0x20/255 × 0.85
    const Image img = draw(scene_frame(MaskLevel::Strong, 1.0f), mask, DotConfig{});
    CHECK_NEAR(img.a(0, 0), 0.85, kTol);
    CHECK_NEAR(img.r(0, 0), 32.0 / 255.0 * 0.85, kTol);
    CHECK_NEAR(img.g(0, 0), 0.0, 1e-9);
}

TEST(overlay_shader_edge_dots_positions_and_alpha) {
    const Image img = draw(scene_frame(MaskLevel::Medium, 0.0f, false, true), MaskConfig{}, DotConfig{});
    // 8 个点的中心（像素 (x-1, y-1) 与 (x, y) 之间）：取 (x, y) 处像素，离中心 0.707 px，覆盖率 1。
    const int pts[8][2] = {{24, 24}, {960, 24}, {1896, 24}, {24, 540}, {1896, 540}, {24, 1056}, {960, 1056}, {1896, 1056}};
    for (const auto& pt : pts) {
        CHECK_NEAR(img.a(pt[0], pt[1]), 0.15, kTol);
        CHECK_NEAR(img.r(pt[0], pt[1]), 200.0 / 255.0 * 0.15, kTol);   // C8C8C8 预乘
        CHECK_NEAR(img.a(pt[0] + 6, pt[1]), 0.0, kTol);                 // 半径 3 px 之外透明
    }
    CHECK_NEAR(img.a(960, 540), 0.0, kTol);   // 中心不是边缘点
    std::size_t lit = 0;
    for (std::uint32_t v : img.px) lit += (v >> 24) != 0 ? 1u : 0u;
    CHECK_TRUE(lit > 8 * 20 && lit < 8 * 50);   // 恰好 8 个半径约 3 px 的点
}

TEST(overlay_shader_center_dot_with_outline_unchanged_by_intensity) {
    for (float i : {0.0f, 1.0f}) {
        const Image img = draw(scene_frame(MaskLevel::Strong, i, true, false), MaskConfig{}, DotConfig{});
        CHECK_NEAR(img.a(960, 540), 0.9, kTol);   // 白点 alpha 0.9
        CHECK_NEAR(img.r(960, 540), 0.9, kTol);
        // 描边环：距中心约 3.5–4 px 的像素为黑色、alpha ≈ 0.6（叠在 i = 0 的透明背景上）
        if (i == 0.0f) {
            CHECK_NEAR(img.a(960 + 3, 540), 0.6, 0.08);
            CHECK_NEAR(img.r(960 + 3, 540), 0.0, 0.02);
            CHECK_NEAR(img.a(960 + 6, 540), 0.0, kTol);
        }
    }
    DotConfig no_outline;
    no_outline.center_dot_outline = false;
    const Image img = draw(scene_frame(MaskLevel::Strong, 0.0f, true, false), MaskConfig{}, no_outline);
    CHECK_NEAR(img.a(960, 540), 0.9, kTol);
    CHECK_NEAR(img.a(960 + 3, 540), 0.0, kTol);
}

// ---------------------------------------------------------------- 帧键（按需 Present）

TEST(overlay_params_invisible_mask_zeroed_exactly) {
    // 中档 1080p：R ≈ 2.898，遮罩内边界 r_in = lerp(R + 0.35, 1, i) 在 i ≤ 0.35 / (R − 0.65) ≈ 0.1557 时退出屏幕角之外。
    const auto lo = Overlay::make_params(scene_frame(MaskLevel::Medium, 0.15f, true, true), MaskConfig{}, DotConfig{},
                                         1920, 1080, 96);
    CHECK_EQ(lo.constants.mask[2], 0.0f);
    CHECK_NEAR(lo.intensity, 0.15, 1e-6);
    CHECK_NEAR(lo.constants.edge[3], 0.15 + 0.6 * 0.15, 1e-6);   // 边缘点 alpha 仍按真实强度
    const auto hi = Overlay::make_params(scene_frame(MaskLevel::Medium, 0.16f, true, true), MaskConfig{}, DotConfig{},
                                         1920, 1080, 96);
    CHECK_NEAR(hi.constants.mask[2], 0.16, 1e-6);
    // 给 0 与给真实强度画出来的像素完全相同（也就是遮罩确实处处为 0）。
    OverlayConstants raw = lo.constants;
    raw.mask[2] = 0.15f;
    const auto a = gpu().render(lo.constants, 1920, 1080);
    const auto b = gpu().render(raw, 1920, 1080);
    CHECK_EQ(a.size(), static_cast<std::size_t>(1920 * 1080));
    CHECK_TRUE(a == b);
}

TEST(overlay_frame_key_visible_changes_only) {
    auto params = [](float i, bool edges = true) {
        return Overlay::make_params(scene_frame(MaskLevel::Medium, i, true, edges), MaskConfig{}, DotConfig{}, 1920,
                                    1080, 96);
    };
    // 淡出尾段：遮罩整片为 0，边缘点 alpha 变化不到一个灰阶 → 看不出差别，不 Present。
    CHECK_TRUE(Overlay::same_visible(params(0.100f), params(0.1005f)));
    // 边缘点 alpha 跨过一个灰阶 → Present。
    CHECK_FALSE(Overlay::same_visible(params(0.100f), params(0.110f)));
    // 回到 0 的那一帧总要 Present（即使 0.0005 与 0 的边缘点 alpha 相同）。
    CHECK_FALSE(Overlay::same_visible(params(0.0005f), params(0.0f)));
    CHECK_TRUE(Overlay::same_visible(params(0.0f), params(0.0f)));
    // 遮罩可见时强度的任何变化都 Present。
    CHECK_FALSE(Overlay::same_visible(params(0.5f), params(0.5001f)));
    // 边缘点关闭时，尾段的强度变化完全看不见。
    CHECK_TRUE(Overlay::same_visible(params(0.05f, false), params(0.10f, false)));
    // 文字、档位等其它字段照常逐位比较。
    auto t1 = params(0.0f);
    auto t2 = params(0.0f);
    t2.toast = L"遮罩：强";
    t2.toast_opacity = 1.0f;
    CHECK_FALSE(Overlay::same_visible(t1, t2));
    const auto strong = Overlay::make_params(scene_frame(MaskLevel::Strong, 0.0f, true, true), MaskConfig{},
                                             DotConfig{}, 1920, 1080, 96);
    CHECK_FALSE(Overlay::same_visible(params(0.0f), strong));
    // 一次完整淡出（60 Hz，release 500 ms）：Present 次数比"强度一变就 Present"少，且最后一帧是 0。
    IntensityModel m;
    m.configure(TriggerConfig{}, MaskConfig{});
    m.reset(1.0);
    IntensityInputs idle;
    RenderParams last = params(1.0f);
    int changed = 0, presented = 0;
    float prev = 1.0f;
    for (int k = 0; k < 120; ++k) {
        const float v = m.update(idle, 1.0 / 60.0);
        if (v != prev) ++changed;
        prev = v;
        const RenderParams p = params(v);
        if (!Overlay::same_visible(p, last)) {
            ++presented;
            last = p;
        }
    }
    CHECK_EQ(last.intensity, 0.0f);
    CHECK_TRUE(changed >= 60);
    CHECK_TRUE(presented < changed * 3 / 4);
}

TEST(overlay_calibration_message_frame) {
    // 变更 2 的校准提示：有提示就算"有文字"（仅文字模式也显示、保持活动节拍）；内容变化（计数增长）下一帧就 Present；
    // 遮罩强度不影响提示本身（提示在着色器之后由 D2D 绘制，参数里原样带过去）。
    OverlayFrame f;
    CHECK_FALSE(f.has_text());
    f.message = L"校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7\n已转 3271 计数";
    CHECK_TRUE(f.has_text());   // scene = false（enabled=0）时也显示
    const auto a = Overlay::make_params(f, MaskConfig{}, DotConfig{}, 1920, 1080, 96);
    CHECK_EQ(a.message, f.message);
    CHECK_EQ(a.constants.mask[2], 0.0f);   // 仅文字模式：不画遮罩
    OverlayFrame g = f;
    g.message = L"校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7\n已转 3272 计数";
    CHECK_FALSE(Overlay::same_visible(a, Overlay::make_params(g, MaskConfig{}, DotConfig{}, 1920, 1080, 96)));
    CHECK_TRUE(Overlay::same_visible(a, Overlay::make_params(f, MaskConfig{}, DotConfig{}, 1920, 1080, 96)));
    // 强档满强度与 i = 0 时提示参数完全相同
    OverlayFrame s0 = scene_frame(MaskLevel::Strong, 0.0f, true, true);
    OverlayFrame s1 = scene_frame(MaskLevel::Strong, 1.0f, true, true);
    s0.message = f.message;
    s1.message = f.message;
    const auto p0 = Overlay::make_params(s0, MaskConfig{}, DotConfig{}, 1920, 1080, 96);
    const auto p1 = Overlay::make_params(s1, MaskConfig{}, DotConfig{}, 1920, 1080, 96);
    CHECK_EQ(p0.message, p1.message);
    CHECK_EQ(p0.dpi_scale, p1.dpi_scale);
    // 提示消失（回到 Idle）的那一帧也要 Present
    OverlayFrame none = f;
    none.message.clear();
    CHECK_FALSE(Overlay::same_visible(a, Overlay::make_params(none, MaskConfig{}, DotConfig{}, 1920, 1080, 96)));
}

TEST(overlay_shader_edge_dots_match_reference_everywhere) {
    // 着色器只对离点列 / 点行足够近的像素做 3 次距离测试；逐像素对照原始定义（8 个点的最小距离）。
    const UINT w = 1280, h = 720;
    const Image img = draw(scene_frame(MaskLevel::Medium, 0.0f, false, true), MaskConfig{}, DotConfig{}, w, h);
    CHECK_EQ(img.px.size(), static_cast<std::size_t>(w) * h);
    if (img.px.size() != static_cast<std::size_t>(w) * h) return;
    const double m = 24, r = 3;
    const double xs[3] = {m, w / 2.0, w - m}, ys[3] = {m, h / 2.0, h - m};
    int bad = 0, lit = 0;
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            double dmin = 1e9;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    if (i == 1 && j == 1) continue;   // 正中心不是边缘点
                    dmin = std::min(dmin, std::hypot(px - xs[i], py - ys[j]));
                }
            }
            const double want = 0.15 * std::clamp(r + 0.5 - dmin, 0.0, 1.0);
            const double got = img.a(static_cast<int>(x), static_cast<int>(y));
            if (std::fabs(got - want) > kTol) ++bad;
            if (got > 0) ++lit;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_TRUE(lit > 8 * 20 && lit < 8 * 50);
}

TEST(overlay_shader_dpi_144_edge_dot_at_36px) {
    const Image img = draw(scene_frame(MaskLevel::Medium, 0.0f, false, true), MaskConfig{}, DotConfig{}, 1920, 1080, 144);
    CHECK_NEAR(img.a(36, 36), 0.15, kTol);
    CHECK_NEAR(img.a(24, 24), 0.0, kTol);
}
