// app.cpp — app/app 的实现：启动顺序（design D13）、MsgWaitForMultipleObjectsEx 主循环（D1）、tick、配置监视与重载（D9）、托盘 / 热键动作、游戏内校准的接线（变更 2）、截屏运动检测的接线（变更 3）、设置窗口的接线（变更 4：消息泵、预览下限、托盘双击与“设置…”、open_settings）、开机自启（D12）、frame / perf / exit 日志。
#include "app/app.h"

#include "i18n/i18n.h"

#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

#include "res/resource.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace mc {

App* App::s_instance_ = nullptr;

namespace {

constexpr double kIdleTickS = 0.030;          // 空闲轮询间隔
constexpr double kActiveSafetyS = 0.100;      // 活动模式下等待 latency 对象 / 定时器的兜底超时
constexpr double kReloadDebounceS = 0.500;    // 配置文件变化后的防抖
constexpr double kTopmostIntervalS = 1.0;     // 置顶兜底
constexpr double kFrameLogIntervalS = 0.250;  // frame 行的最长间隔
constexpr double kPerfIntervalS = 5.0;        // perf 行间隔
constexpr UINT kModalTimerMs = 30;
constexpr int kMaxMessagesPerPump = 4096;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MotionCalm";

std::wstring module_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return L"MotionCalm.exe";
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring dir_of(const std::wstring& path) {
    const auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring(L".") : path.substr(0, p);
}

std::wstring file_of(const std::wstring& path) {
    const auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring full_path(const std::wstring& path) {
    const DWORD n = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) return path;
    std::wstring out(n, L'\0');
    const DWORD m = ::GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
    if (m == 0 || m >= n) return path;
    out.resize(m);
    return out;
}

std::wstring windows_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return L"?";
    const auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (fn == nullptr || fn(&vi) != 0) return L"?";
    return std::format(L"{}.{}.{}", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
}

double process_cpu_s() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto secs = [](const FILETIME& f) {
        ULARGE_INTEGER li{};
        li.LowPart = f.dwLowDateTime;
        li.HighPart = f.dwHighDateTime;
        return static_cast<double>(li.QuadPart) / 1e7;
    };
    return secs(kernel) + secs(user);
}

std::wstring version_string() {
    return std::format(L"{}.{}.{}.{}", MC_VERSION_MAJOR, MC_VERSION_MINOR, MC_VERSION_PATCH, MC_VERSION_BUILD);
}

std::wstring join_args(std::span<const std::wstring> args) {
    std::wstring s;
    for (const auto& a : args) {
        if (!s.empty()) s += L' ';
        if (a.find_first_of(L" \t") != std::wstring::npos) {
            s += L'"';
            s += a;
            s += L'"';
        } else {
            s += a;
        }
    }
    return s;
}

std::wstring one_line(std::wstring s) {
    for (auto& ch : s) {
        if (ch == L'\n' || ch == L'\r') ch = L'/';
    }
    return s;
}

MaskLevel next_level(MaskLevel l) {
    switch (l) {
        case MaskLevel::Light: return MaskLevel::Medium;
        case MaskLevel::Medium: return MaskLevel::Strong;
        case MaskLevel::Strong: return MaskLevel::Light;
    }
    return MaskLevel::Medium;
}

Sensitivity next_sensitivity(Sensitivity s) {
    switch (s) {
        case Sensitivity::Low: return Sensitivity::Medium;
        case Sensitivity::Medium: return Sensitivity::High;
        case Sensitivity::High: return Sensitivity::Low;
    }
    return Sensitivity::Medium;
}

using S = i18n::StrId;
using i18n::tr;

const wchar_t* on_off(bool on) { return tr(on ? S::On : S::Off); }

}  // namespace

// ---------------------------------------------------------------- 命令行

CommandLine parse_command_line(std::span<const std::wstring> args) {
    CommandLine c;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (iequals(a, L"--config")) {
            if (i + 1 < args.size()) {
                c.config_path = args[++i];
            } else {
                c.errors.push_back(L"--config 缺少路径");
            }
        } else if (iequals(a, L"--verbose")) {
            c.verbose = true;
        } else if (iequals(a, L"--no-capture")) {
            c.no_capture = true;
        } else if (iequals(a, L"--force-game")) {
            c.force_game = true;
        } else if (iequals(a, L"--portable")) {
            c.portable = true;
        } else if (iequals(a, L"--exit-after")) {
            if (i + 1 < args.size()) {
                const std::string v = wide_to_utf8(trim(std::wstring_view(args[++i])));
                double secs = 0;
                const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), secs);
                if (ec == std::errc{} && ptr == v.data() + v.size() && secs > 0 && std::isfinite(secs)) {
                    c.exit_after_s = secs;
                } else {
                    c.errors.push_back(L"--exit-after 的值无效：" + args[i]);
                }
            } else {
                c.errors.push_back(L"--exit-after 缺少秒数");
            }
        } else {
            c.unknown.push_back(a);
        }
    }
    return c;
}

std::wstring format_capture_debug_field(bool user_on, CaptureState state, double deg_per_s, double confidence,
                                        double zoom_deg_per_s, double roll_deg_per_s) {
    if (!user_on) return i18n::tr(i18n::StrId::Off);
    switch (state) {
        case CaptureState::Running: {
            std::wstring s = i18n::format(i18n::StrId::DebugCaptureRunning,
                                          {{L"deg", std::to_wstring(std::lround(deg_per_s))},
                                           {L"conf", std::format(L"{:.2f}", std::clamp(confidence, 0.0, 1.0))}});
            if (zoom_deg_per_s >= 0 && roll_deg_per_s >= 0) {
                s += i18n::format(i18n::StrId::DebugCaptureForward, {{L"zoom", std::to_wstring(std::lround(zoom_deg_per_s))},
                                                                     {L"roll", std::to_wstring(std::lround(roll_deg_per_s))}});
            }
            return s;
        }
        case CaptureState::Paused:
        case CaptureState::Recovering: return i18n::tr(i18n::StrId::DebugCapturePaused);
        case CaptureState::Failed: return i18n::tr(i18n::StrId::DebugCaptureFailed);
        case CaptureState::Off: return i18n::tr(i18n::StrId::Off);
    }
    return i18n::tr(i18n::StrId::Off);
}

// ---------------------------------------------------------------- 生命周期

App::App(HINSTANCE instance) : instance_(instance), t0_(qpc_now_s()) {}

App::~App() { shutdown(); }

int App::run() {
    if (!init()) {
        log::warn("startup failed; exiting");
        shutdown();
        return 1;
    }
    loop();
    shutdown();
    return 0;
}

