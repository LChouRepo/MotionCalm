// renderer.cpp — overlay/renderer 的实现：D3D11 设备 + CreateSwapChainForComposition + DComp 目标 / visual + D3DCompile 着色器 + D2D/DWrite 文字 + GPU 时间戳。
#include "overlay/renderer.h"

#include "i18n/i18n.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "overlay/shaders.h"
#include "util/clock.h"
#include "util/log.h"

namespace mc {
namespace {

constexpr UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr double kNotReadyFallbackS = 0.100;   // latency 对象这么久不触发时照常渲染（Present 最多阻塞一帧）
constexpr double kGpuReportS = 5.0;

bool is_device_lost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
}

std::string_view blob_text(ID3DBlob* blob) {
    if (blob == nullptr || blob->GetBufferSize() == 0) return {};
    std::string_view s(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

D2D1_COLOR_F color(float r, float g, float b, float a) { return D2D1_COLOR_F{r, g, b, a}; }

}  // namespace

// ---------------------------------------------------------------- 初始化 / 释放

bool Renderer::init(HWND hwnd, UINT width, UINT height, bool gpu_timing) {
    shutdown();
    hwnd_ = hwnd;
    width_ = std::max<UINT>(1, width);
    height_ = std::max<UINT>(1, height);
    gpu_timing_ = gpu_timing;
    if (!create_device() || !compile_shaders() || !create_pipeline() || !create_swap_chain(hwnd)) {
        shutdown();
        return false;
    }
    if (!create_text_resources()) log::warn("renderer: text layer unavailable (toast / debug bar will not be drawn)");
    if (!create_targets()) {
        shutdown();
        return false;
    }
    warm_up_text();
    ComPtr<IDXGIAdapter> adapter;
    std::wstring adapter_name = L"?";
    if (SUCCEEDED(dxgi_device_->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) adapter_name = desc.Description;
    }
    log::info(L"renderer ready: adapter=\"{}\" feature_level=0x{:x} size={}x{}", adapter_name,
              static_cast<unsigned>(device_->GetFeatureLevel()), width_, height_);
    return true;
}

void Renderer::shutdown() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    release_targets();
    for (auto& q : gpu_queries_) q = GpuQuery{};
    gpu_next_ = 0;
    gpu_current_ = SIZE_MAX;
    brush_.Reset();
    toast_format_.Reset();
    debug_format_.Reset();
    message_format_.Reset();
    text_scale_ = 0.0f;
    d2d_context_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dwrite_.Reset();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    if (latency_handle_ != nullptr) {
        ::CloseHandle(latency_handle_);
        latency_handle_ = nullptr;
    }
    frame_ready_ = false;
    not_ready_since_ = -1.0;
    frame_done_.Reset();
    frame_pending_ = false;
    swap_chain_.Reset();
    raster_.Reset();
    cbuffer_.Reset();
    ps_.Reset();
    vs_.Reset();
    dxgi_device_.Reset();
    context_.Reset();
    device_.Reset();
}

bool Renderer::create_device() {
    static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // PREVENT_INTERNAL_THREADING_OPTIMIZATIONS：不要驱动的工作线程。我们每帧只有十来个调用，驱动线程的
    // 交接与自旋（实测占进程 CPU 周期的近一半）比工作本身贵得多。
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                     static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device_, nullptr,
                                     &context_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        // 没装 SDK Layers（Graphics Tools）时 DEBUG 标志会失败：去掉重试。
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                 static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device_, nullptr,
                                 &context_);
    }
    if (FAILED(hr)) {
        log::warn("D3D11CreateDevice(HARDWARE) failed hr=0x{:08X}; trying WARP", static_cast<unsigned>(hr));
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, kLevels,
                                 static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device_, nullptr,
                                 &context_);
    }
    if (FAILED(hr)) {
        log::warn("D3D11CreateDevice failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    hr = device_.As(&dxgi_device_);
    if (FAILED(hr)) {
        log::warn("QueryInterface(IDXGIDevice) failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool Renderer::compile_shaders() {
    if (vs_blob_ && ps_blob_) return true;   // 设备重建时复用
#ifdef NDEBUG
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const std::size_t len = std::strlen(shaders::kOverlayHlsl);
    struct Stage {
        const char* entry;
        const char* target;
        ComPtr<ID3DBlob>* out;
    };
    const Stage stages[] = {{"VSMain", "vs_4_0", &vs_blob_}, {"PSMain", "ps_4_0", &ps_blob_}};
    for (const Stage& st : stages) {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = ::D3DCompile(shaders::kOverlayHlsl, len, "overlay.hlsl", nullptr, nullptr, st.entry,
                                        st.target, flags, 0, &code, &errors);
        const std::string_view msg = blob_text(errors.Get());
        if (FAILED(hr)) {
            log::warn("D3DCompile {} ({}) failed hr=0x{:08X}: {}", st.entry, st.target, static_cast<unsigned>(hr),
                      msg);
            return false;
        }
        if (!msg.empty()) log::warn("D3DCompile {} ({}) warnings: {}", st.entry, st.target, msg);
        *st.out = code;
    }
    log::debug("shaders compiled: vs={} bytes ps={} bytes, no errors/warnings", vs_blob_->GetBufferSize(),
               ps_blob_->GetBufferSize());
    return true;
}

bool Renderer::create_pipeline() {
    HRESULT hr = device_->CreateVertexShader(vs_blob_->GetBufferPointer(), vs_blob_->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) {
        hr = device_->CreatePixelShader(ps_blob_->GetBufferPointer(), ps_blob_->GetBufferSize(), nullptr, &ps_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(OverlayConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&bd, nullptr, &cbuffer_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = device_->CreateRasterizerState(&rd, &raster_);
    }
    if (SUCCEEDED(hr)) {
        // 帧完成事件：下一帧只在它完成后才画（见 render）。
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        hr = device_->CreateQuery(&qd, &frame_done_);
    }
    if (FAILED(hr)) {
        log::warn("renderer pipeline creation failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool Renderer::create_swap_chain(HWND hwnd) {
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = dxgi_device_->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        log::warn("cannot get IDXGIFactory2 hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width_;
    d.Height = height_;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    d.Flags = kSwapFlags;
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForComposition(device_.Get(), &d, nullptr, &sc1);
    if (SUCCEEDED(hr)) hr = sc1.As(&swap_chain_);
    if (FAILED(hr)) {
        log::warn("CreateSwapChainForComposition failed hr=0x{:08X}", static_cast<unsigned>(hr));
        swap_chain_.Reset();
        return false;
    }
    hr = swap_chain_->SetMaximumFrameLatency(1);
    if (FAILED(hr)) log::warn("SetMaximumFrameLatency(1) failed hr=0x{:08X}", static_cast<unsigned>(hr));
    latency_handle_ = swap_chain_->GetFrameLatencyWaitableObject();

    hr = ::DCompositionCreateDevice(dxgi_device_.Get(), IID_PPV_ARGS(&dcomp_device_));
    if (SUCCEEDED(hr)) hr = dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_);
    if (SUCCEEDED(hr)) hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (SUCCEEDED(hr)) hr = dcomp_visual_->SetContent(swap_chain_.Get());
    if (SUCCEEDED(hr)) hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (SUCCEEDED(hr)) hr = dcomp_device_->Commit();
    if (FAILED(hr)) {
        log::warn("DirectComposition setup failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool Renderer::create_text_resources() {
    D2D1_FACTORY_OPTIONS opts{};
    opts.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                     reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_);
    if (SUCCEEDED(hr)) hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_context_);
    if (SUCCEEDED(hr)) {
        // 透明目标上不能用 ClearType。
        d2d_context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        hr = d2d_context_->CreateSolidColorBrush(color(1, 1, 1, 1), &brush_);
    }
    if (SUCCEEDED(hr)) {
        hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    }
    if (FAILED(hr)) {
        log::warn("D2D/DWrite setup failed hr=0x{:08X}", static_cast<unsigned>(hr));
        brush_.Reset();
        d2d_context_.Reset();
        d2d_device_.Reset();
        d2d_factory_.Reset();
        dwrite_.Reset();
        return false;
    }
    return true;
}

bool Renderer::create_targets() {
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        log::warn("render target creation failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }
    if (d2d_context_) {
        ComPtr<IDXGISurface> surface;
        hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&surface));
        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if (SUCCEEDED(hr)) hr = d2d_context_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &d2d_target_);
        if (SUCCEEDED(hr)) {
            d2d_context_->SetTarget(d2d_target_.Get());
            d2d_context_->SetDpi(96.0f, 96.0f);
        } else {
            log::warn("D2D target bitmap creation failed hr=0x{:08X}", static_cast<unsigned>(hr));
            d2d_target_.Reset();
        }
    }
    return true;
}

void Renderer::release_targets() {
    if (d2d_context_) d2d_context_->SetTarget(nullptr);
    d2d_target_.Reset();
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
}

bool Renderer::resize(UINT width, UINT height) {
    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    if (!ready()) {
        width_ = width;
        height_ = height;
        return false;
    }
    if (width == width_ && height == height_) return true;
    // design D7：1) 释放 D2D 目标与 RTV  2) ClearState + Flush  3) ResizeBuffers（flags 与创建时一致）  4) 重建
    release_targets();
    context_->ClearState();
    context_->Flush();
    const HRESULT hr = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kSwapFlags);
    if (FAILED(hr)) {
        log::warn("ResizeBuffers({}x{}) failed hr=0x{:08X}", width, height, static_cast<unsigned>(hr));
        return false;
    }
    width_ = width;
    height_ = height;
    log::debug("renderer resized to {}x{}", width_, height_);
    return create_targets();
}

// ---------------------------------------------------------------- 绘制

RenderResult Renderer::render(const RenderParams& p) {
    if (!ready() || !rtv_) return RenderResult::Failed;
    if (gpu_timing_) gpu_collect();

    // 只在"交换链腾出了位置"并且"上一帧的 GPU 工作已经完成"时才画。否则 Present 会在驱动里等待——
    // NVIDIA 的驱动是自旋等待：游戏 / 桌面让 GPU 忙起来时，实测每次 Present 白白烧掉 2–6 ms CPU。
    // 两个条件都不满足超过 kNotReadyFallbackS 时照常渲染（最坏退化为每 100 ms 一帧，不会卡住）。
    const double now = qpc_now_s();
    bool ready_now = true;
    if (!frame_ready_ && latency_handle_ != nullptr) {
        if (::WaitForSingleObjectEx(latency_handle_, 0, FALSE) == WAIT_OBJECT_0) {
            frame_ready_ = true;   // 消耗了一个 latency 名额，直到真正 Present 前都保留它
        } else {
            ready_now = false;
            ++stats_.latency_waits;
        }
    }
    if (ready_now && frame_pending_ && frame_done_) {
        // 不带 DONOTFLUSH：上一帧的命令如果还在命令缓冲里（例如 Present 返回了 WAS_STILL_DRAWING），先提交它。
        if (context_->GetData(frame_done_.Get(), nullptr, 0, 0) == S_OK) {
            frame_pending_ = false;
        } else {
            ready_now = false;
            ++stats_.gpu_waits;
        }
    }
    if (!ready_now) {
        if (not_ready_since_ < 0) not_ready_since_ = now;
        if (now - not_ready_since_ < kNotReadyFallbackS) return RenderResult::NotReady;
    }
    // 等了太久：这一帧允许 Present 阻塞（不带 DO_NOT_WAIT）。
    const bool fallback = not_ready_since_ >= 0 && now - not_ready_since_ >= kNotReadyFallbackS;

    if (gpu_timing_) gpu_begin();
    context_->UpdateSubresource(cbuffer_.Get(), 0, nullptr, &p.constants, 0, 0);
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);   // 着色器直接写最终预乘颜色：不混合、不 Clear
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cbuffer_.Get();
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->Draw(3, 0);

    HRESULT hr = S_OK;
    if (d2d_context_ && d2d_target_ && (!p.toast.empty() || p.debug_bar || !p.message.empty())) {
        d2d_context_->BeginDraw();
        d2d_context_->SetTransform(D2D1_MATRIX_3X2_F{1, 0, 0, 1, 0, 0});
        draw_text(p);
        hr = d2d_context_->EndDraw();
    }
    if (gpu_timing_) gpu_end();
    if (frame_done_) {
        context_->End(frame_done_.Get());
        frame_pending_ = true;
    }
    if (is_device_lost(hr)) {
        log::warn("D2D EndDraw reported device loss hr=0x{:08X}", static_cast<unsigned>(hr));
        return RenderResult::DeviceLost;
    }
    if (FAILED(hr)) log::warn("D2D EndDraw failed hr=0x{:08X}", static_cast<unsigned>(hr));

    // DO_NOT_WAIT：驱动若还要等（自旋）就返回 WAS_STILL_DRAWING，这一帧留到下个节拍；等太久时才允许阻塞。
    const double present_t0 = gpu_timing_ ? qpc_now_s() : 0.0;
    hr = swap_chain_->Present(1, fallback ? 0u : static_cast<UINT>(DXGI_PRESENT_DO_NOT_WAIT));
    if (gpu_timing_) {
        const double ms = (qpc_now_s() - present_t0) * 1000.0;
        stats_.present_ms_sum += ms;
        stats_.present_ms_max = std::max(stats_.present_ms_max, ms);
    }
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        // 驱动要等：这次不 Present（latency 名额保留），下一个节拍重画再试。
        ++stats_.present_busy;
        if (not_ready_since_ < 0) not_ready_since_ = now;
        return RenderResult::NotReady;
    }
    frame_ready_ = false;
    not_ready_since_ = -1.0;
    if (is_device_lost(hr)) {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        log::warn("Present reported device loss hr=0x{:08X} reason=0x{:08X}", static_cast<unsigned>(hr),
                  static_cast<unsigned>(reason));
        return RenderResult::DeviceLost;
    }
    if (FAILED(hr)) {
        log::warn("Present failed hr=0x{:08X}", static_cast<unsigned>(hr));
        return RenderResult::Failed;
    }
    ++presents_;
    ++stats_.presents;
    if (fallback) ++stats_.fallbacks;
    return RenderResult::Presented;
}

void Renderer::ensure_text_formats(float scale) {
    const i18n::Lang lang = i18n::current();   // localization：字体与排版语言随界面语言
    if (!dwrite_ || (scale == text_scale_ && lang == text_lang_ && toast_format_ && debug_format_ && message_format_)) {
        return;
    }
    toast_format_.Reset();
    debug_format_.Reset();
    message_format_.Reset();
    text_scale_ = scale;
    text_lang_ = lang;
    const wchar_t* font = i18n::tr(i18n::StrId::FontUi);   // 雅黑 / 正黑 / Segoe UI（缺字时 DirectWrite 自动回退）
    const wchar_t* locale = lang == i18n::Lang::En ? L"en-US" : i18n::lang_code(lang);
    HRESULT hr = dwrite_->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 20.0f * scale, locale, &toast_format_);
    if (SUCCEEDED(hr)) {
        toast_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        toast_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        hr = dwrite_->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, locale, &debug_format_);
    }
    if (SUCCEEDED(hr)) {
        debug_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        debug_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        debug_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // 校准提示（overlay-rendering spec "校准提示"）：18 DIP，每行水平居中。
        hr = dwrite_->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 18.0f * scale, locale, &message_format_);
    }
    if (SUCCEEDED(hr)) {
        message_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        message_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (FAILED(hr)) {
        log::warn("CreateTextFormat failed hr=0x{:08X}", static_cast<unsigned>(hr));
        toast_format_.Reset();
        debug_format_.Reset();
        message_format_.Reset();
    }
}

