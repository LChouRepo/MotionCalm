// game_detect.h — "是否在游戏里"：纯规则（evaluate_game / compute_gates / 名单操作，可单测）与 Win32 采样器 GameDetector 分离。
#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

// [trigger] mouse_lock_rule：strict = 隐藏且（被限制或每帧回中）；hidden = 光标隐藏即可。
enum class MouseLockRule { Strict = 0, Hidden = 1 };

// 默认排除名单（game-detection spec，恰好 22 项，顺序即 INI 中的顺序）。
inline constexpr std::array<std::wstring_view, 22> kDefaultExcludeList{
    L"chrome.exe",      L"msedge.exe",      L"firefox.exe",   L"brave.exe",       L"opera.exe",
    L"opera_gx.exe",    L"vivaldi.exe",     L"arc.exe",       L"vlc.exe",         L"PotPlayerMini64.exe",
    L"PotPlayer64.exe", L"PotPlayerMini.exe", L"mpc-hc64.exe", L"mpc-hc.exe",     L"mpv.exe",
    L"wmplayer.exe",    L"Video.UI.exe",    L"Netflix.exe",   L"Spotify.exe",     L"Discord.exe",
    L"explorer.exe",    L"obs64.exe",
};

// 判定阈值（spec 常量）。
inline constexpr double kLockMinRawPath = 8.0;       // strict：100 ms 原始位移路径 > 8 计数
inline constexpr double kLockMaxCursorDisp = 3.0;    // strict：而光标位移 < 3 px
inline constexpr long kFullscreenTolerancePx = 1;    // 四边与显示器相差都 ≤ 1 px

// [detect] 节的三个名单（项按 ASCII 大小写不敏感比较）。
struct DetectLists {
    std::vector<std::wstring> exclude;
    std::vector<std::wstring> force_game;
    std::vector<std::wstring> known_games;
    bool operator==(const DetectLists&) const = default;
};

// exclude = 默认 22 项，另两项为空。
DetectLists default_detect_lists();

// 一次 Win32 采样的原始数据（纯数据，规则函数只看它）。exe 为空表示没有前台窗口。
struct GameSample {
    HWND fg = nullptr;
    HMONITOR monitor = nullptr;
    std::wstring exe;               // 前台进程 exe 文件名（ASCII 小写、不含路径）；取不到时为小写窗口类名
    std::wstring class_name;        // 前台窗口类名（原样）
    bool own_process = false;       // 前台窗口属于 MotionCalm 自身
    RECT window{};                  // GetWindowRect
    RECT monitor_rect{};            // 前台所在（最近）显示器的 rcMonitor
    RECT virtual_screen{};          // SM_X/Y/CX/CYVIRTUALSCREEN
    bool cursor_showing = true;     // GetCursorInfo 的 CURSOR_SHOWING
    RECT clip{};                    // GetClipCursor
    double raw_path_100ms = 0;      // 最近 100 ms 原始相对位移 Σ√(dx²+dy²)（计数）
    double cursor_disp_100ms = 0;   // 当前光标位置与约 100 ms 前采样位置的距离（像素）
};

// 规则计算结果（DESIGN §3.3）。
struct GameContext {
    HWND fg = nullptr;
    std::wstring exe;                  // 前台 exe（小写）
    HMONITOR monitor = nullptr;
    bool fullscreen = false;           // 窗口矩形覆盖整个显示器（±1 px）
    bool cursor_hidden = false, cursor_clipped = false, cursor_locked = false;
    bool excluded = false;             // 在排除名单且不在 force_game
    bool known_game = false;           // 在 force_game ∪ known_games（或 --force-game 模式）
    bool in_game = false;              // !excluded && (known_game || (fullscreen && cursor_hidden))
};

// 各输入源门控。
struct Gates {
    bool mouse = false, keys = false, pad = false, capture = false;
    bool operator==(const Gates&) const = default;
};

// ---------------------------------------------------------------- 纯规则

// name 是否在名单中（ASCII 大小写不敏感）。
bool list_contains(const std::vector<std::wstring>& list, std::wstring_view name);
// 不在名单中时追加 name，返回是否追加了。
bool list_add_unique(std::vector<std::wstring>& list, std::wstring_view name);
// 删除名单中所有与 name 相同（大小写不敏感）的项，返回是否删除了。
bool list_remove(std::vector<std::wstring>& list, std::wstring_view name);

// 四条边与显示器矩形相差都 ≤ 1 px。
bool is_fullscreen(const RECT& window, const RECT& monitor);
// 光标限制矩形与虚拟屏幕矩形任一边不同。
bool is_clipped(const RECT& clip, const RECT& virtual_screen);
// strict：hidden && (clipped || (raw_path_100ms > 8 && cursor_disp_100ms < 3))；hidden 模式：hidden。
bool is_cursor_locked(bool hidden, bool clipped, double raw_path_100ms, double cursor_disp_100ms,
                      MouseLockRule rule);
