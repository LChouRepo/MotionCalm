// page_calib.cpp — "校准"页：校准记录列表视图（游戏 / 每度鼠标计数 / 校准日期，按 exe 升序，空表时列表中央显示说明）、重测（指引）、删除（确认框）、未校准游戏的每度鼠标计数。
#include <algorithm>
#include <cwchar>

#include "res/resource.h"
#include "ui/pages/page.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

class CalibPage final : public Page {
public:
    using Page::Page;

protected:
    void build() override {
        add_description(tr(S::CalDesc));

        add_heading(tr(S::CalHeadRecords));
        HWND list_label = make_label(tr(S::CalListLabel));
        add_row(list_label, {}, 20, false, true);
        list_ = make(WC_LISTVIEWW, L"",
                     LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP | WS_GROUP,
                     IDC_CAL_LIST);
        ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                       LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[] = {tr(S::CalColGame), tr(S::CalColCpd), tr(S::CalColDate)};
        const int fmts[] = {LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT};
        for (int k = 0; k < 3; ++k) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
            col.fmt = fmts[k];
            col.cx = 100;
            col.pszText = const_cast<wchar_t*>(titles[k]);
            ::SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(&col));
        }
        add_row(nullptr, {{list_, -1, 168, 0}}, layout::kRowH, false, true);
        HWND recal = make_button(tr(S::CalRecal), IDC_CAL_RECAL);
        HWND del = make_button(tr(S::CalDelete), IDC_CAL_DELETE);
        add_row(nullptr, {{recal, 96}, {del, 96, 0, 0}}, layout::kRowH, false, true);
        hint_item_ = add_helper(L"", false, true);
        hint_ = item_text(hint_item_);
        helpers_off(hint_);
        add_helper(tr(S::CalHelp), false, true);

        add_heading(tr(S::CalHeadUncal));
        add_number(tr(S::CalDefaultCpdLabel), IDC_CAL_DEFAULT_CPD, {"trigger", "default_counts_per_degree"}, 1, 1, nullptr);
        add_helper(tr(S::CalDefaultCpdHelp));
    }

    void style_changed() override { set_column_widths(); }

    void load_extra() override {
        set_column_widths();
        std::vector<CalibEntry> entries = host_.calibrations();
        std::sort(entries.begin(), entries.end(), [](const CalibEntry& a, const CalibEntry& b) { return a.exe < b.exe; });
        if (!shown_valid_ || entries != shown_) rebuild(entries);
        const std::wstring hk = calibrate_hotkey();
        if (hk != empty_hotkey_) {   // 空表说明里的快捷键跟着绑定变化
            empty_hotkey_ = hk;
            ::InvalidateRect(list_, nullptr, TRUE);
        }
        update_buttons();
        set_text(hint_, hint_exe_.empty() ? std::wstring() : recalibrate_hint(hint_exe_, hk));
        set_item_collapsed(hint_item_, hint_exe_.empty());
    }

    bool command(int id, int code, HWND) override {
        if (code != BN_CLICKED) return false;
        if (id == IDC_CAL_DELETE) {
            const CalibEntry* e = selected();
            if (e == nullptr) return true;
            const CalibEntry entry = *e;
            if (!site_.confirm(delete_calibration_confirm(entry.exe, entry.counts_per_degree))) return true;
            host_.delete_calibration(entry.exe);
            if (hint_exe_ == entry.exe) hint_exe_.clear();
            site_.notice(i18n::format(S::CalNoticeDeleted, {{L"exe", entry.exe}}));
            site_.request_refresh();
            return true;
        }
        if (id == IDC_CAL_RECAL) {
            const CalibEntry* e = selected();
            if (e == nullptr || !host_.status().calibration_idle) return true;
            hint_exe_ = e->exe;
            site_.request_refresh();
            return true;
        }
        return false;
    }

    bool notify(NMHDR* nm, LRESULT* result) override {
        if (nm->idFrom != IDC_CAL_LIST) return false;
        if (nm->code == LVN_ITEMCHANGED) {
            const auto* lv = reinterpret_cast<const NMLISTVIEW*>(nm);
            if ((lv->uChanged & LVIF_STATE) != 0 && ((lv->uOldState ^ lv->uNewState) & LVIS_SELECTED) != 0) {
                const CalibEntry* e = selected();
                if (!hint_exe_.empty() && (e == nullptr || e->exe != hint_exe_)) {
                    hint_exe_.clear();
                    site_.request_refresh();
                }
                update_buttons();
            }
            *result = 0;
            return true;
        }
        if (nm->code == LVN_GETEMPTYMARKUP) {   // 空列表：列表区域中央显示灰色说明（comctl32 v6）
            auto* m = reinterpret_cast<NMLVEMPTYMARKUP*>(nm);
            m->dwFlags = EMF_CENTERED;
            wcsncpy_s(m->szMarkup, calibration_empty_text(calibrate_hotkey()).c_str(), _TRUNCATE);
            *result = TRUE;
            return true;
        }
        return false;
    }

