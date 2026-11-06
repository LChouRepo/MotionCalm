// main.cpp — capturetest：在主显示器上跑截屏运动检测管线（CaptureService）N 秒，每秒打印 fps / dx / dy / 置信度 / °/s / GPU ms / CPU %。只读屏幕，不画任何东西，不保存帧。
//
//   capturetest [--seconds N] [--grid WxH] [--hfov DEG] [--max-shift N] [--min-corr X] [--hold-ms N]
//               [--log PATH] [--no-gpu-timing]
// 默认 10 s、160×90、HFOV 90。日志级别为 debug（开启 GPU 时间戳与 "capture perf:" 行）；--log 时写到该文件。
// Ctrl+C 提前结束。退出码：0 正常；1 参数错误；2 截屏服务启动失败；3 全程没有取到帧。
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "capture/cpu_clock.h"
#include "capture/desktop_dup.h"
#include "util/clock.h"
#include "util/log.h"

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI on_ctrl(DWORD) {
    g_quit.store(true);
    return TRUE;
}

struct Options {
    int seconds = 10;
    mc::CaptureParams params;
    std::wstring log_path;
    bool gpu_timing = true;
};

void usage() {
    std::fprintf(stderr,
                 "usage: capturetest [--seconds N] [--grid WxH] [--hfov DEG] [--max-shift N] [--min-corr X]\n"
                 "                   [--hold-ms N] [--log PATH] [--no-gpu-timing]\n");
}

bool parse(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](const wchar_t** v) {
            if (i + 1 >= argc) return false;
            *v = argv[++i];
            return true;
        };
        const wchar_t* v = nullptr;
        if (a == L"--seconds" && next(&v)) {
            o->seconds = std::max(1, _wtoi(v));
        } else if (a == L"--grid" && next(&v)) {
            int w = 0, h = 0;
            if (swscanf_s(v, L"%dx%d", &w, &h) != 2) return false;
            o->params.grid_w = w;
            o->params.grid_h = h;
        } else if (a == L"--hfov" && next(&v)) {
            o->params.assumed_hfov_deg = _wtof(v);
        } else if (a == L"--max-shift" && next(&v)) {
            o->params.max_shift = _wtoi(v);
        } else if (a == L"--min-corr" && next(&v)) {
            o->params.min_correlation = _wtof(v);
        } else if (a == L"--hold-ms" && next(&v)) {
            o->params.hold_ms = _wtoi(v);
        } else if (a == L"--log" && next(&v)) {
            o->log_path = v;
        } else if (a == L"--no-gpu-timing") {
            o->gpu_timing = false;
        } else {
            return false;
        }
    }
    return true;
}

