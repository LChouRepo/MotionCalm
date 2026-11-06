// test_hotkey_parse.cpp — tray/hotkeys 的单元测试：热键字符串语法、规范化、按键名、往返一致、按表序去重、动作表默认值。
#include "test_framework.h"

#include <windows.h>

#include <array>
#include <iterator>
#include <string>
#include <vector>

#include "config/config.h"
#include "tray/hotkeys.h"
#include "tray/tray.h"

using namespace mc;

namespace {

// tray-and-hotkeys spec 列出的全部按键名（规范写法）。
std::vector<std::wstring> spec_key_names() {
    std::vector<std::wstring> v;
    for (int i = 1; i <= 24; ++i) v.push_back(L"F" + std::to_wstring(i));
    for (wchar_t c = L'A'; c <= L'Z'; ++c) v.push_back(std::wstring(1, c));
    for (wchar_t c = L'0'; c <= L'9'; ++c) v.push_back(std::wstring(1, c));
    for (const wchar_t* n : {L"Space", L"Tab", L"Esc", L"Pause", L"Insert", L"Delete", L"Home", L"End", L"PgUp",
                             L"PgDn", L"Up", L"Down", L"Left", L"Right"}) {
        v.push_back(n);
    }
    for (int i = 0; i <= 9; ++i) v.push_back(L"Num" + std::to_wstring(i));
    for (const wchar_t* n : {L"VK_OEM_1", L"VK_OEM_PLUS", L"VK_OEM_COMMA", L"VK_OEM_MINUS", L"VK_OEM_PERIOD",
                             L"VK_OEM_2", L"VK_OEM_3", L"VK_OEM_4", L"VK_OEM_5", L"VK_OEM_6", L"VK_OEM_7", L"VK_OEM_8",
                             L"VK_OEM_102"}) {
        v.push_back(n);
    }
    return v;
}

Hotkey hk(std::wstring_view s) { return parse_hotkey(s).hotkey; }

}  // namespace

TEST(hotkey_parse_canonical_and_loose) {
    const Hotkey expected{kModCtrl | kModAlt, VK_F9};
    for (const wchar_t* s : {L"Ctrl+Alt+F9", L"ctrl + alt + f9", L"Alt+Ctrl+F9", L"  CTRL+ALT+F9  "}) {
        const HotkeyParseResult r = parse_hotkey(s);
        CHECK_TRUE(r.ok);
        CHECK_FALSE(r.none);
        CHECK_FALSE(r.single_key);
        CHECK_TRUE(r.hotkey == expected);
        CHECK_EQ(format_hotkey(r.hotkey), L"Ctrl+Alt+F9");
        CHECK_TRUE(r.error.empty());
    }
    // 修饰键取值与 RegisterHotKey 的 MOD_* 相同。
    CHECK_EQ(kModAlt, static_cast<std::uint32_t>(MOD_ALT));
    CHECK_EQ(kModCtrl, static_cast<std::uint32_t>(MOD_CONTROL));
    CHECK_EQ(kModShift, static_cast<std::uint32_t>(MOD_SHIFT));
    CHECK_EQ(kModWin, static_cast<std::uint32_t>(MOD_WIN));
}

TEST(hotkey_modifier_order_format) {
    const HotkeyParseResult r = parse_hotkey(L"Win+Shift+a");
    CHECK_TRUE(r.ok);
    CHECK_EQ(r.hotkey.mods, kModShift | kModWin);
    CHECK_EQ(r.hotkey.vk, static_cast<std::uint32_t>('A'));
    CHECK_EQ(format_hotkey(r.hotkey), L"Shift+Win+A");
    CHECK_EQ(format_hotkey(Hotkey{kModWin | kModShift | kModAlt | kModCtrl, VK_NEXT}), L"Ctrl+Alt+Shift+Win+PgDn");
    CHECK_EQ(format_hotkey(Hotkey{}), L"None");
}

