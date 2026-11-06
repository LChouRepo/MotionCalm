// hotkeys.h — 热键动作表（INI [hotkeys] 的模式）、热键字符串 ⇄ 组合键（纯函数）、绑定去重，以及 RegisterHotKey 管理。
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc {

// 修饰键位，与 RegisterHotKey 的 MOD_* 取值相同，可直接传给它。
inline constexpr std::uint32_t kModAlt = 0x0001;     // MOD_ALT
inline constexpr std::uint32_t kModCtrl = 0x0002;    // MOD_CONTROL
inline constexpr std::uint32_t kModShift = 0x0004;   // MOD_SHIFT
inline constexpr std::uint32_t kModWin = 0x0008;     // MOD_WIN
inline constexpr std::uint32_t kModMask = kModAlt | kModCtrl | kModShift | kModWin;

// 一个组合键：mods 为 kMod* 的按位或，vk 为虚拟键码；vk == 0 表示"不绑定"。
struct Hotkey {
    std::uint32_t mods = 0;
    std::uint32_t vk = 0;
    bool empty() const { return vk == 0; }
    bool operator==(const Hotkey&) const = default;
};

// 动作。枚举值是稳定的动作 id（RegisterHotKey 的 id = 枚举值 + 1），新动作只在末尾追加，已有动作的 id 不变；
// 动作表 / INI [hotkeys] 的顺序由 kHotkeyActions 决定（与枚举值无关，见 hotkey_index）。
enum class HotkeyAction : int {
    Toggle = 0,
    CycleLevel = 1,
    ToggleCenterDot = 2,
    DebugBar = 3,
    CycleSensitivity = 4,
    ToggleEdgeDots = 5,
    MarkGame = 6,
    ExcludeApp = 7,
    Exit = 8,
    Calibrate = 9,       // 变更 2（in-game-calibration）
    ToggleCapture = 10,  // 变更 3（capture-motion-detection）
    OpenSettings = 11,   // 变更 4（settings-window）
};

struct HotkeyActionInfo {
    HotkeyAction action;
    std::string_view ini_key;            // [hotkeys] 中的键名
    std::wstring_view name;              // 中文名（重复绑定 toast、设置窗口用）
    std::wstring_view default_binding;   // 默认热键字符串
};

inline constexpr std::size_t kHotkeyActionCount = 12;

// 热键动作表（tray-and-hotkeys spec "热键动作表"，12 项，顺序固定 = INI [hotkeys] 顺序）。
inline constexpr std::array<HotkeyActionInfo, kHotkeyActionCount> kHotkeyActions{{
    {HotkeyAction::Toggle, "toggle", L"启用 / 关闭遮罩", L"Ctrl+Alt+F9"},
    {HotkeyAction::CycleLevel, "cycle_level", L"循环遮罩强度", L"Ctrl+Alt+F10"},
    {HotkeyAction::ToggleCenterDot, "toggle_center_dot", L"中心参照点开关", L"Ctrl+Alt+F11"},
    {HotkeyAction::DebugBar, "debug_bar", L"显示调试信息", L"Ctrl+Alt+F8"},
    {HotkeyAction::Calibrate, "calibrate", L"校准当前游戏", L"Ctrl+Alt+F7"},
    {HotkeyAction::ToggleCapture, "toggle_capture", L"截屏运动检测开关", L"Ctrl+Alt+F6"},
    {HotkeyAction::CycleSensitivity, "cycle_sensitivity", L"循环触发灵敏度", L"None"},
    {HotkeyAction::ToggleEdgeDots, "toggle_edge_dots", L"边缘参照点开关", L"None"},
    {HotkeyAction::OpenSettings, "open_settings", L"打开设置", L"None"},
    {HotkeyAction::MarkGame, "mark_game", L"把当前程序视为游戏", L"None"},
    {HotkeyAction::ExcludeApp, "exclude_app", L"把当前程序加入排除名单", L"None"},
    {HotkeyAction::Exit, "exit", L"退出", L"None"},
}};

// 动作在当前界面语言下的名字（快捷键页、重复绑定 toast）；HotkeyActionInfo::name 是 zh-CN 源文本。
std::wstring_view hotkey_action_label(HotkeyAction a);

// 动作在表中的序号（= Config::hotkeys、TrayMenuState::hotkey_text 的下标）。
constexpr std::size_t hotkey_index(HotkeyAction a) {
    for (std::size_t i = 0; i < kHotkeyActionCount; ++i) {
        if (kHotkeyActions[i].action == a) return i;
    }
    return kHotkeyActionCount;   // 不可能：每个动作都在表中（见下面的 static_assert）
}
// 表中恰好包含 id 0..kHotkeyActionCount-1 的每个动作各一次。
constexpr bool hotkey_table_is_permutation() {
    for (std::size_t id = 0; id < kHotkeyActionCount; ++id) {
        std::size_t n = 0;
        for (const auto& a : kHotkeyActions) {
            if (static_cast<std::size_t>(a.action) == id) ++n;
        }
        if (n != 1) return false;
    }
    return true;
}
static_assert(hotkey_table_is_permutation(), "kHotkeyActions must list every HotkeyAction exactly once");
// 动作的表项。
constexpr const HotkeyActionInfo& hotkey_info(HotkeyAction a) { return kHotkeyActions[hotkey_index(a)]; }
// 按 INI 键名查动作（ASCII 大小写不敏感）。
std::optional<HotkeyAction> hotkey_action_from_key(std::string_view ini_key);
// 每个动作的默认热键字符串（按表序），用作 Config::hotkeys 的默认值。
std::array<std::wstring, kHotkeyActionCount> default_hotkey_strings();

// 解析结果。ok && none 表示"不绑定"；ok && !none 时 hotkey 有效；!ok 时 error 为可读的中文原因。
struct HotkeyParseResult {
    bool ok = false;
    bool none = false;
    bool single_key = false;   // 解析成功且不带修饰键（会吃掉游戏里的该键，需记 warn）
    Hotkey hotkey;
    std::wstring error;
};

// 解析 "Ctrl+Alt+Shift+Win+<Key>"：记号以 + 分隔、前后可有空白、不区分大小写、修饰键顺序任意且各至多一次、
// 按键恰好一个且在末尾；"None"（不区分大小写）或空串表示不绑定。
HotkeyParseResult parse_hotkey(std::wstring_view text);
// 规范化输出：修饰键按 Ctrl、Alt、Shift、Win 排列，再接按键规范名；空组合输出 "None"。
std::wstring format_hotkey(const Hotkey& hk);
// 虚拟键码的规范名（例如 VK_PRIOR → "PgUp"）；不支持的键返回空。
std::wstring_view hotkey_key_name(std::uint32_t vk);
// 按规范名查虚拟键码（不区分大小写）；未知返回 0。
std::uint32_t hotkey_key_from_name(std::wstring_view name);

// 按键名表的一项（供往返测试与设置窗口枚举）。
struct HotkeyKeyName {
    std::wstring_view name;
    std::uint32_t vk;
};
// 全部支持的按键名（规范写法）。
std::span<const HotkeyKeyName> hotkey_key_names();

// ---------------------------------------------------------------- 绑定去重

struct HotkeyBinding {
    HotkeyAction action;
    Hotkey hotkey;
};
struct HotkeyDuplicate {
    HotkeyAction action;   // 被忽略的动作
    HotkeyAction winner;   // 表中靠前、实际生效的动作
    Hotkey hotkey;
};
struct HotkeyInvalid {
    HotkeyAction action;
    std::wstring text;     // 用户写的原串
    std::wstring error;    // parse_hotkey 给出的原因
};
struct ResolvedHotkeys {
    std::vector<HotkeyBinding> active;          // 生效的绑定（表序）
    std::vector<HotkeyDuplicate> duplicates;    // 与表中靠前的动作组合相同而被忽略的
    std::vector<HotkeyInvalid> invalid;         // 字符串非法的
    std::vector<HotkeyAction> single_key;       // active 中不带修饰键的动作
};

// strings[i] 对应 kHotkeyActions[i]（缺的按 None）；按表序解析与去重，靠前的生效。
ResolvedHotkeys resolve_bindings(std::span<const std::wstring> strings);

// ---------------------------------------------------------------- RegisterHotKey 管理（Win32）

// 把 resolve_bindings 的结果注册为全局热键（MOD_NOREPEAT，id = 动作枚举值 + 1），注销时全部撤销。
class HotkeyManager {
public:
    HotkeyManager() = default;
    ~HotkeyManager() { unregister_all(); }
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // 先注销已有的，再注册 bindings.active 中的每一项；返回注册失败（多半是被其他程序占用）的项。
    std::vector<HotkeyBinding> register_all(HWND hwnd, const ResolvedHotkeys& bindings);
    // 注销本管理器注册过的全部热键。
    void unregister_all();
    // 当前已成功注册的绑定。
    const std::vector<HotkeyBinding>& registered() const { return registered_; }
    // WM_HOTKEY 的 wParam（id）→ 动作；不是本表的 id 返回 nullopt。
    static std::optional<HotkeyAction> action_from_id(WPARAM id);
    // 动作对应的 RegisterHotKey id（枚举值 + 1；与表中位置无关）。
    static int id_for(HotkeyAction a) { return static_cast<int>(a) + 1; }

private:
    HWND hwnd_ = nullptr;
    std::vector<HotkeyBinding> registered_;
};

}  // namespace mc
