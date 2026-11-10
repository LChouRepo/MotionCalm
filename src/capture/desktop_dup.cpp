// desktop_dup.cpp — CaptureService 的实现：显示器 → 适配器 / 输出匹配、截屏线程自己的 D3D11 设备、DuplicateOutput(1)、取帧循环（高刷新率抽帧）、暂停 / 释放、失败恢复、性能日志、网格回调。
#include "capture/desktop_dup.h"

#include <d3d11.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cmath>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "capture/cpu_clock.h"
#include "capture/gpu_downscale.h"
#include "capture/motion_estimator.h"
#include "capture/similarity_estimator.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/win.h"

namespace mc {

static_assert(std::atomic<double>::is_always_lock_free, "speed handoff must be lock-free");
static_assert(std::atomic<HMONITOR>::is_always_lock_free, "target handoff must be lock-free");
static_assert(std::atomic<CaptureState>::is_always_lock_free, "state handoff must be lock-free");

namespace {

unsigned hrx(HRESULT hr) { return static_cast<unsigned>(hr); }

int rotation_degrees(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_ROTATE90: return 90;
        case DXGI_MODE_ROTATION_ROTATE180: return 180;
        case DXGI_MODE_ROTATION_ROTATE270: return 270;
        default: return 0;
    }
}

// 变更 5：低于这两个流速时相似变换估计直接用短基线链的结果（不做长基线精化），见 SimilarityParams。
// 前进 / 倾斜有了独立的低阈值（最低一档的起点：前进 10 °/s、倾斜 8 °/s）后，门槛取 4 °/s，约为最低起点的一半。
constexpr double kRefineMinZoomDegPerS = 4.0;
constexpr double kRefineMinRollDegPerS = 4.0;

DWORD wait_ms(double seconds) {
    if (!(seconds > 0)) return 1;
    return static_cast<DWORD>(std::min(std::ceil(seconds * 1000.0), 3600000.0));
}

// 一个桌面复制会话：目标显示器所属适配器上的独立 D3D11 设备 + IDXGIOutputDuplication（design D2 / D3）。
struct DupSession {
    HMONITOR monitor = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    DXGI_OUTDUPL_DESC desc{};
    std::wstring adapter_name, output_name;
    int desktop_w = 0, desktop_h = 0;   // DXGI_OUTPUT_DESC.DesktopCoordinates（桌面方向）
    int rotation_deg = 0;
    bool dup1 = false;                  // 走了 IDXGIOutput5::DuplicateOutput1
    bool frame_held = false;
    double opened_s = 0;
    bool confirmed = false;             // 交付过帧，或存活超过 2 s

    bool open() const { return dup != nullptr; }

    void close() {
        if (dup && frame_held) dup->ReleaseFrame();
        frame_held = false;
        dup.Reset();
        if (context) {
            context->ClearState();
            context->Flush();
        }
        context.Reset();
        device.Reset();
        monitor = nullptr;
        confirmed = false;
    }

    HRESULT build(HMONITOR mon) {
        close();
        dup1 = false;
        if (mon == nullptr) return E_INVALIDARG;
        // 每次都新建 factory：显示配置变化后旧 factory 失效（IsCurrent() == false）。
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        DXGI_OUTPUT_DESC od{};
        for (UINT i = 0; !output; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> o;
                if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC d{};
                if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == mon) {
                    adapter = a;
                    output = o;
                    od = d;
                    break;
                }
            }
        }
        if (!output) return DXGI_ERROR_NOT_FOUND;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        adapter_name = ad.Description;
        output_name = od.DeviceName;
        desktop_w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
        desktop_h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;