TEST(hotkey_key_names) {
    CHECK_EQ(hk(L"Ctrl+PgUp").vk, static_cast<std::uint32_t>(VK_PRIOR));
    CHECK_EQ(hk(L"Alt+Num5").vk, static_cast<std::uint32_t>(VK_NUMPAD5));
    CHECK_EQ(hk(L"Ctrl+VK_OEM_3").vk, 0xC0u);   // 反引号键
    CHECK_EQ(hk(L"Shift+Up").vk, static_cast<std::uint32_t>(VK_UP));
    CHECK_EQ(hk(L"Ctrl+0").vk, static_cast<std::uint32_t>('0'));   // 主键盘 0，不是小键盘
    CHECK_EQ(hk(L"Esc").vk, static_cast<std::uint32_t>(VK_ESCAPE));
    CHECK_EQ(hk(L"Ctrl+F24").vk, static_cast<std::uint32_t>(VK_F24));
    CHECK_EQ(hk(L"Ctrl+PgUp").mods, kModCtrl);
    CHECK_EQ(hk(L"Alt+Num5").mods, kModAlt);
    CHECK_EQ(hk(L"Ctrl+vk_oem_plus").vk, static_cast<std::uint32_t>(VK_OEM_PLUS));   // 不区分大小写
    CHECK_EQ(hotkey_key_name(VK_NEXT), L"PgDn");
    CHECK_EQ(hotkey_key_from_name(L"delete"), static_cast<std::uint32_t>(VK_DELETE));
    CHECK_EQ(hotkey_key_from_name(L"F25"), 0u);
}

TEST(hotkey_none_and_empty) {
    for (const wchar_t* s : {L"None", L"none", L"NONE", L"", L"   "}) {
        const HotkeyParseResult r = parse_hotkey(s);
        CHECK_TRUE(r.ok);
        CHECK_TRUE(r.none);
        CHECK_TRUE(r.hotkey.empty());
        CHECK_TRUE(r.error.empty());
    }
}

TEST(hotkey_invalid_cases) {
    for (const wchar_t* s : {L"Ctrl+Alt", L"Ctrl+F9+F10", L"Ctrl+Ctrl+F9", L"Ctrl+Foo", L"F25", L"Ctrl++F9", L"Ctrl+",
                             L"+F9", L"Ctrl+Alt+F99", L"Ctrl+None"}) {
        const HotkeyParseResult r = parse_hotkey(s);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }
    // 错误原因可读、贴切。
    CHECK(parse_hotkey(L"Ctrl+Alt").error.find(L"缺少按键") != std::wstring::npos);
    CHECK(parse_hotkey(L"Ctrl+F9+F10").error.find(L"只能有一个按键") != std::wstring::npos);
    CHECK(parse_hotkey(L"Ctrl+Ctrl+F9").error.find(L"重复") != std::wstring::npos);
    CHECK(parse_hotkey(L"Ctrl+Foo").error.find(L"Foo") != std::wstring::npos);
    CHECK(parse_hotkey(L"F25").error.find(L"F25") != std::wstring::npos);
}

TEST(hotkey_roundtrip_all_keys_all_mods) {
    const auto names = spec_key_names();
    CHECK_EQ(names.size(), 97);
    CHECK_EQ(hotkey_key_names().size(), names.size());
    int failures = 0;
    for (const auto& name : names) {
        const std::uint32_t vk = hotkey_key_from_name(name);
        CHECK(vk != 0);
        CHECK_EQ(hotkey_key_name(vk), name);   // 规范名就是 spec 的写法
        for (std::uint32_t mods = 0; mods < 16; ++mods) {
            const Hotkey original{mods, vk};
            const std::wstring text = format_hotkey(original);
            const HotkeyParseResult r = parse_hotkey(text);
            if (!r.ok || !(r.hotkey == original) || format_hotkey(r.hotkey) != text) ++failures;
        }
    }
    CHECK_EQ(failures, 0);
}

TEST(hotkey_single_key_flagged) {
    const HotkeyParseResult r = parse_hotkey(L"F8");
    CHECK_TRUE(r.ok);
    CHECK_TRUE(r.single_key);
    CHECK_EQ(r.hotkey.mods, 0u);
    CHECK_EQ(r.hotkey.vk, static_cast<std::uint32_t>(VK_F8));

    auto strings = default_hotkey_strings();
    strings[hotkey_index(HotkeyAction::DebugBar)] = L"F8";
    const ResolvedHotkeys res = resolve_bindings(strings);
    CHECK_EQ(res.single_key.size(), 1);
    if (!res.single_key.empty()) CHECK_TRUE(res.single_key[0] == HotkeyAction::DebugBar);
    CHECK_EQ(res.active.size(), 6);   // F8 仍然注册（默认 6 个绑定）
}

