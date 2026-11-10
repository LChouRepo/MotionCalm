// gpu_downscale.cpp — GpuDownscaler 的实现：构建时预编译的全屏三角形 + 区域平均 PS（capture/downscale.hlsl）、3 槽 staging 环与 DO_NOT_WAIT 读回、时间戳查询。
#include "capture/gpu_downscale.h"

#include <cstddef>
#include <cstring>
#include <format>

// downscale.hlsl 的字节码（scripts\build_shaders.cmd 用 fxc 生成到 %MC_OBJ%\shaders\，const BYTE 数组；需要 <windows.h>，已由 gpu_downscale.h 引入）。
#include "downscale_ps.h"
#include "downscale_vs.h"

namespace mc {

namespace {

// 与 capture/downscale.hlsl 的 cbuffer Params 一一对应。
struct DownscaleConstants {
    float src_w, src_h;
    float cell_w, cell_h;
    float step_x, step_y;
    std::int32_t taps_x, taps_y;
};
static_assert(sizeof(DownscaleConstants) == 32, "cbuffer layout");

// SRV 不能用 TYPELESS 格式：换成对应的可采样格式。
DXGI_FORMAT srv_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

}  // namespace

bool GpuDownscaler::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool gpu_timing) {
    release();
    error_.clear();
    if (dev == nullptr || ctx == nullptr) {
        error_ = "no device";
        return false;
    }
    dev_ = dev;
    ctx_ = ctx;
    // 字节码是构建时编译进 exe 的静态数组（fxc /WX：有警告就构建失败），这里只在设备上创建着色器。
    HRESULT hr = dev_->CreateVertexShader(kDownscaleVS, sizeof(kDownscaleVS), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = dev_->CreatePixelShader(kDownscalePS, sizeof(kDownscalePS), nullptr, &ps_);
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dev_->CreateSamplerState(&sd, &sampler_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(DownscaleConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = dev_->CreateBuffer(&bd, nullptr, &cbuffer_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        hr = dev_->CreateRasterizerState(&rd, &raster_);
    }
    if (FAILED(hr)) {
        error_ = std::format("pipeline creation failed hr=0x{:08X}", static_cast<unsigned>(hr));
        release();
        return false;
    }
    timing_ = false;
    if (gpu_timing) {
        bool ok = true;
        for (TimingQuery& q : queries_) {
            const D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            const D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
            ok = ok && SUCCEEDED(dev_->CreateQuery(&dj, &q.disjoint)) && SUCCEEDED(dev_->CreateQuery(&ts, &q.begin)) &&
                 SUCCEEDED(dev_->CreateQuery(&ts, &q.end));
            q.in_flight = false;
        }
        timing_ = ok;
        if (!ok) {
            for (TimingQuery& q : queries_) q = TimingQuery{};
        }
    }
    return true;
}

bool GpuDownscaler::set_grid(int grid_w, int grid_h) {
    rtv_.Reset();
    rt_.Reset();
    for (Slot& s : slots_) s = Slot{};
    grid_w_ = grid_h_ = 0;
    if (!dev_ || grid_w <= 0 || grid_h <= 0) {
        error_ = "set_grid: no device or bad size";
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(grid_w);
    td.Height = static_cast<UINT>(grid_h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &rt_);
    if (SUCCEEDED(hr)) hr = dev_->CreateRenderTargetView(rt_.Get(), nullptr, &rtv_);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (Slot& s : slots_) {
        if (SUCCEEDED(hr)) hr = dev_->CreateTexture2D(&td, nullptr, &s.tex);
    }
    if (FAILED(hr)) {
        error_ = std::format("grid resources ({}x{}) failed hr=0x{:08X}", grid_w, grid_h, static_cast<unsigned>(hr));
        rtv_.Reset();
        rt_.Reset();
        for (Slot& s : slots_) s = Slot{};
        return false;
    }
    grid_w_ = grid_w;
    grid_h_ = grid_h;
    cb_src_w_ = cb_src_h_ = 0;   // 下一次 submit 重新计算布局
    return true;
}

void GpuDownscaler::release() {
    for (Slot& s : slots_) s = Slot{};
    for (TimingQuery& q : queries_) q = TimingQuery{};
    src_copy_.Reset();
    rtv_.Reset();
    rt_.Reset();
    raster_.Reset();
    cbuffer_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    ctx_.Reset();
    dev_.Reset();
    timing_ = false;
    grid_w_ = grid_h_ = 0;
    cb_src_w_ = cb_src_h_ = 0;
    layout_ = DownscaleLayout{};
}

HRESULT GpuDownscaler::ensure_source_copy(const D3D11_TEXTURE2D_DESC& d) {
    if (src_copy_) {
        D3D11_TEXTURE2D_DESC cd{};
        src_copy_->GetDesc(&cd);
        if (cd.Width == d.Width && cd.Height == d.Height && cd.Format == d.Format) return S_OK;
        src_copy_.Reset();
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = d.Width;
    td.Height = d.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = d.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return dev_->CreateTexture2D(&td, nullptr, &src_copy_);
}

int GpuDownscaler::oldest_pending() const {
    int best = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[i].pending && (best < 0 || slots_[i].seq < slots_[best].seq)) best = i;
    }
    return best;
}

int GpuDownscaler::pending() const {
    int n = 0;
    for (const Slot& s : slots_) n += s.pending ? 1 : 0;
    return n;
}

void GpuDownscaler::drop_pending() {
    for (Slot& s : slots_) s.pending = false;
}

HRESULT GpuDownscaler::submit(ID3D11Texture2D* src, double frame_time_s) {
    if (!ready() || src == nullptr) return E_UNEXPECTED;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (sd.Width == 0 || sd.Height == 0) return E_INVALIDARG;

    ID3D11Texture2D* tex = src;
    if ((sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || sd.SampleDesc.Count != 1 || sd.ArraySize != 1) {
        const HRESULT hr = ensure_source_copy(sd);
        if (FAILED(hr)) return hr;
        ctx_->CopyResource(src_copy_.Get(), src);
        tex = src_copy_.Get();
    }
    // 每帧新建 SRV：很便宜，也不必缓存可能被系统换掉的纹理指针（design D4）。
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = srv_format(sd.Format);
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MostDetailedMip = 0;
    vd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = dev_->CreateShaderResourceView(tex, &vd, &srv);
    if (FAILED(hr)) return hr;

    if (static_cast<int>(sd.Width) != cb_src_w_ || static_cast<int>(sd.Height) != cb_src_h_) {
        layout_ = downscale_layout(static_cast<int>(sd.Width), static_cast<int>(sd.Height), grid_w_, grid_h_);
        DownscaleConstants c{};
        c.src_w = static_cast<float>(sd.Width);
        c.src_h = static_cast<float>(sd.Height);
        c.cell_w = static_cast<float>(layout_.cell_w);
        c.cell_h = static_cast<float>(layout_.cell_h);
        c.step_x = static_cast<float>(layout_.step_x);
        c.step_y = static_cast<float>(layout_.step_y);
        c.taps_x = layout_.taps_x;
        c.taps_y = layout_.taps_y;
        ctx_->UpdateSubresource(cbuffer_.Get(), 0, nullptr, &c, 0, 0);
        cb_src_w_ = static_cast<int>(sd.Width);
        cb_src_h_ = static_cast<int>(sd.Height);
    }

    // 空闲槽；全部待读时丢弃最旧的（估计器会按时间戳跨过这一帧）。
    int k = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (!slots_[i].pending) {
            k = i;
            break;
        }
    }
    if (k < 0) {
        k = oldest_pending();
        slots_[k].pending = false;
        ++dropped_;
    }

    TimingQuery* tq = nullptr;
    if (timing_) {
        for (TimingQuery& q : queries_) {
            if (!q.in_flight) {
                tq = &q;
                break;
            }
        }
    }
    if (tq != nullptr) {
        ctx_->Begin(tq->disjoint.Get());
        ctx_->End(tq->begin.Get());
    }

    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(grid_w_);
    vp.Height = static_cast<float>(grid_h_);
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {srv.Get()};
    ctx_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    ctx_->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer* cbs[] = {cbuffer_.Get()};
    ctx_->PSSetConstantBuffers(0, 1, cbs);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* no_srv[] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, no_srv);   // 不让桌面纹理继续绑在管线上
    ID3D11RenderTargetView* no_rtv[] = {nullptr};
    ctx_->OMSetRenderTargets(1, no_rtv, nullptr);

    if (tq != nullptr) {
        ctx_->End(tq->end.Get());
        ctx_->End(tq->disjoint.Get());
        tq->in_flight = true;
    }

    ctx_->CopyResource(slots_[k].tex.Get(), rt_.Get());
    slots_[k].pending = true;
    slots_[k].seq = ++seq_;
    slots_[k].frame_time_s = frame_time_s;
    ctx_->Flush();
    return dev_->GetDeviceRemovedReason();
}

GpuDownscaler::Read GpuDownscaler::try_read(float* out, double* frame_time_s, HRESULT* out_hr) {
    if (out_hr != nullptr) *out_hr = S_OK;
    const int k = oldest_pending();
    if (k < 0) return Read::None;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx_->Map(slots_[k].tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return Read::Pending;
    if (FAILED(hr)) {
        slots_[k].pending = false;
        if (out_hr != nullptr) *out_hr = hr;
        return Read::Error;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(grid_w_) * sizeof(float);
    for (int y = 0; y < grid_h_; ++y) {
        std::memcpy(out + static_cast<std::size_t>(y) * static_cast<std::size_t>(grid_w_),
                    static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, row_bytes);
    }
    ctx_->Unmap(slots_[k].tex.Get(), 0);
    slots_[k].pending = false;
    if (frame_time_s != nullptr) *frame_time_s = slots_[k].frame_time_s;
    return Read::Ready;
}

int GpuDownscaler::poll_gpu_ms(double* sum_ms) {
    if (!timing_) return 0;
    int n = 0;
    for (TimingQuery& q : queries_) {
        if (!q.in_flight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx_->GetData(q.disjoint.Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx_->GetData(q.begin.Get(), &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx_->GetData(q.end.Get(), &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.in_flight = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        if (sum_ms != nullptr) *sum_ms += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        ++n;
    }
    return n;
}

}  // namespace mc