        static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                    D3D_FEATURE_LEVEL_10_0};
        // 与 overlay 相同：每帧只有十来个调用，驱动工作线程的交接比工作本身更贵。
        UINT flags = D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels,
                                 static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
            flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);   // 没装 SDK Layers
            hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels,
                                     static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &device, nullptr,
                                     &context);
        }
        if (FAILED(hr)) {
            close();
            return hr;
        }
        ComPtr<IDXGIOutput1> out1;
        hr = output.As(&out1);
        if (SUCCEEDED(hr)) hr = out1->DuplicateOutput(device.Get(), &dup);
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            // 个别 HDR / FP16 桌面模式只支持 DuplicateOutput1；着色器对两种格式都按 float 采样。
            ComPtr<IDXGIOutput5> out5;
            if (SUCCEEDED(output.As(&out5))) {
                const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
                hr = out5->DuplicateOutput1(device.Get(), 0, static_cast<UINT>(std::size(formats)), formats, &dup);
                dup1 = SUCCEEDED(hr);
            }
        }
        if (FAILED(hr)) {
            close();
            return hr;
        }
        dup->GetDesc(&desc);
        rotation_deg = rotation_degrees(desc.Rotation);
        monitor = mon;
        opened_s = qpc_now_s();
        confirmed = false;
        return S_OK;
    }
};

}  // namespace

// ---------------------------------------------------------------- Impl

struct CaptureService::Impl {
    // ---- 两个线程共享（原子 / 互斥量）
    std::atomic<bool> stop{false};
    std::atomic<HMONITOR> target{nullptr};
    std::atomic<bool> feature_on{false};
    std::atomic<bool> wanted{false};
    std::atomic<std::uint64_t> feature_epoch{0};   // feature_on 每次 false → true 加一
    std::atomic<CaptureState> state{CaptureState::Off};
    std::atomic<double> speed{0.0};
    std::atomic<double> zoom{0.0};      // 变更 5：与 speed 同一发布时刻 speed_t
    std::atomic<double> roll{0.0};
    std::atomic<double> speed_t{-1e300};
    std::mutex params_mu;
    CaptureParams params;   // params_mu 保护
    std::atomic<std::uint64_t> params_gen{0};
    mutable std::mutex debug_mu;
    CaptureDebug dbg;       // debug_mu 保护
    UniqueHandle event;     // 自动重置；set_target / update_params / stop 时 SetEvent
    std::thread thread;

    // ---- 只在截屏线程使用
    DupSession session;
    GpuDownscaler down;
    MotionEstimator estimator;
    SimilarityEstimator similarity;   // 变更 5
    double cur_hfov_for_refine = -1;
    SpeedHold hold;
    SpeedHold zoom_hold, roll_hold;   // 变更 5
    RebuildPolicy policy;
    FrameRateLimiter limiter;   // 高刷新率显示器上只处理约每 13.75 ms 一帧
    GridSink sink;              // start() 时从 CaptureService 复制；空 = 无
    PerfWindow perf;
    CaptureParams cur;
    bool configured = false;
    std::vector<float> gray;
    OutputGeometry geom;
    int tex_w = 0, tex_h = 0;
    int discard = 0;
    bool failed = false;
    double paused_since = -1;
    double next_retry = 0;
    bool first_grid_logged = false;
    double last_cpu_sample_s = 0;
    CaptureDebug local;   // 截屏线程的工作副本，每轮循环复制进 dbg

    void run();
    void wait(DWORD ms) { ::WaitForSingleObject(event.get(), ms); }
    void set_state(CaptureState s, double now);
    void publish(double v, double now);   // 同时发布前进 / 倾斜的 SpeedHold::value(now)
    void reset_holds();
    void sync_debug();
    void apply_params();
    void update_hold_geometry();
    void enter_idle(CaptureState s, double now);
    bool open_session(HMONITOR mon);
    void close_session(const char* reason);
    void on_build_failed(HRESULT hr, const char* what);
    void session_lost(HRESULT hr, const char* what);
    void step();
    void release_frame();
    void drain();
    void emit_grid(double frame_t);
    void perf_tick(double now);
    void log_first_grid();
};

void CaptureService::Impl::set_state(CaptureState s, double now) {
    const CaptureState prev = state.load(std::memory_order_relaxed);
    if (prev == s) return;
    if (s == CaptureState::Running) {
        const double cpu = thread_cpu_seconds();
        perf.reset(now, cpu);   // 只统计取帧期间；暂停 / 重建后重新开窗口
    }
    state.store(s, std::memory_order_release);
    local.state = s;
    log::debug("capture state: {} -> {}", capture_state_name(prev), capture_state_name(s));
}