private:
    // 指引是正文颜色（不是灰色辅助文字）。
    void helpers_off(HWND h) { text_hwnd_normal_ = h; }

    bool text_color(HWND ctl, COLORREF* color) override {
        if (ctl != text_hwnd_normal_) return false;
        *color = ::GetSysColor(COLOR_WINDOWTEXT);
        return true;
    }

    void set_column_widths() {
        const int widths[] = {px(210), px(150), px(110)};
        for (int k = 0; k < 3; ++k) ::SendMessageW(list_, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(k), widths[k]);
    }

    void rebuild(const std::vector<CalibEntry>& entries) {
        const CalibEntry* sel = selected();
        const std::wstring keep = sel != nullptr ? sel->exe : std::wstring();
        ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        int select = -1;
        for (std::size_t k = 0; k < entries.size(); ++k) {
            const CalibEntry& e = entries[k];
            std::wstring exe = e.exe;
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = static_cast<int>(k);
            it.pszText = exe.data();
            const int row = static_cast<int>(::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it)));
            std::wstring cpd = format_cpd_cell(e.counts_per_degree);
            std::wstring date = format_date_cell(e.date);
            LVITEMW sub{};
            sub.iSubItem = 1;
            sub.pszText = cpd.data();
            ::SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&sub));
            sub.iSubItem = 2;
            sub.pszText = date.data();
            ::SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&sub));
            if (!keep.empty() && e.exe == keep) select = row;
        }
        shown_ = entries;
        shown_valid_ = true;
        if (select >= 0) {
            LVITEMW st{};
            st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            st.state = LVIS_SELECTED | LVIS_FOCUSED;
            ::SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(select), reinterpret_cast<LPARAM>(&st));
        }
        ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(list_, nullptr, TRUE);
        const CalibEntry* now = selected();
        if (now == nullptr || now->exe != hint_exe_) hint_exe_.clear();
    }

    const CalibEntry* selected() const {
        const int sel = static_cast<int>(::SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        if (sel < 0 || static_cast<std::size_t>(sel) >= shown_.size()) return nullptr;
        return &shown_[static_cast<std::size_t>(sel)];
    }

    void update_buttons() {
        const bool has = selected() != nullptr;
        set_enabled(IDC_CAL_DELETE, has);
        set_enabled(IDC_CAL_RECAL, has && host_.status().calibration_idle);
    }

    // calibrate 当前绑定的规范字符串；None 或写法非法时为空。
    std::wstring calibrate_hotkey() const {
        const auto raw = get({"hotkeys", "calibrate"});
        if (!raw) return {};
        const HotkeyParseResult p = parse_hotkey(*raw);
        return p.ok && !p.none ? format_hotkey(p.hotkey) : std::wstring();
    }

    HWND list_ = nullptr;
    HWND hint_ = nullptr;
    HWND text_hwnd_normal_ = nullptr;
    int hint_item_ = -1;
    std::vector<CalibEntry> shown_;
    bool shown_valid_ = false;
    std::wstring hint_exe_;
    std::wstring empty_hotkey_ = L"?";
};

}  // namespace

std::unique_ptr<Page> make_calib_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<CalibPage>(host, site);
}

}  // namespace mc::ui
