// game_detect.cpp — detect/game_detect 的实现：纯规则函数 + GameDetector 的 Win32 采样（前台 exe、窗口 / 显示器矩形、光标状态）。
#include "detect/game_detect.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "util/strings.h"

namespace mc {

// ---------------------------------------------------------------- 纯规则

DetectLists default_detect_lists() {
    DetectLists l;
    l.exclude.reserve(kDefaultExcludeList.size());
    for (const auto& name : kDefaultExcludeList) l.exclude.emplace_back(name);
    return l;
}

bool list_contains(const std::vector<std::wstring>& list, std::wstring_view name) {
    return std::any_of(list.begin(), list.end(), [&](const std::wstring& s) { return iequals(s, name); });
}

bool list_add_unique(std::vector<std::wstring>& list, std::wstring_view name) {
    if (name.empty() || list_contains(list, name)) return false;
    list.emplace_back(name);
    return true;
}

bool list_remove(std::vector<std::wstring>& list, std::wstring_view name) {
    const auto old = list.size();
    std::erase_if(list, [&](const std::wstring& s) { return iequals(s, name); });
    return list.size() != old;
}

bool is_fullscreen(const RECT& w, const RECT& m) {
    return std::labs(w.left - m.left) <= kFullscreenTolerancePx && std::labs(w.top - m.top) <= kFullscreenTolerancePx &&
           std::labs(w.right - m.right) <= kFullscreenTolerancePx &&
           std::labs(w.bottom - m.bottom) <= kFullscreenTolerancePx;
}

bool is_clipped(const RECT& clip, const RECT& vs) {
    return clip.left != vs.left || clip.top != vs.top || clip.right != vs.right || clip.bottom != vs.bottom;
}

bool is_cursor_locked(bool hidden, bool clipped, double raw_path_100ms, double cursor_disp_100ms,
                      MouseLockRule rule) {
    if (rule == MouseLockRule::Hidden) return hidden;
    return hidden &&
           (clipped || (raw_path_100ms > kLockMinRawPath && cursor_disp_100ms < kLockMaxCursorDisp));
}

GameContext evaluate_game(const GameSample& s, const DetectLists& lists, MouseLockRule rule, bool force_game_mode) {
    GameContext c;
    c.fg = s.fg;
    c.exe = s.exe;
    c.monitor = s.monitor;
    if (s.exe.empty() || s.own_process) return c;   // 没有前台，或前台是自己：不是游戏，全部门控关闭

    c.fullscreen = is_fullscreen(s.window, s.monitor_rect);
    c.cursor_hidden = !s.cursor_showing;
    c.cursor_clipped = is_clipped(s.clip, s.virtual_screen);
    const bool forced = list_contains(lists.force_game, s.exe);
    c.excluded = !forced && list_contains(lists.exclude, s.exe);
    if (force_game_mode) {
        c.known_game = true;
        c.cursor_locked = true;
    } else {
        c.known_game = forced || list_contains(lists.known_games, s.exe);
        c.cursor_locked =
            is_cursor_locked(c.cursor_hidden, c.cursor_clipped, s.raw_path_100ms, s.cursor_disp_100ms, rule);
    }
    c.in_game = !c.excluded && (c.known_game || (c.fullscreen && c.cursor_hidden));
    return c;
}

Gates compute_gates(const GameContext& c, bool enabled, bool mouse, bool keyboard, bool gamepad, bool capture) {
    Gates g;
    const bool base = enabled && !c.excluded;
    g.mouse = base && mouse && c.cursor_locked;
    g.keys = base && keyboard && c.known_game;
    g.pad = base && gamepad && (c.fullscreen || c.cursor_hidden);
    g.capture = base && capture && c.known_game;
    return g;
}

bool should_record_known_game(const GameContext& c, const DetectLists& lists, bool force_game_mode) {
    return !force_game_mode && !c.exe.empty() && c.cursor_locked && !c.excluded &&
           !list_contains(lists.known_games, c.exe) && !list_contains(lists.force_game, c.exe);
}

bool mark_as_game(DetectLists& lists, std::wstring_view exe) {
    if (exe.empty()) return false;
    bool changed = list_add_unique(lists.force_game, exe);
    changed = list_remove(lists.exclude, exe) || changed;
    return changed;
}

bool exclude_app(DetectLists& lists, std::wstring_view exe) {
    if (exe.empty()) return false;
    bool changed = list_add_unique(lists.exclude, exe);
    changed = list_remove(lists.force_game, exe) || changed;
    changed = list_remove(lists.known_games, exe) || changed;
    return changed;
}

bool is_shell_tray_class(std::wstring_view cls) {
    return iequals(cls, L"Shell_TrayWnd") || iequals(cls, L"Shell_SecondaryTrayWnd") ||
           iequals(cls, L"NotifyIconOverflowWindow") || iequals(cls, L"TopLevelWindowForOverflowXamlIsland");
}

std::wstring exe_name_from_path(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring_view name = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    return to_lower_ascii(name);
}

// ---------------------------------------------------------------- GameDetector（Win32 采样）

void GameDetector::set_rules(MouseLockRule rule, bool force_game_mode) {
    rule_ = rule;
    force_game_mode_ = force_game_mode;
}

double GameDetector::now_s() {
    if (qpc_freq_.QuadPart == 0) ::QueryPerformanceFrequency(&qpc_freq_);
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(qpc_freq_.QuadPart);
}

std::wstring GameDetector::exe_for(HWND fg, DWORD pid, std::wstring_view class_name) {
    if (fg == cache_hwnd_ && pid == cache_pid_ && !cache_exe_.empty()) return cache_exe_;
    std::wstring exe;
    if (HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = static_cast<DWORD>(std::size(buf));
        if (::QueryFullProcessImageNameW(h, 0, buf, &len) && len > 0) exe = exe_name_from_path({buf, len});
        ::CloseHandle(h);
    }
    if (exe.empty()) exe = to_lower_ascii(class_name);   // 拿不到映像名：用窗口类名
    cache_hwnd_ = fg;
    cache_pid_ = pid;
    cache_exe_ = exe;
    return exe;
}

bool GameDetector::poll(const MouseLockHint& hint, const DetectLists& lists, bool force) {
    const double now = now_s();
    if (!force && sampled_ && now - last_sample_t_ < kMinIntervalS) return false;
    sampled_ = true;
    last_sample_t_ = now;

    GameSample s;
    s.fg = ::GetForegroundWindow();
    if (s.fg != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(s.fg, &pid);
        s.own_process = pid == ::GetCurrentProcessId();
        if (s.fg != class_hwnd_ || pid != class_pid_) {   // 窗口类名不会变：每个前台窗口只查一次
            wchar_t cls[256] = {};
            const int n = ::GetClassNameW(s.fg, cls, static_cast<int>(std::size(cls)));
            class_name_.assign(cls, n > 0 ? static_cast<std::size_t>(n) : 0);
            class_hwnd_ = s.fg;
            class_pid_ = pid;
        }
        s.class_name = class_name_;
        s.exe = exe_for(s.fg, pid, s.class_name);
        if (!::GetWindowRect(s.fg, &s.window)) s.window = RECT{};
        s.monitor = ::MonitorFromWindow(s.fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (s.monitor != nullptr && ::GetMonitorInfoW(s.monitor, &mi)) s.monitor_rect = mi.rcMonitor;
    }

    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    s.virtual_screen = RECT{vx, vy, vx + ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
                            vy + ::GetSystemMetrics(SM_CYVIRTUALSCREEN)};

    CURSORINFO ci{};
    ci.cbSize = sizeof ci;
    s.cursor_showing = ::GetCursorInfo(&ci) ? (ci.flags & CURSOR_SHOWING) != 0 : true;
    if (!::GetClipCursor(&s.clip)) s.clip = s.virtual_screen;

    // 光标位置历史（约 200 ms）：cursor_disp_100ms = |当前 − 最接近 100 ms 前的那次采样|。
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        std::erase_if(cursor_history_, [&](const CursorSample& c) { return c.t < now - kCursorHistoryS; });
        cursor_history_.push_back(CursorSample{now, pt});
    }
    if (!cursor_history_.empty()) {
        const CursorSample& cur = cursor_history_.back();
        const CursorSample* best = &cur;
        double best_err = 1e300;
        for (const auto& c : cursor_history_) {
            const double err = std::fabs(c.t - (cur.t - kCursorLagS));
            if (err < best_err) {
                best_err = err;
                best = &c;
            }
        }
        const double dx = static_cast<double>(cur.pt.x - best->pt.x);
        const double dy = static_cast<double>(cur.pt.y - best->pt.y);
        s.cursor_disp_100ms = std::sqrt(dx * dx + dy * dy);
        // 历史还不够长（启动后第一次采样、或采样中断 > 200 ms 后）：当前点只能和自己比，位移恒为 0，
        // 会把"光标隐藏 + 鼠标在动"的普通程序误判为每帧回中的锁定。这时不认 strict 的回中分支。
        if (cur.t - best->t < kCursorLagS * 0.5) s.cursor_disp_100ms = 1e9;
    }
    s.raw_path_100ms = hint.raw_path_100ms;

    sample_ = std::move(s);
    ctx_ = evaluate_game(sample_, lists, rule_, force_game_mode_);
    update_current_program();
    return true;
}

void GameDetector::reevaluate(const DetectLists& lists) {
    ctx_ = evaluate_game(sample_, lists, rule_, force_game_mode_);
    if (sample_.fg != nullptr && sample_.fg == current_hwnd_) current_.in_game = ctx_.in_game;
}

void GameDetector::update_current_program() {
    const GameSample& s = sample_;
    if (s.fg == nullptr || s.own_process || s.exe.empty() || is_shell_tray_class(s.class_name)) return;
    if (s.fg != current_hwnd_ || s.exe != current_.exe) {   // 新的一段前台期间
        current_hwnd_ = s.fg;
        current_.exe = s.exe;
        current_.locked = ctx_.cursor_locked;
    } else {
        current_.locked = current_.locked || ctx_.cursor_locked;
    }
    current_.in_game = ctx_.in_game;
}

}  // namespace mc