void CaptureService::Impl::publish(double v, double now) {
    const double z = zoom_hold.value(now);
    const double r = cur.detect_roll ? roll_hold.value(now) : 0.0;
    speed.store(v, std::memory_order_release);
    zoom.store(z, std::memory_order_release);
    roll.store(r, std::memory_order_release);
    speed_t.store(now, std::memory_order_release);
    local.published_deg_per_s = v;
    local.zoom_deg_per_s = z;
    local.roll_deg_per_s = r;
    if (state.load(std::memory_order_relaxed) == CaptureState::Running) {
        perf.speed_sum += v;
        ++perf.speed_samples;
    }
}

void CaptureService::Impl::reset_holds() {
    hold.reset();
    zoom_hold.reset();
    roll_hold.reset();
}

void CaptureService::Impl::sync_debug() {
    std::lock_guard lock(debug_mu);
    dbg = local;
}

void CaptureService::Impl::update_hold_geometry() {
    SpeedHoldParams hp;
    hp.grid_w = geom.swap_axes ? cur.grid_h : cur.grid_w;
    hp.grid_h = geom.swap_axes ? cur.grid_w : cur.grid_h;
    hp.screen_w_px = geom.screen_w;
    hp.screen_h_px = geom.screen_h;
    hp.assumed_hfov_deg = cur.assumed_hfov_deg;
    hp.hold_ms = cur.hold_ms;
    hold.configure(hp);   // 不清除已有测量
    zoom_hold.configure(hp);
    roll_hold.configure(hp);
    // 相似变换估计在纹理方向的网格上工作：每格在屏幕上的高 / 宽（纹理宽高即扫描输出方向）。
    if (tex_w > 0 && tex_h > 0 && cur.grid_w > 0 && cur.grid_h > 0) {
        const double asp = (static_cast<double>(tex_h) / cur.grid_h) / (static_cast<double>(tex_w) / cur.grid_w);
        if (std::fabs(asp - similarity.params().pixel_aspect) > 1e-9) {
            SimilarityParams sp = similarity.params();
            sp.pixel_aspect = asp;
            similarity.configure(sp);   // 丢弃历史帧
        }
    }
    local.grid_w = cur.grid_w;
    local.grid_h = cur.grid_h;
    local.screen_w = geom.screen_w;
    local.screen_h = geom.screen_h;
}

void CaptureService::Impl::apply_params() {
    CaptureParams p;
    {
        std::lock_guard lock(params_mu);
        p = params;
    }
    const bool grid_changed = !same_grid(p, cur);
    const bool estimator_changed =
        !configured || grid_changed || p.max_shift != cur.max_shift || p.min_correlation != cur.min_correlation;
    const bool similarity_changed =
        !configured || grid_changed || p.min_correlation != cur.min_correlation || p.baseline_ms != cur.baseline_ms;
    cur = p;
    if (estimator_changed) {
        MotionEstimatorParams ep;
        ep.width = p.grid_w;
        ep.height = p.grid_h;
        ep.max_shift = p.max_shift;
        ep.min_correlation = p.min_correlation;
        estimator.configure(ep);   // 同时丢弃参考帧
        gray.assign(static_cast<std::size_t>(p.grid_w) * static_cast<std::size_t>(p.grid_h), 0.0f);
    }
    if (similarity_changed) {
        SimilarityParams sp = similarity.params();
        sp.width = p.grid_w;
        sp.height = p.grid_h;
        sp.min_confidence = p.min_correlation;
        sp.baseline_s = p.baseline_ms / 1000.0;
        similarity.configure(sp);   // 丢弃历史帧
    }
    if (!configured || p.assumed_hfov_deg != cur_hfov_for_refine) {
        // 省算力的门槛（见 SimilarityParams）：前进流速 kRefineMinZoomDegPerS、倾斜 kRefineMinRollDegPerS 以下不做长基线精化。
        SimilarityParams sp = similarity.params();
        sp.refine_min_log_rate = kRefineMinZoomDegPerS / (p.assumed_hfov_deg / 2.0);
        sp.refine_min_deg_rate = kRefineMinRollDegPerS;
        similarity.set_refine_thresholds(sp.refine_min_log_rate, sp.refine_min_deg_rate);
        cur_hfov_for_refine = p.assumed_hfov_deg;
    }
    if (configured && grid_changed && session.open()) {
        down.drop_pending();
        if (down.set_grid(p.grid_w, p.grid_h)) {
            log::info("capture grid rebuilt: {}x{}", p.grid_w, p.grid_h);
        } else {
            log::warn("capture grid rebuild failed: {}", down.error());
            session_lost(E_OUTOFMEMORY, "grid rebuild");
        }
    }
    configured = true;
    update_hold_geometry();
}

