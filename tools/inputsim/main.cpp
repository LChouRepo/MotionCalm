// inputsim/main.cpp — 集成测试用鼠标模拟器：按 --profile 分段，用 SendInput 以给定 counts/s 发送相对移动
// （MOUSEEVENTF_MOVE，Raw Input 能收到），高精度可等待计时器 + timeBeginPeriod(1) 定时，结束时打印每段实际发送量。
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCircleHz = 1.0;   // circle：每秒转一整圈

enum class Kind { Idle, Turn, TurnV, Circle };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Idle: return "idle";
        case Kind::Turn: return "turn";
        case Kind::TurnV: return "turnv";
        case Kind::Circle: return "circle";
    }
    return "?";
}

struct Segment {
    Kind kind = Kind::Idle;
    double seconds = 0;
    double rate = 0;   // counts/s；turn/turnv 可为负（反方向），circle 为负则逆时针
    // 结果
    long long sent_dx = 0, sent_dy = 0;   // 累计（有符号）
    long long abs_dx = 0, abs_dy = 0;     // 累计 |dx|、|dy|
    long long events = 0;                 // 成功的 SendInput 次数
    double t0 = 0, t1 = 0;                // 相对开始的秒数：t0 计划开始，t1 实际结束
};

struct Options {
    std::wstring profile;
    double hz = 500;
    double delay = 0;
    bool dry_run = false;
};

void print_help() {
    std::printf(
        "inputsim - relative mouse movement generator (SendInput) for MotionCalm integration tests\n"
        "\n"
        "usage: inputsim.exe --profile \"<seg>,<seg>,...\" [--hz 500] [--delay s] [--dry-run]\n"
        "\n"
        "segments (run back to back, seconds may be fractional):\n"
        "  idle:<s>              send nothing for <s> seconds\n"
        "  turn:<s>@<cps>        horizontal move, <cps> counts per second (negative = left)\n"
        "  turnv:<s>@<cps>       vertical move (positive = down, negative = up)\n"
        "  circle:<s>@<cps>      dx/dy rotating once per second, path speed <cps> (negative = CCW)\n"
        "\n"
        "options:\n"
        "  --profile P   required, e.g. \"idle:2,turn:3@8000,idle:3,turnv:1@3000\"\n"
        "  --hz N        send rate in events per second (default 500, range 1..8000)\n"
        "  --delay s     wait before starting (default 0)\n"
        "  --dry-run     do not send anything and do not wait; print what would be sent\n"
        "  --help        show this help and exit\n"
        "\n"
        "Counts are spread evenly over the ticks (time-based, fractional remainders carried),\n"
        "so each segment sends exactly trunc(cps * seconds) counts. Ctrl+C stops early.\n"
        "Summary lines (one per segment, machine readable):\n"
        "  seg=<n> kind=<k> dur=<s> rate=<cps> expect=<counts> sent_dx=.. sent_dy=.. abs_dx=.. abs_dy=..\n"
        "  events=.. t0=<local yyyy-mm-ddThh:mm:ss.mmm> t1=<local ...>\n");
}

bool parse_double(const std::wstring& s, double& out) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const double v = std::wcstod(s.c_str(), &end);
    if (errno != 0 || !end || *end != L'\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::wstring trim(const std::wstring& s) {
    const auto b = s.find_first_not_of(L" \t\"");
    if (b == std::wstring::npos) return L"";
    const auto e = s.find_last_not_of(L" \t\"");
    return s.substr(b, e - b + 1);
}

