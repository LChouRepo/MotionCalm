// test_capture_service.cpp — CaptureService 线程生命周期（不取任何屏幕：目标显示器为空或不存在）：Off / Paused / Recovering 状态、发布值为 0、参数收拢、stop 及时返回、关 → 开清零失败计数、网格回调的设置时机；周期计数 CPU 时钟；app 侧的调试条截屏字段与 --no-capture 参数。
#include "test_framework.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "app/app.h"
#include "capture/cpu_clock.h"
#include "capture/desktop_dup.h"
#include "util/clock.h"

using namespace mc;

namespace {

// 轮询 pred 直到为真或超时（毫秒）。
bool wait_until(const std::function<bool()>& pred, int timeout_ms = 2000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) return false;
        ::Sleep(2);
    }
    return true;
}

// 不对应任何显示器的句柄：DXGI 枚举找不到对应输出 → 建立失败（不会创建设备，也不会复制任何屏幕）。
HMONITOR bogus_monitor() { return reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(0x5EED0001u)); }

}  // namespace

TEST(capture_service_not_started_is_inert) {
    CaptureService s;
    CHECK_FALSE(s.started());
    CHECK_TRUE(s.state() == CaptureState::Off);
    CHECK_EQ(s.speed(), 0.0);
    s.set_target(nullptr, true, true);   // 未启动：无操作
    s.update_params(CaptureParams{});
    CHECK_TRUE(s.debug().state == CaptureState::Off);
    s.stop();   // 未启动时 stop 也安全
}

TEST(capture_service_lifecycle_without_monitor) {
    CaptureService s;
    CHECK_TRUE(s.start(CaptureParams{}));
    CHECK_TRUE(s.started());
    CHECK_FALSE(s.start(CaptureParams{}));   // 已启动
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Off; }));

    // 开启但没有目标显示器 → 暂停（不建会话），发布 0
    s.set_target(nullptr, true, true);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Paused; }));
    CHECK_EQ(s.speed(), 0.0);
    CHECK_EQ(s.debug().sessions, 0u);

    // 不需要取帧 → 暂停
    s.set_target(nullptr, true, false);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Paused; }));

    // 关闭 → Off
    s.set_target(nullptr, false, false);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Off; }));
    CHECK_EQ(s.speed(qpc_now_s()), 0.0);

    const auto t0 = std::chrono::steady_clock::now();
    s.stop();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("   stop() from Off took %.2f ms\n", ms);
    CHECK(ms < 500.0);   // spec：≤ 500 ms join
    CHECK_FALSE(s.started());
    CHECK_TRUE(s.state() == CaptureState::Off);
}

TEST(capture_service_unknown_monitor_recovers_and_toggle_resets) {
    CaptureService s;
    CHECK_TRUE(s.start(CaptureParams{}));
    s.set_target(bogus_monitor(), true, true);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Recovering; }));
    CHECK_TRUE(wait_until([&] { return s.debug().build_failures >= 1; }));
    CHECK_EQ(s.speed(), 0.0);
    CHECK_EQ(s.debug().sessions, 0u);
    // 2 s 重试等待期间也能立即响应关闭
    s.set_target(bogus_monitor(), false, true);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Off; }, 200));
    // 关 → 开：立即重新尝试（不等 2 s），失败计数从 0 开始
    const std::uint64_t before = s.debug().build_failures;
    s.set_target(bogus_monitor(), true, true);
    CHECK_TRUE(wait_until([&] { return s.debug().build_failures > before; }, 500));
    CHECK_TRUE(s.state() == CaptureState::Recovering);
    // 暂停优先于重建
    s.set_target(bogus_monitor(), true, false);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Paused; }, 200));
    const auto t0 = std::chrono::steady_clock::now();
    s.stop();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 500.0);
}

TEST(capture_service_params_sanitized_and_applied) {
    CaptureService s;
    CaptureParams p;
    p.grid_w = 999;    // → 320
    p.grid_h = 108;
    CHECK_TRUE(s.start(p));
    CHECK_TRUE(wait_until([&] { return s.debug().grid_w == 320 && s.debug().grid_h == 108; }));
    p.grid_w = 192;
    s.update_params(p);
    CHECK_TRUE(wait_until([&] { return s.debug().grid_w == 192; }));
    s.stop();
}