void CaptureService::Impl::enter_idle(CaptureState s, double now) {
    const CaptureState prev = state.load(std::memory_order_relaxed);
    if (prev == CaptureState::Running || prev == CaptureState::Recovering) {
        down.drop_pending();
        estimator.reset();   // 恢复取帧后的第一帧只作参考帧
        similarity.reset();
        limiter.reset();     // 恢复取帧后的第一帧一定处理
    }
    reset_holds();
    set_state(s, now);
    publish(0.0, now);
}

void CaptureService::Impl::close_session(const char* reason) {
    const bool was_open = session.open();
    down.release();
    session.close();
    estimator.reset();
    similarity.reset();
    limiter.reset();
    if (was_open && reason != nullptr) log::info("capture released ({})", reason);
}

bool CaptureService::Impl::open_session(HMONITOR mon) {
    const double t0 = qpc_now_s();
    HRESULT hr = session.build(mon);
    const char* what = "DuplicateOutput";
    if (SUCCEEDED(hr)) {
        what = "downscale init";
        if (!down.init(session.device.Get(), session.context.Get(), log::enabled(log::Level::Debug))) {
            log::warn("capture downscale init failed: {}", down.error());
            hr = E_FAIL;
        } else if (!down.set_grid(cur.grid_w, cur.grid_h)) {
            log::warn("capture downscale grid failed: {}", down.error());
            hr = E_OUTOFMEMORY;
        }
        if (!down.compile_messages().empty()) log::warn("capture shader warnings: {}", down.compile_messages());
    }
    if (FAILED(hr)) {
        down.release();
        session.close();
        on_build_failed(hr, what);
        return false;
    }
    ++local.sessions;
    discard = kDiscardFramesAfterBuild;
    estimator.reset();
    similarity.reset();
    limiter.reset();
    first_grid_logged = false;
    tex_w = static_cast<int>(session.desc.ModeDesc.Width);
    tex_h = static_cast<int>(session.desc.ModeDesc.Height);
    geom = output_geometry(session.rotation_deg, tex_w, tex_h);
    if (geom.screen_w <= 0 || geom.screen_h <= 0) {
        geom.screen_w = session.desktop_w;
        geom.screen_h = session.desktop_h;
    }
    update_hold_geometry();
    local.gpu_timing = down.gpu_timing();
    const double now = qpc_now_s();
    const DownscaleLayout l = downscale_layout(tex_w, tex_h, cur.grid_w, cur.grid_h);
    local.taps_x = l.taps_x;
    local.taps_y = l.taps_y;
    log::info(L"capture started: {}x{} adapter=\"{}\" output=\"{}\" rotation={} grid={}x{} taps={}x{} dup1={} "
              L"gpu_timing={} ({:.1f} ms)",
              session.desktop_w, session.desktop_h, session.adapter_name, session.output_name, session.rotation_deg,
              cur.grid_w, cur.grid_h, l.taps_x, l.taps_y, session.dup1 ? 1 : 0, down.gpu_timing() ? 1 : 0,
              (now - t0) * 1000.0);
    set_state(CaptureState::Running, now);
    return true;
}

