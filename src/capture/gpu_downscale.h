// gpu_downscale.h — 截屏线程的 GPU 降采样：桌面纹理 → grid_w×grid_h R32_FLOAT 灰度（每格 Tx×Ty 个双线性样本的区域平均）→ 3 槽 staging 环 → 不等待的读回；可选 GPU 时间戳。
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>

#include "capture/capture_policy.h"
#include "util/win.h"

namespace mc {

// 降采样着色器源码（VSMain：SV_VertexID 全屏三角形；PSMain：区域平均灰度）。供测试检查编译结果。
extern const char kDownscaleHlsl[];

// 只在创建它的线程上使用（截屏线程有自己的 D3D11 设备，见 design D2）。非线程安全。
class GpuDownscaler {
public:
    static constexpr int kSlots = 3;

    GpuDownscaler() = default;
    ~GpuDownscaler() { release(); }
    GpuDownscaler(const GpuDownscaler&) = delete;
    GpuDownscaler& operator=(const GpuDownscaler&) = delete;

    // 在 dev 上创建着色器、采样器、常量缓冲、光栅化状态；gpu_timing 为真时再建时间戳查询（失败只是关掉计时）。
    // 着色器字节码在 release() 后保留，下次 init 不再编译。失败返回 false，原因在 error()。
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool gpu_timing);
    // （重新）创建 grid_w×grid_h 的渲染目标与 staging 环；丢弃未读回的槽。
    bool set_grid(int grid_w, int grid_h);
    // 释放所有设备对象（保留编译好的字节码）。
    void release();

    bool ready() const { return rtv_ != nullptr; }
    int grid_w() const { return grid_w_; }
    int grid_h() const { return grid_h_; }
    bool gpu_timing() const { return timing_; }
    const DownscaleLayout& layout() const { return layout_; }
    const std::string& error() const { return error_; }
    // 着色器编译的警告文本（正常为空）。
    const std::string& compile_messages() const { return compile_messages_; }

    // 把 src（整张纹理）降采样到渲染目标，复制进下一个空闲 staging 槽并 Flush；frame_time_s 随槽保存。
    // 所有槽都在等读回时丢弃最旧的一个（计入 dropped()）。src 没有 SHADER_RESOURCE 绑定时先复制到内部纹理。
    // 返回 S_OK 或失败的 HRESULT（设备移除等，调用方应重建）。
    HRESULT submit(ID3D11Texture2D* src, double frame_time_s);

    enum class Read { None, Ready, Pending, Error };
    // 用 D3D11_MAP_FLAG_DO_NOT_WAIT 映射最旧的待读槽：
    //   Ready   out 写入 grid_w×grid_h 个 float（行主序），*frame_time_s 为该帧时间；
    //   Pending GPU 还没画完（DXGI_ERROR_WAS_STILL_DRAWING），留到下一轮；
    //   None    没有待读的槽；
    //   Error   Map 失败（*hr），该槽被丢弃。
    Read try_read(float* out, double* frame_time_s, HRESULT* hr = nullptr);
    // 丢弃所有待读的槽（暂停 / 换参考帧时）。
    void drop_pending();
    int pending() const;
    std::uint64_t dropped() const { return dropped_; }

    // 非阻塞收取已完成的 GPU 计时（D3D11_ASYNC_GETDATA_DONOTFLUSH）：返回新样本数，累加毫秒到 *sum_ms。
    int poll_gpu_ms(double* sum_ms);

private:
    struct Slot {
        ComPtr<ID3D11Texture2D> tex;
        bool pending = false;
        std::uint64_t seq = 0;
        double frame_time_s = 0;
    };
    struct TimingQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool in_flight = false;
    };

    bool compile();
    HRESULT ensure_source_copy(const D3D11_TEXTURE2D_DESC& d);
    int oldest_pending() const;

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3DBlob> vs_blob_, ps_blob_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cbuffer_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Texture2D> rt_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> src_copy_;   // 源纹理不能直接建 SRV 时的中转
    Slot slots_[kSlots];
    TimingQuery queries_[4];
    bool timing_ = false;
    int grid_w_ = 0, grid_h_ = 0;
    int cb_src_w_ = 0, cb_src_h_ = 0;   // 常量缓冲对应的源尺寸（变化时才更新）
    DownscaleLayout layout_;
    std::uint64_t seq_ = 0;
    std::uint64_t dropped_ = 0;
    std::string error_;
    std::string compile_messages_;
};

}  // namespace mc