bool App::init() {
    s_instance_ = this;

    // 1. 命令行
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        ::LocalFree(argv);
    }
    cmd_ = parse_command_line(args);
    cmdline_text_ = join_args(args);

    // 2. 路径：日志总在 exe 目录；配置默认也在 exe 目录
    exe_path_ = module_path();
    exe_dir_ = dir_of(exe_path_);
    config_path_ = cmd_.config_path.empty() ? exe_dir_ + L"\\MotionCalm.ini" : full_path(cmd_.config_path);
    config_dir_ = dir_of(config_path_);
    config_name_ = file_of(config_path_);
    log_path_ = exe_dir_ + L"\\MotionCalm.log";

    // 3. 日志与 start 行
    log::init(log_path_, cmd_.verbose ? log::Level::Debug : log::Level::Info);
    log::info(L"start MotionCalm v{} windows={} pid={} config=\"{}\" args=\"{}\"", version_string(),
              windows_version(), ::GetCurrentProcessId(), config_path_, cmdline_text_);
    for (const auto& u : cmd_.unknown) log::warn(L"ignored unknown command-line argument: {}", u);
    for (const auto& e : cmd_.errors) log::warn(L"command line: {}", e);
    if (cmd_.force_game) {
        log::warn("TEST MODE --force-game: every foreground program (except MotionCalm) is treated as a known, "
                  "cursor-locked game; known_games is not written; the exclude list still applies");
    }
    if (cmd_.no_capture) log::info("截屏检测已由 --no-capture 禁用 (no capture thread this run; INI unchanged)");
    if (cmd_.exit_after_s > 0) {
        exit_deadline_ = t0_ + cmd_.exit_after_s;
        log::info("--exit-after {:.3f} s", cmd_.exit_after_s);
    }

    // 4. 配置（模板来自 RCDATA）
    if (!load_template()) {
        log::warn("embedded INI template (RCDATA {}) missing; running with defaults, saving disabled",
                  IDR_DEFAULT_INI);
        config_writable_ = false;
        cfg_ = Config{};
    } else {
        load_initial_config();
    }
    if (!cmd_.verbose) log::set_level(cfg_.log_level);

    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(co)) log::warn("CoInitializeEx failed hr=0x{:08X}", static_cast<unsigned>(co));

    // 5. host 窗口
    if (!create_host_window()) return false;

    // 6. Raw Input（相对事件的 Σdx 同步交给校准状态机，它只在 Run1 / Run2 中累加）
    raw_.set_on_relative_dx([this](long long dx) { calib_.on_mouse_dx(dx); });
    raw_.register_devices(host_);

    // 7. overlay（窗口 + D3D/DComp/D2D）
    if (!overlay_.init(instance_, log::enabled(log::Level::Debug))) {
        log::warn("overlay window could not be created; mask and toasts are unavailable");
    }

    // 8. 托盘
    // 不用 LoadIconMetric：它只在 comctl32 v6（SxS）里导出，而 tests.exe 没有 manifest 也链接本文件。
    tray_icon_ = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                 ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
                                                 LR_DEFAULTCOLOR));
    taskbar_created_msg_ = Tray::taskbar_created_message();
    if (taskbar_created_msg_ != 0) ::ChangeWindowMessageFilterEx(host_, taskbar_created_msg_, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(host_, WM_APP_ALREADY_RUNNING, MSGFLT_ALLOW, nullptr);
    tray_.add(host_, WM_APP_TRAY, tray_icon_);

    // 9. 热键 / 模型 / 自启 / 截屏可见性（统一入口 apply_config）；随后启动截屏线程（只建线程，进游戏才取帧）
    apply_config(true);
    if (!config_writable_) show_toast(tr(S::ToastConfigWriteFailed));
    start_capture();

    // 10. 前台变化事件（out-of-context，不是注入式钩子）
    fg_hook_ = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                 &App::foreground_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (fg_hook_ == nullptr) log::warn("SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed error={}", ::GetLastError());

    // 11. 配置目录监视
    start_watcher();

    // 12. 活动模式的帧定时器（高精度，不可用时退回普通 waitable timer）
    frame_timer_.reset(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS));
    if (!frame_timer_) frame_timer_.reset(::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));

    last_tick_t_ = qpc_now_s();
    next_idle_tick_ = last_tick_t_;
    perf_t0_ = last_tick_t_;
    perf_presents0_ = overlay_.presents();
    ULONG64 cycles = 0;
    ::QueryProcessCycleTime(::GetCurrentProcess(), &cycles);
    perf_cycles0_ = cycles;
    ULONG64 main_cycles = 0;
    ::QueryThreadCycleTime(::GetCurrentThread(), &main_cycles);
    perf_main_cycles0_ = main_cycles;
    log::info("ready: hotkeys={} tray={} overlay={}", hotkeys_.registered().size(), tray_.added() ? "ok" : "pending",
              overlay_.hwnd() != nullptr ? "ok" : "unavailable");
    return true;
}

void App::shutdown() {
    if (shut_down_) return;
    // 设置窗口开着时先按"关闭"的步骤关掉它（提交编辑框 → 保存 → 清预览 → 恢复热键 → 销毁），再落盘挂起的改动，
    // 然后才写 exit 行（settings-window「关闭窗口」、configuration「设置窗口改动的保存时机」）。
    settings_.close();
    flush_config();
    shut_down_ = true;
    if (fg_hook_ != nullptr) {
        ::UnhookWinEvent(fg_hook_);
        fg_hook_ = nullptr;
    }
    hotkeys_.unregister_all();
    tray_.remove();
    raw_.unregister_devices();
    stop_watcher();
    capture_.stop();   // 截屏线程 join（≤ 16 ms 内看到停止标志），释放桌面复制对象
    overlay_.poll_stats(qpc_now_s(), true);    // 最后一个不满 5 s 的统计窗口（--verbose）
    write_perf_log(qpc_now_s(), 0.0, true);
    const std::uint64_t presents = overlay_.presents();
    overlay_.shutdown();
    if (host_ != nullptr) {
        ::KillTimer(host_, kModalTimerId);
        ::KillTimer(host_, kSaveTimerId);
        ::KillTimer(host_, kTrayClickTimerId);
        ::DestroyWindow(host_);
        host_ = nullptr;
    }
    if (host_class_registered_) {
        ::UnregisterClassW(kHostClassName, instance_);
        host_class_registered_ = false;
    }
    if (tray_icon_ != nullptr) {
        ::DestroyIcon(tray_icon_);
        tray_icon_ = nullptr;
    }
    frame_timer_.reset();
    const double wall = qpc_now_s() - t0_;
    const double cpu = process_cpu_s();
    const double pct = wall > 0 ? cpu / wall * 100.0 : 0.0;
    log::info("exit reason={} wall_s={:.3f} cpu_s={:.3f} cpu_pct={:.2f} presents={}", quit_reason_, wall, cpu, pct,
              presents);
    log::shutdown();
    ::CoUninitialize();
    if (s_instance_ == this) s_instance_ = nullptr;
}

bool App::create_host_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &App::host_proc;
    wc.hInstance = instance_;
    wc.lpszClassName = kHostClassName;
    wc.hIcon = ::LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    if (::RegisterClassExW(&wc) == 0) {
        log::warn("RegisterClassEx(MotionCalm.Host) failed error={}", ::GetLastError());
        return false;
    }
    host_class_registered_ = true;
    // 隐藏的普通顶层窗口（不是 HWND_MESSAGE）：TrackPopupMenu 前要能成为前台、能收到 TaskbarCreated 广播（design D2）。
    host_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, L"MotionCalm", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, instance_, this);
    if (host_ == nullptr) {
        log::warn("CreateWindowEx(MotionCalm.Host) failed error={}", ::GetLastError());
        return false;
    }
    return true;
}

void App::request_quit(const char* reason) {
    if (!quit_) {
        quit_ = true;
        quit_reason_ = reason;
        log::info("quit requested: {}", reason);
    }
    if (in_menu_) ::EndMenu();
}

// ---------------------------------------------------------------- 主循环（design D1）

