// page_general.cpp — "通用"页：开机自启、录屏可见性、日志级别（--verbose 附注）、排除名单 / 视为游戏两个名单的增删（互斥规则与托盘相同）、已知游戏数量与清空（确认框）、打开配置文件。"恢复默认…"在窗口底部状态栏。
#include <string>
#include <vector>

#include "detect/game_detect.h"
#include "res/resource.h"
#include "ui/pages/page.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

constexpr SettingKey kExclude{"detect", "exclude"};
constexpr SettingKey kForce{"detect", "force_game"};
constexpr SettingKey kKnown{"detect", "known_games"};
constexpr SettingKey kLanguage{"ui", "language"};

class GeneralPage final : public Page {
public:
    using Page::Page;

protected:
    void build() override {
        add_description(tr(S::GenDesc));

        // localization：界面语言。选项各用自己的语言显示（"跟随系统"按当前语言）。
        add_heading(tr(S::GenHeadLanguage));
        add_combo(tr(S::GenLanguageLabel), IDC_GEN_LANGUAGE, kLanguage,
                  {{tr(S::LangAuto), L"auto", L""},
                   {tr(S::LangZhCn), L"zh-CN", L""},
                   {tr(S::LangZhTw), L"zh-TW", L""},
                   {tr(S::LangEn), L"en", L""}},
                  200);
        add_helper(tr(S::GenLanguageHelp));

        add_heading(tr(S::GenHeadStartup));
        add_check(tr(S::GenAutostartLabel), IDC_GEN_AUTOSTART, tr(S::GenAutostartCheck), {"general", "autostart"});
        add_combo(tr(S::GenHideLabel), IDC_GEN_HIDE, {"general", "hide_from_capture"},
                  {{tr(S::GenHideAuto), L"auto", L""},
                   {tr(S::GenHideAlways), L"always", L""},
                   {tr(S::GenHideNever), L"never", L""}},
                  240);
        add_helper(tr(S::GenHideHelp));
        HWND log_label = make_label(tr(S::GenLogLabel));
        HWND log_combo = make(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, IDC_GEN_LOGLEVEL);
        verbose_note_ = make(L"Static", tr(S::GenVerboseNote), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | WS_GROUP,
                             IDC_GEN_VERBOSE_NOTE);
        add_row(log_label, {{log_combo, 120, 0, 12}, {verbose_note_, -1, 20, 0}});
        bind_combo_items(log_combo, {"general", "log_level"},
                         {{tr(S::GenLogDebug), L"debug", L""}, {tr(S::GenLogInfo), L"info", L""}, {tr(S::GenLogWarn), L"warn", L""}});

        add_heading(tr(S::GenHeadLists));
        list_row(tr(S::GenExcludeLabel), IDC_GEN_EXCLUDE_LIST, IDC_GEN_EXCLUDE_EDIT, IDC_GEN_EXCLUDE_ADD, IDC_GEN_EXCLUDE_DEL,
                 tr(S::GenExcludeCue));
        add_helper(tr(S::GenExcludeHelp));
        list_row(tr(S::GenForceLabel), IDC_GEN_FORCE_LIST, IDC_GEN_FORCE_EDIT, IDC_GEN_FORCE_ADD, IDC_GEN_FORCE_DEL,
                 tr(S::GenForceCue));
        add_helper(tr(S::GenForceHelp));
        HWND known_label = make_label(tr(S::GenKnownLabel));
        HWND count = make(L"Static", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | WS_GROUP, IDC_GEN_KNOWN_COUNT);
        HWND clear = make_button(tr(S::GenKnownClear), IDC_GEN_KNOWN_CLEAR);
        add_row(known_label, {{count, 112, 20, 8}, {clear, 176, 0, 0}});
        add_helper(tr(S::GenKnownHelp));

        add_heading(tr(S::GenHeadConfig));
        HWND cfg_label = make_label(tr(S::GenConfigLabel));
        HWND open = make_button(tr(S::GenOpenConfig), IDC_GEN_OPEN_CONFIG);
        add_row(cfg_label, {{open, 144, 0, 0}});
        add_helper(tr(S::GenConfigHelp));
    }

