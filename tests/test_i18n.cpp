// test_i18n.cpp — localization：三张文字表完整（每个 ID 非空、占位符集合一致）、格式化、系统语言 → 界面语言的映射、
// [ui] language 的读写，以及切换语言后几处界面文字确实换了（zh-TW / en 的单位与用语）。
#include "test_framework.h"

#include <algorithm>
#include <string>
#include <vector>

#include "app/app.h"
#include "config/config.h"
#include "i18n/i18n.h"
#include "tray/hotkeys.h"
#include "tray/tray.h"
#include "ui/settings_logic.h"
#include "util/strings.h"

using namespace mc;
using i18n::Lang;
using i18n::StrId;

namespace {

// 测试结束时恢复 zh-CN（其他测试按 zh-CN 源文本断言）。
struct LangGuard {
    explicit LangGuard(Lang l) { i18n::set_language(l); }
    ~LangGuard() { i18n::set_language(Lang::ZhCN); }
    LangGuard(const LangGuard&) = delete;
    LangGuard& operator=(const LangGuard&) = delete;
};

std::vector<std::wstring> sorted(std::vector<std::wstring> v) {
    std::sort(v.begin(), v.end());
    return v;
}

bool contains(std::wstring_view s, std::wstring_view part) { return s.find(part) != std::wstring_view::npos; }

}  // namespace

TEST(i18n_every_id_has_text_in_all_languages) {
    int empty = 0;
    for (std::size_t i = 0; i < i18n::kStrCount; ++i) {
        for (const wchar_t* const* table : {i18n::kStringsZhCn, i18n::kStringsZhTw, i18n::kStringsEn}) {
            const wchar_t* s = table[i];
            if (s == nullptr || *s == 0) {
                ++empty;
                std::printf("   empty: %s\n", i18n::kStrNames[i] != nullptr ? i18n::kStrNames[i] : "?");
            }
        }
        CHECK_TRUE(i18n::kStrNames[i] != nullptr && *i18n::kStrNames[i] != 0);
    }
    CHECK_EQ(empty, 0);
    std::printf("   %zu strings x 3 languages\n", i18n::kStrCount);
}

TEST(i18n_placeholders_match_across_languages) {
    int mismatches = 0;
    for (std::size_t i = 0; i < i18n::kStrCount; ++i) {
        const auto id = static_cast<StrId>(i);
        const auto cn = sorted(i18n::placeholders(i18n::tr(id, Lang::ZhCN)));
        const auto tw = sorted(i18n::placeholders(i18n::tr(id, Lang::ZhTW)));
        const auto en = sorted(i18n::placeholders(i18n::tr(id, Lang::En)));
        if (cn != tw || cn != en) {
            ++mismatches;
            std::printf("   placeholder mismatch: %s\n", i18n::kStrNames[i]);
        }
    }
    CHECK_EQ(mismatches, 0);
}

TEST(i18n_access_keys_present_in_all_languages) {
    // zh-CN 带访问键（&X）的文字，在 zh-TW / en 里也必须有访问键。
    int missing = 0;
    for (std::size_t i = 0; i < i18n::kStrCount; ++i) {
        const auto id = static_cast<StrId>(i);
        const std::wstring_view cn = i18n::tr(id, Lang::ZhCN);
        if (cn.find(L"(&") == std::wstring_view::npos) continue;
        for (const Lang l : {Lang::ZhTW, Lang::En}) {
            if (std::wstring_view(i18n::tr(id, l)).find(L'&') == std::wstring_view::npos) {
                ++missing;
                std::printf("   no access key: %s\n", i18n::kStrNames[i]);
            }
        }
    }
    CHECK_EQ(missing, 0);
}

TEST(i18n_format_text) {
    CHECK_EQ(i18n::format_text(L"删除 {exe}（每度 {cpd} 计数）", {{L"exe", L"cs2.exe"}, {L"cpd", L"18.4"}}),
             L"删除 cs2.exe（每度 18.4 计数）");
    CHECK_EQ(i18n::format_text(L"{a}{a}{b}", {{L"a", L"x"}}), L"xx{b}");   // 没有实参的占位符原样保留
    CHECK_EQ(i18n::format_text(L"{ not} {} {A}", {{L"not", L"?"}}), L"{ not} {} {A}");
    CHECK_EQ(i18n::format_text(L"", {}), L"");
    const auto ph = i18n::placeholders(L"{hk} … {n} … {hk}");
    CHECK_EQ(ph.size(), 2);
    if (ph.size() == 2) {
        CHECK_EQ(ph[0], L"hk");
        CHECK_EQ(ph[1], L"n");
    }
}

TEST(i18n_locale_name_mapping) {
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-TW") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-HK") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-MO") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-Hant") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-Hant-HK") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"ZH_tw") == Lang::ZhTW);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-CN") == Lang::ZhCN);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-SG") == Lang::ZhCN);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-Hans") == Lang::ZhCN);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh-Hans-HK") == Lang::ZhCN);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zh") == Lang::ZhCN);
    CHECK_TRUE(i18n::lang_from_locale_name(L"en-US") == Lang::En);
    CHECK_TRUE(i18n::lang_from_locale_name(L"ja-JP") == Lang::En);
    CHECK_TRUE(i18n::lang_from_locale_name(L"") == Lang::En);
    CHECK_TRUE(i18n::lang_from_locale_name(L"zhx") == Lang::En);
    // 本机的推断结果只打印（取决于系统设置）
    std::printf("   this machine: %ls\n", i18n::lang_code(i18n::detect_system_language()));
    CHECK_TRUE(i18n::resolve(i18n::LangSetting::En) == Lang::En);
    CHECK_TRUE(i18n::resolve(i18n::LangSetting::ZhTW) == Lang::ZhTW);
    i18n::LangSetting st{};
    CHECK_TRUE(i18n::parse_lang_setting(L"ZH-tw", &st) && st == i18n::LangSetting::ZhTW);
    CHECK_TRUE(i18n::parse_lang_setting(L"auto", &st) && st == i18n::LangSetting::Auto);
    CHECK_FALSE(i18n::parse_lang_setting(L"fr", &st));
}