void App::arm_frame_timer() {
    if (!frame_timer_) return;
    const double period = 1.0 / std::max(24.0, overlay_.refresh_hz());
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(period * 1e7);   // 相对时间，100 ns 单位
    timer_armed_ = ::SetWaitableTimer(frame_timer_.get(), &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

void App::loop() {
    while (!quit_) {
        const double now = qpc_now_s();
        HANDLE handles[2] = {};
        DWORD n = 0;
        DWORD dir_index = MAXDWORD;
        DWORD frame_index = MAXDWORD;
        bool frame_is_latency = false;
        if (dir_event_ && dir_pending_) {
            dir_index = n;
            handles[n++] = dir_event_.get();
        }

        // 消息本身不触发 tick：tick 只在"到期"时执行。
        double tick_due;
        if (tick_now_) {
            tick_due = now;
        } else if (active_) {
            HANDLE frame_handle = nullptr;
            const HANDLE latency = overlay_.frame_latency_handle();
            if (last_presented_ && latency != nullptr) {
                // 上一个 tick Present 过：等交换链的 frame-latency 对象（按刷新率节拍）。
                frame_handle = latency;
                frame_is_latency = true;
            } else if (frame_timer_) {
                // 没 Present 过：latency 对象会一直处于触发状态，改等高精度定时器（1 / 刷新率）。
                if (!timer_armed_) arm_frame_timer();
                if (timer_armed_) frame_handle = frame_timer_.get();
            }
            if (frame_handle != nullptr) {
                frame_index = n;
                handles[n++] = frame_handle;
                tick_due = last_tick_t_ + kActiveSafetyS;
            } else {
                tick_due = last_tick_t_ + 1.0 / std::max(24.0, overlay_.refresh_hz());
            }
        } else {
            tick_due = next_idle_tick_;   // 空闲：30 ms 轮询，不 Present
        }
        double wake_at = tick_due;
        if (reload_deadline_ > 0) wake_at = std::min(wake_at, reload_deadline_);
        if (exit_deadline_ > 0) wake_at = std::min(wake_at, exit_deadline_);
        DWORD timeout = 0;
        if (wake_at > now) timeout = static_cast<DWORD>(std::ceil((wake_at - now) * 1000.0));

        // 原始输入（QS_RAWINPUT）一般不唤醒：1–8 kHz 鼠标逐个唤醒的系统调用开销远超目标，改为每次醒来用
        // GetRawInputBuffer 批量读取（design D3 的可选优化）。例外：在游戏里、鼠标项生效、且事件稀疏
        // （系统把后台鼠标输入合并成 ~125 Hz 一包）时，为每包醒来读取，让速度窗口拿到真实的到达时刻
        // （见 RawInput::wants_event_wake）；醒来只读取，不 tick。其余消息（热键、托盘…）照常唤醒。
        DWORD wake_mask = QS_ALLINPUT & ~static_cast<DWORD>(QS_RAWINPUT);
        if (raw_wake_ && raw_.wants_event_wake(now)) wake_mask = QS_ALLINPUT;
        const DWORD r =
            ::MsgWaitForMultipleObjectsEx(n, n > 0 ? handles : nullptr, timeout, wake_mask, MWMO_INPUTAVAILABLE);
        ++wakeups_;
        const bool frame_signaled = frame_index != MAXDWORD && r == WAIT_OBJECT_0 + frame_index;
        const bool dir_signaled = dir_index != MAXDWORD && r == WAIT_OBJECT_0 + dir_index;
        if (frame_signaled) {
            if (frame_is_latency) {
                overlay_.note_frame_ready();   // 已消耗一次 latency 信号，渲染时不再等
            } else {
                timer_armed_ = false;
            }
        }

        raw_.drain(qpc_now_s());   // 先批量读走原始输入（连同 WM_INPUT），pump 时就不必逐条处理
        pump_messages();
        if (quit_) break;
        if (dir_signaled) on_watcher_signaled();

        const double t = qpc_now_s();
        if (reload_deadline_ > 0 && t >= reload_deadline_) {
            reload_deadline_ = 0;
            check_external_reload();
        }
        if (exit_deadline_ > 0 && t >= exit_deadline_) {
            request_quit("exit-after");
            break;
        }
        if (tick_now_ || frame_signaled || t >= tick_due) {
            tick();
            if (timer_armed_) {
                ::CancelWaitableTimer(frame_timer_.get());
                timer_armed_ = false;
            }
        }
    }
}

void App::pump_messages() {
    MSG msg;
    for (int i = 0; i < kMaxMessagesPerPump && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (msg.message == WM_QUIT) {
            request_quit("quit");
            return;
        }
        if (settings_.pre_translate(msg)) continue;   // 设置窗口的对话框导航（IsDialogMessage）与 Ctrl+Tab
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
}

void App::tick() {
    if (shut_down_) return;
    const double now = qpc_now_s();
    const double dt = std::clamp(now - last_tick_t_, 0.0, IntensityModel::kMaxDt);
    last_tick_t_ = now;
    tick_now_ = false;

    // 1. 手柄
    Gamepad::State pad{};
    if (cfg_.trigger.gamepad) pad = pad_.poll(now, cfg_.trigger.gamepad_deadzone, cfg_.trigger.gamepad_max_deg_per_s);

    // 2. 游戏判定（≤ 60 Hz；前台变化事件后强制采样）
    if (detector_.poll(MouseLockHint{raw_.raw_path_100ms(now)}, cfg_.detect, force_sample_)) {
        force_sample_ = false;
        const GameContext& c = detector_.ctx();
        if (c.fg != last_fg_) {
            last_fg_ = c.fg;
            raw_.clear_keys();   // 前台一变就清空按下状态（防卡键）
        }
        if (should_record_known_game(c, cfg_.detect, cmd_.force_game)) {
            const std::wstring exe = c.exe;
            if (list_add_unique(cfg_.detect.known_games, exe)) {
                log::info(L"known game recorded (first cursor lock): {}", exe);
                save_config();
                detector_.reevaluate(cfg_.detect);
            }
        }
    }
    const GameContext& ctx = detector_.ctx();

    // 3. 校准：超时 / 离开游戏的取消检查（只在进行中）
    if (calib_.active()) {
        const CalibState from = calib_.state();
        const long long n = calib_.current_abs_count();
        apply_calib_effect(calib_.update(now, make_calib_context(ctx)), from, n);
        log_calib_progress(now);
    }

    // 4. 门控（截屏项另需 [trigger] capture）
    const Gates gates = compute_gates(ctx, cfg_.enabled, cfg_.trigger.mouse, cfg_.trigger.keyboard,
                                      cfg_.trigger.gamepad, cfg_.trigger.capture);

    // 5. 截屏运动检测（capture-motion-detection design D9）：先按开启状态设好截屏排除，再告诉截屏线程是否取帧。
    //    feature_on 传 user_on（不含"采集失败"）：否则 Failed 会让 feature_on 关 → 开，截屏线程就会反复重试。
    const bool cap_user_on = capture_user_on();
    const bool cap_on = cap_user_on && capture_.state() != CaptureState::Failed;
    apply_capture_visibility(cap_on);
    HMONITOR cap_mon = ctx.monitor;   // overlay 跟随的显示器（前台窗口所在）
    if (cap_mon == nullptr) cap_mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const bool cap_wanted = cap_on && gates.capture;
    capture_.set_target(cap_mon, cap_user_on, cap_wanted);   // 只在有变化时唤醒截屏线程
    capture_mon_ = cap_mon;
    capture_wanted_ = cap_wanted;
    const double capture_deg = cap_on ? capture_.speed(now) : 0.0;   // 过期（> 250 ms）/ 暂停 / 失败时为 0
    // 变更 5：前进 / 后退的径向流速（未乘 forward_gain）与倾斜角速度，规则同上；detect_roll=0 时倾斜恒为 0。
    const double zoom_deg = cap_on ? capture_.zoom_speed(now) : 0.0;
    const double roll_deg = cap_on ? capture_.roll_speed(now) : 0.0;

    // 6. 强度模型：counts_per_degree 取前台 exe 的校准值，没有则用 default_counts_per_degree
    const double fallback_cpd =
        cfg_.trigger.default_counts_per_degree > 0 ? cfg_.trigger.default_counts_per_degree : 18.2;
    const double cpd = counts_per_degree_for(ctx.exe, cfg_.calibration, fallback_cpd);
    if (cpd != cpd_used_ || ctx.exe != cpd_exe_) {
        if (!ctx.exe.empty()) {
            log::debug(L"counts_per_degree exe={} cpd={:.3f} ({})", ctx.exe, cpd,
                       cfg_.calibration.contains(ctx.exe) ? L"calibrated" : L"default");
        }
        cpd_used_ = cpd;
        cpd_exe_ = ctx.exe;
    }
    const double mouse_deg = raw_.counts_per_s(now) / cpd;
    const bool moving = raw_.moving(now);
    IntensityInputs in;
    in.mouse_deg_per_s = mouse_deg;
    in.moving = moving;
    in.pad_deg_per_s = pad.pad_deg_per_s;
    in.pad_move = pad.pad_move;
    in.capture_deg_per_s = capture_deg;
    in.capture_zoom_deg_per_s = zoom_deg;
    in.capture_roll_deg_per_s = roll_deg;
    in.gate_mouse = gates.mouse;
    in.gate_keys = gates.keys;
    in.gate_pad = gates.pad;
    in.gate_capture = cap_wanted;   // = cap_on && gates.capture
    model_.update(in, dt);

    // 7. 组装帧。设置窗口的预览强度 p > 0 时进入预览模式（overlay-rendering「设置窗口预览强度」）：不论 enabled /
    //    in_game 都画遮罩与参照点，强度取 max(模型输出, p)；p 只作用于渲染，模型状态不变。
    const float preview = settings_.is_open() ? preview_floor_ : 0.0f;
    const bool preview_on = preview > 0.0f;
    const bool normal_scene = cfg_.enabled && ctx.in_game;
    OverlayFrame f;
    f.scene = normal_scene || preview_on;
    f.intensity = normal_scene ? model_.value() : 0.0f;
    if (preview_on) f.intensity = std::max(f.intensity, preview);
    f.level = cfg_.mask.level;
    f.show_center_dot = cfg_.dots.center_dot;
    f.show_edge_dots = cfg_.dots.edge_dots;
    if (!toast_.empty()) {
        const double age = now - toast_t_;
        if (age < Overlay::kToastTotalS) {
            f.toast = toast_;
            f.toast_age_s = age;
        } else {
            toast_.clear();
        }
    }
    if (debug_bar_) {
        f.debug_bar = true;
        f.debug_text = make_debug_text(ctx, mouse_deg, moving, pad.pad_deg_per_s, capture_deg, zoom_deg, roll_deg);
        f.debug_target = model_.target();
        f.debug_value = model_.value();
    }
    // 校准提示：只在进行中非空；f.has_text() 因此为真 → enabled=0 时也以仅文字模式显示，并保持活动节拍。
    if (calib_.active()) {
        const std::wstring hk = calibrate_hotkey_text();
        f.message = calib_.message(hk.empty() ? std::wstring_view(L"None") : std::wstring_view(hk));
    }

    // 8. 显示 / 隐藏、跟随显示器、置顶兜底、按需 Present
    const bool want_visible = f.scene || f.has_text();
    if (want_visible && overlay_.hwnd() != nullptr) {
        HMONITOR mon = ctx.monitor;   // 仅文字模式也用前台窗口所在的显示器
        // 预览且不在游戏里：显示在设置窗口所在的显示器上（在游戏里时仍跟随游戏）。
        if (preview_on && !ctx.in_game && settings_.hwnd() != nullptr) {
            mon = ::MonitorFromWindow(settings_.hwnd(), MONITOR_DEFAULTTONEAREST);
        }
        if (mon == nullptr) mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        overlay_.follow_monitor(mon);
        if (now - last_topmost_t_ >= kTopmostIntervalS) {
            overlay_.recheck_topmost();   // 兜底：确实被盖住（或丢了 TOPMOST）才重新置顶
            last_topmost_t_ = now;
        }
    }
    overlay_.set_visible(want_visible);
    last_presented_ = overlay_.render(f, cfg_.mask, cfg_.dots);
    overlay_.poll_stats(now);

    // 9. 日志、节拍、退出；"采集失败"状态变化时刷新设置窗口（截屏复选框的文字与灰显）
    write_frame_log(now, ctx, mouse_deg, moving, pad.pad_deg_per_s, capture_deg, zoom_deg, roll_deg, preview);
    const bool cap_failed = capture_.state() == CaptureState::Failed;
    if (cap_failed != capture_failed_seen_) {
        capture_failed_seen_ = cap_failed;
        settings_.refresh();
    }
    active_ = want_visible && (model_.value() > 0.0f || model_.target() > 0.0f || f.has_text() || overlay_.dirty());
    raw_wake_ = f.scene && gates.mouse;   // 只有鼠标速度真正参与强度计算时才值得为原始输入醒来
    next_idle_tick_ = now + kIdleTickS;
    write_perf_log(now, (qpc_now_s() - now) * 1000.0);
    if (exit_deadline_ > 0 && now >= exit_deadline_) request_quit("exit-after");
}

void App::write_frame_log(double now, const GameContext& ctx, double mouse_deg, bool moving, double pad_deg,
                          double capture_deg, double zoom_deg, double roll_deg, float preview) {
    if (!log::enabled(log::Level::Debug)) return;
    const double t = now - t0_;
    const double target = model_.target();
    const double value = model_.value();   // 始终是模型值（不含预览下限），verify.ps1 按此解析
    const double p = preview;
    const FrameLog& last = frame_log_;
    const bool write = !last.valid || std::fabs(target - last.target) >= 0.001 ||
                       std::fabs(value - last.value) >= 0.001 || std::fabs(p - last.preview) >= 0.001 ||
                       ctx.in_game != last.in_game || ctx.cursor_locked != last.locked || ctx.exe != last.exe ||
                       t - last.t >= kFrameLogIntervalS;
    if (!write) return;
    // 预览强度只在 > 0 时追加在行尾，不改变已有字段的顺序与写法；变更 5 的 zoom / roll 总在最后。
    const std::string preview_field = p > 0.0 ? std::format(" preview={:.3f}", p) : std::string();
    log::debug("frame t={:.3f} in_game={} locked={} mouse={:.1f} moving={} pad={:.1f} capture={:.1f} target={:.3f} "
               "value={:.3f} exe={}{} zoom={:.1f} roll={:.1f}",
               t, ctx.in_game ? 1 : 0, ctx.cursor_locked ? 1 : 0, mouse_deg, moving ? 1 : 0, pad_deg, capture_deg,
               target, value, wide_to_utf8(ctx.exe), preview_field, zoom_deg, roll_deg);
    frame_log_.preview = p;
    frame_log_.valid = true;
    frame_log_.t = t;
    frame_log_.target = target;
    frame_log_.value = value;
    frame_log_.in_game = ctx.in_game;
    frame_log_.locked = ctx.cursor_locked;
    frame_log_.exe = ctx.exe;
}

void App::write_perf_log(double now, double tick_ms, bool flush) {
    if (!flush) {
        ++perf_ticks_;
        perf_sum_ms_ += tick_ms;
        perf_max_ms_ = std::max(perf_max_ms_, tick_ms);
    }
    if (!flush && now - perf_t0_ < kPerfIntervalS) return;
    const std::uint64_t presents = overlay_.presents();
    const std::uint64_t inputs = raw_.mouse_events();
    // 进程 CPU 周期（含驱动线程）：GetProcessTimes 按时钟中断采样，短促的唤醒常被记成 0，周期计数更可靠。
    ULONG64 cycles = 0;
    ::QueryProcessCycleTime(::GetCurrentProcess(), &cycles);
    // 主线程周期：与 proc_mcycles 的差 = 图形驱动 / 运行时自己的线程。
    ULONG64 main_cycles = 0;
    ::QueryThreadCycleTime(::GetCurrentThread(), &main_cycles);
    log::debug("perf ticks={} avg_tick_ms={:.3f} max_tick_ms={:.3f} presents={} wakeups={} mouse_events={} "
               "proc_mcycles={} main_mcycles={} window_s={:.2f}",
               perf_ticks_, perf_ticks_ > 0 ? perf_sum_ms_ / perf_ticks_ : 0.0, perf_max_ms_,
               presents - perf_presents0_, wakeups_ - perf_wakeups0_, inputs - perf_inputs0_,
               (cycles - perf_cycles0_) / 1000000, (main_cycles - perf_main_cycles0_) / 1000000, now - perf_t0_);
    perf_cycles0_ = cycles;
    perf_main_cycles0_ = main_cycles;
    perf_t0_ = now;
    perf_ticks_ = 0;
    perf_sum_ms_ = 0;
    perf_max_ms_ = 0;
    perf_presents0_ = presents;
    perf_wakeups0_ = wakeups_;
    perf_inputs0_ = inputs;
}

// ---------------------------------------------------------------- host 窗口

LRESULT CALLBACK App::host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->handle_host_message(hwnd, msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle_host_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT:
            raw_.handle(lp);
            return ::DefWindowProcW(hwnd, msg, wp, lp);   // RIM_INPUT 时系统靠它清理（design D3）
        case WM_HOTKEY:
            if (const auto action = HotkeyManager::action_from_id(wp)) do_hotkey_action(*action, "hotkey");
            return 0;
        case WM_APP_TRAY:
            on_tray_message(wp, lp);
            return 0;
        case WM_APP_ALREADY_RUNNING:
            log::info("a second instance was started and exited");
            show_toast(tr(S::ToastAlreadyRunning));
            return 0;
        case WM_TIMER:
            if (wp == kModalTimerId) {
                if (modal_depth_ > 0 && !shut_down_) tick();
                return 0;
            }
            if (wp == kSaveTimerId) {
                on_save_timer();
                return 0;
            }
            if (wp == kTrayClickTimerId) {
                ::KillTimer(hwnd, kTrayClickTimerId);
                if (tray_click_pending_) {
                    tray_click_pending_ = false;
                    show_tray_menu(tray_click_pt_);
                }
                return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
            tick_now_ = true;
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp != 0) {
                request_quit("session-end");
                shutdown();
            }
            return 0;
        case WM_CLOSE:
            request_quit("close");
            return 0;
        default:
            if (msg != 0 && msg == taskbar_created_msg_) {
                tray_.readd();
                return 0;
            }
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void CALLBACK App::foreground_event_proc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (App* app = s_instance_) app->on_foreground_changed();
}

void App::on_foreground_changed() {
    if (shut_down_) return;
    // 回调里立即：重新置顶、清空键盘状态、强制采样；显示器跟随与显示 / 隐藏在紧接着的 tick 中完成。
    overlay_.keep_topmost();
    last_topmost_t_ = qpc_now_s();
    raw_.clear_keys();
    force_sample_ = true;
    tick_now_ = true;
}

void App::enter_modal() {
    if (modal_depth_++ == 0 && host_ != nullptr) ::SetTimer(host_, kModalTimerId, kModalTimerMs, nullptr);
}

void App::leave_modal() {
    if (modal_depth_ > 0 && --modal_depth_ == 0 && host_ != nullptr) ::KillTimer(host_, kModalTimerId);
}

void App::on_tray_message(WPARAM wp, LPARAM lp) {
    // NOTIFYICON_VERSION_4：LOWORD(lp) 是事件，wp 是锚点坐标。左键单击（NIN_SELECT）等过系统双击间隔、没有第二次点击
    // 才弹菜单；左键双击打开设置窗口、不弹菜单，并忽略随后一个双击间隔内的 NIN_SELECT；右键与键盘立即弹菜单
    // （tray-and-hotkeys「托盘图标」，settings-window design D10）。
    const UINT ev = LOWORD(lp);
    if (shut_down_) return;
    const POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
    switch (ev) {
        case NIN_SELECT:
            if (in_menu_ || qpc_now_s() < tray_ignore_select_until_) return;
            tray_click_pt_ = pt;
            tray_click_pending_ = true;
            ::SetTimer(host_, kTrayClickTimerId, ::GetDoubleClickTime(), nullptr);
            return;
        case WM_LBUTTONDBLCLK:
            ::KillTimer(host_, kTrayClickTimerId);
            tray_click_pending_ = false;
            tray_ignore_select_until_ = qpc_now_s() + ::GetDoubleClickTime() / 1000.0;
            if (!in_menu_) open_settings("tray double-click");
            return;
        case WM_CONTEXTMENU:
        case NIN_KEYSELECT:
            ::KillTimer(host_, kTrayClickTimerId);
            tray_click_pending_ = false;
            show_tray_menu(pt);
            return;
        default: return;
    }
}

void App::show_tray_menu(POINT pt) {
    if (in_menu_ || shut_down_) return;
    in_menu_ = true;
    enter_modal();
    const TrayMenuState state = make_menu_state();
    menu_calib_exes_.clear();
    for (const auto& item : state.calibrations) menu_calib_exes_.push_back(item.first);
    const UINT cmd = tray_.show_menu(state, pt);
    leave_modal();
    in_menu_ = false;
    if (cmd != kTrayCmdNone) do_tray_command(cmd);
}

TrayMenuState App::make_menu_state() const {
    TrayMenuState s;
    const CurrentProgram& cp = detector_.current_program();
    s.status_line = format_status_line(cp.valid() && cp.in_game, cp.exe, cp.locked, cfg_.enabled, cfg_.mask.level);
    s.enabled = cfg_.enabled;
    s.level = cfg_.mask.level;
    s.sensitivity = cfg_.trigger.sensitivity;
    s.center_dot = cfg_.dots.center_dot;
    s.edge_dots = cfg_.dots.edge_dots;
    s.capture = cfg_.trigger.capture;
    s.capture_cli_disabled = cmd_.no_capture;
    s.capture_failed = capture_.state() == CaptureState::Failed;
    s.debug_bar = debug_bar_;
    s.autostart = cfg_.autostart;
    s.has_current_program = cp.valid();
    for (const auto& b : bindings_.active) s.hotkey_text[hotkey_index(b.action)] = format_hotkey(b.hotkey);
    for (const auto& [exe, cpd] : cfg_.calibration) s.calibrations.emplace_back(exe, cpd);   // std::map：已按字母序
    return s;
}

std::wstring App::make_debug_text(const GameContext& ctx, double mouse_deg, bool moving, double pad_deg,
                                 double capture_deg, double zoom_deg, double roll_deg) const {
    const CaptureState cs = capture_.state();
    const double confidence = cs == CaptureState::Running ? capture_.debug().confidence : 0.0;
    std::wstring s = i18n::format(
        S::DebugLine,
        {{L"mouse", std::to_wstring(std::lround(mouse_deg))},
         {L"moving", tr(moving ? S::DebugMoving : S::DebugStill)},
         {L"pad", std::to_wstring(std::lround(pad_deg))},
         {L"capture", format_capture_debug_field(capture_user_on(), cs, capture_deg, confidence, zoom_deg, roll_deg)},
         {L"exe", ctx.exe.empty() ? std::wstring(tr(S::None)) : ctx.exe},
         {L"locked", tr(ctx.cursor_locked ? S::DebugLocked : S::DebugUnlocked)}});
    if (ctx.excluded) {
        s += tr(S::DebugExcluded);
    } else if (!ctx.in_game) {
        s += tr(S::DebugNotGame);
    }
    return s;
}

// ---------------------------------------------------------------- 配置

bool App::load_template() {
    const HRSRC res = ::FindResourceW(instance_, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA);
    if (res == nullptr) return false;
    const HGLOBAL g = ::LoadResource(instance_, res);
    const void* p = g != nullptr ? ::LockResource(g) : nullptr;
    const DWORD n = ::SizeofResource(instance_, res);
    if (p == nullptr || n == 0) return false;
    template_.assign(static_cast<const char*>(p), n);
    return true;
}

void App::load_initial_config() {
    if (!cmd_.config_path.empty()) ::CreateDirectoryW(config_dir_.c_str(), nullptr);   // --config 指向的新目录
    const LoadedFile lf = load_or_create(config_path_, template_);
    config_writable_ = lf.writable;
    if (lf.created) log::info(L"config file created from the embedded template: {}", config_path_);
    if (!lf.writable) {
        log::warn(L"config file cannot be created or read: {} — running with defaults, changes will not be saved",
                  config_path_);
    }
    apply_text(lf.text, true);
}

void App::apply_text(const std::string& text, bool initial) {
    std::vector<std::string> warnings;
    cfg_ = load_config(text, &warnings);
    for (const auto& w : warnings) log::warn("config: {}", w);
    apply_language();   // 先定界面语言：随后的热键冲突等 toast 用新语言
    last_hash_ = content_hash(text);
    if (save_pending_) {   // 文件被外部改过：以磁盘内容为准，丢弃还没落盘的设置窗口改动
        save_pending_ = false;
        if (host_ != nullptr) ::KillTimer(host_, kSaveTimerId);
        log::info("config reloaded while a settings-window change was not yet saved; the file on disk wins");
    }
    if (!initial) {
        // 重载可能删除 / 改动了校准对象的条目，而且热键会重新注册：进行中的校准静默取消（design D3）。
        if (calib_.active()) {
            log::info(L"calib cancelled (config reloaded) state={} exe={}",
                      utf8_to_wide(calib_state_name(calib_.state())), calib_.target_exe());
            calib_.cancel();
        }
        apply_config(false);
    }
}

void App::apply_language() {
    // localization：[ui] language（auto = 跟随 Windows 显示语言）→ 当前界面语言。托盘菜单每次弹出时重建、
    // overlay 文字每帧生成、设置窗口在下一次刷新时按新语言重建分页。
    const i18n::Lang lang = i18n::resolve(cfg_.ui.language);
    if (language_applied_ && lang == i18n::current()) return;
    const bool changed = language_applied_;
    language_applied_ = true;
    i18n::set_language(lang);
    log::info(L"ui language: {} (setting {})", i18n::lang_code(lang),
              cfg_.ui.language == i18n::LangSetting::Auto ? L"auto" : i18n::lang_code(lang));
    if (changed) settings_.refresh();
}

void App::apply_config(bool initial) {
    if (!cmd_.verbose) log::set_level(cfg_.log_level);
    model_.configure(cfg_.trigger, cfg_.mask);
    raw_.set_velocity_window(static_cast<double>(cfg_.trigger.velocity_window_ms) / 1000.0);
    detector_.set_rules(cfg_.mouse_lock_rule, cmd_.force_game);
    detector_.reevaluate(cfg_.detect);
    if (!cfg_.trigger.gamepad) pad_.reset();
    register_hotkeys();
    apply_autostart_from_config();
    apply_capture_visibility(capture_on());   // 重载后立即按新值生效（下一个 tick 再按截屏状态复核）
    capture_.update_params(cfg_.capture);     // 未启动时（--no-capture / 启动顺序中）什么都不做
    force_sample_ = true;
    tick_now_ = true;
    log::info(L"config {}: enabled={} level={} sensitivity={} capture={} capture_sensitivity={} known_games={} "
              L"force_game={} exclude={}",
              initial ? L"loaded" : L"applied", cfg_.enabled ? 1 : 0, level_name_zh(cfg_.mask.level),
              sensitivity_name_zh(cfg_.trigger.sensitivity), cfg_.trigger.capture ? 1 : 0,
              sensitivity_name_zh(cfg_.trigger.capture_sensitivity), cfg_.detect.known_games.size(),
              cfg_.detect.force_game.size(), cfg_.detect.exclude.size());
    settings_.refresh();   // 重载：设置窗口开着时重新载入各页
}

void App::save_config(bool from_settings) {
    // 每次保存都写出全部当前值：设置窗口挂起的防抖保存随之完成（configuration「设置窗口改动的保存时机」）。
    if (save_pending_) {
        save_pending_ = false;
        if (host_ != nullptr) ::KillTimer(host_, kSaveTimerId);
    }
    if (!from_settings) settings_.refresh();   // 托盘 / 热键 / 校准 / 已知游戏追加…改了配置：设置窗口同步
    if (!config_writable_ || template_.empty()) {
        log::debug("config save skipped (not writable)");
        return;
    }
    const std::string text = save_config_text(cfg_, template_);
    if (save_file_atomic(config_path_, text)) {
        last_hash_ = content_hash(text);   // 自己的写入不触发重载
        log::debug("config saved{}", from_settings ? " (settings window)" : "");
        settings_.config_saved();
    } else {
        log::warn(L"saving config failed: {} (error={})", config_path_, ::GetLastError());
    }
}

bool App::start_watcher() {
    const HANDLE h = ::CreateFileW(config_dir_.c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::warn(L"cannot watch config directory {} (error={}); external edits need 重新加载配置", config_dir_,
                  ::GetLastError());
        return false;
    }
    dir_handle_.reset(h);
    dir_event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!dir_event_) {
        dir_handle_.reset();
        return false;
    }
    return issue_watch();
}

bool App::issue_watch() {
    dir_pending_ = false;
    if (!dir_handle_ || !dir_event_) return false;
    dir_ov_ = OVERLAPPED{};
    dir_ov_.hEvent = dir_event_.get();
    ::ResetEvent(dir_event_.get());
    if (!::ReadDirectoryChangesW(dir_handle_.get(), dir_buf_.data(), static_cast<DWORD>(sizeof dir_buf_), FALSE,
                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME, nullptr, &dir_ov_,
                                 nullptr)) {
        log::warn("ReadDirectoryChangesW failed error={}; config auto-reload disabled", ::GetLastError());
        return false;
    }
    dir_pending_ = true;
    return true;
}

void App::stop_watcher() {
    if (dir_pending_ && dir_handle_) {
        ::CancelIoEx(dir_handle_.get(), &dir_ov_);
        DWORD bytes = 0;
        ::GetOverlappedResult(dir_handle_.get(), &dir_ov_, &bytes, TRUE);
    }
    dir_pending_ = false;
    dir_handle_.reset();
    dir_event_.reset();
}

void App::on_watcher_signaled() {
    DWORD bytes = 0;
    if (!::GetOverlappedResult(dir_handle_.get(), &dir_ov_, &bytes, FALSE)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_INCOMPLETE) return;
        log::warn("config directory watch error={}; re-arming", err);
        issue_watch();
        return;
    }
    dir_pending_ = false;
    bool relevant = bytes == 0;   // 缓冲溢出：不知道变了什么，按可能变化处理
    const auto* base = reinterpret_cast<const BYTE*>(dir_buf_.data());
    std::size_t offset = 0;
    while (!relevant && bytes >= sizeof(FILE_NOTIFY_INFORMATION) && offset < bytes) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
        const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (iequals(name, config_name_)) relevant = true;
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
    if (relevant) {
        reload_deadline_ = qpc_now_s() + kReloadDebounceS;   // 再次变化就顺延
        log::debug("config file change noticed ({} bytes of notifications); reload check in 500 ms", bytes);
    }
    issue_watch();
}

void App::check_external_reload() {
    std::string text;
    if (!read_file(config_path_, &text)) {
        log::warn(L"config file changed but cannot be read: {}", config_path_);
        return;
    }
    if (content_hash(text) == last_hash_) {
        log::debug("config change ignored: content equals the last read/write");
        return;
    }
    apply_text(text, false);
    log::info("已重新加载配置 (file changed on disk)");
}

void App::reload_from_menu() {
    std::string text;
    if (!read_file(config_path_, &text)) {
        log::warn(L"reload: cannot read {}", config_path_);
        show_toast(tr(S::ToastConfigReadFailed));
        return;
    }
    const std::wstring before = toast_;
    const double before_t = toast_t_;
    apply_text(text, false);
    log::info("已重新加载配置 (menu)");
    // 重载时若有热键冲突 toast，一并显示，不被覆盖。
    if (toast_t_ != before_t && !toast_.empty() && toast_ != before) {
        show_toast(std::wstring(tr(S::ToastReloaded)) + L"\n" + toast_);
    } else {
        show_toast(tr(S::ToastReloaded));
    }
}

// ---------------------------------------------------------------- 热键、自启、截屏可见性

void App::register_hotkeys(bool from_settings) {
    if (host_ == nullptr) return;
    hotkeys_.unregister_all();   // 重载时 MUST 先注销再注册
    bindings_ = resolve_bindings(cfg_.hotkeys);
    if (hotkeys_paused_) {
        // 设置窗口的按键捕获框有焦点：先不注册，捕获结束（pause_hotkeys(false)）时按当前配置注册。
        log::debug("hotkeys not registered now: paused while the settings window captures a key");
        return;
    }
    for (const auto& inv : bindings_.invalid) {
        log::warn(L"hotkey {}={} is invalid ({}); the action stays unbound",
                  utf8_to_wide(hotkey_info(inv.action).ini_key), inv.text, inv.error);
    }
    for (const auto a : bindings_.single_key) {
        Hotkey hk;
        for (const auto& b : bindings_.active) {
            if (b.action == a) hk = b.hotkey;
        }
        log::warn(L"hotkey {} ({}) has no modifier: 单键会吃掉游戏里的该键", format_hotkey(hk),
                  utf8_to_wide(hotkey_info(a).ini_key));
    }
    const auto failed = hotkeys_.register_all(host_, bindings_);
    std::wstring msg;
    if (!failed.empty()) {
        std::wstring keys;
        for (const auto& f : failed) {
            if (!keys.empty()) keys += tr(S::ToastHotkeyListSep);
            keys += format_hotkey(f.hotkey);
            log::warn(L"hotkey {} ({}) could not be registered: already in use by another program",
                      format_hotkey(f.hotkey), utf8_to_wide(hotkey_info(f.action).ini_key));
        }
        msg = i18n::format(S::ToastHotkeysOccupied, {{L"keys", keys}});
    }
    for (const auto& d : bindings_.duplicates) {
        log::warn(L"hotkey {} is bound twice; {} wins, {} ignored", format_hotkey(d.hotkey),
                  utf8_to_wide(hotkey_info(d.winner).ini_key), utf8_to_wide(hotkey_info(d.action).ini_key));
        if (!msg.empty()) msg += L"\n";
        msg += i18n::format(S::ToastHotkeyDuplicate,
                            {{L"hk", format_hotkey(d.hotkey)}, {L"name", std::wstring(hotkey_action_label(d.action))}});
    }
    std::wstring list;
    for (const auto& b : hotkeys_.registered()) {
        if (!list.empty()) list += L", ";
        list += utf8_to_wide(hotkey_info(b.action).ini_key) + L"=" + format_hotkey(b.hotkey);
    }
    log::info(L"hotkeys registered: {} [{}]{}", hotkeys_.registered().size(), list,
              from_settings ? L" (settings window)" : L"");
    // 设置窗口发起的注册失败 / 重复只在快捷键页标红，不弹 toast（tray-and-hotkeys「热键注册与冲突」）。
    if (!msg.empty() && !from_settings) show_toast(msg);
}

bool App::sync_autostart(bool want) {
    HKEY key = nullptr;
    LONG rc = ::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (rc == ERROR_FILE_NOT_FOUND) {
        if (!want) return true;
        rc = ::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr,
                               &key, nullptr);
    }
    if (rc != ERROR_SUCCESS) {
        log::warn("autostart: cannot open HKCU\\...\\Run (error={})", rc);
        return false;
    }
    ScopeExit close_key([&] { ::RegCloseKey(key); });

    DWORD type = 0, bytes = 0;
    const bool exists = ::RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS;
    std::wstring current;
    if (exists && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
        current.resize(bytes / sizeof(wchar_t));
        if (::RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<BYTE*>(current.data()), &bytes) !=
            ERROR_SUCCESS) {
            current.clear();
        }
        while (!current.empty() && current.back() == L'\0') current.pop_back();
    }
    if (want) {
        const std::wstring desired = L"\"" + exe_path_ + L"\"";
        if (exists && type == REG_SZ && current == desired) return true;
        rc = ::RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(desired.c_str()),
                              static_cast<DWORD>((desired.size() + 1) * sizeof(wchar_t)));
        if (rc != ERROR_SUCCESS) {
            log::warn("autostart: RegSetValueEx failed (error={})", rc);
            return false;
        }
        log::info(L"autostart: Run\\MotionCalm = {}", desired);
        return true;
    }
    if (!exists) return true;
    rc = ::RegDeleteValueW(key, kRunValue);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        log::warn("autostart: RegDeleteValue failed (error={})", rc);
        return false;
    }
    log::info("autostart: Run\\MotionCalm removed");
    return true;
}