void Renderer::warm_up_text() {
    // 第一次排版中文会加载字体文件并栅格化字形（约 100+ ms）：在初始化时做掉，避免第一条 toast 卡一下。
    // 画到后台缓冲上、alpha 为 0；下一帧的着色器会覆盖每个像素。
    if (!d2d_context_ || !d2d_target_ || !dwrite_) return;
    const double t0 = qpc_now_s();
    ensure_text_formats(1.0f);
    if (!toast_format_ || !debug_format_ || !message_format_ || !brush_) return;
    // 按当前界面语言拼一段样本：常见 toast、调试条、校准提示里的字形。
    std::wstring sample = L"0123456789°/s · c0.93 Ctrl+Alt+F7 MotionCalm ";
    using i18n::StrId;
    for (const StrId id : {StrId::ToastMaskLevel, StrId::LevelLight, StrId::LevelMedium, StrId::LevelStrong, StrId::ToastMaskOn,
                           StrId::ToastMaskOff, StrId::ToastSensitivity, StrId::SensLow, StrId::SensHigh,
                           StrId::ToastCenterDot, StrId::ToastEdgeDots, StrId::ToastDebug, StrId::On, StrId::Off,
                           StrId::DebugLine, StrId::DebugMoving, StrId::DebugStill, StrId::DebugLocked,
                           StrId::DebugUnlocked, StrId::DebugExcluded, StrId::DebugNotGame, StrId::DebugCaptureForward,
                           StrId::DebugCapturePaused, StrId::DebugCaptureFailed, StrId::ToastHotkeysOccupied,
                           StrId::ToastAlreadyRunning, StrId::ToastConfigWriteFailed, StrId::CalibRun1,
                           StrId::CalibBetween, StrId::CalibDone, StrId::CalibBadCount, StrId::CalibInconsistent,
                           StrId::CalibTimeout, StrId::CalibLeftGame, StrId::ToastCapture, StrId::ToastCaptureCliDisabled}) {
        sample += i18n::tr(id);
        sample += L' ';
    }
    d2d_context_->BeginDraw();
    brush_->SetColor(color(1, 1, 1, 0));
    for (IDWriteTextFormat* fmt : {toast_format_.Get(), debug_format_.Get(), message_format_.Get()}) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_->CreateTextLayout(sample.c_str(), static_cast<UINT32>(sample.size()), fmt, 4000.0f,
                                                200.0f, &layout))) {
            d2d_context_->DrawTextLayout(D2D1_POINT_2F{0, 0}, layout.Get(), brush_.Get(),
                                         D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
    const HRESULT hr = d2d_context_->EndDraw();
    log::debug("text layer warm-up {:.1f} ms hr=0x{:08X}", (qpc_now_s() - t0) * 1000.0, static_cast<unsigned>(hr));
}

void Renderer::draw_text(const RenderParams& p) {
    ensure_text_formats(p.dpi_scale);
    if (!toast_format_ || !debug_format_ || !message_format_ || !brush_) return;
    if (p.debug_bar) draw_debug_bar(p);
    if (!p.toast.empty() && p.toast_opacity > 0.0f) draw_toast(p);
    if (!p.message.empty()) draw_message(p);   // 在 toast 之后画：toast 在顶部，提示在其下方，互不遮挡
}

void Renderer::draw_message(const RenderParams& p) {
    // overlay-rendering spec "校准提示"：文字块水平居中，上沿在 overlay 高度的 22% 处；每行居中，
    // "Microsoft YaHei UI" 18 DIP，白色；背景为 alpha 0.6 的黑色圆角矩形，四周 12 DIP 内边距（均乘 DPI / 96）。
    // 在着色器之后绘制，所以不受遮罩强度影响。
    const float s = p.dpi_scale;
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(p.message.c_str(), static_cast<UINT32>(p.message.size()),
                                         message_format_.Get(), w * 0.8f, h * 0.5f, &layout))) {
        return;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float pad = 12.0f * s;
    const float box_w = std::ceil(m.width + 2.0f * pad);
    const float box_h = std::ceil(m.height + 2.0f * pad);
    const float x = std::round((w - box_w) * 0.5f);
    const float y = std::round(h * 0.22f);
    const D2D1_ROUNDED_RECT rr{D2D1_RECT_F{x, y, x + box_w, y + box_h}, 8.0f * s, 8.0f * s};
    brush_->SetColor(color(0, 0, 0, 0.60f));
    d2d_context_->FillRoundedRectangle(rr, brush_.Get());
    brush_->SetColor(color(1, 1, 1, 1));
    // 布局框宽 0.8 W、各行在框内居中：把最宽一行（左端在 m.left）放到背景框正中，整块就在屏幕上水平居中。
    const float tx = x + (box_w - m.width) * 0.5f - m.left;
    d2d_context_->DrawTextLayout(D2D1_POINT_2F{tx, y + pad - m.top}, layout.Get(), brush_.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::draw_toast(const RenderParams& p) {
    const float s = p.dpi_scale;
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(p.toast.c_str(), static_cast<UINT32>(p.toast.size()), toast_format_.Get(),
                                         w * 0.8f, h * 0.5f, &layout))) {
        return;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float pad_x = 18.0f * s, pad_y = 9.0f * s;
    const float box_w = m.width + 2.0f * pad_x;
    const float box_h = m.height + 2.0f * pad_y;
    const float x = (w - box_w) * 0.5f;
    const float y = 56.0f * s;
    const float a = std::clamp(p.toast_opacity, 0.0f, 1.0f);
    const D2D1_ROUNDED_RECT rr{D2D1_RECT_F{x, y, x + box_w, y + box_h}, 8.0f * s, 8.0f * s};
    brush_->SetColor(color(0, 0, 0, 0.80f * a));
    d2d_context_->FillRoundedRectangle(rr, brush_.Get());
    brush_->SetColor(color(1, 1, 1, a));
    d2d_context_->DrawTextLayout(D2D1_POINT_2F{x + pad_x - m.left, y + pad_y - m.top}, layout.Get(), brush_.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Renderer::draw_debug_bar(const RenderParams& p) {
    const float s = p.dpi_scale;
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(p.debug_text.c_str(), static_cast<UINT32>(p.debug_text.size()),
                                         debug_format_.Get(), w * 0.95f, h * 0.2f, &layout))) {
        return;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const float bar_w = 360.0f * s, bar_h = 6.0f * s, gap = 5.0f * s, pad = 10.0f * s;
    const float content_w = std::max(bar_w, m.width);
    const float pw = content_w + 2.0f * pad;
    const float ph = pad + bar_h + gap + bar_h + gap + m.height + pad;
    const float px = (w - pw) * 0.5f;
    const float py = h - 48.0f * s - ph;
    brush_->SetColor(color(0, 0, 0, 0.62f));
    d2d_context_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1_RECT_F{px, py, px + pw, py + ph}, 6.0f * s, 6.0f * s},
                                       brush_.Get());
    const float bx = (w - bar_w) * 0.5f;
    const float by1 = py + pad;
    const float by2 = by1 + bar_h + gap;
    const float t = std::clamp(p.debug_target, 0.0f, 1.0f);
    const float v = std::clamp(p.debug_value, 0.0f, 1.0f);
    brush_->SetColor(color(1, 1, 1, 0.18f));
    d2d_context_->FillRectangle(D2D1_RECT_F{bx, by1, bx + bar_w, by1 + bar_h}, brush_.Get());
    d2d_context_->FillRectangle(D2D1_RECT_F{bx, by2, bx + bar_w, by2 + bar_h}, brush_.Get());
    brush_->SetColor(color(1.0f, 0.70f, 0.25f, 0.95f));   // target
    if (t > 0) d2d_context_->FillRectangle(D2D1_RECT_F{bx, by1, bx + bar_w * t, by1 + bar_h}, brush_.Get());
    brush_->SetColor(color(0.35f, 0.85f, 1.0f, 0.95f));   // value
    if (v > 0) d2d_context_->FillRectangle(D2D1_RECT_F{bx, by2, bx + bar_w * v, by2 + bar_h}, brush_.Get());
    const float ty = by2 + bar_h + gap;
    const float tx = (w - w * 0.95f) * 0.5f;
    brush_->SetColor(color(1, 1, 1, 0.95f));
    d2d_context_->DrawTextLayout(D2D1_POINT_2F{tx, ty - m.top}, layout.Get(), brush_.Get(),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// ---------------------------------------------------------------- GPU 计时

void Renderer::gpu_begin() {
    gpu_current_ = SIZE_MAX;
    GpuQuery& q = gpu_queries_[gpu_next_];
    if (q.pending) return;   // 4 帧前的结果还没回来：这一帧不计时
    if (!q.disjoint) {
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device_->CreateQuery(&dj, &q.disjoint)) || FAILED(device_->CreateQuery(&ts, &q.begin)) ||
            FAILED(device_->CreateQuery(&ts, &q.end))) {
            q = GpuQuery{};
            gpu_timing_ = false;
            log::warn("GPU timestamp queries unavailable; gpu timing disabled");
            return;
        }
    }
    context_->Begin(q.disjoint.Get());
    context_->End(q.begin.Get());
    gpu_current_ = gpu_next_;
}