bool parse_profile(const std::wstring& text, std::vector<Segment>& out) {
    std::wstring rest = text;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t comma = rest.find(L',', start);
        const std::wstring item = trim(rest.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
        start = comma == std::wstring::npos ? rest.size() + 1 : comma + 1;
        if (item.empty()) continue;

        const std::size_t colon = item.find(L':');
        if (colon == std::wstring::npos) {
            std::printf("inputsim: segment '%ls' needs <kind>:<seconds>\n", item.c_str());
            return false;
        }
        const std::wstring kind = item.substr(0, colon);
        std::wstring dur = item.substr(colon + 1);
        std::wstring rate;
        const std::size_t at = dur.find(L'@');
        if (at != std::wstring::npos) {
            rate = dur.substr(at + 1);
            dur = dur.substr(0, at);
        }

        Segment seg;
        if (kind == L"idle") seg.kind = Kind::Idle;
        else if (kind == L"turn") seg.kind = Kind::Turn;
        else if (kind == L"turnv") seg.kind = Kind::TurnV;
        else if (kind == L"circle") seg.kind = Kind::Circle;
        else {
            std::printf("inputsim: unknown segment kind '%ls' (idle|turn|turnv|circle)\n", kind.c_str());
            return false;
        }
        if (!parse_double(trim(dur), seg.seconds) || seg.seconds <= 0 || seg.seconds > 3600) {
            std::printf("inputsim: bad duration in '%ls' (0 < s <= 3600)\n", item.c_str());
            return false;
        }
        if (seg.kind == Kind::Idle) {
            if (!rate.empty()) {
                std::printf("inputsim: idle takes no @rate ('%ls')\n", item.c_str());
                return false;
            }
        } else if (!parse_double(trim(rate), seg.rate) || std::fabs(seg.rate) > 1e6) {
            std::printf("inputsim: '%ls' needs @<counts_per_second> (|cps| <= 1e6)\n", item.c_str());
            return false;
        }
        out.push_back(seg);
    }
    if (out.empty()) {
        std::printf("inputsim: empty profile\n");
        return false;
    }
    return true;
}

// 0 = 继续，1 = 已打印帮助，2 = 参数错误
int parse_args(int argc, wchar_t** argv, Options& o) {
    if (argc <= 1) {
        print_help();
        return 2;   // 不带参数不动鼠标
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](std::wstring& v) -> bool {
            if (i + 1 >= argc) {
                std::printf("inputsim: %ls needs a value\n", a.c_str());
                return false;
            }
            v = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            print_help();
            return 1;
        } else if (a == L"--profile") {
            if (!value(v)) return 2;
            o.profile = v;
        } else if (a == L"--hz") {
            if (!value(v) || !parse_double(v, o.hz) || o.hz < 1 || o.hz > 8000) {
                std::printf("inputsim: bad --hz (1..8000)\n");
                return 2;
            }
        } else if (a == L"--delay") {
            if (!value(v) || !parse_double(v, o.delay) || o.delay < 0 || o.delay > 3600) {
                std::printf("inputsim: bad --delay (0..3600)\n");
                return 2;
            }
        } else if (a == L"--dry-run") {
            o.dry_run = true;
        } else {
            std::printf("inputsim: unknown argument '%ls' (see --help)\n", a.c_str());
            return 2;
        }
    }
    if (o.profile.empty()) {
        std::printf("inputsim: --profile is required (see --help)\n");
        return 2;
    }
    return 0;
}

// ------------------------------------------------------------------ clock

std::atomic<bool> g_stop{false};
HANDLE g_stop_event = nullptr;

BOOL WINAPI console_ctrl_handler(DWORD) {
    g_stop = true;
    if (g_stop_event) SetEvent(g_stop_event);
    return TRUE;
}

class Clock {
public:
    explicit Clock(bool dry) : dry_(dry) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        freq_ = static_cast<double>(f.QuadPart);
        if (!dry_) {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            high_res_ = timer_ != nullptr;
            if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }
    ~Clock() {
        if (timer_) CloseHandle(timer_);
    }
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    bool high_res() const { return high_res_; }

    void start() {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        q0_ = c.QuadPart;
        GetSystemTimePreciseAsFileTime(&ft0_);
        sim_ = 0;
    }

