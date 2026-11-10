// main.cpp — capturetest：在主显示器上跑截屏运动检测管线（CaptureService）N 秒，每秒打印 fps / dx / dy / 置信度 / °/s / GPU ms / CPU %；可把送进估计器的灰度网格录成片段（--record），或不截屏、离线回放片段（--replay）。只读屏幕，不画任何东西。
//
//   capturetest [--seconds N] [--grid WxH] [--hfov DEG] [--max-shift N] [--min-corr X] [--hold-ms N]
//               [--log PATH] [--no-gpu-timing] [--record PATH]
//   capturetest --replay PATH [--csv OUT] [--hfov DEG] [--max-shift N] [--min-corr X] [--hold-ms N]
// 默认 10 s、160×90、HFOV 90。日志级别为 debug（开启 GPU 时间戳与 "capture perf:" 行）；--log 时写到该文件。
// --record：截屏线程把每个网格拷进内存，结束后写成网格片段文件（格式见 src/capture/grid_recording.h）；
//           网格尺寸或几何（纹理 / 屏幕尺寸、旋转）中途变化、或达到内存上限时停止录制并提示，已录的部分照样写出。
// --replay：不截屏，读片段，用 GridPipeline（与截屏线程同一套估计逻辑）跑一遍，打印汇总；--csv 每帧写一行。
//           片段旁有 <name>.expect（tests/fixtures/README.md）时先用其中的回放参数（命令行显式给出的优先），再检查其中的界限。
// Ctrl+C 提前结束。退出码：0 正常；1 参数错误；2 截屏服务启动失败；3 全程没有取到帧 / 片段为空；
//                         4 片段读写或 CSV 写入失败；5 回放结果不满足 .expect 中的界限。
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "capture/cpu_clock.h"
#include "capture/desktop_dup.h"
#include "capture/grid_pipeline.h"
#include "capture/grid_recording.h"
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
    std::filesystem::path record_path;   // --record
    std::filesystem::path replay_path;   // --replay
    std::filesystem::path csv_path;      // --csv（只用于 --replay）
    bool grid_given = false;
    // 命令行显式给出的估计参数（--replay 时优先于 .expect 里的回放参数）
    std::optional<double> hfov, min_corr;
    std::optional<int> max_shift, hold_ms;
};

void usage() {
    std::fprintf(stderr,
                 "usage: capturetest [--seconds N] [--grid WxH] [--hfov DEG] [--max-shift N] [--min-corr X]\n"
                 "                   [--hold-ms N] [--log PATH] [--no-gpu-timing] [--record PATH]\n"
                 "       capturetest --replay PATH [--csv OUT] [--hfov DEG] [--max-shift N] [--min-corr X]\n"
                 "                   [--hold-ms N]\n"
                 "  --record PATH   save the grids fed to the estimators to a grid clip file\n"
                 "  --replay PATH   no screen capture: run a grid clip through the estimators and print a summary\n"
                 "  --csv OUT       with --replay: write one CSV row per frame\n"
                 "exit codes: 0 ok, 1 bad arguments, 2 capture service failed to start, 3 no frames,\n"
                 "            4 clip / CSV file error, 5 replay expectations (.expect) not met\n");
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
            o->grid_given = true;
        } else if (a == L"--hfov" && next(&v)) {
            o->params.assumed_hfov_deg = _wtof(v);
            o->hfov = o->params.assumed_hfov_deg;
        } else if (a == L"--max-shift" && next(&v)) {
            o->params.max_shift = _wtoi(v);
            o->max_shift = o->params.max_shift;
        } else if (a == L"--min-corr" && next(&v)) {
            o->params.min_correlation = _wtof(v);
            o->min_corr = o->params.min_correlation;
        } else if (a == L"--hold-ms" && next(&v)) {
            o->params.hold_ms = _wtoi(v);
            o->hold_ms = o->params.hold_ms;
        } else if (a == L"--log" && next(&v)) {
            o->log_path = v;
        } else if (a == L"--no-gpu-timing") {
            o->gpu_timing = false;
        } else if (a == L"--record" && next(&v)) {
            o->record_path = v;
        } else if (a == L"--replay" && next(&v)) {
            o->replay_path = v;
        } else if (a == L"--csv" && next(&v)) {
            o->csv_path = v;
        } else {
            return false;
        }
    }
    if (!o->record_path.empty() && !o->replay_path.empty()) return false;   // 二选一
    if (!o->csv_path.empty() && o->replay_path.empty()) return false;       // --csv 只配 --replay
    return true;
}