void Renderer::gpu_end() {
    if (gpu_current_ == SIZE_MAX) return;
    GpuQuery& q = gpu_queries_[gpu_current_];
    context_->End(q.end.Get());
    context_->End(q.disjoint.Get());
    q.pending = true;
    gpu_next_ = (gpu_next_ + 1) % gpu_queries_.size();
    gpu_current_ = SIZE_MAX;
}

void Renderer::gpu_collect() {
    if (!context_) return;
    for (GpuQuery& q : gpu_queries_) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (context_->GetData(q.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (context_->GetData(q.begin.Get(), &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (context_->GetData(q.end.Get(), &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        ++gpu_frames_;
        gpu_sum_ms_ += ms;
        gpu_max_ms_ = std::max(gpu_max_ms_, ms);
    }
}

void Renderer::poll_gpu_stats(double now_s, bool flush) {
    if (!gpu_timing_ || !context_) return;
    gpu_collect();
    if (gpu_last_report_ <= 0) gpu_last_report_ = now_s;
    if (!flush && now_s - gpu_last_report_ < kGpuReportS) return;
    if (gpu_frames_ > 0 || stats_.presents > 0 || stats_.latency_waits + stats_.gpu_waits + stats_.present_busy > 0) {
        log::debug("gpu frames={} avg_ms={:.3f} max_ms={:.3f} presents={} present_avg_ms={:.3f} present_max_ms={:.3f} "
                   "not_ready_latency={} not_ready_gpu={} present_busy={} fallbacks={}",
                   gpu_frames_, gpu_frames_ > 0 ? gpu_sum_ms_ / gpu_frames_ : 0.0, gpu_max_ms_, stats_.presents,
                   stats_.presents > 0 ? stats_.present_ms_sum / static_cast<double>(stats_.presents) : 0.0,
                   stats_.present_ms_max, stats_.latency_waits, stats_.gpu_waits, stats_.present_busy,
                   stats_.fallbacks);
    }
    stats_ = FrameStats{};
    gpu_frames_ = 0;
    gpu_sum_ms_ = 0;
    gpu_max_ms_ = 0;
    gpu_last_report_ = now_s;
}

}  // namespace mc
