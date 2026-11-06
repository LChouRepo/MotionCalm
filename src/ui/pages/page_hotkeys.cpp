// page_hotkeys.cpp — "快捷键"页：按热键动作表为每个动作生成一行（动作名 | 按键捕获框 |"清除"），有问题时在下方显示状态（红字 + 错误图标 / 琥珀色字 + 警告图标，正常时什么也不显示）；捕获框获得焦点时暂停全局热键、提交时试注册。
#include <string>
#include <vector>

#include "res/resource.h"
#include "ui/hotkey_edit.h"
#include "ui/pages/page.h"
#include "util/strings.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

constexpr UINT WM_APP_FOCUS_CHECK = WM_APP + 41;   // 捕获框失去焦点后检查焦点是否去了另一个捕获框

// 每个动作标签的访问键（按 INI 键；表里没有的动作不加）。
wchar_t access_key_for(std::string_view ini_key) {
    struct Pair {
        std::string_view key;
        wchar_t letter;
    };
    static constexpr Pair kKeys[] = {
        {"toggle", L'T'},          {"cycle_level", L'L'},      {"toggle_center_dot", L'C'}, {"debug_bar", L'D'},
        {"calibrate", L'A'},       {"toggle_capture", L'P'},   {"cycle_sensitivity", L'S'}, {"toggle_edge_dots", L'E'},
        {"open_settings", L'O'},   {"mark_game", L'G'},        {"exclude_app", L'X'},       {"exit", L'Q'},
    };
    for (const auto& p : kKeys) {
        if (p.key == ini_key) return p.letter;
    }
    return 0;
}

// 给动作名加访问键：zh 写成 "名字(&T)"；英文里字母在名字中出现时直接在第一个出现处加 &（Windows 英文惯例），否则写成 "Name (&P)"。
std::wstring with_access_key(std::wstring_view name, wchar_t key) {
    std::wstring s(name);
    if (key == 0) return s;
    if (i18n::current() == i18n::Lang::En) {
        const wchar_t lower = static_cast<wchar_t>(key >= L'A' && key <= L'Z' ? key - L'A' + L'a' : key);
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (s[k] == key || s[k] == lower) {
                s.insert(k, 1, L'&');
                return s;
            }
        }
    }
    s += i18n::current() == i18n::Lang::En ? L" (&" : L"(&";
    s += key;
    s += L")";
    return s;
}

bool high_contrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

using LoadIconWithScaleDownFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);

// 系统小图标（16 px × DPI）；LoadIconWithScaleDown 属于 comctl32 v6，动态取（tests.exe 没有 v6 manifest）。
HICON load_system_icon(PCWSTR id, int size) {
    static LoadIconWithScaleDownFn fn = [] {
        HMODULE m = ::GetModuleHandleW(L"comctl32.dll");
        return m != nullptr ? reinterpret_cast<LoadIconWithScaleDownFn>(::GetProcAddress(m, "LoadIconWithScaleDown"))
                            : nullptr;
    }();
    HICON icon = nullptr;
    if (fn != nullptr && SUCCEEDED(fn(nullptr, id, size, size, &icon))) return icon;
    return static_cast<HICON>(::CopyImage(::LoadIconW(nullptr, id), IMAGE_ICON, size, size, 0));
}

class HotkeysPage final : public Page, public HotkeyEditSink {
public:
    using Page::Page;

    ~HotkeysPage() override {
        for (auto& r : rows_) detach_hotkey_edit(r.edit);   // 页对象先于窗口销毁的兜底
        destroy_icons();
    }

    void on_deactivate() override { resume(); }

    // ---- HotkeyEditSink
    void hotkey_focus(int row, bool focused) override {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return;
        Row& r = rows_[static_cast<std::size_t>(row)];
        if (focused) {
            if (!paused_) {
                paused_ = true;
                host_.pause_hotkeys(true);
            }
            r.capturing = true;
            set_text(r.edit, L"");   // 显示占位提示"按下新的组合键…"
        } else {
            r.capturing = false;
            r.notice.clear();
            ::PostMessageW(hwnd_, WM_APP_FOCUS_CHECK, 0, 0);
        }
    }