void CaptureService::Impl::on_build_failed(HRESULT hr, const char* what) {
    ++local.build_failures;
    const bool give_up = policy.on_failure(static_cast<std::int32_t>(hr));
    log::debug("capture {} failed hr=0x{:08X} (consecutive counted failures: {})", what, hrx(hr), policy.count());
    const double now = qpc_now_s();
    if (give_up) {
        failed = true;
        const bool busy = hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == DXGI_ERROR_SESSION_DISCONNECTED;
        log::warn("capture failed {} times in a row (last hr=0x{:08X}); capture motion detection disabled for this "
                  "run{}",
                  policy.count(), hrx(hr), busy ? " (another program may be capturing the screen)" : "");
        enter_idle(CaptureState::Failed, now);
        return;
    }
    next_retry = now + kRetryIntervalS;
    set_state(CaptureState::Recovering, now);
    publish(hold.on_no_update(now), now);
}

void CaptureService::Impl::session_lost(HRESULT hr, const char* what) {
    const bool confirmed = session.confirmed;
    close_session(nullptr);
    if (!confirmed) {
        // 会话还没交付过帧就失效：按建立失败处理（计数，2 s 后重试），避免每轮循环都重建。
        on_build_failed(hr, what);
        return;
    }
    const double now = qpc_now_s();
    log::info("capture access lost ({} hr=0x{:08X}); rebuilding", what, hrx(hr));
    next_retry = 0;   // 立即重建一次
    set_state(CaptureState::Recovering, now);
    publish(hold.on_no_update(now), now);
}

void CaptureService::Impl::release_frame() {
    if (session.frame_held) {
        session.dup->ReleaseFrame();   // 失败（访问丢失）会在下一次 AcquireNextFrame 再报出来
        session.frame_held = false;
    }
}

void CaptureService::Impl::log_first_grid() {
    first_grid_logged = true;
    if (!log::enabled(log::Level::Debug) || gray.empty()) return;
    float lo = gray[0], hi = gray[0];
    double sum = 0;
    for (const float v : gray) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        sum += v;
    }
    log::debug("capture first grid {}x{}: min={:.4f} max={:.4f} mean={:.4f}", cur.grid_w, cur.grid_h, lo, hi,
               sum / static_cast<double>(gray.size()));
}

void CaptureService::Impl::drain() {
    double gpu_sum = 0;
    const int n = down.poll_gpu_ms(&gpu_sum);
    if (n > 0) {
        perf.gpu_ms_sum += gpu_sum;
        perf.gpu_samples += static_cast<std::uint64_t>(n);
        local.gpu_ms_sum += gpu_sum;
        local.gpu_samples += static_cast<std::uint64_t>(n);
    }
    for (;;) {
        const double c0 = qpc_now_s();
        double ft = 0;
        HRESULT hr = S_OK;
        const GpuDownscaler::Read r = down.try_read(gray.data(), &ft, &hr);
        if (r == GpuDownscaler::Read::Error) {
            log::debug("capture staging Map failed hr=0x{:08X}", hrx(hr));
            break;
        }
        if (r != GpuDownscaler::Read::Ready) break;
        if (!first_grid_logged) log_first_grid();
        if (sink) emit_grid(ft);   // 录制工具：与估计器看到的完全相同的网格
        MotionEstimate e = estimator.push(gray.data(), ft);
        // 变更 5：相似变换估计（纹理方向的网格；平移提示 = 投影互相关的结果，交换坐标轴之前）
        const double s0 = qpc_now_s();
        const TranslationHint hint{e.dx, e.dy};
        const SimilarityEstimate se = similarity.push(gray.data(), ft, e.valid ? &hint : nullptr);
        local.sim_ms_sum += (qpc_now_s() - s0) * 1000.0;
        if (geom.swap_axes) std::swap(e.dx, e.dy);
        ++local.frames;
        ++perf.frames;
        if (discard > 0) {
            --discard;   // 建立 / 重建后的前 2 帧只更新参考帧
        } else {
            hold.on_estimate(e, e.dt_s, ft);
            zoom_hold.on_value(se.valid ? zoom_to_deg_per_s(se.scale, se.dt_s, cur.assumed_hfov_deg) : std::nullopt, ft);
            roll_hold.on_value(se.valid ? roll_to_deg_per_s(se.rotation_deg, se.dt_s) : std::nullopt, ft);
            if (se.valid) ++local.sim_valid_count;
            if (e.dt_s > 0) {
                ++local.estimates;
                local.confidence_sum += e.confidence;
                local.abs_dx_sum += std::fabs(e.dx);
                local.abs_dy_sum += std::fabs(e.dy);
                if (!e.valid) {
                    ++local.lowconf;
                    ++perf.lowconf;
                }
            }
        }
        local.dx = e.dx;
        local.dy = e.dy;
        local.confidence = e.confidence;
        local.valid = e.valid;
        local.measured_deg_per_s = hold.last_measurement();
        local.scale = se.scale;
        local.rotation_deg = se.rotation_deg;
        local.sim_confidence = se.confidence;
        local.sim_valid = se.valid;
        local.sim_dt_s = se.dt_s;
        const double ms = (qpc_now_s() - c0) * 1000.0;
        perf.cpu_ms_sum += ms;
        local.cpu_ms_sum += ms;
    }
    local.dropped = down.dropped();
}