    void load_extra() override {
        set_part_hidden(verbose_note_, !host_.status().verbose);
        const DetectLists l = lists();
        fill(IDC_GEN_EXCLUDE_LIST, l.exclude, &shown_exclude_, &shown_valid_[0]);
        fill(IDC_GEN_FORCE_LIST, l.force_game, &shown_force_, &shown_valid_[1]);
        set_text(item(IDC_GEN_KNOWN_COUNT), known_games_label(l.known_games.size()));
        set_enabled(IDC_GEN_KNOWN_CLEAR, !l.known_games.empty());
        const bool have_lists = get(kExclude).has_value() && get(kForce).has_value() && get(kKnown).has_value();
        for (int id : {IDC_GEN_EXCLUDE_LIST, IDC_GEN_EXCLUDE_EDIT, IDC_GEN_EXCLUDE_ADD, IDC_GEN_FORCE_LIST,
                       IDC_GEN_FORCE_EDIT, IDC_GEN_FORCE_ADD}) {
            set_enabled(id, have_lists);
        }
        update_delete_buttons();
    }

    bool command(int id, int code, HWND) override {
        switch (id) {
            case IDC_GEN_EXCLUDE_LIST:
            case IDC_GEN_FORCE_LIST:
                if (code == LBN_SELCHANGE) update_delete_buttons();
                return true;
            case IDC_GEN_EXCLUDE_ADD:
                if (code == BN_CLICKED) add_name(true);
                return true;
            case IDC_GEN_FORCE_ADD:
                if (code == BN_CLICKED) add_name(false);
                return true;
            case IDC_GEN_EXCLUDE_DEL:
                if (code == BN_CLICKED) remove_selected(true);
                return true;
            case IDC_GEN_FORCE_DEL:
                if (code == BN_CLICKED) remove_selected(false);
                return true;
            case IDC_GEN_KNOWN_CLEAR:
                if (code == BN_CLICKED) {
                    const std::size_t n = lists().known_games.size();
                    if (site_.confirm(clear_known_games_confirm(n))) {
                        host_.clear_known_games();
                        site_.notice(tr(S::GenNoticeCleared));
                        site_.request_refresh();
                    }
                }
                return true;
            case IDC_GEN_OPEN_CONFIG:
                if (code == BN_CLICKED) host_.open_config_file();
                return true;
            default: return false;
        }
    }

    // 名称输入框里按 Enter = 点"添加"。
    bool commit_custom(HWND focus) override {
        if (focus == item(IDC_GEN_EXCLUDE_EDIT)) {
            add_name(true);
            return true;
        }
        if (focus == item(IDC_GEN_FORCE_EDIT)) {
            add_name(false);
            return true;
        }
        return false;
    }

private:
    // 列表 + 下一行"名称输入框 / 添加 / 删除"。
    void list_row(const wchar_t* label, int list_id, int edit_id, int add_id, int del_id, const wchar_t* cue) {
        HWND lbl = make_label(label);
        HWND lb = make(L"ListBox", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP,
                       list_id);
        add_row(lbl, {{lb, 260, 88, 0}});
        HWND edit = make(L"Edit", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, edit_id, WS_EX_CLIENTEDGE);
        ::SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        HWND add = make_button(tr(S::GenAdd), add_id);
        HWND del = make_button(tr(S::GenRemove), del_id);
        add_row(nullptr, {{edit, 132}, {add, 60}, {del, 60, 0, 0}});
    }