void App::apply_autostart_from_config() {
    if (!sync_autostart(cfg_.autostart)) {
        log::warn("autostart sync failed (autostart={})", cfg_.autostart ? 1 : 0);
        show_toast(tr(S::ToastAutostartFailed));
    }
}

void App::apply_capture_visibility(bool cap_on) {
    // overlay-rendering「截屏与录屏可见性」：auto 跟随截屏检测的开启状态（--no-capture、capture=0、采集失败时都算未开启），
    // always / never 强制。Overlay 只在结果变化时调用 SetWindowDisplayAffinity，并写一条 info。
    bool excluded = cap_on;
    if (cfg_.hide_from_capture == HideFromCapture::Always) excluded = true;
    if (cfg_.hide_from_capture == HideFromCapture::Never) excluded = false;
    const bool never_and_on = cfg_.hide_from_capture == HideFromCapture::Never && cap_on;
    const bool was_never_and_on = capture_vis_known_ && capture_vis_on_;
    if (never_and_on && !was_never_and_on) {
        log::warn("hide_from_capture=never while capture motion detection is on: the overlay stays visible to its own "
                  "desktop capture, so the mask may affect the motion estimate");
    }
    capture_vis_on_ = never_and_on;
    capture_vis_known_ = true;
    overlay_.set_capture_excluded(excluded);
}