TEST(i18n_config_language_key) {
    const Config def;
    CHECK_TRUE(def.ui.language == i18n::LangSetting::Auto);
    CHECK_EQ(config_get_value(def, "ui", "language").value_or(L"?"), L"auto");
    Config c;
    CHECK_TRUE(config_set_value(c, "ui", "language", L"zh-tw"));
    CHECK_TRUE(c.ui.language == i18n::LangSetting::ZhTW);
    CHECK_EQ(config_get_value(c, "ui", "language").value_or(L"?"), L"zh-TW");
    CHECK_FALSE(config_set_value(c, "ui", "language", L"fr"));
    CHECK_TRUE(c.ui.language == i18n::LangSetting::ZhTW);
    std::vector<std::string> w;
    const Config bad = load_config("[ui]\nlanguage=klingon\n", &w);
    CHECK_EQ(w.size(), 1);
    CHECK_TRUE(bad.ui.language == i18n::LangSetting::Auto);
    const Config en = load_config("[ui]\nlanguage=en\n");
    CHECK_TRUE(en.ui.language == i18n::LangSetting::En);
    // 恢复默认设置保留界面语言（[ui] 整节保留）
    CHECK_TRUE(restore_defaults(en).ui.language == i18n::LangSetting::En);
}

TEST(i18n_switching_changes_ui_text) {
    {
        const LangGuard g(Lang::ZhTW);
        CHECK_EQ(ui::format_readout(0.60, ui::Readout::ScreenHeight), L"螢幕高度的 60%");
        CHECK_EQ(ui::format_readout(100, ui::Readout::Millis), L"100 毫秒");
        CHECK_EQ(ui::format_readout(60, ui::Readout::DegPerSec), L"60°/秒");
        CHECK_EQ(ui::format_readout(1.0, ui::Readout::Gain), L"1.0 倍");
        CHECK_EQ(std::wstring(hotkey_action_label(HotkeyAction::Exit)), L"結束");
        CHECK_EQ(format_status_line(true, L"cs2.exe", true, true, MaskLevel::Strong), L"遊戲：cs2.exe · 已鎖定 · 遮罩：強");
        CHECK_TRUE(contains(i18n::tr(StrId::TabHotkeys), L"快速鍵"));
        CHECK_TRUE(contains(i18n::tr(StrId::TrayAutostart), L"開機時自動啟動"));
    }
    {
        const LangGuard g(Lang::En);
        CHECK_EQ(ui::format_readout(0.60, ui::Readout::ScreenHeight), L"60% of screen height");
        CHECK_EQ(ui::format_readout(100, ui::Readout::Millis), L"100 ms");
        CHECK_EQ(ui::format_readout(60, ui::Readout::DegPerSec), L"60°/s");
        CHECK_EQ(ui::format_readout(3, ui::Readout::Pixels), L"3 px");
        CHECK_EQ(ui::format_readout(1.5, ui::Readout::Gain), L"1.5×");
        CHECK_EQ(ui::range_text(1, 5000, 0), L"Allowed range: 1–5000");
        CHECK_EQ(format_status_line(false, L"", false, false, MaskLevel::Medium), L"Game: none · Mask: off");
        CHECK_EQ(format_capture_item(true, false), L"Screen motion detection (disabled by --no-capture)");
        CHECK_EQ(format_capture_debug_field(true, CaptureState::Paused, 0, 0, 0, 0), L"paused");
        CHECK_EQ(ui::known_games_label(3), L"Known games: 3");
        CHECK_EQ(std::wstring(i18n::tr(StrId::TraySettings)), L"Settings…");
    }
    // 守卫恢复为 zh-CN
    CHECK_TRUE(i18n::current() == Lang::ZhCN);
    CHECK_EQ(ui::format_readout(0.60, ui::Readout::ScreenHeight), L"屏高的 60%");
}

TEST(i18n_traditional_wording_rules) {
    // 台湾用语抽查：不得残留大陆用语（设置、鼠标、屏幕、快捷键、程序、默认、视频、配置文件、日志、托盘、手柄、菜单、窗口、信息、支持、加载、保存、用户、打开）。
    const wchar_t* banned[] = {L"设置", L"設置", L"鼠標", L"屏幕", L"螢幕高的", L"快捷鍵", L"程序名", L"默認", L"預設預設",
                               L"視頻", L"配置文件", L"日誌", L"托盤", L"手柄", L"菜單", L"窗口", L"信息", L"支持",
                               L"加載", L"保存", L"用戶", L"打開", L"質量", L"數據"};
    int hits = 0;
    for (std::size_t i = 0; i < i18n::kStrCount; ++i) {
        const std::wstring_view s = i18n::tr(static_cast<StrId>(i), Lang::ZhTW);
        if (static_cast<StrId>(i) == StrId::LangZhCn) continue;   // "简体中文" 是语言名本身
        for (const wchar_t* b : banned) {
            if (s.find(b) != std::wstring_view::npos) {
                ++hits;
                std::printf("   zh-TW %s contains a mainland term\n", i18n::kStrNames[i]);
            }
        }
    }
    CHECK_EQ(hits, 0);
}
