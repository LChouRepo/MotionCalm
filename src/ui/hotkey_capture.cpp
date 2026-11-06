// hotkey_capture.cpp — ui/hotkey_capture 的实现（纯逻辑）。
#include "ui/hotkey_capture.h"

#include "i18n/i18n.h"
#include "util/strings.h"

namespace mc::ui {
namespace {

constexpr std::uint32_t kNavBlockers = kModCtrl | kModAlt | kModWin;
constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

std::wstring_view row_name(std::size_t i) {
    return i < kHotkeyActionCount ? hotkey_action_label(kHotkeyActions[i].action) : std::wstring_view(L"?");
}

std::wstring spaced_mods(std::uint32_t mods) {
    std::wstring s;
    if (mods & kModCtrl) s += L"Ctrl + ";
    if (mods & kModAlt) s += L"Alt + ";
    if (mods & kModShift) s += L"Shift + ";
    if (mods & kModWin) s += L"Win + ";
    return s;
}

}  // namespace

std::wstring hotkey_display_text(std::wstring_view raw) {
    const HotkeyParseResult p = parse_hotkey(raw);
    if (!p.ok) return std::wstring(raw);
    if (p.none) return i18n::tr(i18n::StrId::HkNone);
    return spaced_mods(p.hotkey.mods) + std::wstring(hotkey_key_name(p.hotkey.vk));
}

std::wstring capture_partial_text(std::uint32_t mods) { return spaced_mods(mods & kModMask) + L"…"; }

bool is_modifier_vk(std::uint32_t vk) {
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN: return true;
        default: return false;
    }
}

bool capture_passes_to_dialog(std::uint32_t vk, std::uint32_t mods) {
    return vk == VK_TAB && (mods & kNavBlockers) == 0;
}

CaptureStep HotkeyCapture::key_down(std::uint32_t vk, std::uint32_t mods) {
    mods &= kModMask;
    CaptureStep step;
    if (is_modifier_vk(vk)) {
        pending_ = true;
        step.kind = CaptureKind::Partial;
        step.text = capture_partial_text(mods);
        return step;
    }
    if (vk == 0 || vk == VK_PROCESSKEY || vk == VK_PACKET) return step;   // 输入法 / 注入字符：忽略
    pending_ = false;
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) {
        step.kind = CaptureKind::Clear;
        step.text = L"None";
        return step;
    }
    if (mods == 0 && vk == VK_ESCAPE) {
        step.kind = CaptureKind::Cancel;
        return step;
    }
    if (hotkey_key_name(vk).empty()) {
        step.kind = CaptureKind::Unsupported;
        return step;
    }
    step.kind = CaptureKind::Commit;
    step.hotkey = Hotkey{mods, vk};
    step.text = format_hotkey(step.hotkey);
    return step;
}

CaptureStep HotkeyCapture::key_up(std::uint32_t vk, std::uint32_t mods_after) {
    CaptureStep step;
    if (!pending_ || !is_modifier_vk(vk)) return step;
    mods_after &= kModMask;
    if (mods_after == 0) {
        pending_ = false;
        step.kind = CaptureKind::Restore;
        return step;
    }
    step.kind = CaptureKind::Partial;
    step.text = capture_partial_text(mods_after);
    return step;
}

std::vector<HotkeyRowStatus> analyze_hotkey_rows(std::span<const std::wstring> raw, const std::vector<bool>& occupied) {
    const std::size_t n = kHotkeyActionCount;
    std::vector<HotkeyRowStatus> rows(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::wstring text = i < raw.size() ? raw[i] : std::wstring(L"None");
        const HotkeyParseResult p = parse_hotkey(text);
        HotkeyRowStatus& r = rows[i];
        r.display = hotkey_display_text(text);
        if (!p.ok) {
            r.state = RowState::Invalid;
        } else if (p.none) {
            r.state = RowState::Unbound;
        } else {
            r.state = RowState::Ok;
            r.hotkey = p.hotkey;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        HotkeyRowStatus& r = rows[i];
        if (r.state == RowState::Invalid) {
            r.tint = RowTint::Error;
            r.text = i18n::tr(i18n::StrId::HkInvalid);
            continue;
        }
        if (!r.hotkey) continue;
        std::size_t earlier = kNoRow, later = kNoRow;
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i || !rows[j].hotkey || *rows[j].hotkey != *r.hotkey) continue;
            if (j < i && earlier == kNoRow) earlier = j;
            if (j > i && later == kNoRow) later = j;
        }
        if (earlier != kNoRow) {
            r.state = RowState::DuplicateInactive;
            r.tint = RowTint::Error;
            r.other = earlier;
            r.text = i18n::format(i18n::StrId::HkDupInactive, {{L"other", std::wstring(row_name(earlier))}});
            continue;
        }
        if (i < occupied.size() && occupied[i]) {
            r.state = RowState::Occupied;
            r.tint = RowTint::Error;
            r.text = i18n::tr(i18n::StrId::HkOccupied);
            continue;
        }
        if (later != kNoRow) {
            r.state = RowState::Duplicate;
            r.tint = RowTint::Error;
            r.other = later;
            r.text = i18n::format(i18n::StrId::HkDup, {{L"other", std::wstring(row_name(later))}});
            continue;
        }
        if ((r.hotkey->mods & kModMask) == 0) {
            r.state = RowState::NoModifier;
            r.tint = RowTint::Warning;
            r.text = i18n::tr(i18n::StrId::HkNoModifier);
        }
    }
    return rows;
}

}  // namespace mc::ui