void CaptureService::Impl::emit_grid(double frame_t) {
    GridFrame g;
    g.gray = gray.data();
    g.grid_w = cur.grid_w;
    g.grid_h = cur.grid_h;
    g.frame_time_s = frame_t;
    g.tex_w = tex_w;
    g.tex_h = tex_h;
    g.screen_w = geom.screen_w;
    g.screen_h = geom.screen_h;
    g.rotation_deg = session.rotation_deg;
    try {
        sink(g);
    } catch (const std::exception& ex) {
        log::warn("capture grid sink threw ({}); sink disabled", ex.what());
        sink = nullptr;
    } catch (...) {
        log::warn("capture grid sink threw; sink disabled");
        sink = nullptr;
    }
}

void CaptureService::Impl::perf_tick(double now) {
    if (now - last_cpu_sample_s >= 0.25) {
        local.thread_cpu_s = thread_cpu_seconds();
        last_cpu_sample_s = now;
    }
    if (now - perf.start_s < kPerfLogIntervalS) return;
    const double cpu = thread_cpu_seconds();
    if (log::enabled(log::Level::Debug)) log::debug("{}", format_perf_line(perf, perf.cpu_pct(now, cpu)));
    perf.reset(now, cpu);
}

void CaptureService::Impl::step() {
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = session.dup->AcquireNextFrame(kAcquireTimeoutMs, &fi, &res);
    double now = qpc_now_s();
    if (!session.confirmed && SUCCEEDED(hr)) {
        session.confirmed = true;
        policy.on_success();
    } else if (!session.confirmed && now - session.opened_s >= kRetryIntervalS) {
        session.confirmed = true;
        policy.on_success();
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        ++local.timeouts;
        ++perf.timeouts;
        drain();   // 画面停下后，最后一帧也要及时处理
        publish(hold.on_no_update(now), now);
        perf_tick(now);
        return;
    }
    if (FAILED(hr)) {
        session_lost(hr, "AcquireNextFrame");
        return;
    }
    session.frame_held = true;
    if (fi.LastPresentTime.QuadPart == 0 || fi.AccumulatedFrames == 0) {
        // 只有鼠标指针更新：不参与估计，参考帧不变。
        release_frame();
        ++local.pointer_only;
        drain();
        publish(hold.on_no_update(now), now);
        perf_tick(now);
        return;
    }
    const double frame_t = static_cast<double>(fi.LastPresentTime.QuadPart) / qpc_frequency();
    if (!limiter.on_frame(frame_t)) {
        // 高刷新率：离上一处理帧太近，不降采样也不估计（参考帧不变，下一处理帧的 dt 按时间戳覆盖这段）。
        res.Reset();
        release_frame();
        ++local.skipped;
        ++perf.skipped;
        drain();   // 上一处理帧的读回仍在这里及时完成
        publish(hold.value(now), now);
        perf_tick(now);
        return;
    }
    ComPtr<ID3D11Texture2D> tex;
    hr = res.As(&tex);
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        if (static_cast<int>(td.Width) != tex_w || static_cast<int>(td.Height) != tex_h) {
            tex_w = static_cast<int>(td.Width);
            tex_h = static_cast<int>(td.Height);
            geom = output_geometry(session.rotation_deg, tex_w, tex_h);
            update_hold_geometry();
            const DownscaleLayout l = downscale_layout(tex_w, tex_h, cur.grid_w, cur.grid_h);
            local.taps_x = l.taps_x;
            local.taps_y = l.taps_y;
        }
        const double s0 = qpc_now_s();
        hr = down.submit(tex.Get(), frame_t);   // Draw → CopyResource(staging) → Flush
        local.submit_ms_sum += (qpc_now_s() - s0) * 1000.0;
        ++local.submits;
    }
    tex.Reset();
    res.Reset();
    release_frame();   // GPU 工作已提交，再释放帧（与微软 DDA 示例同序）
    if (FAILED(hr)) {
        session_lost(hr, "downscale");
        return;
    }
    now = qpc_now_s();
    drain();   // 映射上一帧（或更早）的槽，不等待 GPU
    publish(hold.value(now), now);
    perf_tick(now);
}

