// hotkeys.cpp — tray/hotkeys 的实现：按键名表、热键字符串解析 / 规范化、按表序去重（纯逻辑），以及 RegisterHotKey 封装。
#include "tray/hotkeys.h"

#include "i18n/i18n.h"

#include <cwchar>

#include "util/strings.h"

namespace mc {

std::wstring_view hotkey_action_label(HotkeyAction a) {
    // StrId::ActToggle … ActExit 与 kHotkeyActions 同序。
    const std::size_t i = hotkey_index(a);
    if (i >= kHotkeyActionCount) return L"?";
    return i18n::tr(static_cast<i18n::StrId>(static_cast<std::size_t>(i18n::StrId::ActToggle) + i));
}

namespace {

constexpr HotkeyKeyName kKeyNames[] = {
    {L"F1", VK_F1}, {L"F2", VK_F2}, {L"F3", VK_F3}, {L"F4", VK_F4}, {L"F5", VK_F5}, {L"F6", VK_F6},
    {L"F7", VK_F7}, {L"F8", VK_F8}, {L"F9", VK_F9}, {L"F10", VK_F10}, {L"F11", VK_F11}, {L"F12", VK_F12},
    {L"F13", VK_F13}, {L"F14", VK_F14}, {L"F15", VK_F15}, {L"F16", VK_F16}, {L"F17", VK_F17},
    {L"F18", VK_F18}, {L"F19", VK_F19}, {L"F20", VK_F20}, {L"F21", VK_F21}, {L"F22", VK_F22},
    {L"F23", VK_F23}, {L"F24", VK_F24}, {L"A", 'A'}, {L"B", 'B'}, {L"C", 'C'}, {L"D", 'D'}, {L"E", 'E'},
    {L"F", 'F'}, {L"G", 'G'}, {L"H", 'H'}, {L"I", 'I'}, {L"J", 'J'}, {L"K", 'K'}, {L"L", 'L'}, {L"M", 'M'},
    {L"N", 'N'}, {L"O", 'O'}, {L"P", 'P'}, {L"Q", 'Q'}, {L"R", 'R'}, {L"S", 'S'}, {L"T", 'T'}, {L"U", 'U'},
    {L"V", 'V'}, {L"W", 'W'}, {L"X", 'X'}, {L"Y", 'Y'}, {L"Z", 'Z'}, {L"0", '0'}, {L"1", '1'}, {L"2", '2'},
    {L"3", '3'}, {L"4", '4'}, {L"5", '5'}, {L"6", '6'}, {L"7", '7'}, {L"8", '8'}, {L"9", '9'},
    {L"Space", VK_SPACE}, {L"Tab", VK_TAB}, {L"Esc", VK_ESCAPE}, {L"Pause", VK_PAUSE},
    {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END},
    {L"PgUp", VK_PRIOR}, {L"PgDn", VK_NEXT}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT}, {L"Num0", VK_NUMPAD0}, {L"Num1", VK_NUMPAD1}, {L"Num2", VK_NUMPAD2},
    {L"Num3", VK_NUMPAD3}, {L"Num4", VK_NUMPAD4}, {L"Num5", VK_NUMPAD5}, {L"Num6", VK_NUMPAD6},
    {L"Num7", VK_NUMPAD7}, {L"Num8", VK_NUMPAD8}, {L"Num9", VK_NUMPAD9}, {L"VK_OEM_1", VK_OEM_1},
    {L"VK_OEM_PLUS", VK_OEM_PLUS}, {L"VK_OEM_COMMA", VK_OEM_COMMA}, {L"VK_OEM_MINUS", VK_OEM_MINUS},
    {L"VK_OEM_PERIOD", VK_OEM_PERIOD}, {L"VK_OEM_2", VK_OEM_2}, {L"VK_OEM_3", VK_OEM_3},
    {L"VK_OEM_4", VK_OEM_4}, {L"VK_OEM_5", VK_OEM_5}, {L"VK_OEM_6", VK_OEM_6}, {L"VK_OEM_7", VK_OEM_7},
    {L"VK_OEM_8", VK_OEM_8}, {L"VK_OEM_102", VK_OEM_102},
};

struct ModName {
    std::wstring_view name;
    std::uint32_t bit;
};
// 规范输出顺序：Ctrl、Alt、Shift、Win。
constexpr ModName kModNames[] = {
    {L"Ctrl", kModCtrl},
    {L"Alt", kModAlt},
    {L"Shift", kModShift},
    {L"Win", kModWin},
};

std::uint32_t modifier_from_name(std::wstring_view tok) {
    for (const auto& m : kModNames) {
        if (iequals(tok, m.name)) return m.bit;
    }
    return 0;
}

std::wstring_view modifier_name(std::uint32_t bit) {
    for (const auto& m : kModNames) {
        if (m.bit == bit) return m.name;
    }
    return L"?";
}

std::wstring quoted(std::wstring_view s) { return L"\"" + std::wstring(s) + L"\""; }

}  // namespace