double ratio(double num, std::uint64_t den) { return den > 0 ? num / static_cast<double>(den) : 0.0; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    if (!parse(argc, argv, &opt)) {
        usage();
        return 1;
    }
    // 与 MotionCalm.exe 的 manifest 一致：Per-Monitor V2，桌面坐标与复制纹理都是物理像素。
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::SetConsoleCtrlHandler(on_ctrl, TRUE);

    // GPU 时间戳只在 debug 级别启用（与 app 相同的规则）。
    const mc::log::Level level = opt.gpu_timing ? mc::log::Level::Debug : mc::log::Level::Info;
    if (!opt.log_path.empty()) {
        if (!mc::log::init(opt.log_path, level)) std::fprintf(stderr, "warning: cannot open log file\n");
    } else {
        mc::log::set_level(level);   // 未 init：只决定 GPU 计时是否开启，日志行被丢弃
    }

    const HMONITOR mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    mc::CaptureService svc;
    if (!svc.start(opt.params)) {
        std::fprintf(stderr, "capture service failed to start\n");
        return 2;
    }
    svc.set_target(mon, true, true);

    std::printf("capturetest: primary monitor, %d s, grid %dx%d, hfov %.0f, max_shift %d, min_corr %.2f, hold %d ms\n",
                opt.seconds, opt.params.grid_w, opt.params.grid_h, opt.params.assumed_hfov_deg, opt.params.max_shift,
                opt.params.min_correlation, opt.params.hold_ms);
    std::printf("  t  state       fps  lowc  tmo   |dx|   |dy|  dx_last dy_last  c_avg c_last  deg/s_avg deg/s_max"
                "  gpu_ms  cpu_ms  sub_ms  thr%%  proc%%  sim_ms simok  zoom  roll\n");

    mc::CaptureDebug prev = svc.debug();
    double prev_proc = mc::process_cpu_seconds();
    double prev_wall = mc::qpc_now_s();
    const double t_start = prev_wall;
    bool printed_session = false;
    std::uint64_t total_frames = 0;
    double sum_speed_all = 0, max_speed_all = 0;
    std::uint64_t n_speed_all = 0;

    for (int sec = 1; sec <= opt.seconds && !g_quit.load(); ++sec) {
        // 这一秒里每 20 ms 采一次主线程会读到的值（speed()），得到平均 / 最大发布值。
        double sum_speed = 0, max_speed = 0;
        int n_speed = 0;
        const double t_end = t_start + sec;
        while (!g_quit.load()) {
            const double now = mc::qpc_now_s();
            if (now >= t_end) break;
            const double v = svc.speed(now);
            sum_speed += v;
            max_speed = std::max(max_speed, v);
            ++n_speed;
            ::Sleep(static_cast<DWORD>(std::clamp((t_end - now) * 1000.0, 1.0, 20.0)));
        }
        const mc::CaptureDebug d = svc.debug();
        const double wall = mc::qpc_now_s();
        const double proc = mc::process_cpu_seconds();
        const double dt = std::max(1e-6, wall - prev_wall);
        if (!printed_session && d.sessions > 0) {
            std::printf("  session: %dx%d grid %dx%d taps %dx%d gpu_timing=%d\n", d.screen_w, d.screen_h, d.grid_w,
                        d.grid_h, d.taps_x, d.taps_y, d.gpu_timing ? 1 : 0);
            printed_session = true;
        }
        const std::uint64_t frames = d.frames - prev.frames;
        const std::uint64_t ests = d.estimates - prev.estimates;
        const std::uint64_t gpu_n = d.gpu_samples - prev.gpu_samples;
        const std::uint64_t subs = d.submits - prev.submits;
        const double gpu_ms = gpu_n > 0 ? (d.gpu_ms_sum - prev.gpu_ms_sum) / static_cast<double>(gpu_n) : -1.0;
        std::printf("%3d  %-10s %5.1f %5llu %4llu %6.2f %6.2f %+7.2f %+7.2f  %5.3f  %5.3f  %9.1f %9.1f  %6.3f  %6.3f"
                    "  %6.3f %5.2f %6.2f  %6.3f %5llu %5.1f %5.1f\n",
                    sec, mc::capture_state_name(d.state), static_cast<double>(frames) / dt,
                    static_cast<unsigned long long>(d.lowconf - prev.lowconf),
                    static_cast<unsigned long long>(d.timeouts - prev.timeouts),
                    ratio(d.abs_dx_sum - prev.abs_dx_sum, ests), ratio(d.abs_dy_sum - prev.abs_dy_sum, ests), d.dx,
                    d.dy, ratio(d.confidence_sum - prev.confidence_sum, ests), d.confidence,
                    n_speed > 0 ? sum_speed / n_speed : 0.0, max_speed, gpu_ms,
                    ratio(d.cpu_ms_sum - prev.cpu_ms_sum, frames), ratio(d.submit_ms_sum - prev.submit_ms_sum, subs),
                    (d.thread_cpu_s - prev.thread_cpu_s) / dt * 100.0, (proc - prev_proc) / dt * 100.0,
                    ratio(d.sim_ms_sum - prev.sim_ms_sum, frames),
                    static_cast<unsigned long long>(d.sim_valid_count - prev.sim_valid_count), d.zoom_deg_per_s,
                    d.roll_deg_per_s);
        std::fflush(stdout);
        total_frames += frames;
        sum_speed_all += sum_speed;
        n_speed_all += static_cast<std::uint64_t>(n_speed);
        max_speed_all = std::max(max_speed_all, max_speed);
        prev = d;
        prev_proc = proc;
        prev_wall = wall;
    }

    const mc::CaptureDebug d = svc.debug();
    const double wall = mc::qpc_now_s() - t_start;
    svc.stop();
    std::printf("summary: %.1f s, state=%s, sessions=%llu build_failures=%llu frames=%llu (%.1f fps) estimates=%llu "
                "lowconf=%llu timeouts=%llu pointer_only=%llu dropped=%llu\n",
                wall, mc::capture_state_name(d.state), static_cast<unsigned long long>(d.sessions),
                static_cast<unsigned long long>(d.build_failures), static_cast<unsigned long long>(d.frames),
                static_cast<double>(d.frames) / std::max(wall, 1e-6), static_cast<unsigned long long>(d.estimates),
                static_cast<unsigned long long>(d.lowconf), static_cast<unsigned long long>(d.timeouts),
                static_cast<unsigned long long>(d.pointer_only), static_cast<unsigned long long>(d.dropped));
    std::printf("         c_avg=%.3f  deg/s avg=%.1f max=%.1f  gpu_ms=%.3f  cpu_ms=%.3f  submit_ms=%.3f  thread_cpu=%.2f%%\n",
                ratio(d.confidence_sum, d.estimates), n_speed_all > 0 ? sum_speed_all / static_cast<double>(n_speed_all) : 0.0,
                max_speed_all, d.gpu_samples > 0 ? d.gpu_ms_sum / static_cast<double>(d.gpu_samples) : -1.0,
                ratio(d.cpu_ms_sum, d.frames), ratio(d.submit_ms_sum, d.submits), d.thread_cpu_s / std::max(wall, 1e-6) * 100.0);
    std::printf("         forward/roll estimator (change 5): sim_ms=%.3f per frame (included in cpu_ms), valid %llu of %llu frames\n",
                ratio(d.sim_ms_sum, d.frames), static_cast<unsigned long long>(d.sim_valid_count),
                static_cast<unsigned long long>(d.frames));
    mc::log::shutdown();
    return total_frames > 0 || d.frames > 0 ? 0 : 3;
}