void App::start_capture() {
    if (cmd_.no_capture) return;   // 不创建截屏线程，也不建任何桌面复制对象（init 里已记过 info）
    if (!capture_.start(cfg_.capture)) {
        log::warn("capture motion detection: the capture thread could not be started; it stays off for this run");
        return;
    }
    const CaptureConfig& p = cfg_.capture;
    log::info(L"capture motion detection: thread started, capture={} capture_sensitivity={} grid={}x{} max_shift={} "
              L"min_correlation={:.2f} assumed_hfov_deg={} hold_ms={} forward_gain={:.1f} detect_roll={} baseline_ms={} "
              L"zoom_thresholds={}/{} roll_thresholds={}/{} (frames are taken only while in a known game)",
              cfg_.trigger.capture ? 1 : 0, sensitivity_name_zh(cfg_.trigger.capture_sensitivity), p.grid_w, p.grid_h,
              p.max_shift, p.min_correlation, p.assumed_hfov_deg, p.hold_ms, cfg_.trigger.forward_gain,
              p.detect_roll ? 1 : 0, p.baseline_ms,
              thresholds_for(cfg_.trigger.sensitivity, cfg_.trigger.zoom_thresholds).v_start,
              thresholds_for(cfg_.trigger.sensitivity, cfg_.trigger.zoom_thresholds).v_full,
              thresholds_for(cfg_.trigger.sensitivity, cfg_.trigger.roll_thresholds).v_start,
              thresholds_for(cfg_.trigger.sensitivity, cfg_.trigger.roll_thresholds).v_full);
}