double ratio(double num, std::uint64_t den) { return den > 0 ? num / static_cast<double>(den) : 0.0; }

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// ---------------------------------------------------------------- --record

// 截屏线程上的网格接收器：拷进内存，运行结束（svc.stop() join 之后）再写文件。
struct Recorder {
    std::mutex mu;
    mc::GridClip clip;
    bool stopped = false;
    std::string stop_reason;

    void on_frame(const mc::GridFrame& f) {
        std::lock_guard lock(mu);
        if (stopped || f.gray == nullptr || f.grid_w <= 0 || f.grid_h <= 0) return;
        mc::GridClipInfo in;
        in.grid_w = f.grid_w;
        in.grid_h = f.grid_h;
        in.tex_w = f.tex_w;
        in.tex_h = f.tex_h;
        in.screen_w = f.screen_w;
        in.screen_h = f.screen_h;
        in.rotation_deg = f.rotation_deg;
        if (clip.frames.empty()) {
            clip.info = in;
        } else {
            const mc::GridClipInfo& c = clip.info;
            if (in.grid_w != c.grid_w || in.grid_h != c.grid_h || in.tex_w != c.tex_w || in.tex_h != c.tex_h ||
                in.screen_w != c.screen_w || in.screen_h != c.screen_h || in.rotation_deg != c.rotation_deg) {
                stopped = true;
                stop_reason = std::format(
                    "grid or geometry changed mid-run (grid {}x{} tex {}x{} screen {}x{} rot {} -> grid {}x{} tex {}x{} "
                    "screen {}x{} rot {}); recording stopped at {} frames",
                    c.grid_w, c.grid_h, c.tex_w, c.tex_h, c.screen_w, c.screen_h, c.rotation_deg, in.grid_w, in.grid_h,
                    in.tex_w, in.tex_h, in.screen_w, in.screen_h, in.rotation_deg, clip.frames.size());
                return;
            }
        }
        const std::size_t n = static_cast<std::size_t>(f.grid_w) * static_cast<std::size_t>(f.grid_h);
        if (static_cast<std::uint64_t>(clip.frames.size() + 1) * n > mc::kGridClipMaxSamples) {
            stopped = true;
            stop_reason = std::format("clip size limit reached; recording stopped at {} frames", clip.frames.size());
            return;
        }
        try {
            mc::GridClipFrame fr;
            fr.t_s = f.frame_time_s;
            fr.gray.assign(f.gray, f.gray + n);
            clip.frames.push_back(std::move(fr));
        } catch (const std::exception&) {   // bad_alloc：不能让异常逃出截屏线程
            stopped = true;
            stop_reason = std::format("out of memory; recording stopped at {} frames", clip.frames.size());
        }
    }
};

// 写片段并打印结果；返回退出码（0 或 4）。只在截屏线程结束之后调用。
int finish_recording(Recorder& rec, const std::filesystem::path& path) {
    std::lock_guard lock(rec.mu);
    if (!rec.stop_reason.empty()) std::printf("record: %s\n", rec.stop_reason.c_str());
    if (rec.clip.frames.empty()) {
        std::printf("record: no grids received; nothing written\n");
        return 0;
    }
    std::string err;
    if (!mc::write_grid_clip(path, rec.clip, &err)) {
        std::fprintf(stderr, "record: cannot write clip: %s\n", err.c_str());
        return 4;
    }
    const mc::GridClip& c = rec.clip;
    const double dur = c.frames.back().t_s - c.frames.front().t_s;
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
    std::printf("record: %zu frames, %.2f s, grid %dx%d, tex %dx%d, screen %dx%d, rotation %d -> %s (%.1f KB)\n",
                c.frames.size(), dur, c.info.grid_w, c.info.grid_h, c.info.tex_w, c.info.tex_h, c.info.screen_w,
                c.info.screen_h, c.info.rotation_deg, u8(path).c_str(), ec ? 0.0 : static_cast<double>(bytes) / 1024.0);
    return 0;
}