std::optional<HotkeyAction> hotkey_action_from_key(std::string_view ini_key) {
    for (const auto& a : kHotkeyActions) {
        if (iequals(a.ini_key, ini_key)) return a.action;
    }
    return std::nullopt;
}

std::array<std::wstring, kHotkeyActionCount> default_hotkey_strings() {
    std::array<std::wstring, kHotkeyActionCount> out;
    for (std::size_t i = 0; i < kHotkeyActionCount; ++i) out[i] = std::wstring(kHotkeyActions[i].default_binding);
    return out;
}

std::span<const HotkeyKeyName> hotkey_key_names() { return kKeyNames; }

std::wstring_view hotkey_key_name(std::uint32_t vk) {
    for (const auto& k : kKeyNames) {
        if (k.vk == vk) return k.name;
    }
    return {};
}

std::uint32_t hotkey_key_from_name(std::wstring_view name) {
    for (const auto& k : kKeyNames) {
        if (iequals(k.name, name)) return k.vk;
    }
    return 0;
}

HotkeyParseResult parse_hotkey(std::wstring_view text) {
    HotkeyParseResult r;
    const std::wstring_view t = trim(text);
    if (t.empty() || iequals(t, L"None")) {
        r.ok = true;
        r.none = true;
        return r;
    }
    const auto tokens = split(t, L'+');
    std::uint32_t mods = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::wstring_view tok = trim(tokens[i]);
        const bool last = i + 1 == tokens.size();
        if (tok.empty()) {
            r.error = L"热键 " + quoted(t) + L" 中有空的记号（多余的 +）";
            return r;
        }
        if (const std::uint32_t m = modifier_from_name(tok); m != 0) {
            if (mods & m) {
                r.error = L"修饰键 " + std::wstring(modifier_name(m)) + L" 重复出现";
                return r;
            }
            mods |= m;
            if (last) {
                r.error = L"热键 " + quoted(t) + L" 缺少按键（只有修饰键）";
                return r;
            }
            continue;
        }
        const std::uint32_t vk = hotkey_key_from_name(tok);
        if (vk == 0) {
            r.error = L"未知按键 " + quoted(tok);
            return r;
        }
        if (!last) {
            r.error = L"热键 " + quoted(t) + L" 只能有一个按键，并且必须放在最后";
            return r;
        }
        r.ok = true;
        r.hotkey = Hotkey{mods, vk};
        r.single_key = mods == 0;
        return r;
    }
    r.error = L"热键 " + quoted(t) + L" 无法解析";
    return r;
}

std::wstring format_hotkey(const Hotkey& hk) {
    if (hk.empty()) return L"None";
    std::wstring out;
    for (const auto& m : kModNames) {
        if (hk.mods & m.bit) {
            out += m.name;
            out += L'+';
        }
    }
    const std::wstring_view key = hotkey_key_name(hk.vk);
    if (!key.empty()) {
        out += key;
    } else {
        wchar_t buf[16];
        swprintf_s(buf, L"0x%02X", static_cast<unsigned>(hk.vk));
        out += buf;
    }
    return out;
}

ResolvedHotkeys resolve_bindings(std::span<const std::wstring> strings) {
    ResolvedHotkeys r;
    for (std::size_t i = 0; i < kHotkeyActionCount; ++i) {
        const HotkeyAction action = kHotkeyActions[i].action;
        const std::wstring text = i < strings.size() ? strings[i] : std::wstring(L"None");
        const HotkeyParseResult p = parse_hotkey(text);
        if (!p.ok) {
            r.invalid.push_back(HotkeyInvalid{action, text, p.error});
            continue;
        }
        if (p.none) continue;
        bool duplicate = false;
        for (const auto& b : r.active) {
            if (b.hotkey == p.hotkey) {
                r.duplicates.push_back(HotkeyDuplicate{action, b.action, p.hotkey});
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        r.active.push_back(HotkeyBinding{action, p.hotkey});
        if (p.single_key) r.single_key.push_back(action);
    }
    return r;
}

// ---------------------------------------------------------------- HotkeyManager

std::vector<HotkeyBinding> HotkeyManager::register_all(HWND hwnd, const ResolvedHotkeys& bindings) {
    unregister_all();
    hwnd_ = hwnd;
    std::vector<HotkeyBinding> failed;
    for (const auto& b : bindings.active) {
        const UINT mods = static_cast<UINT>(b.hotkey.mods & kModMask) | MOD_NOREPEAT;
        if (::RegisterHotKey(hwnd, id_for(b.action), mods, static_cast<UINT>(b.hotkey.vk))) {
            registered_.push_back(b);
        } else {
            failed.push_back(b);
        }
    }
    return failed;
}

void HotkeyManager::unregister_all() {
    for (const auto& b : registered_) ::UnregisterHotKey(hwnd_, id_for(b.action));
    registered_.clear();
    hwnd_ = nullptr;
}

std::optional<HotkeyAction> HotkeyManager::action_from_id(WPARAM id) {
    if (id >= 1 && id <= kHotkeyActionCount) return static_cast<HotkeyAction>(static_cast<int>(id) - 1);
    return std::nullopt;
}

}  // namespace mc
