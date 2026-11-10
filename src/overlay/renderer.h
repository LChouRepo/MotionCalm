// renderer.h — D3D11 + DirectComposition + D2D/DWrite 渲染器：waitable 合成交换链（预乘 alpha）、全屏遮罩着色器、toast / 调试条 / 校准提示文字层、设备丢失重建、GPU 计时。
#pragma once

#include "i18n/i18n.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>

#include <array>
#include <cstdint>
#include <string>

#include "util/win.h"

namespace mc {

// 着色器常量（与 overlay/overlay.hlsl 的 cbuffer OverlayConstants 一一对应：7 个 float4，改一边必须同时改另一边）。
// 所有 px 量都已乘以 DPI / 96。着色器的合成顺序（design D7）：遮罩 → 边缘点 over → 中心点描边环 over → 中心点 over；
// over = src + dst·(1 − src.a)。
struct OverlayConstants {
    float size_radii[4]{};    // W, H, rx, ry
    float mask[4]{};          // R, feather, intensity, max_opacity
    float mask_rgb[4]{};      // rgb, -
    float center[4]{};        // 半径 px, 描边 px, 开关, 描边开关
    float center_rgba[4]{};   // rgb, alpha
    float edge[4]{};          // 半径 px, 内缩 px, 开关, alpha
    float edge_rgb[4]{};      // rgb, -
    bool operator==(const OverlayConstants&) const = default;
};
static_assert(sizeof(OverlayConstants) % 16 == 0, "cbuffer size must be a multiple of 16");

// 一帧的完整内容；与上一次 Present 的值比较即"帧键"（design D8，比较规则见 Overlay::same_visible）。
struct RenderParams {
    OverlayConstants constants;
    float intensity = 0.0f;          // 平滑后的强度（只用于帧键：回到 0 的那一帧必须 Present）
    float dpi_scale = 1.0f;          // DPI / 96（字号、文字边距）
    std::wstring toast;              // 空 = 没有 toast
    float toast_opacity = 0.0f;      // 0..1
    bool debug_bar = false;
    std::wstring debug_text;
    float debug_target = 0.0f;       // 调试条上条（target）
    float debug_value = 0.0f;        // 调试条下条（value）
    std::wstring message;            // 变更 2 的校准提示（按 L'\n' 分行；空 = 没有）
    bool operator==(const RenderParams&) const = default;
};

enum class RenderResult {
    Presented,    // 已 Present
    NotReady,     // 交换链还没准备好接收新帧（frame latency 对象未触发），稍后再试
    Failed,       // 其他失败（已记日志），资源仍可用
    DeviceLost,   // 设备移除 / 重置 / D2D 需要重建：调用方应 shutdown() 后重新 init()
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer() { shutdown(); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 为 hwnd 创建全部资源（D3D11 设备、合成交换链、DComp 目标、着色器、D2D/DWrite）。失败返回 false（已记日志）。
    bool init(HWND hwnd, UINT width, UINT height, bool gpu_timing);
    // 释放全部资源（可重复调用）。
    void shutdown();
    bool ready() const { return swap_chain_ != nullptr; }

    // 交换链尺寸变化：按 design D7 的顺序 ResizeBuffers 并重建 RTV / D2D 目标。
    bool resize(UINT width, UINT height);
    UINT width() const { return width_; }
    UINT height() const { return height_; }

    // 画一帧并 Present(1, 0)。
    RenderResult render(const RenderParams& params);

    // 交换链的 frame-latency 可等待对象（SetMaximumFrameLatency(1)）；未初始化时为 nullptr。
    HANDLE frame_latency_handle() const { return latency_handle_; }
    // 主循环已经在 frame-latency 对象上等待成功（消耗了一次信号）：下一次 render 不必再等。
    void note_frame_ready() { frame_ready_ = true; }

    // 累计 Present 次数。
    std::uint64_t presents() const { return presents_; }
    // 收集已完成的 GPU 计时查询；距上次输出 ≥ 5 s 且有数据时写一行 debug：gpu frames=<n> avg_ms=<x> max_ms=<y>，
    // 后面附 Present 次数与耗时、因交换链 / GPU 未就绪而推迟的次数、Present 返回 WAS_STILL_DRAWING 的次数。
    void poll_gpu_stats(double now_s, bool flush = false);

private:
    bool create_device();
    bool create_swap_chain(HWND hwnd);
    bool create_targets();
    void release_targets();
    bool create_pipeline();
    bool create_text_resources();
    void ensure_text_formats(float scale);
    void warm_up_text();
    void draw_text(const RenderParams& p);
    void draw_toast(const RenderParams& p);
    void draw_debug_bar(const RenderParams& p);
    void draw_message(const RenderParams& p);
    void gpu_begin();
    void gpu_end();
    void gpu_collect();

    struct GpuQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };

    HWND hwnd_ = nullptr;
    UINT width_ = 0, height_ = 0;
    bool gpu_timing_ = false;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIDevice> dxgi_device_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    HANDLE latency_handle_ = nullptr;
    bool frame_ready_ = false;
    double not_ready_since_ = -1.0;
    ComPtr<ID3D11Query> frame_done_;   // D3D11_QUERY_EVENT，在每帧的绘制之后 End
    bool frame_pending_ = false;       // frame_done_ 已 End、还没确认完成
    ComPtr<IDCompositionDevice> dcomp_device_;
    ComPtr<IDCompositionTarget> dcomp_target_;
    ComPtr<IDCompositionVisual> dcomp_visual_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cbuffer_;
    ComPtr<ID3D11RasterizerState> raster_;

    // D2D / DWrite
    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_context_;
    ComPtr<ID2D1Bitmap1> d2d_target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> toast_format_, debug_format_, message_format_;
    float text_scale_ = 0.0f;
    i18n::Lang text_lang_ = i18n::Lang::ZhCN;   // 文字格式按哪种界面语言创建

    // GPU 计时（--verbose）
    std::array<GpuQuery, 4> gpu_queries_{};
    std::size_t gpu_next_ = 0;
    std::size_t gpu_current_ = SIZE_MAX;
    std::uint32_t gpu_frames_ = 0;
    double gpu_sum_ms_ = 0, gpu_max_ms_ = 0;
    double gpu_last_report_ = 0;
    // 按需渲染的统计（--verbose，随 gpu 行每 5 s 输出一次）
    struct FrameStats {
        std::uint32_t presents = 0, latency_waits = 0, gpu_waits = 0, present_busy = 0, fallbacks = 0;
        double present_ms_sum = 0, present_ms_max = 0;   // Present 调用本身的墙钟耗时
    } stats_;

    std::uint64_t presents_ = 0;
};

}  // namespace mc