    double now() const {
        if (dry_) return sim_;
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart - q0_) / freq_;
    }

    // 等到相对时间 t（秒）。被 Ctrl+C 打断时返回 false。
    bool wait_until(double t) {
        if (g_stop) return false;
        if (dry_) {
            if (t > sim_) sim_ = t;
            return true;
        }
        for (;;) {
            const double remaining = t - now();
            if (remaining <= 0) return !g_stop;
            if (timer_) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(remaining * 1e7);   // 相对时间，100 ns 单位
                if (due.QuadPart == 0) due.QuadPart = -1;
                SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
                const HANDLE hs[2] = {timer_, g_stop_event};
                const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0 + 1 || g_stop) return false;
                return true;   // 计时器到点（可能早/晚 <1 ms，发送量按实际时间算，不影响总数）
            }
            const DWORD ms = static_cast<DWORD>(std::ceil(remaining * 1000.0));
            if (WaitForSingleObject(g_stop_event, ms) == WAIT_OBJECT_0) return false;
        }
    }

    // 相对时间 → 本地时间字符串 yyyy-mm-ddThh:mm:ss.mmm
    std::string local_time(double rel) const {
        ULARGE_INTEGER u{};
        u.LowPart = ft0_.dwLowDateTime;
        u.HighPart = ft0_.dwHighDateTime;
        u.QuadPart += static_cast<ULONGLONG>(std::llround((rel > 0 ? rel : 0.0) * 1e7));
        FILETIME ft{};
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        SYSTEMTIME utc{}, loc{};
        FileTimeToSystemTime(&ft, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", loc.wYear, loc.wMonth, loc.wDay,
                      loc.wHour, loc.wMinute, loc.wSecond, loc.wMilliseconds);
        return buf;
    }

private:
    bool dry_;
    bool high_res_ = false;
    double freq_ = 1;
    LONGLONG q0_ = 0;
    FILETIME ft0_{};
    double sim_ = 0;
    HANDLE timer_ = nullptr;
};

// ------------------------------------------------------------------ sending

long long g_send_failures = 0;
DWORD g_last_send_error = 0;

bool send_move(long long dx, long long dy, bool dry) {
    if (dry) return true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(dx);
    in.mi.dy = static_cast<LONG>(dy);
    in.mi.dwFlags = MOUSEEVENTF_MOVE;   // 相对移动
    if (SendInput(1, &in, sizeof(INPUT)) != 1) {
        ++g_send_failures;
        g_last_send_error = GetLastError();
        return false;
    }
    return true;
}

// 段内时间 ts 时应累计发送的（有符号）计数
void target_at(const Segment& s, double ts, double& tx, double& ty) {
    tx = ty = 0;
    switch (s.kind) {
        case Kind::Idle: break;
        case Kind::Turn: tx = s.rate * ts; break;
        case Kind::TurnV: ty = s.rate * ts; break;
        case Kind::Circle: {
            // 速度 (v cos ωt, v sin ωt) 的积分；|速度| = |v|
            const double w = 2.0 * kPi * kCircleHz;
            tx = s.rate / w * std::sin(w * ts);
            ty = s.rate / w * (1.0 - std::cos(w * ts));
            break;
        }
    }
}

long long trunc_ll(double v) {
    if (v > static_cast<double>(LLONG_MAX / 2)) return LLONG_MAX / 2;
    if (v < static_cast<double>(LLONG_MIN / 2)) return LLONG_MIN / 2;
    return static_cast<long long>(v);   // 向零截断
}

// 运行一段；返回 false 表示被 Ctrl+C 打断。
bool run_segment(Segment& s, double seg_start, double hz, Clock& clock, bool dry) {
    const double seg_end = seg_start + s.seconds;
    s.t0 = seg_start;
    if (s.kind == Kind::Idle) {
        const bool ok = clock.wait_until(seg_end);
        s.t1 = clock.now();
        return ok;
    }
    double k_last = 0;
    for (;;) {
        // 下一个节拍：对齐到 seg_start + k/hz；k 单调递增，落后时直接跳到当前节拍（不补发空节拍）
        const double since = clock.now() - seg_start;
        double k = std::floor((since > 0 ? since : 0.0) * hz) + 1.0;
        if (k < k_last + 1.0) k = k_last + 1.0;
        k_last = k;
        double t_tick = seg_start + k / hz;
        if (t_tick > seg_end) t_tick = seg_end;
        if (!clock.wait_until(t_tick)) {
            s.t1 = clock.now();
            return false;
        }
        const double now = clock.now();
        const double ts = (now < seg_end ? now : seg_end) - seg_start;
        double tx = 0, ty = 0;
        target_at(s, ts, tx, ty);
        const long long dx = trunc_ll(tx) - s.sent_dx;
        const long long dy = trunc_ll(ty) - s.sent_dy;
        if (dx != 0 || dy != 0) {
            if (send_move(dx, dy, dry)) {
                s.sent_dx += dx;
                s.sent_dy += dy;
                s.abs_dx += dx < 0 ? -dx : dx;
                s.abs_dy += dy < 0 ? -dy : dy;
                ++s.events;
            }
        }
        if (now >= seg_end) {
            s.t1 = now;
            return true;
        }
    }
}