    void bind_combo_items(HWND combo, SettingKey key, std::vector<Choice> choices) {
        for (const auto& c : choices) {
            ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::wstring(c.label).c_str()));
        }
        bind_combo(::GetDlgCtrlID(combo), key, std::move(choices));
    }

    DetectLists lists() const {
        DetectLists l;
        l.exclude = parse_name_list(get(kExclude).value_or(L""));
        l.force_game = parse_name_list(get(kForce).value_or(L""));
        l.known_games = parse_name_list(get(kKnown).value_or(L""));
        return l;
    }

    void fill(int list_id, const std::vector<std::wstring>& items, std::vector<std::wstring>* shown, bool* valid) {
        if (*valid && *shown == items) return;
        *valid = true;
        HWND lb = item(list_id);
        std::wstring keep;
        const auto sel = static_cast<int>(::SendMessageW(lb, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && static_cast<std::size_t>(sel) < shown->size()) keep = (*shown)[static_cast<std::size_t>(sel)];
        ::SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        ::SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        int reselect = -1;
        for (std::size_t k = 0; k < items.size(); ++k) {
            ::SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[k].c_str()));
            if (!keep.empty() && items[k] == keep) reselect = static_cast<int>(k);
        }
        if (reselect >= 0) ::SendMessageW(lb, LB_SETCURSEL, static_cast<WPARAM>(reselect), 0);
        ::SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(lb, nullptr, TRUE);
        *shown = items;
    }

    void update_delete_buttons() {
        set_enabled(IDC_GEN_EXCLUDE_DEL, ::SendMessageW(item(IDC_GEN_EXCLUDE_LIST), LB_GETCURSEL, 0, 0) >= 0);
        set_enabled(IDC_GEN_FORCE_DEL, ::SendMessageW(item(IDC_GEN_FORCE_LIST), LB_GETCURSEL, 0, 0) >= 0);
    }

    void add_name(bool to_exclude) {
        HWND edit = item(to_exclude ? IDC_GEN_EXCLUDE_EDIT : IDC_GEN_FORCE_EDIT);
        DetectLists l = lists();
        std::wstring name;
        const NameCheck c = check_list_name(text_of(edit), to_exclude ? l.exclude : l.force_game, &name);
        if (c != NameCheck::Ok) {
            balloon(edit, tr(S::GenCannotAdd), std::wstring(name_check_message(c)));
            return;
        }
        // 互斥规则与托盘的两个标记操作相同（detect/game_detect）。
        std::vector<SettingValue> values;
        if (to_exclude) {
            exclude_app(l, name);   // 加入排除名单，并从视为游戏与已知游戏中移除
            values = {{kExclude, join_name_list(l.exclude)},
                      {kForce, join_name_list(l.force_game)},
                      {kKnown, join_name_list(l.known_games)}};
        } else {
            mark_as_game(l, name);   // 加入视为游戏，并从排除名单中移除
            values = {{kExclude, join_name_list(l.exclude)}, {kForce, join_name_list(l.force_game)}};
        }
        if (write(values)) {
            set_text(edit, L"");
            site_.notice(i18n::format(to_exclude ? S::GenNoticeAddedExclude : S::GenNoticeAddedForce, {{L"name", name}}));
        }
    }

    void remove_selected(bool from_exclude) {
        HWND lb = item(from_exclude ? IDC_GEN_EXCLUDE_LIST : IDC_GEN_FORCE_LIST);
        const auto sel = static_cast<int>(::SendMessageW(lb, LB_GETCURSEL, 0, 0));
        const std::vector<std::wstring>& shown = from_exclude ? shown_exclude_ : shown_force_;
        if (sel < 0 || static_cast<std::size_t>(sel) >= shown.size()) return;
        const std::wstring name = shown[static_cast<std::size_t>(sel)];
        DetectLists l = lists();
        std::vector<std::wstring>& target = from_exclude ? l.exclude : l.force_game;
        list_remove(target, name);
        if (write({{from_exclude ? kExclude : kForce, join_name_list(target)}})) {
            site_.notice(i18n::format(from_exclude ? S::GenNoticeRemovedExclude : S::GenNoticeRemovedForce, {{L"name", name}}));
        }
    }

    HWND verbose_note_ = nullptr;
    std::vector<std::wstring> shown_exclude_, shown_force_;
    bool shown_valid_[2] = {false, false};
};

}  // namespace

std::unique_ptr<Page> make_general_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<GeneralPage>(host, site);
}

}  // namespace mc::ui