    void hotkey_step(int row, const CaptureStep& step) override {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return;
        Row& r = rows_[static_cast<std::size_t>(row)];
        switch (step.kind) {
            case CaptureKind::Partial:
                r.capturing = true;
                r.notice.clear();
                set_text(r.edit, step.text);
                break;
            case CaptureKind::Restore:   // 只按了修饰键又松开：回到等待输入
                r.capturing = true;
                set_text(r.edit, L"");
                break;
            case CaptureKind::Cancel:    // Esc：放弃这次修改，显示原值
                r.capturing = false;
                r.notice.clear();
                refresh_rows();
                break;
            case CaptureKind::Unsupported:
                r.capturing = true;
                r.notice = tr(S::HkUnsupported);
                set_text(r.edit, L"");
                refresh_rows();
                break;
            case CaptureKind::Clear:
                r.capturing = false;
                r.notice.clear();
                commit(row, L"None");
                break;
            case CaptureKind::Commit: {
                r.capturing = false;
                r.notice.clear();
                const HotkeyAction action = kHotkeyActions[static_cast<std::size_t>(row)].action;
                r.tested = step.hotkey;
                r.occupied = !host_.hotkey_available(action, step.hotkey);   // 暂停期间试注册
                commit(row, step.text);
                break;
            }
            case CaptureKind::Ignore: break;
        }
    }

protected:
    void build() override {
        add_description(tr(S::HkDesc));
        rows_.resize(kHotkeyActionCount);
        for (std::size_t i = 0; i < kHotkeyActionCount; ++i) {
            Row& r = rows_[i];
            const int n = static_cast<int>(i);
            const std::wstring label = with_access_key(hotkey_action_label(kHotkeyActions[i].action),
                                                       access_key_for(kHotkeyActions[i].ini_key));
            r.label = make_label(label.c_str());
            r.edit = make(L"Edit", L"", ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, IDC_HK_EDIT_BASE + n,
                          WS_EX_CLIENTEDGE);
            ::SendMessageW(r.edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::HkCue)));
            r.clear = make_button(tr(S::HkClear), IDC_HK_CLEAR_BASE + n);
            r.icon = make(L"Static", L"", SS_ICON | SS_CENTERIMAGE | WS_GROUP, IDC_HK_ICON_BASE + n);
            r.status = make(L"Static", L"", SS_LEFT | SS_NOPREFIX | WS_GROUP, IDC_HK_STATUS_BASE + n);
            add_row(r.label, {{r.edit, 176}, {r.clear, 64, 0, 0}});
            r.status_item = add_row(nullptr, {{r.icon, 16, 16, 6}, {r.status, -1, 20, 0}}, 20);
            set_item_tight(r.status_item);
            attach_hotkey_edit(r.edit, n, this);
        }
        set_item_group_start(add_helper(tr(S::HkTip), false, true));
        load_icons();
    }

    void style_changed() override {
        destroy_icons();
        load_icons();
    }

    void load_extra() override { refresh_rows(); }

    bool command(int id, int code, HWND) override {
        if (id >= IDC_HK_CLEAR_BASE && id < IDC_HK_CLEAR_BASE + static_cast<int>(rows_.size())) {
            if (code == BN_CLICKED) commit(id - IDC_HK_CLEAR_BASE, L"None");
            return true;
        }
        return false;
    }

    bool text_color(HWND ctl, COLORREF* color) override {
        if (high_contrast()) return false;
        for (const auto& r : rows_) {
            if (ctl != r.status) continue;
            if (r.tint == RowTint::Error) *color = kErrorTextColor;
            if (r.tint == RowTint::Warning) *color = kWarningTextColor;
            return true;
        }
        return false;
    }

    bool message(UINT msg, WPARAM, LPARAM, INT_PTR* result) override {
        if (msg != WM_APP_FOCUS_CHECK) return false;
        HWND focus = ::GetFocus();
        bool in_capture = false;
        for (const auto& r : rows_) in_capture = in_capture || r.edit == focus;
        if (!in_capture) resume();
        refresh_rows();
        *result = 0;
        return true;
    }

