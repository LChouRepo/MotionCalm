// test_settings_hotkeys.cpp — ui/hotkey_capture：捕获框按键状态机（半成品、提交、清除、带修饰的 Delete、Esc 取消、只按修饰键后松开、不支持的键、Tab 导航）、显示写法，以及逐行冲突 / 警告评估（spec「快捷键冲突与警告」+ docs/settings-ui.md §5）。
#include "test_framework.h"

#include <windows.h>

#include <string>
#include <vector>

#include "tray/hotkeys.h"
#include "ui/hotkey_capture.h"

using namespace mc;
using namespace mc::ui;

namespace {

std::vector<std::wstring> defaults() {
    std::vector<std::wstring> v;
    for (const auto& a : kHotkeyActions) v.emplace_back(a.default_binding);
    return v;
}

std::size_t row(std::string_view key) {
    const auto a = hotkey_action_from_key(key);
    return a ? hotkey_index(*a) : kHotkeyActionCount;
}

std::vector<bool> none_occupied() { return std::vector<bool>(kHotkeyActionCount, false); }

}  // namespace

TEST(settings_capture_combo_commits_canonical) {
    // spec：焦点在"循环遮罩强度"的捕获框，按住 Ctrl+Shift 再按 F10 → Ctrl+Shift+F10
    HotkeyCapture c;
    CaptureStep s = c.key_down(VK_CONTROL, kModCtrl);
    CHECK_EQ(s.kind, CaptureKind::Partial);
    CHECK_EQ(s.text, L"Ctrl + …");
    s = c.key_down(VK_SHIFT, kModCtrl | kModShift);
    CHECK_EQ(s.kind, CaptureKind::Partial);
    CHECK_EQ(s.text, L"Ctrl + Shift + …");
    CHECK_TRUE(c.pending());
    s = c.key_down(VK_F10, kModCtrl | kModShift);
    CHECK_EQ(s.kind, CaptureKind::Commit);
    CHECK_EQ(s.text, L"Ctrl+Shift+F10");   // 写进 INI 的规范字符串
    CHECK_TRUE(s.hotkey == (Hotkey{kModCtrl | kModShift, VK_F10}));
    CHECK_FALSE(c.pending());
    // 提交之后松开修饰键不再恢复
    CHECK_EQ(c.key_up(VK_SHIFT, kModCtrl).kind, CaptureKind::Ignore);
    CHECK_EQ(c.key_up(VK_CONTROL, 0).kind, CaptureKind::Ignore);
}

TEST(settings_capture_shift_ctrl_end_is_canonical) {
    // configuration spec：为 exit 捕获到 Shift+Ctrl+End → exit=Ctrl+Shift+End
    HotkeyCapture c;
    c.key_down(VK_SHIFT, kModShift);
    c.key_down(VK_CONTROL, kModShift | kModCtrl);
    const CaptureStep s = c.key_down(VK_END, kModShift | kModCtrl);
    CHECK_EQ(s.kind, CaptureKind::Commit);
    CHECK_EQ(s.text, L"Ctrl+Shift+End");
}

TEST(settings_capture_delete_and_backspace_clear) {
    HotkeyCapture c;
    CaptureStep s = c.key_down(VK_DELETE, 0);
    CHECK_EQ(s.kind, CaptureKind::Clear);
    CHECK_EQ(s.text, L"None");
    s = c.key_down(VK_BACK, 0);
    CHECK_EQ(s.kind, CaptureKind::Clear);
    // 带修饰键的 Delete 照常捕获
    s = c.key_down(VK_DELETE, kModCtrl | kModShift);
    CHECK_EQ(s.kind, CaptureKind::Commit);
    CHECK_EQ(s.text, L"Ctrl+Shift+Delete");
    s = c.key_down(VK_BACK, kModAlt);
    CHECK_EQ(s.kind, CaptureKind::Unsupported);   // Backspace 不在热键语法的按键表里
}

TEST(settings_capture_escape_cancels_without_modifiers) {
    HotkeyCapture c;
    CHECK_EQ(c.key_down(VK_ESCAPE, 0).kind, CaptureKind::Cancel);
    const CaptureStep s = c.key_down(VK_ESCAPE, kModCtrl | kModAlt);
    CHECK_EQ(s.kind, CaptureKind::Commit);
    CHECK_EQ(s.text, L"Ctrl+Alt+Esc");
}