TEST(hotkey_duplicate_first_wins) {
    auto strings = default_hotkey_strings();
    strings[hotkey_index(HotkeyAction::Exit)] = L"ctrl+alt+F9";   // 与 toggle 相同
    const ResolvedHotkeys res = resolve_bindings(strings);
    CHECK_EQ(res.active.size(), 6);   // 默认 6 个，exit 因重复被忽略
    bool toggle_active = false, exit_active = false;
    for (const auto& b : res.active) {
        if (b.action == HotkeyAction::Toggle) toggle_active = true;
        if (b.action == HotkeyAction::Exit) exit_active = true;
    }
    CHECK_TRUE(toggle_active);
    CHECK_FALSE(exit_active);
    CHECK_EQ(res.duplicates.size(), 1);
    if (res.duplicates.size() == 1) {
        CHECK_TRUE(res.duplicates[0].action == HotkeyAction::Exit);
        CHECK_TRUE(res.duplicates[0].winner == HotkeyAction::Toggle);
        CHECK_EQ(format_hotkey(res.duplicates[0].hotkey), L"Ctrl+Alt+F9");
        CHECK_EQ(hotkey_info(res.duplicates[0].action).name, L"退出");
    }
    CHECK_TRUE(res.invalid.empty());
}

TEST(hotkey_invalid_binding_reported) {
    auto strings = default_hotkey_strings();
    strings[hotkey_index(HotkeyAction::Toggle)] = L"Ctrl+Alt+F99";
    const ResolvedHotkeys res = resolve_bindings(strings);
    CHECK_EQ(res.invalid.size(), 1);
    if (res.invalid.size() == 1) {
        CHECK_TRUE(res.invalid[0].action == HotkeyAction::Toggle);
        CHECK_EQ(res.invalid[0].text, L"Ctrl+Alt+F99");
        CHECK_FALSE(res.invalid[0].error.empty());
    }
    CHECK_EQ(res.active.size(), 5);   // 默认 6 个减去非法的 toggle
}