// ---------------------------------------------------------------- 动作

void App::do_hotkey_action(HotkeyAction action, const char* source) {
    log::debug("action {} via {}", hotkey_info(action).ini_key, source);
    switch (action) {
        case HotkeyAction::Toggle: action_toggle(); break;
        case HotkeyAction::CycleLevel: action_set_level(next_level(cfg_.mask.level)); break;
        case HotkeyAction::ToggleCenterDot: action_toggle_center_dot(); break;
        case HotkeyAction::DebugBar: action_toggle_debug_bar(); break;
        case HotkeyAction::CycleSensitivity: action_set_sensitivity(next_sensitivity(cfg_.trigger.sensitivity)); break;
        case HotkeyAction::ToggleEdgeDots: action_toggle_edge_dots(); break;
        case HotkeyAction::MarkGame: action_mark_game(); break;
        case HotkeyAction::ExcludeApp: action_exclude_app(); break;
        case HotkeyAction::Exit: request_quit(source); break;
        case HotkeyAction::Calibrate: action_calibrate(source); break;
        case HotkeyAction::ToggleCapture: action_toggle_capture(); break;
        case HotkeyAction::OpenSettings: open_settings(source); break;   // 不弹 toast：窗口出现本身就是反馈
    }
}

void App::do_tray_command(UINT cmd) {
    switch (cmd) {
        case kTrayCmdToggle: do_hotkey_action(HotkeyAction::Toggle, "menu"); break;
        case kTrayCmdLevelLight: action_set_level(MaskLevel::Light); break;
        case kTrayCmdLevelMedium: action_set_level(MaskLevel::Medium); break;
        case kTrayCmdLevelStrong: action_set_level(MaskLevel::Strong); break;
        case kTrayCmdSensLow: action_set_sensitivity(Sensitivity::Low); break;
        case kTrayCmdSensMedium: action_set_sensitivity(Sensitivity::Medium); break;
        case kTrayCmdSensHigh: action_set_sensitivity(Sensitivity::High); break;
        case kTrayCmdCenterDot: do_hotkey_action(HotkeyAction::ToggleCenterDot, "menu"); break;
        case kTrayCmdEdgeDots: do_hotkey_action(HotkeyAction::ToggleEdgeDots, "menu"); break;
        case kTrayCmdCapture: do_hotkey_action(HotkeyAction::ToggleCapture, "menu"); break;
        case kTrayCmdMarkGame: do_hotkey_action(HotkeyAction::MarkGame, "menu"); break;
        case kTrayCmdExcludeApp: do_hotkey_action(HotkeyAction::ExcludeApp, "menu"); break;
        case kTrayCmdOpenConfig: open_file(config_path_); break;
        case kTrayCmdReload: reload_from_menu(); break;
        case kTrayCmdDebugBar: do_hotkey_action(HotkeyAction::DebugBar, "menu"); break;
        case kTrayCmdAutostart: action_toggle_autostart(); break;
        case kTrayCmdOpenLog: open_file(log_path_); break;
        case kTrayCmdAbout: show_about(); break;
        case kTrayCmdExit: do_hotkey_action(HotkeyAction::Exit, "menu"); break;
        case kTrayCmdCalibrate: show_calibrate_help(); break;   // 只弹说明框，不改变校准状态（design D6）
        case kTrayCmdSettings: do_hotkey_action(HotkeyAction::OpenSettings, "menu"); break;
        default:
            if (cmd >= kTrayCmdCalibDeleteBase && cmd <= kTrayCmdCalibDeleteLast) {
                const std::size_t i = cmd - kTrayCmdCalibDeleteBase;
                if (i < menu_calib_exes_.size()) remove_calibration(menu_calib_exes_[i], false);
            }
            break;
    }
}