TEST(settings_capture_modifiers_only_then_release_restores) {
    // spec：按下 Ctrl+Alt 后全部松开 → 恢复，配置不变
    HotkeyCapture c;
    c.key_down(VK_CONTROL, kModCtrl);
    c.key_down(VK_MENU, kModCtrl | kModAlt);
    CaptureStep s = c.key_up(VK_MENU, kModCtrl);
    CHECK_EQ(s.kind, CaptureKind::Partial);
    CHECK_EQ(s.text, L"Ctrl + …");
    s = c.key_up(VK_CONTROL, 0);
    CHECK_EQ(s.kind, CaptureKind::Restore);
    CHECK_FALSE(c.pending());
    // 没有按过修饰键时，松开普通键什么也不做
    CHECK_EQ(c.key_up('A', 0).kind, CaptureKind::Ignore);
}

TEST(settings_capture_unsupported_keys) {
    HotkeyCapture c;
    CHECK_EQ(c.key_down(VK_RETURN, 0).kind, CaptureKind::Unsupported);
    CHECK_EQ(c.key_down(VK_SNAPSHOT, kModCtrl).kind, CaptureKind::Unsupported);
    CHECK_EQ(c.key_down(VK_CAPITAL, 0).kind, CaptureKind::Unsupported);
    CHECK_EQ(c.key_down(VK_PROCESSKEY, 0).kind, CaptureKind::Ignore);   // 输入法
    // 单键（无修饰）可以提交，警告由行评估给出
    const CaptureStep s = c.key_down(VK_F11, 0);
    CHECK_EQ(s.kind, CaptureKind::Commit);
    CHECK_EQ(s.text, L"F11");
}

TEST(settings_capture_tab_navigation_rule) {
    CHECK_TRUE(capture_passes_to_dialog(VK_TAB, 0));
    CHECK_TRUE(capture_passes_to_dialog(VK_TAB, kModShift));
    CHECK_FALSE(capture_passes_to_dialog(VK_TAB, kModCtrl | kModAlt));
    CHECK_FALSE(capture_passes_to_dialog(VK_TAB, kModWin));
    CHECK_FALSE(capture_passes_to_dialog(VK_F9, 0));
    CHECK_TRUE(is_modifier_vk(VK_LWIN));
    CHECK_TRUE(is_modifier_vk(VK_RMENU));
    CHECK_FALSE(is_modifier_vk(VK_F1));
}

TEST(settings_hotkey_display_text) {
    CHECK_EQ(hotkey_display_text(L"Ctrl+Alt+F9"), L"Ctrl + Alt + F9");
    CHECK_EQ(hotkey_display_text(L"alt + ctrl + f9"), L"Ctrl + Alt + F9");   // 用户写法 → 规范显示
    CHECK_EQ(hotkey_display_text(L"Win+Shift+a"), L"Shift + Win + A");
    CHECK_EQ(hotkey_display_text(L"None"), L"无");
    CHECK_EQ(hotkey_display_text(L""), L"无");
    CHECK_EQ(hotkey_display_text(L"F11"), L"F11");
    CHECK_EQ(hotkey_display_text(L"Ctrl+Alt+F99"), L"Ctrl+Alt+F99");   // 非法写法原样显示
    CHECK_EQ(capture_partial_text(0), L"…");
    CHECK_EQ(capture_partial_text(kModCtrl | kModAlt | kModShift | kModWin), L"Ctrl + Alt + Shift + Win + …");
}

TEST(settings_rows_defaults_are_clean) {
    const auto rows = analyze_hotkey_rows(defaults(), none_occupied());
    CHECK_EQ(rows.size(), kHotkeyActionCount);
    CHECK_EQ(rows[row("toggle")].display, L"Ctrl + Alt + F9");
    CHECK_EQ(rows[row("toggle")].state, RowState::Ok);
    CHECK_EQ(rows[row("exit")].display, L"无");
    CHECK_EQ(rows[row("exit")].state, RowState::Unbound);
    for (const auto& r : rows) {
        CHECK_EQ(r.tint, RowTint::None);   // 正常时什么也不显示
        CHECK_TRUE(r.text.empty());
    }
}

