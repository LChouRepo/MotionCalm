// test_settings_lists.cpp — ui/settings_logic 的名单与文案部分：名单解析 / 连接、添加校验（空、非法字符、重复）、与 detect 的互斥规则配合、已知游戏计数、校准列表的显示文字、重测指引与空表说明、确认框文案、截屏复选框文字、数值读数。
#include "test_framework.h"

#include <string>
#include <vector>

#include "detect/game_detect.h"
#include "ui/settings_logic.h"

using namespace mc;
using namespace mc::ui;

TEST(settings_name_list_parse_join) {
    const auto l = parse_name_list(L" a.exe ;B.exe;;a.EXE; c.exe ");
    CHECK_EQ(l.size(), 3u);
    CHECK_EQ(l[0], L"a.exe");
    CHECK_EQ(l[1], L"B.exe");
    CHECK_EQ(l[2], L"c.exe");
    CHECK_EQ(join_name_list(l), L"a.exe;B.exe;c.exe");
    CHECK_TRUE(parse_name_list(L"").empty());
    CHECK_EQ(join_name_list({}), L"");
}

TEST(settings_name_check_rules) {
    const std::vector<std::wstring> exclude{L"chrome.exe", L"msedge.exe"};
    std::wstring name;
    CHECK_EQ(check_list_name(L"  tool.exe  ", exclude, &name), NameCheck::Ok);
    CHECK_EQ(name, L"tool.exe");
    // spec：向排除名单添加 Chrome.EXE → 被拒绝，提示"已在名单中"
    CHECK_EQ(check_list_name(L"Chrome.EXE", exclude), NameCheck::Duplicate);
    CHECK_EQ(name_check_message(NameCheck::Duplicate), L"已在名单中");
    CHECK_EQ(check_list_name(L"   ", exclude), NameCheck::Empty);
    CHECK_EQ(check_list_name(L"a;b.exe", exclude), NameCheck::BadChar);
    CHECK_EQ(check_list_name(L"a=b.exe", exclude), NameCheck::BadChar);
    CHECK_FALSE(name_check_message(NameCheck::Empty).empty());
    CHECK_FALSE(name_check_message(NameCheck::BadChar).empty());
    CHECK_TRUE(name_check_message(NameCheck::Ok).empty());
}

TEST(settings_list_mutual_exclusion_reuses_detect_rules) {
    // spec：tool.exe 在 known_games 中，加入排除名单 → known_games 中没有它
    DetectLists l;
    l.exclude = parse_name_list(L"chrome.exe");
    l.force_game = parse_name_list(L"tool.exe;mygame.exe");
    l.known_games = parse_name_list(L"newgame.exe;tool.exe");
    exclude_app(l, L"tool.exe");
    CHECK_EQ(join_name_list(l.exclude), L"chrome.exe;tool.exe");
    CHECK_EQ(join_name_list(l.force_game), L"mygame.exe");
    CHECK_EQ(join_name_list(l.known_games), L"newgame.exe");
    CHECK_EQ(known_games_label(l.known_games.size()), L"已知游戏：1 个");
    // 加入"视为游戏"时从排除名单移除
    mark_as_game(l, L"Chrome.exe");
    CHECK_EQ(join_name_list(l.exclude), L"tool.exe");
    CHECK_TRUE(list_contains(l.force_game, L"chrome.exe"));
    CHECK_EQ(known_games_label(0), L"已知游戏：0 个");
}

TEST(settings_calibration_cells) {
    // spec：INI cs2.exe=18.4 → "cs2.exe | 18.40 | —"
    CHECK_EQ(format_cpd_cell(18.4), L"18.40");
    CHECK_EQ(format_cpd_cell(35.714), L"35.71");
    CHECK_EQ(format_date_cell(L""), L"—");
    CHECK_EQ(format_date_cell(L"2026-11-01"), L"2026-11-01");
    CHECK_EQ(format_date_cell(L"昨天"), L"—");
    CHECK_TRUE(is_calendar_date(L"2024-02-29"));
    CHECK_FALSE(is_calendar_date(L"2026-02-29"));
    CHECK_FALSE(is_calendar_date(L"2026-13-01"));
    CHECK_FALSE(is_calendar_date(L"2026-04-31"));
    CHECK_FALSE(is_calendar_date(L"2026-1-01"));
    CHECK_FALSE(is_calendar_date(L"2026/11/01"));
    CHECK_FALSE(is_calendar_date(L"1899-12-31"));
}