// 采样 → GameContext。exe 为空或 own_process 时全部为 false。force_game_mode（--force-game）：
// 视为 known_game 且 cursor_locked，排除名单照常生效。
GameContext evaluate_game(const GameSample& sample, const DetectLists& lists, MouseLockRule rule,
                          bool force_game_mode);
// 门控：mouse = enabled && mouse && !excluded && locked；keys = … && known_game；
// pad = … && (fullscreen || cursor_hidden)；capture = enabled && capture && !excluded && known_game
// （capture = [trigger] capture；--no-capture / 采集失败由调用方再与 capture_on 相与，motion-trigger「输入源门控」）。
Gates compute_gates(const GameContext& ctx, bool enabled, bool mouse, bool keyboard, bool gamepad,
                    bool capture = false);
// 是否应把 ctx.exe 追加到 known_games：非 force 模式、已锁定、未排除、尚不在 force_game / known_games。
bool should_record_known_game(const GameContext& ctx, const DetectLists& lists, bool force_game_mode);
// "把当前程序视为游戏"：加入 force_game（去重），并从 exclude 移除。返回名单是否变化。
bool mark_as_game(DetectLists& lists, std::wstring_view exe);
// "把当前程序加入排除名单"：加入 exclude（去重），并从 force_game 与 known_games 移除。返回名单是否变化。
bool exclude_app(DetectLists& lists, std::wstring_view exe);
// 任务栏 / 通知区窗口类（Shell_TrayWnd 等），这些前台不算"当前程序"。
bool is_shell_tray_class(std::wstring_view class_name);
// 从完整路径取文件名并转 ASCII 小写："C:\Games\MyGame.EXE" → "mygame.exe"。
std::wstring exe_name_from_path(std::wstring_view path);

// ---------------------------------------------------------------- Win32 采样器

// 调用方提供的锁定判定辅助量（来自 Raw Input 的 100 ms 路径窗口）。
struct MouseLockHint {
    double raw_path_100ms = 0;
};

// "当前程序"：最近一个既不属于本进程、也不是任务栏 / 通知区窗口的前台程序（托盘状态行、标记动作用）。
struct CurrentProgram {
    std::wstring exe;          // 小写 exe；为空表示还没有
    bool in_game = false;      // 它最近一次在前台时的 in_game
    bool locked = false;       // 它最近一次前台期间是否曾达到 cursor_locked
    bool valid() const { return !exe.empty(); }
};

class GameDetector {
public:
    static constexpr double kMinIntervalS = 0.016;    // 两次采样至少间隔 16 ms（≤ 60 Hz）
    static constexpr double kCursorHistoryS = 0.200;  // 光标位置历史长度
    static constexpr double kCursorLagS = 0.100;      // cursor_disp_100ms 的回看时间

    // 设定锁定规则与 --force-game 模式。
    void set_rules(MouseLockRule rule, bool force_game_mode);
    // 采样一次前台 / 光标状态并按规则更新 ctx()。距上次不足 16 ms 且 !force 时跳过，返回 false。
    bool poll(const MouseLockHint& hint, const DetectLists& lists, bool force = false);
    // 不重新采样，只按新名单重算 ctx()（例如标记 / 排除之后）。
    void reevaluate(const DetectLists& lists);
    // 最近一次的判定结果。
    const GameContext& ctx() const { return ctx_; }
    // 最近一次的原始采样。
    const GameSample& sample() const { return sample_; }
    // 当前程序。
    const CurrentProgram& current_program() const { return current_; }

private:
    struct CursorSample {
        double t;
        POINT pt;
    };
    double now_s();
    std::wstring exe_for(HWND fg, DWORD pid, std::wstring_view class_name);
    void update_current_program();

    MouseLockRule rule_ = MouseLockRule::Strict;
    bool force_game_mode_ = false;
    LARGE_INTEGER qpc_freq_{};
    bool sampled_ = false;
    double last_sample_t_ = 0;
    GameSample sample_;
    GameContext ctx_;
    CurrentProgram current_;
    HWND current_hwnd_ = nullptr;
    std::vector<CursorSample> cursor_history_;
    // exe 缓存：同一前台窗口、同一 pid 时不重复查询映像名。
    HWND cache_hwnd_ = nullptr;
    DWORD cache_pid_ = 0;
    std::wstring cache_exe_;
    HWND class_hwnd_ = nullptr;        // class_name_ 对应的前台窗口与进程
    DWORD class_pid_ = 0;
    std::wstring class_name_;
};

}  // namespace mc