void App::action_toggle() {
    cfg_.enabled = !cfg_.enabled;
    model_.reset(0.0);
    save_config();
    show_toast(tr(cfg_.enabled ? S::ToastMaskOn : S::ToastMaskOff));
}

void App::action_set_level(MaskLevel level) {
    cfg_.mask.level = level;
    model_.configure(cfg_.trigger, cfg_.mask);
    save_config();
    show_toast(i18n::format(S::ToastMaskLevel, {{L"level", std::wstring(level_name(level))}}));
}

void App::action_set_sensitivity(Sensitivity s) {
    cfg_.trigger.sensitivity = s;
    model_.configure(cfg_.trigger, cfg_.mask);
    save_config();
    show_toast(i18n::format(S::ToastSensitivity, {{L"level", std::wstring(sensitivity_name(s))}}));
}

void App::action_toggle_center_dot() {
    cfg_.dots.center_dot = !cfg_.dots.center_dot;
    save_config();
    show_toast(i18n::format(S::ToastCenterDot, {{L"state", on_off(cfg_.dots.center_dot)}}));
}

void App::action_toggle_edge_dots() {
    cfg_.dots.edge_dots = !cfg_.dots.edge_dots;
    save_config();
    show_toast(i18n::format(S::ToastEdgeDots, {{L"state", on_off(cfg_.dots.edge_dots)}}));
}

void App::action_toggle_debug_bar() {
    debug_bar_ = !debug_bar_;   // 运行期状态，不写入 INI
    show_toast(i18n::format(S::ToastDebug, {{L"state", on_off(debug_bar_)}}));
}

void App::action_mark_game() {
    const CurrentProgram& cp = detector_.current_program();
    if (!cp.valid()) {
        show_toast(tr(S::ToastNoProgram));
        return;
    }
    const std::wstring exe = cp.exe;
    mark_as_game(cfg_.detect, exe);
    detector_.reevaluate(cfg_.detect);
    force_sample_ = true;
    save_config();
    log::info(L"marked as game: {}", exe);
    show_toast(i18n::format(S::ToastMarkedGame, {{L"exe", exe}}));
}

