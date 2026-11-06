// tray.h — 通知区图标（Shell_NotifyIcon，NOTIFYICON_VERSION_4，TaskbarCreated 后重新添加）与动态构建的中文托盘菜单（变更 4："设置…"为默认项）。
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "model/intensity.h"
#include "tray/hotkeys.h"

namespace mc {

// 托盘菜单命令 ID（TrackPopupMenuEx 的返回值）。
enum TrayCommand : UINT {
    kTrayCmdNone = 0,
    kTrayCmdToggle = 100,
    kTrayCmdLevelLight,
    kTrayCmdLevelMedium,
    kTrayCmdLevelStrong,
    kTrayCmdSensLow,
    kTrayCmdSensMedium,
    kTrayCmdSensHigh,
    kTrayCmdCenterDot,
    kTrayCmdEdgeDots,
    kTrayCmdMarkGame,
    kTrayCmdExcludeApp,
    kTrayCmdOpenConfig,
    kTrayCmdReload,
    kTrayCmdDebugBar,
    kTrayCmdAutostart,
    kTrayCmdOpenLog,
    kTrayCmdAbout,
    kTrayCmdExit,
    kTrayCmdCalibrate,                 // 变更 2："校准当前游戏…"（只弹说明框）
    kTrayCmdCalibEmpty,                // "校准列表"为空时的灰色"（无）"，不可点
    kTrayCmdCapture,                   // 变更 3："截屏运动检测"勾选项
    kTrayCmdSettings,                  // 变更 4："设置…"（默认项，加粗）
    // "校准列表 ▸ 删除 <exe>"：kTrayCmdCalibDeleteBase + TrayMenuState::calibrations 的下标
    kTrayCmdCalibDeleteBase = 1000,
    kTrayCmdCalibDeleteLast = 1999,
};

// 构建菜单所需的当前状态（菜单弹出那一刻由 app 填写）。
struct TrayMenuState {
    std::wstring status_line;   // "游戏：cs2.exe · 已锁定 · 遮罩：中"
    bool enabled = true;
    MaskLevel level = MaskLevel::Medium;
    Sensitivity sensitivity = Sensitivity::Medium;
    bool center_dot = true;
    bool edge_dots = true;
    bool capture = true;                // [trigger] capture（INI 值）
    bool capture_cli_disabled = false;  // 以 --no-capture 启动：灰显并改文字
    bool capture_failed = false;        // 截屏服务处于"采集失败"：改文字
    bool debug_bar = false;
    bool autostart = false;
    bool has_current_program = false;   // false 时"视为游戏 / 加入排除名单"置灰
    // 每个动作当前生效绑定的规范字符串；未绑定为空（菜单不显示热键列）。
    std::array<std::wstring, kHotkeyActionCount> hotkey_text{};
    // [calibration] 的全部条目（exe 小写, counts_per_degree），按 exe 字母序；"校准列表"子菜单按此构建。
    std::vector<std::pair<std::wstring, double>> calibrations;
};

// "校准列表"子菜单一项的文字："删除 {exe}（每度 {值，1 位小数} 计数）"（按当前界面语言）。
std::wstring format_calibration_item(std::wstring_view exe, double counts_per_degree);

// "截屏运动检测"菜单项的文字（不含热键列）：--no-capture → "截屏运动检测（已由 --no-capture 禁用）"；
// 采集失败 → "截屏运动检测（采集失败）"；否则 "截屏运动检测"。纯函数。
std::wstring format_capture_item(bool cli_disabled, bool failed);

// 状态行：游戏：{exe} · {已锁定|未锁定} · 遮罩：{轻|中|强|已关闭}；没有在游戏里的当前程序时为 游戏：无 · 遮罩：{…}。纯函数。
std::wstring format_status_line(bool has_game, std::wstring_view exe, bool locked, bool enabled, MaskLevel level);
// 档位 / 灵敏度的中文名（日志用，不随界面语言变化）。
std::wstring_view level_name_zh(MaskLevel level);
std::wstring_view sensitivity_name_zh(Sensitivity s);
// 档位 / 灵敏度在当前界面语言下的名字（菜单、toast）。
std::wstring_view level_name(MaskLevel level);
std::wstring_view sensitivity_name(Sensitivity s);

class Tray {
public:
    Tray() = default;
    ~Tray() { remove(); }
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // 添加图标（NIM_ADD + NIM_SETVERSION(4)），提示文字 "MotionCalm"。失败返回 false（之后 TaskbarCreated 时会再试）。
    bool add(HWND owner, UINT callback_message, HICON icon);
    // 资源管理器重启（TaskbarCreated）后重新添加。
    bool readd();
    // 移除图标（NIM_DELETE）。
    void remove();
    bool added() const { return added_; }

    // 在 pt 处弹出菜单（SetForegroundWindow → TrackPopupMenuEx(TPM_RETURNCMD) → PostMessage(WM_NULL)），返回所选命令。
    UINT show_menu(const TrayMenuState& state, POINT pt);

    // RegisterWindowMessageW(L"TaskbarCreated")。
    static UINT taskbar_created_message();

private:
    NOTIFYICONDATAW make_data() const;

    HWND owner_ = nullptr;
    UINT callback_ = 0;
    HICON icon_ = nullptr;
    bool added_ = false;
};

}  // namespace mc