TEST(settings_recalibrate_hint_and_empty_text) {
    // spec：calibrate=Ctrl+Alt+F7，选中 cs2.exe，点"重测"
    CHECK_EQ(recalibrate_hint(L"cs2.exe", L"Ctrl+Alt+F7"),
             L"请切回 cs2.exe，锁定视角后按 Ctrl+Alt+F7 开始校准；完成后会覆盖这条记录。");
    const std::wstring unbound = recalibrate_hint(L"cs2.exe", L"");
    CHECK(unbound.find(L"快捷键") != std::wstring::npos);
    CHECK(unbound.find(L"cs2.exe") != std::wstring::npos);
    CHECK_EQ(calibration_empty_text(L"Ctrl+Alt+F7"), L"还没有校准过任何游戏。进入游戏后按 Ctrl+Alt+F7 开始。");
    CHECK(calibration_empty_text(L"").find(L"快捷键") != std::wstring::npos);
}

TEST(settings_confirm_texts) {
    const ConfirmText r = restore_defaults_confirm();
    CHECK_EQ(r.verb, L"恢复默认");
    CHECK(r.content.find(L"将把遮罩、参照物、触发、快捷键恢复为默认值。校准数据、游戏名单和开机自启保持不变。") == 0);
    const ConfirmText k = clear_known_games_confirm(3);
    CHECK_EQ(k.verb, L"清空");
    CHECK(k.content.find(L"将清空自动识别的 3 个游戏。之后需要在游戏里转一次视角才会重新识别。") == 0);
    const ConfirmText d = delete_calibration_confirm(L"cs2.exe", 36.4);
    CHECK_EQ(d.instruction, L"删除 cs2.exe 的校准？");   // spec 的确认框文字
    CHECK_EQ(d.content, L"删除 cs2.exe 的校准（每度 36.4 计数）？删除后该游戏改用默认灵敏度估算。");
    CHECK_EQ(d.verb, L"删除");
}

TEST(settings_capture_checkbox_label) {
    CHECK_EQ(capture_checkbox_label(false, false), L"截屏运动检测");
    CHECK_EQ(capture_checkbox_label(true, false), L"截屏运动检测（已由 --no-capture 禁用）");
    CHECK_EQ(capture_checkbox_label(false, true), L"截屏运动检测（采集失败）");
    CHECK_EQ(capture_checkbox_label(true, true), L"截屏运动检测（已由 --no-capture 禁用）");
}

TEST(settings_readouts_plain_units) {
    // docs/settings-ui.md §2："75%"、"屏高的 60%"、"100 毫秒"、"60°/秒"、"3 像素"
    CHECK_EQ(format_readout(0.75, Readout::Percent), L"75%");
    CHECK_EQ(format_readout(1.5, Readout::Gain), L"1.5 倍");   // 变更 5：前进 / 后退的灵敏度
    CHECK_EQ(format_readout(3.0, Readout::Gain), L"3.0 倍");
    CHECK_EQ(format_readout(0.5, Readout::Gain), L"0.5 倍");
    CHECK_EQ(format_readout(0.60, Readout::ScreenHeight), L"屏高的 60%");
    CHECK_EQ(format_readout(1.0, Readout::ScreenHeight), L"屏高的 100%");
    CHECK_EQ(format_readout(100, Readout::Millis), L"100 毫秒");
    CHECK_EQ(format_readout(60, Readout::DegPerSec), L"60°/秒");
    CHECK_EQ(format_readout(3, Readout::Pixels), L"3 像素");
    CHECK_EQ(format_readout(90, Readout::Degrees), L"90°");
    CHECK_EQ(format_readout(1.25, Readout::Times), L"1.25 倍");
    CHECK_EQ(format_readout(0.35, Readout::Percent), L"35%");
    CHECK_EQ(format_readout(0.0, Readout::Percent), L"0%");
    // 不在步长上的 INI 值照实显示（spec「数值控件的取值规则」）
    CHECK_EQ(format_readout(0.355, Readout::Percent), L"35.5%");
    CHECK_EQ(format_readout(0.07, Readout::Percent), L"7%");
}