void App::action_exclude_app() {
    const CurrentProgram& cp = detector_.current_program();
    if (!cp.valid()) {
        show_toast(tr(S::ToastNoProgram));
        return;
    }
    const std::wstring exe = cp.exe;
    exclude_app(cfg_.detect, exe);
    detector_.reevaluate(cfg_.detect);
    force_sample_ = true;
    save_config();
    log::info(L"excluded: {}", exe);
    show_toast(i18n::format(S::ToastExcluded, {{L"exe", exe}}));
}

void App::action_toggle_autostart() {
    const bool old = cfg_.autostart;
    cfg_.autostart = !old;
    if (!sync_autostart(cfg_.autostart)) {
        cfg_.autostart = old;   // 失败：恢复原值
        log::warn("autostart toggle failed; reverted to {}", old ? 1 : 0);
        show_toast(tr(S::ToastAutostartFailed));
        settings_.refresh();
        return;
    }
    save_config();
}

void App::action_toggle_capture() {
    if (cmd_.no_capture) {
        log::info("toggle_capture ignored: started with --no-capture (INI unchanged)");
        show_toast(tr(S::ToastCaptureCliDisabled));
        return;
    }
    cfg_.trigger.capture = !cfg_.trigger.capture;
    save_config();
    // 立即告诉截屏线程：关 → 100 ms 内停止取帧并释放会话；开 → 清零失败计数（可从"采集失败"重新开始）。
    // 即使同一轮消息里连按两次、中间没有 tick，截屏线程也能看到这一次"关 → 开"。wanted 先给 false：
    // 下一个 tick 先设好截屏排除再让它取帧。
    capture_.set_target(capture_mon_, cfg_.trigger.capture, false);
    capture_.update_params(cfg_.capture);
    log::info("capture motion detection toggled {}", cfg_.trigger.capture ? "on" : "off");
    show_toast(i18n::format(S::ToastCapture, {{L"state", on_off(cfg_.trigger.capture)}}));
}

// ---------------------------------------------------------------- 校准（变更 2）

CalibContext App::make_calib_context(const GameContext& ctx) const {
    CalibContext c;
    c.in_game = ctx.in_game;
    // 单屏全屏游戏常常只把光标回中、不 ClipCursor：strict 规则下只有鼠标在动时才算锁定。
    // 所以本次前台期间出现过锁定也算（--force-game 模式下 evaluate_game 已置为 true）。
    const CurrentProgram& cur = detector_.current_program();
    c.cursor_locked = ctx.cursor_locked || (cur.valid() && cur.exe == ctx.exe && cur.locked);
    c.exe = ctx.exe;
    return c;
}

std::wstring App::calibrate_hotkey_text() const {
    for (const auto& b : bindings_.active) {
        if (b.action == HotkeyAction::Calibrate) return format_hotkey(b.hotkey);
    }
    return {};
}

void App::action_calibrate(const char* source) {
    const double now = qpc_now_s();
    const CalibState from = calib_.state();
    const long long n = calib_.current_abs_count();
    const GameContext& ctx = detector_.ctx();
    if (from == CalibState::Idle) {
        log::debug(L"calib start request via {}: exe={} in_game={} locked={}", utf8_to_wide(source),
                   ctx.exe.empty() ? std::wstring(L"-") : ctx.exe, ctx.in_game ? 1 : 0, ctx.cursor_locked ? 1 : 0);
    }
    apply_calib_effect(calib_.on_hotkey(now, make_calib_context(ctx)), from, n);
    calib_log_count_ = -1;   // 新状态的计数行立即写一次
    log_calib_progress(now);
    tick_now_ = true;        // 提示立即出现 / 更新
}

void App::apply_calib_effect(const CalibEffect& effect, CalibState from, long long count_before) {
    const CalibState to = calib_.state();
    if (to != from || effect.save) {
        // Run2 结算成功时经过瞬时的 Done：日志里写成 Run2->Done->Idle。
        log::debug("calib state={}->{}{} count={} c1={}", calib_state_name(from), effect.save ? "Done->" : "",
                   calib_state_name(to), count_before, calib_.first_count());
        settings_.refresh();   // 校准页"重测"按钮只在 Idle 时可用
    }
    if (effect.save) {
        const CalibSave& s = *effect.save;
        // 值 + 结算时的本地日期，同名条目连同旧日期一起覆盖（turn-calibration「结果计算、保存与生效」）
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        const std::wstring exe = to_lower_ascii(std::wstring_view(s.exe));
        const std::wstring date = calib_format_date(st.wYear, st.wMonth, st.wDay);
        cfg_.calibration[exe] = s.counts_per_degree;
        cfg_.calibration_dates[exe] = date;
        save_config();   // 下一个 tick 起生效；设置窗口开着时校准页随之刷新
        log::info(L"calib saved exe={} c1={} c2={} cpd={} date={}", s.exe, s.c1, s.c2,
                  utf8_to_wide(calib_format_cpd(s.counts_per_degree)), date);
    }
    if (!effect.toast.empty()) show_toast(effect.toast);
}

void App::log_calib_progress(double now) {
    if (!log::enabled(log::Level::Debug)) return;
    const CalibState st = calib_.state();
    if (st != CalibState::Run1 && st != CalibState::Run2) return;
    const long long n = calib_.current_abs_count();
    if (calib_log_count_ >= 0) {
        if (n == calib_log_count_ && now - calib_log_t_ < 1.0) return;   // 不变时每秒一行
        if (now - calib_log_t_ < 0.25) return;                            // 变化时最多每 250 ms 一行
    }
    log::debug("calib run={} count={} t_in_state={:.3f}", st == CalibState::Run1 ? 1 : 2, n,
               now - calib_.entered_at());
    calib_log_count_ = n;
    calib_log_t_ = now;
}

void App::show_calibrate_help() {
    const std::wstring hk = calibrate_hotkey_text();
    const std::wstring text = hk.empty() ? std::wstring(tr(S::CalibHelpNoHotkey)) : i18n::format(S::CalibHelp, {{L"hk", hk}});
    log::debug("calibrate help shown (hotkey {})", hk.empty() ? std::string("unbound") : wide_to_utf8(hk));
    enter_modal();
    ::MessageBoxW(nullptr, text.c_str(), tr(S::CalibHelpTitle),
                  MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    leave_modal();
}

void App::remove_calibration(const std::wstring& exe_in, bool from_settings) {
    const std::wstring exe = to_lower_ascii(std::wstring_view(exe_in));
    const auto it = cfg_.calibration.find(exe);
    if (it == cfg_.calibration.end()) return;
    cfg_.calibration.erase(it);
    cfg_.calibration_dates.erase(exe);
    if (calib_.active() && calib_.target_exe() == exe) {
        log::info(L"calib cancelled (entry deleted) exe={}", exe);
        calib_.cancel();
    }
    log::info(L"calibration deleted: {} (via {})", exe, from_settings ? L"settings window" : L"tray");
    // 下一个 tick 起该 exe 回到 default_counts_per_degree
    if (from_settings) {
        schedule_settings_save();   // 按设置窗口改动的保存时机；确认框与列表变化就是反馈，不弹 toast
        settings_.refresh();
        tick_now_ = true;
    } else {
        save_config();
        show_toast(i18n::format(S::ToastCalibDeleted, {{L"exe", exe}}));
    }
}

void App::open_file(const std::wstring& path) {
    const auto r = reinterpret_cast<INT_PTR>(::ShellExecuteW(host_, L"open", path.c_str(), nullptr, nullptr,
                                                             SW_SHOWNORMAL));
    if (r > 32) return;
    // 没有关联程序时改用记事本。
    const std::wstring args = L"\"" + path + L"\"";
    const auto r2 = reinterpret_cast<INT_PTR>(::ShellExecuteW(host_, nullptr, L"notepad.exe", args.c_str(), nullptr,
                                                              SW_SHOWNORMAL));
    if (r2 <= 32) log::warn(L"cannot open {} (ShellExecute {} / notepad {})", path, r, r2);
}

void App::show_about() {
    const std::wstring text = i18n::format(S::AboutBody, {{L"ver", std::format(L"{}.{}", MC_VERSION_MAJOR, MC_VERSION_MINOR)},
                                                          {L"full", version_string()},
                                                          {L"config", config_path_},
                                                          {L"log", log_path_}});
    enter_modal();
    ::MessageBoxW(host_, text.c_str(), tr(S::AboutTitle), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    leave_modal();
}

void App::show_toast(std::wstring text) {
    log::info(L"toast: {}", one_line(text));
    toast_ = std::move(text);
    toast_t_ = qpc_now_s();   // 新 toast 立即替换旧的并重新计时
    tick_now_ = true;
}

}  // namespace mc