void CaptureService::Impl::run() {
    ::SetThreadDescription(::GetCurrentThread(), L"MotionCalm capture");
    tsc_frequency();   // 一次性校准（约 10 ms），之后 thread_cpu_seconds() 不再阻塞
    std::uint64_t seen_gen = ~0ull;
    std::uint64_t seen_epoch = feature_epoch.load(std::memory_order_acquire);
    while (!stop.load(std::memory_order_acquire)) {
        const std::uint64_t gen = params_gen.load(std::memory_order_acquire);
        if (gen != seen_gen) {
            seen_gen = gen;
            apply_params();
        }
        const bool on = feature_on.load(std::memory_order_acquire);
        const bool want = wanted.load(std::memory_order_acquire);
        const HMONITOR mon = target.load(std::memory_order_acquire);
        const std::uint64_t epoch = feature_epoch.load(std::memory_order_acquire);
        if (epoch != seen_epoch) {   // 关 → 开（哪怕截屏线程没看到中间的"关"）
            seen_epoch = epoch;
            failed = false;
            policy.reset();
            next_retry = 0;
        }
        const double now = qpc_now_s();

        if (!on) {
            close_session("capture detection off");
            failed = false;
            policy.reset();
            next_retry = 0;
            paused_since = -1;
            enter_idle(CaptureState::Off, now);
            sync_debug();
            wait(INFINITE);
            continue;
        }
        if (failed) {
            close_session(nullptr);
            enter_idle(CaptureState::Failed, now);
            sync_debug();
            wait(INFINITE);
            continue;
        }
        if (!want || mon == nullptr) {
            // 暂停：不取帧，发布 0；会话保留 5 s（Alt-Tab 抖动不反复建会话），之后释放。
            enter_idle(CaptureState::Paused, now);
            sync_debug();
            if (session.open()) {
                if (paused_since < 0) paused_since = now;
                const double left = kReleaseAfterPauseS - (now - paused_since);
                if (left <= 0) {
                    close_session("paused for 5 s");
                    sync_debug();
                    wait(INFINITE);
                } else {
                    wait(wait_ms(left));
                }
            } else {
                wait(INFINITE);
            }
            continue;
        }
        paused_since = -1;
        if (session.open() && session.monitor != mon) {
            close_session("target monitor changed");
            next_retry = 0;
        }
        if (!session.open()) {
            if (now < next_retry) {
                set_state(CaptureState::Recovering, now);
                publish(hold.on_no_update(now), now);
                sync_debug();
                wait(wait_ms(next_retry - now));
                continue;
            }
            if (!open_session(mon)) {
                sync_debug();
                continue;
            }
        }
        set_state(CaptureState::Running, now);   // 暂停后恢复（会话沿用，不重建）
        step();
        sync_debug();
    }
    close_session("capture service stopped");
    const double now = qpc_now_s();
    reset_holds();
    set_state(CaptureState::Off, now);
    publish(0.0, now);
    sync_debug();
}

// ---------------------------------------------------------------- CaptureService

CaptureService::CaptureService() = default;