TEST(settings_rows_internal_duplicate_first_wins) {
    // spec：toggle=Ctrl+Alt+F9 时把 debug_bar 设为 Ctrl+Alt+F9
    auto raw = defaults();
    raw[row("debug_bar")] = L"Ctrl+Alt+F9";
    const auto rows = analyze_hotkey_rows(raw, none_occupied());
    const auto& t = rows[row("toggle")];
    const auto& d = rows[row("debug_bar")];
    CHECK_EQ(t.state, RowState::Duplicate);
    CHECK_EQ(t.tint, RowTint::Error);
    CHECK_EQ(t.text, L"与「显示调试信息」重复");
    CHECK_EQ(d.state, RowState::DuplicateInactive);
    CHECK_EQ(d.tint, RowTint::Error);
    CHECK_EQ(d.text, L"与「启用 / 关闭遮罩」重复（未生效）");
    CHECK_EQ(d.other, row("toggle"));
}

TEST(settings_rows_three_way_duplicate) {
    auto raw = defaults();
    raw[row("debug_bar")] = L"Ctrl+Alt+F9";
    raw[row("exit")] = L"ctrl+alt+f9";
    const auto rows = analyze_hotkey_rows(raw, none_occupied());
    CHECK_EQ(rows[row("toggle")].state, RowState::Duplicate);
    CHECK_EQ(rows[row("debug_bar")].state, RowState::DuplicateInactive);
    CHECK_EQ(rows[row("exit")].state, RowState::DuplicateInactive);
    CHECK_EQ(rows[row("exit")].text, L"与「启用 / 关闭遮罩」重复（未生效）");
}

TEST(settings_rows_none_never_duplicates) {
    auto raw = defaults();   // 默认就有 6 个 None
    const auto rows = analyze_hotkey_rows(raw, none_occupied());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].state == RowState::Unbound) CHECK_TRUE(rows[i].text.empty());
    }
}

TEST(settings_rows_no_modifier_warns) {
    // spec：把 toggle_center_dot 设为 F11 → 警告（文案按界面规范）
    auto raw = defaults();
    raw[row("toggle_center_dot")] = L"F11";
    const auto rows = analyze_hotkey_rows(raw, none_occupied());
    const auto& r = rows[row("toggle_center_dot")];
    CHECK_EQ(r.state, RowState::NoModifier);
    CHECK_EQ(r.tint, RowTint::Warning);
    CHECK_EQ(r.text, L"单键会在游戏里失灵");
}

TEST(settings_rows_occupied_is_error) {
    auto raw = defaults();
    raw[row("cycle_level")] = L"Ctrl+Alt+K";
    auto occ = none_occupied();
    occ[row("cycle_level")] = true;
    const auto rows = analyze_hotkey_rows(raw, occ);
    const auto& r = rows[row("cycle_level")];
    CHECK_EQ(r.state, RowState::Occupied);
    CHECK_EQ(r.tint, RowTint::Error);
    CHECK_EQ(r.text, L"已被其他程序占用，换一个组合键");
    CHECK_EQ(r.display, L"Ctrl + Alt + K");
    // 未生效的重复行不看外部占用
    raw[row("exit")] = L"Ctrl+Alt+K";
    occ[row("exit")] = true;
    const auto rows2 = analyze_hotkey_rows(raw, occ);
    CHECK_EQ(rows2[row("exit")].state, RowState::DuplicateInactive);
    CHECK_EQ(rows2[row("cycle_level")].state, RowState::Occupied);
}

TEST(settings_rows_invalid_string) {
    auto raw = defaults();
    raw[row("toggle")] = L"Ctrl+Alt+F99";
    const auto rows = analyze_hotkey_rows(raw, none_occupied());
    const auto& r = rows[row("toggle")];
    CHECK_EQ(r.state, RowState::Invalid);
    CHECK_EQ(r.tint, RowTint::Error);
    CHECK_EQ(r.display, L"Ctrl+Alt+F99");
    CHECK_FALSE(r.hotkey.has_value());
}

TEST(settings_rows_short_input_treated_as_none) {
    const std::vector<std::wstring> raw{L"Ctrl+Alt+F9"};   // 只给第一行，其余按 None
    const auto rows = analyze_hotkey_rows(raw, {});
    CHECK_EQ(rows.size(), kHotkeyActionCount);
    CHECK_EQ(rows[0].state, RowState::Ok);
    CHECK_EQ(rows[1].state, RowState::Unbound);
}