TEST(capture_service_grid_sink_set_before_start) {
    // 网格回调：start() 之前设置；没有取到帧时从不调用；start() 之后的设置被忽略；计数从 0 开始。
    CaptureService s;
    int calls = 0;
    s.set_grid_sink([&calls](const GridFrame& g) {
        if (g.gray != nullptr && g.grid_w > 0) ++calls;
    });
    CHECK_TRUE(s.start(CaptureParams{}));
    s.set_grid_sink(GridSink{});   // 已启动：忽略（不影响截屏线程手里的那份）
    s.set_target(nullptr, true, true);
    CHECK_TRUE(wait_until([&] { return s.state() == CaptureState::Paused; }));
    const CaptureDebug d = s.debug();
    CHECK_EQ(d.frames, 0u);
    CHECK_EQ(d.skipped, 0u);
    s.stop();
    CHECK_EQ(calls, 0);
    // stop() 之后可以清掉 / 重新设置（下次 start() 生效）
    s.set_grid_sink(nullptr);
    CHECK_TRUE(s.start(CaptureParams{}));
    s.stop();
}

TEST(capture_cpu_clock_counts_busy_not_sleep) {   // 截屏线程的 cpu_pct 用周期计数（GetThreadTimes 对短促的忙碌几乎读到 0）
    const double hz = tsc_frequency();
    std::printf("   TSC frequency: %.1f MHz\n", hz / 1e6);
    CHECK(hz > 1e8);
    const double c0 = thread_cpu_seconds();
    ::Sleep(100);
    const double slept = thread_cpu_seconds() - c0;
    const double c1 = thread_cpu_seconds();
    const double t1 = qpc_now_s();
    volatile double sink = 0;
    while (qpc_now_s() - t1 < 0.1) sink = sink + 1.0;   // 忙 100 ms
    const double busy = thread_cpu_seconds() - c1;
    std::printf("   thread cpu: sleep 100 ms -> %.4f s, busy 100 ms -> %.4f s\n", slept, busy);
    CHECK(slept >= 0.0);
    CHECK(slept < 0.01);
    CHECK(busy > 0.05);
    CHECK(busy < 0.15);
    CHECK(process_cpu_seconds() >= thread_cpu_seconds() - 1e-3);
}

TEST(capture_debug_field_text) {
    // overlay-rendering「调试条」的截屏字段（app/app.h format_capture_debug_field）。
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Running, 337.4, 0.934), L"337°/s c0.93");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Running, 0.0, 0.0), L"0°/s c0.00");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Running, 12.6, 1.0), L"13°/s c1.00");
    // 变更 5：取帧时在后面接前进 / 倾斜
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Running, 337.4, 0.934, 292.6, 0.4),
             L"337°/s c0.93 · 前进 293°/s · 倾斜 0°/s");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Running, 0.0, 1.0, 0.0, 360.0),
             L"0°/s c1.00 · 前进 0°/s · 倾斜 360°/s");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Paused, 0, 0, 100, 100), L"暂停");
    CHECK_EQ(format_capture_debug_field(false, CaptureState::Running, 0, 0, 100, 100), L"关");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Paused, 300, 0.9), L"暂停");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Recovering, 300, 0.9), L"暂停");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Failed, 0, 0), L"失败");
    CHECK_EQ(format_capture_debug_field(true, CaptureState::Off, 0, 0), L"关");
    // --no-capture / capture=0：不论服务状态都是"关"
    CHECK_EQ(format_capture_debug_field(false, CaptureState::Running, 300, 0.9), L"关");
    CHECK_EQ(format_capture_debug_field(false, CaptureState::Failed, 0, 0), L"关");
}

TEST(capture_command_line_flag) {
    // configuration「命令行参数」：--no-capture 被接受（app 据此不启动截屏线程），不算未知参数。
    const std::wstring args[] = {L"--no-capture", L"--verbose"};
    const CommandLine c = parse_command_line(args);
    CHECK_TRUE(c.no_capture);
    CHECK_TRUE(c.verbose);
    CHECK_TRUE(c.unknown.empty());
    const std::wstring none[] = {L"--verbose"};
    CHECK_FALSE(parse_command_line(none).no_capture);
}