TEST(hotkey_action_table_defaults) {
    CHECK_EQ(kHotkeyActions.size(), 12);
    struct Expected {
        const char* key;
        const wchar_t* name;
        const wchar_t* binding;
        HotkeyAction action;
    };
    // tray-and-hotkeys delta（settings-window，叠加在 capture-motion-detection / in-game-calibration 之上）的 12 项，
    // 顺序即 INI [hotkeys] 顺序。
    const Expected expected[] = {
        {"toggle", L"启用 / 关闭遮罩", L"Ctrl+Alt+F9", HotkeyAction::Toggle},
        {"cycle_level", L"循环遮罩强度", L"Ctrl+Alt+F10", HotkeyAction::CycleLevel},
        {"toggle_center_dot", L"中心参照点开关", L"Ctrl+Alt+F11", HotkeyAction::ToggleCenterDot},
        {"debug_bar", L"显示调试信息", L"Ctrl+Alt+F8", HotkeyAction::DebugBar},
        {"calibrate", L"校准当前游戏", L"Ctrl+Alt+F7", HotkeyAction::Calibrate},
        {"toggle_capture", L"截屏运动检测开关", L"Ctrl+Alt+F6", HotkeyAction::ToggleCapture},
        {"cycle_sensitivity", L"循环触发灵敏度", L"None", HotkeyAction::CycleSensitivity},
        {"toggle_edge_dots", L"边缘参照点开关", L"None", HotkeyAction::ToggleEdgeDots},
        {"open_settings", L"打开设置", L"None", HotkeyAction::OpenSettings},
        {"mark_game", L"把当前程序视为游戏", L"None", HotkeyAction::MarkGame},
        {"exclude_app", L"把当前程序加入排除名单", L"None", HotkeyAction::ExcludeApp},
        {"exit", L"退出", L"None", HotkeyAction::Exit},
    };
    CHECK_EQ(std::size(expected), kHotkeyActions.size());
    for (std::size_t i = 0; i < kHotkeyActions.size() && i < std::size(expected); ++i) {
        CHECK_EQ(kHotkeyActions[i].ini_key, expected[i].key);
        CHECK_EQ(kHotkeyActions[i].name, expected[i].name);
        CHECK_EQ(kHotkeyActions[i].default_binding, expected[i].binding);
        CHECK_TRUE(kHotkeyActions[i].action == expected[i].action);
        CHECK_EQ(hotkey_index(kHotkeyActions[i].action), i);
        CHECK_TRUE(&hotkey_info(expected[i].action) == &kHotkeyActions[i]);
        CHECK_TRUE(hotkey_action_from_key(expected[i].key) == kHotkeyActions[i].action);
        // RegisterHotKey id = 枚举值 + 1，往返一致
        const int id = HotkeyManager::id_for(kHotkeyActions[i].action);
        CHECK_TRUE(HotkeyManager::action_from_id(static_cast<WPARAM>(id)) == kHotkeyActions[i].action);
    }
    // 已有动作的 id 不因插入 calibrate / toggle_capture 而改变（core-overlay 的 1..9），calibrate 追加为 10，
    // toggle_capture 追加为 11（表中位置在 calibrate 之后，与 id 无关），open_settings 追加为 12（表中在 toggle_edge_dots 之后）。
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::Toggle), 1);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::DebugBar), 4);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::CycleSensitivity), 5);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::Exit), 9);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::Calibrate), 10);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::ToggleCapture), 11);
    CHECK_EQ(hotkey_index(HotkeyAction::ToggleCapture), 5);
    CHECK_EQ(HotkeyManager::id_for(HotkeyAction::OpenSettings), 12);
    CHECK_EQ(hotkey_index(HotkeyAction::OpenSettings), 8);
    CHECK_FALSE(HotkeyManager::action_from_id(0).has_value());
    CHECK_TRUE(HotkeyManager::action_from_id(12) == HotkeyAction::OpenSettings);
    CHECK_FALSE(HotkeyManager::action_from_id(13).has_value());
    CHECK_TRUE(hotkey_action_from_key("CALIBRATE") == HotkeyAction::Calibrate);
    CHECK_TRUE(hotkey_action_from_key("Toggle_Capture") == HotkeyAction::ToggleCapture);

    // 默认绑定只注册 6 个：Ctrl+Alt+F9 / F10 / F11 / F8 / F7 / F6。
    const ResolvedHotkeys res = resolve_bindings(Config{}.hotkeys);
    CHECK_EQ(res.active.size(), 6);
    std::vector<std::wstring> active;
    for (const auto& b : res.active) active.push_back(format_hotkey(b.hotkey));
    const std::vector<std::wstring> want{L"Ctrl+Alt+F9", L"Ctrl+Alt+F10", L"Ctrl+Alt+F11", L"Ctrl+Alt+F8",
                                         L"Ctrl+Alt+F7", L"Ctrl+Alt+F6"};
    CHECK_TRUE(active == want);
    CHECK_TRUE(res.duplicates.empty() && res.invalid.empty() && res.single_key.empty());
}

TEST(hotkey_old_ini_without_calibrate) {
    // core-overlay 版本的 [hotkeys]（没有 calibrate、toggle_capture）：两者都用默认值 Ctrl+Alt+F7 / F6，照样注册
    // （这里另把 exit 绑定为 Ctrl+Alt+End，所以共 7 个）。
    const Config c = load_config(
        "[hotkeys]\r\ntoggle=Ctrl+Alt+F9\r\ncycle_level=Ctrl+Alt+F10\r\ntoggle_center_dot=Ctrl+Alt+F11\r\n"
        "debug_bar=Ctrl+Alt+F8\r\ncycle_sensitivity=None\r\ntoggle_edge_dots=None\r\nmark_game=None\r\n"
        "exclude_app=None\r\nexit=Ctrl+Alt+End\r\n");
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Calibrate)], L"Ctrl+Alt+F7");
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::ToggleCapture)], L"Ctrl+Alt+F6");
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Exit)], L"Ctrl+Alt+End");
    CHECK_TRUE(c.extras.empty());
    const ResolvedHotkeys res = resolve_bindings(c.hotkeys);
    CHECK_EQ(res.active.size(), 7);
    bool calibrate_bound = false, capture_bound = false;
    for (const auto& b : res.active) {
        if (b.action == HotkeyAction::Calibrate) calibrate_bound = format_hotkey(b.hotkey) == L"Ctrl+Alt+F7";
        if (b.action == HotkeyAction::ToggleCapture) capture_bound = format_hotkey(b.hotkey) == L"Ctrl+Alt+F6";
    }
    CHECK_TRUE(calibrate_bound);
    CHECK_TRUE(capture_bound);
    // 自定义：calibrate=Shift+F7
    const Config d = load_config("[hotkeys]\ncalibrate=Shift+F7\n");
    CHECK_EQ(d.hotkeys[hotkey_index(HotkeyAction::Calibrate)], L"Shift+F7");
}

