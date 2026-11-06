// hotkey_capture.h — 快捷键页的纯逻辑（可单测）：按键捕获框的按键处理（修饰键半成品、提交、清除、取消、恢复、不支持的键、Tab 导航）、组合键的显示写法，以及逐行的冲突 / 警告评估（内部重复按动作表先到先得、外部占用、无修饰键、写法非法）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tray/hotkeys.h"

namespace mc::ui {

// ---------------------------------------------------------------- 显示写法（docs/settings-ui.md §5）

// 捕获框里显示的文字：合法组合 → "Ctrl + Alt + F9"；None / 空 → "无"；写法非法 → 原样。
// （写进 INI 的仍是规范字符串 "Ctrl+Alt+F9" / "None"。）
std::wstring hotkey_display_text(std::wstring_view raw);
// 只按住修饰键时的半成品："Ctrl + Alt + …"；mods == 0 时为 "…"。
std::wstring capture_partial_text(std::uint32_t mods);

// ---------------------------------------------------------------- 按键捕获

// VK_SHIFT / VK_CONTROL / VK_MENU（含左右变体）、VK_LWIN / VK_RWIN。
bool is_modifier_vk(std::uint32_t vk);
// 这个按键是否应交给对话框做焦点导航（不被捕获）：不带 Ctrl / Alt / Win 的 Tab（含 Shift+Tab）。
bool capture_passes_to_dialog(std::uint32_t vk, std::uint32_t mods);

enum class CaptureKind {
    Ignore,        // 什么也不做
    Partial,       // 显示 text（修饰键半成品）
    Commit,        // 提交 hotkey（规范字符串见 text）
    Clear,         // 提交 None（不带修饰键的 Backspace / Delete）
    Cancel,        // 不带修饰键的 Esc：放弃本次捕获，恢复原值
    Restore,       // 只按了修饰键又全部松开：回到等待输入（值不变）
    Unsupported,   // 语法不支持的键（Enter、PrintScreen…）：不改值，状态提示"不支持该按键"
};

struct CaptureStep {
    CaptureKind kind = CaptureKind::Ignore;
    Hotkey hotkey;        // Commit 时有效
    std::wstring text;    // Partial：半成品；Commit：规范字符串；Clear："None"
};

// 一个捕获框的按键状态机（每次获得焦点时 reset）。mods 是按键时刻按住的修饰键（kMod* 位，由
// GetKeyState 读出，包含正在按下的这个键）。
class HotkeyCapture {
public:
    // WM_KEYDOWN / WM_SYSKEYDOWN（自动重复由调用方过滤）。
    CaptureStep key_down(std::uint32_t vk, std::uint32_t mods);
    // WM_KEYUP / WM_SYSKEYUP；mods_after = 松开这个键之后仍按住的修饰键。
    CaptureStep key_up(std::uint32_t vk, std::uint32_t mods_after);
    void reset() { pending_ = false; }
    // 正在显示修饰键半成品。
    bool pending() const { return pending_; }

private:
    bool pending_ = false;
};

// ---------------------------------------------------------------- 逐行评估（spec「快捷键冲突与警告」+ docs/settings-ui.md §5）

// 状态的表现：Error = 红字 + 系统错误小图标；Warning = 琥珀色字 + 系统警告小图标；None = 什么也不显示。
enum class RowTint { None, Error, Warning };
enum class RowState {
    Unbound,            // None
    Ok,
    NoModifier,         // 警告：不带修饰键
    Duplicate,          // 错误：与表中靠后的某行相同（本行生效）
    DuplicateInactive,  // 错误：与表中靠前的某行相同（本行未生效）
    Occupied,           // 错误：被其他程序占用
    Invalid,            // 错误：写法无法识别（只可能来自手改 INI）
};

struct HotkeyRowStatus {
    RowState state = RowState::Unbound;
    RowTint tint = RowTint::None;
    std::wstring display;               // 捕获框文字（hotkey_display_text）
    std::wstring text;                  // 状态文字（Ok / Unbound 为空）
    std::optional<Hotkey> hotkey;       // 解析出的组合（Unbound / Invalid 时为空）
    std::size_t other = static_cast<std::size_t>(-1);   // 重复时对方的行号
};

// raw[i] 是 kHotkeyActions[i] 的 INI 原串（缺的按 None）；occupied[i] = 该行的组合试注册失败。
// 内部重复按表序先到先得：靠前一行"与「对方」重复"，靠后各行"与「靠前一行」重复（未生效）"；
// 未生效的行不再看外部占用。错误优先于警告。
std::vector<HotkeyRowStatus> analyze_hotkey_rows(std::span<const std::wstring> raw, const std::vector<bool>& occupied);

}  // namespace mc::ui