CaptureService::~CaptureService() { stop(); }

bool CaptureService::start(const CaptureParams& params) {
    if (impl_) return false;
    auto impl = std::make_unique<Impl>();
    impl->params = sanitize_capture_params(params);
    impl->params_gen.store(1, std::memory_order_release);
    impl->sink = grid_sink_;   // 线程创建之前写入，std::thread 的构造保证截屏线程看得到
    impl->event.reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!impl->event) {
        log::warn("capture: CreateEvent failed err={}", ::GetLastError());
        return false;
    }
    try {
        Impl* raw = impl.get();
        impl->thread = std::thread([raw] { raw->run(); });
    } catch (const std::system_error& e) {
        log::warn("capture: cannot start thread: {}", e.what());
        return false;
    }
    impl_ = std::move(impl);
    log::debug("capture service started (grid {}x{})", impl_->params.grid_w, impl_->params.grid_h);
    return true;
}

void CaptureService::stop() {
    if (!impl_) return;
    impl_->stop.store(true, std::memory_order_release);
    ::SetEvent(impl_->event.get());
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_.reset();
}

bool CaptureService::started() const { return impl_ != nullptr; }

void CaptureService::set_grid_sink(GridSink sink) {
    if (impl_) {
        log::debug("capture: set_grid_sink after start() ignored");
        return;
    }
    grid_sink_ = std::move(sink);
}

void CaptureService::set_target(HMONITOR mon, bool feature_on, bool wanted) {
    if (!impl_) return;
    bool changed = impl_->target.exchange(mon, std::memory_order_acq_rel) != mon;
    const bool was_on = impl_->feature_on.exchange(feature_on, std::memory_order_acq_rel);
    if (was_on != feature_on) {
        changed = true;
        if (feature_on) impl_->feature_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    if (impl_->wanted.exchange(wanted, std::memory_order_acq_rel) != wanted) changed = true;
    if (changed) ::SetEvent(impl_->event.get());
}

void CaptureService::update_params(const CaptureParams& params) {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->params_mu);
        impl_->params = sanitize_capture_params(params);
    }
    impl_->params_gen.fetch_add(1, std::memory_order_acq_rel);
    ::SetEvent(impl_->event.get());
}

double CaptureService::speed(double now_s) const {
    if (!impl_) return 0.0;
    const CaptureState s = impl_->state.load(std::memory_order_acquire);
    if (s != CaptureState::Running && s != CaptureState::Recovering) return 0.0;
    const double t = impl_->speed_t.load(std::memory_order_acquire);
    const double v = impl_->speed.load(std::memory_order_acquire);
    return speed_is_fresh(t, now_s) ? v : 0.0;
}

double CaptureService::speed() const { return speed(qpc_now_s()); }

double CaptureService::zoom_speed(double now_s) const {
    if (!impl_) return 0.0;
    const CaptureState s = impl_->state.load(std::memory_order_acquire);
    if (s != CaptureState::Running && s != CaptureState::Recovering) return 0.0;
    const double t = impl_->speed_t.load(std::memory_order_acquire);
    const double v = impl_->zoom.load(std::memory_order_acquire);
    return speed_is_fresh(t, now_s) ? v : 0.0;
}

double CaptureService::roll_speed(double now_s) const {
    if (!impl_) return 0.0;
    const CaptureState s = impl_->state.load(std::memory_order_acquire);
    if (s != CaptureState::Running && s != CaptureState::Recovering) return 0.0;
    const double t = impl_->speed_t.load(std::memory_order_acquire);
    const double v = impl_->roll.load(std::memory_order_acquire);
    return speed_is_fresh(t, now_s) ? v : 0.0;
}

CaptureState CaptureService::state() const {
    return impl_ ? impl_->state.load(std::memory_order_acquire) : CaptureState::Off;
}

CaptureDebug CaptureService::debug() const {
    if (!impl_) return CaptureDebug{};
    std::lock_guard lock(impl_->debug_mu);
    CaptureDebug d = impl_->dbg;
    d.state = impl_->state.load(std::memory_order_acquire);
    return d;
}

}  // namespace mc