// ---------------------------------------------------------------- --replay

int run_replay(const Options& opt) {
    mc::GridClip clip;
    std::string err;
    if (!mc::read_grid_clip(opt.replay_path, &clip, &err)) {
        std::fprintf(stderr, "replay: cannot read clip: %s\n", err.c_str());
        return 4;
    }
    if (opt.grid_given) std::printf("replay: --grid ignored (the grid size comes from the clip)\n");

    // 参数：默认值 ← .expect 的回放参数 ← 命令行显式给出的
    mc::CaptureParams p;
    const std::filesystem::path expect_path = mc::replay_expectations_path(opt.replay_path);
    std::optional<mc::ReplayExpectations> expect;
    std::error_code ec;
    if (std::filesystem::exists(expect_path, ec)) {
        mc::ReplayExpectations e;
        if (!mc::read_replay_expectations(expect_path, &e, &err)) {
            std::fprintf(stderr, "replay: bad expectations file %s: %s\n", u8(expect_path).c_str(), err.c_str());
            return 4;
        }
        e.apply_to(&p);
        expect = std::move(e);
    }
    if (opt.hfov) p.assumed_hfov_deg = *opt.hfov;
    if (opt.max_shift) p.max_shift = *opt.max_shift;
    if (opt.min_corr) p.min_correlation = *opt.min_corr;
    if (opt.hold_ms) p.hold_ms = *opt.hold_ms;

    const mc::GridClipInfo& in = clip.info;
    const double dur = clip.frames.size() > 1 ? clip.frames.back().t_s - clip.frames.front().t_s : 0.0;
    std::printf("replay: %s\n", u8(opt.replay_path).c_str());
    std::printf("  clip: %zu frames, %.2f s (%.1f fps), grid %dx%d, tex %dx%d, screen %dx%d, rotation %d\n",
                clip.frames.size(), dur, dur > 0 ? static_cast<double>(clip.frames.size() - 1) / dur : 0.0, in.grid_w,
                in.grid_h, in.tex_w, in.tex_h, in.screen_w, in.screen_h, in.rotation_deg);
    if (clip.frames.empty()) {
        std::printf("replay: clip has no frames\n");
        return 3;
    }

    mc::GridReplayResult res;
    if (!mc::replay_grid_clip(clip, p, &res, &err)) {
        std::fprintf(stderr, "replay: %s\n", err.c_str());
        return 4;
    }
    const mc::CaptureParams used = mc::GridPipeline(p, in).params();   // sanitize 之后实际使用的参数（拷贝）
    std::printf("  params: hfov %.0f, max_shift %d, min_corr %.2f, hold %d ms, baseline %d ms, roll %s%s\n",
                used.assumed_hfov_deg, used.max_shift, used.min_correlation, used.hold_ms, used.baseline_ms,
                used.detect_roll ? "on" : "off", expect ? " (with .expect overrides)" : "");
    const mc::GridPipelineSummary& s = res.summary;
    std::printf("summary: frames=%llu discarded=%llu estimates=%llu valid=%llu (%.1f%%) lowconf=%llu (%.1f%%) "
                "c_avg=%.3f sim_valid=%llu (%.1f%%)\n",
                static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.discarded),
                static_cast<unsigned long long>(s.estimates), static_cast<unsigned long long>(s.valid),
                s.valid_fraction() * 100.0, static_cast<unsigned long long>(s.lowconf), s.lowconf_fraction() * 100.0,
                s.confidence_avg(), static_cast<unsigned long long>(s.sim_valid), s.sim_valid_fraction() * 100.0);
    std::printf("         published deg/s  speed mean=%.1f max=%.1f  zoom mean=%.1f max=%.1f  roll mean=%.1f max=%.1f\n",
                s.mean_speed(), s.speed_max, s.mean_zoom(), s.zoom_max, s.mean_roll(), s.roll_max);

    int rc = 0;
    if (!opt.csv_path.empty()) {
        std::ofstream csv(opt.csv_path, std::ios::binary | std::ios::trunc);
        if (csv) {
            csv << mc::grid_replay_csv_header() << '\n';
            for (std::size_t k = 0; k < res.frames.size(); ++k) csv << mc::grid_replay_csv_row(k, res.frames[k]) << '\n';
            csv.flush();
        }
        if (!csv) {
            std::fprintf(stderr, "replay: cannot write CSV %s\n", u8(opt.csv_path).c_str());
            rc = 4;
        } else {
            std::printf("  csv: %zu rows -> %s\n", res.frames.size(), u8(opt.csv_path).c_str());
        }
    }
    if (expect) {
        const std::vector<std::string> fails = mc::check_replay_expectations(*expect, s);
        std::printf("  expectations (%s): %zu bounds, %s\n", u8(expect_path.filename()).c_str(), expect->bounds.size(),
                    fails.empty() ? "PASS" : "FAIL");
        for (const std::string& f : fails) std::printf("    FAIL %s\n", f.c_str());
        if (!fails.empty() && rc == 0) rc = 5;
    }
    return rc;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options opt;
    if (!parse(argc, argv, &opt)) {
        usage();
        return 1;
    }
    if (!opt.replay_path.empty()) return run_replay(opt);   // 离线回放：不碰屏幕

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
    Recorder rec;   // 必须比 svc 活得久：svc 析构时才 join 截屏线程
    mc::CaptureService svc;
    if (!opt.record_path.empty()) svc.set_grid_sink([&rec](const mc::GridFrame& f) { rec.on_frame(f); });
    if (!svc.start(opt.params)) {
        std::fprintf(stderr, "capture service failed to start\n");
        return 2;
    }
    svc.set_target(mon, true, true);

    std::printf("capturetest: primary monitor, %d s, grid %dx%d, hfov %.0f, max_shift %d, min_corr %.2f, hold %d ms\n",
                opt.seconds, opt.params.grid_w, opt.params.grid_h, opt.params.assumed_hfov_deg, opt.params.max_shift,
                opt.params.min_correlation, opt.params.hold_ms);
    if (!opt.record_path.empty()) std::printf("  recording grids to %s\n", u8(opt.record_path).c_str());
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
    svc.stop();   // join 截屏线程：之后 rec 不会再被写
    std::printf("summary: %.1f s, state=%s, sessions=%llu build_failures=%llu frames=%llu (%.1f fps) estimates=%llu "
                "lowconf=%llu timeouts=%llu pointer_only=%llu skipped=%llu dropped=%llu\n",
                wall, mc::capture_state_name(d.state), static_cast<unsigned long long>(d.sessions),
                static_cast<unsigned long long>(d.build_failures), static_cast<unsigned long long>(d.frames),
                static_cast<double>(d.frames) / std::max(wall, 1e-6), static_cast<unsigned long long>(d.estimates),
                static_cast<unsigned long long>(d.lowconf), static_cast<unsigned long long>(d.timeouts),
                static_cast<unsigned long long>(d.pointer_only), static_cast<unsigned long long>(d.skipped),
                static_cast<unsigned long long>(d.dropped));
    std::printf("         c_avg=%.3f  deg/s avg=%.1f max=%.1f  gpu_ms=%.3f  cpu_ms=%.3f  submit_ms=%.3f  thread_cpu=%.2f%%\n",
                ratio(d.confidence_sum, d.estimates), n_speed_all > 0 ? sum_speed_all / static_cast<double>(n_speed_all) : 0.0,
                max_speed_all, d.gpu_samples > 0 ? d.gpu_ms_sum / static_cast<double>(d.gpu_samples) : -1.0,
                ratio(d.cpu_ms_sum, d.frames), ratio(d.submit_ms_sum, d.submits), d.thread_cpu_s / std::max(wall, 1e-6) * 100.0);
    std::printf("         forward/roll estimator (change 5): sim_ms=%.3f per frame (included in cpu_ms), valid %llu of %llu frames\n",
                ratio(d.sim_ms_sum, d.frames), static_cast<unsigned long long>(d.sim_valid_count),
                static_cast<unsigned long long>(d.frames));
    int rc = total_frames > 0 || d.frames > 0 ? 0 : 3;
    if (!opt.record_path.empty()) {
        const int rec_rc = finish_recording(rec, opt.record_path);
        if (rec_rc != 0) rc = rec_rc;
    }
    mc::log::shutdown();
    return rc;
}