private:
    struct Row {
        HWND label = nullptr, edit = nullptr, clear = nullptr, icon = nullptr, status = nullptr;
        int status_item = -1;
        std::optional<Hotkey> tested;     // occupied 对应的组合
        bool occupied = false;
        bool capturing = false;           // 捕获框有焦点、正等待 / 显示半成品
        std::wstring notice;              // 一次性提示（"不支持该按键"）
        RowTint tint = RowTint::None;
    };

    void load_icons() {
        const int size = px(16);
        error_icon_ = load_system_icon(IDI_ERROR, size);
        warning_icon_ = load_system_icon(IDI_WARNING, size);
        for (auto& r : rows_) r.tint = RowTint::None;   // 下次刷新时重新设置图标
    }

    void destroy_icons() {
        for (auto& r : rows_) {
            if (r.icon != nullptr && ::IsWindow(r.icon)) ::SendMessageW(r.icon, STM_SETICON, 0, 0);
        }
        if (error_icon_ != nullptr) ::DestroyIcon(error_icon_);
        if (warning_icon_ != nullptr) ::DestroyIcon(warning_icon_);
        error_icon_ = warning_icon_ = nullptr;
    }

    void commit(int row, const std::wstring& canonical) {
        if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return;
        write({{{"hotkeys", kHotkeyActions[static_cast<std::size_t>(row)].ini_key}, canonical}});
        refresh_rows();
    }

    void resume() {
        if (!paused_) return;
        paused_ = false;
        host_.pause_hotkeys(false);
        // 恢复注册之后，外部占用的情况可能变了：下次刷新时重新试注册。
        for (auto& r : rows_) r.tested.reset();
    }

    void refresh_rows() {
        if (rows_.empty() || hwnd_ == nullptr) return;
        std::vector<std::wstring> raw(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const auto v = get({"hotkeys", kHotkeyActions[i].ini_key});
            raw[i] = v ? *v : std::wstring(L"None");
            set_enabled(rows_[i].edit, v.has_value());
        }
        // 外部占用：组合变化（或热键恢复注册）后重新试注册。
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            Row& r = rows_[i];
            const HotkeyParseResult p = parse_hotkey(raw[i]);
            set_enabled(r.clear, p.ok && !p.none);
            if (!p.ok || p.none) {
                r.tested.reset();
                r.occupied = false;
                continue;
            }
            if (!r.tested || *r.tested != p.hotkey) {
                r.tested = p.hotkey;
                r.occupied = !host_.hotkey_available(kHotkeyActions[i].action, p.hotkey);
            }
        }
        std::vector<bool> occupied(rows_.size());
        for (std::size_t i = 0; i < rows_.size(); ++i) occupied[i] = rows_[i].occupied;
        const std::vector<HotkeyRowStatus> st = analyze_hotkey_rows(raw, occupied);
        bool layout_changed = false;
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            Row& r = rows_[i];
            if (!r.capturing) set_text(r.edit, st[i].display);
            const std::wstring text = r.notice.empty() ? st[i].text : r.notice;
            const RowTint tint = r.notice.empty() ? st[i].tint : RowTint::Warning;
            set_text(r.status, text);
            if (tint != r.tint) {
                r.tint = tint;
                HICON icon = tint == RowTint::Error ? error_icon_ : tint == RowTint::Warning ? warning_icon_ : nullptr;
                ::SendMessageW(r.icon, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
                ::InvalidateRect(r.status, nullptr, TRUE);
                layout_changed = true;
            }
            set_item_collapsed(r.status_item, text.empty());
        }
        if (layout_changed) relayout();
    }

    std::vector<Row> rows_;
    bool paused_ = false;
    HICON error_icon_ = nullptr;
    HICON warning_icon_ = nullptr;
};

}  // namespace

std::unique_ptr<Page> make_hotkeys_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<HotkeysPage>(host, site);
}

}  // namespace mc::ui