TEST(hotkey_manager_register_smoke) {
    // 用不常见的组合注册到本线程（hwnd = nullptr），再让第二个管理器注册同一组合：应报告为"被占用"。
    std::array<std::wstring, kHotkeyActionCount> strings;
    strings.fill(L"None");
    strings[hotkey_index(HotkeyAction::Exit)] = L"Ctrl+Alt+Shift+F23";
    const ResolvedHotkeys res = resolve_bindings(strings);
    CHECK_EQ(res.active.size(), 1);

    HotkeyManager first;
    const auto failed_first = first.register_all(nullptr, res);
    CHECK_EQ(first.registered().size() + failed_first.size(), 1);
    if (first.registered().size() == 1) {
        HotkeyManager second;
        const auto failed_second = second.register_all(nullptr, res);
        CHECK_EQ(failed_second.size(), 1);   // 已被占用
        CHECK_TRUE(second.registered().empty());
        if (failed_second.size() == 1) CHECK_TRUE(failed_second[0].action == HotkeyAction::Exit);
    }
    first.unregister_all();
    CHECK_TRUE(first.registered().empty());
    HotkeyManager third;   // 注销后可以再注册
    CHECK_TRUE(third.register_all(nullptr, res).empty());
    third.unregister_all();
}

TEST(hotkey_toggle_capture_binding) {
    // 旧 INI 没有 toggle_capture（in-game-calibration 版本的 [hotkeys]）：用默认 Ctrl+Alt+F6，保存时出现在 calibrate 之后。
    const Config c = load_config(
        "[hotkeys]\r\ntoggle=Ctrl+Alt+F9\r\ncycle_level=Ctrl+Alt+F10\r\ntoggle_center_dot=Ctrl+Alt+F11\r\n"
        "debug_bar=Ctrl+Alt+F8\r\ncalibrate=Ctrl+Alt+F7\r\ncycle_sensitivity=None\r\ntoggle_edge_dots=None\r\n"
        "mark_game=None\r\nexclude_app=None\r\nexit=None\r\n");
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::ToggleCapture)], L"Ctrl+Alt+F6");
    CHECK_TRUE(c.extras.empty());
    CHECK_EQ(resolve_bindings(c.hotkeys).active.size(), 6);
    // toggle_capture=None：不注册，其余 5 个照常
    const Config d = load_config("[hotkeys]\ntoggle_capture=None\n");
    const ResolvedHotkeys rd = resolve_bindings(d.hotkeys);
    CHECK_EQ(rd.active.size(), 5);
    for (const auto& b : rd.active) CHECK_FALSE(b.action == HotkeyAction::ToggleCapture);
    // 自定义组合
    const Config e = load_config("[hotkeys]\ntoggle_capture=Shift+F6\n");
    CHECK_EQ(e.hotkeys[hotkey_index(HotkeyAction::ToggleCapture)], L"Shift+F6");
    CHECK_EQ(hotkey_info(HotkeyAction::ToggleCapture).name, L"截屏运动检测开关");
}

TEST(tray_capture_item_text) {
    // tray-and-hotkeys「截屏运动检测的切换」：菜单项文字。
    CHECK_EQ(format_capture_item(false, false), L"截屏运动检测");
    CHECK_EQ(format_capture_item(false, true), L"截屏运动检测（采集失败）");
    CHECK_EQ(format_capture_item(true, false), L"截屏运动检测（已由 --no-capture 禁用）");
    CHECK_EQ(format_capture_item(true, true), L"截屏运动检测（已由 --no-capture 禁用）");   // --no-capture 优先
}