long long expected_counts(const Segment& s) {
    return s.kind == Kind::Idle ? 0 : trunc_ll(std::fabs(s.rate * s.seconds));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options opt;
    const int pr = parse_args(argc, argv, opt);
    if (pr == 1) return 0;
    if (pr != 0) return 2;

    std::vector<Segment> segs;
    if (!parse_profile(opt.profile, segs)) return 2;

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    double total = 0;
    for (const auto& s : segs) total += s.seconds;
    std::printf("[inputsim] %zu segments, %.3f s, %.0f Hz, delay %.3f s%s\n", segs.size(), total, opt.hz, opt.delay,
                opt.dry_run ? ", DRY RUN (nothing is sent)" : "");
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        if (s.kind == Kind::Idle) {
            std::printf("  #%zu %-6s %7.3f s\n", i + 1, kind_name(s.kind), s.seconds);
        } else {
            std::printf("  #%zu %-6s %7.3f s @ %.0f counts/s  (%lld counts)\n", i + 1, kind_name(s.kind), s.seconds,
                        s.rate, expected_counts(s));
        }
    }

    if (!opt.dry_run) timeBeginPeriod(1);
    Clock clock(opt.dry_run);
    if (!opt.dry_run) std::printf("[inputsim] timer: %s\n", clock.high_res() ? "high-resolution waitable" : "waitable + timeBeginPeriod(1)");

    bool interrupted = false;
    if (opt.delay > 0 && !opt.dry_run) {
        std::printf("[inputsim] starting in %.3f s\n", opt.delay);
        if (WaitForSingleObject(g_stop_event, static_cast<DWORD>(opt.delay * 1000.0)) == WAIT_OBJECT_0) interrupted = true;
    }

    std::size_t done = 0;
    if (!interrupted) {
        clock.start();
        std::printf("[inputsim] start %s\n", clock.local_time(0).c_str());
        double seg_start = 0;
        for (auto& s : segs) {
            if (!run_segment(s, seg_start, opt.hz, clock, opt.dry_run)) {
                interrupted = true;
                ++done;
                break;
            }
            ++done;
            seg_start += s.seconds;   // 按计划时间排下一段，不累积漂移
        }
    }
    const double elapsed = interrupted && done == 0 ? 0.0 : clock.now();
    if (!opt.dry_run) timeEndPeriod(1);

    std::printf("[inputsim] summary%s\n", interrupted ? " (INTERRUPTED)" : "");
    long long tdx = 0, tdy = 0, tev = 0;
    for (std::size_t i = 0; i < done; ++i) {
        const Segment& s = segs[i];
        std::printf("seg=%zu kind=%s dur=%.3f rate=%.0f expect=%lld sent_dx=%lld sent_dy=%lld abs_dx=%lld abs_dy=%lld "
                    "events=%lld t0=%s t1=%s\n",
                    i + 1, kind_name(s.kind), s.seconds, s.rate, expected_counts(s), s.sent_dx, s.sent_dy, s.abs_dx,
                    s.abs_dy, s.events, clock.local_time(s.t0).c_str(), clock.local_time(s.t1).c_str());
        tdx += s.sent_dx;
        tdy += s.sent_dy;
        tev += s.events;
    }
    std::printf("total sent_dx=%lld sent_dy=%lld events=%lld elapsed=%.3f send_failures=%lld\n", tdx, tdy, tev, elapsed,
                g_send_failures);
    if (g_send_failures > 0) {
        std::printf("[inputsim] WARNING: %lld SendInput calls failed (last error %lu). If the foreground window is "
                    "elevated, run inputsim elevated too (UIPI).\n",
                    g_send_failures, g_last_send_error);
    }
    if (g_stop_event) CloseHandle(g_stop_event);
    return (interrupted || g_send_failures > 0) ? 1 : 0;
}
