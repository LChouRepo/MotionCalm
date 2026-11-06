// test_config_access.cpp — 设置窗口用的按键访问（config_get_value / config_set_value / config_key_range）、[ui] 节、
// [calibration] 日期、恢复默认 / 清空已知游戏（settings-window 变更的 configuration delta）。
#include "test_framework.h"

#include <windows.h>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "calib/calibration.h"
#include "config/config.h"
#include "config/ini.h"
#include "util/strings.h"

using namespace mc;

namespace {

std::string template_text() {
    std::wstring f = utf8_to_wide(__FILE__);
    const auto slash = f.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring(L".") : f.substr(0, slash);
    std::string text;
    const bool ok = read_file(dir + L"\\..\\src\\res\\MotionCalm.default.ini", &text);
    CHECK_TRUE(ok);
    return text;
}

bool has(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

// 模板里除 [calibration] 外的每个键（小写节, 小写键, 模板中的值：标量键剥离行尾注释，列表键原样）。
struct TemplateKey {
    std::string section, key, value;
};
std::vector<TemplateKey> template_keys(const std::string& tmpl) {
    static const std::set<std::string> kLists = {"detect.exclude", "detect.force_game", "detect.known_games"};
    std::vector<TemplateKey> out;
    for (const auto& e : ini::parse_ini(tmpl)) {
        if (e.section == "calibration") continue;
        const std::string name = e.section + "." + e.key;
        const std::string v(kLists.count(name) ? std::string_view(e.value) : ini::strip_inline_comment(e.value));
        out.push_back({e.section, e.key, v});
    }
    return out;
}

std::wstring get_or(const Config& c, std::string_view s, std::string_view k) {
    return config_get_value(c, s, k).value_or(L"<nullopt>");
}

}  // namespace

// ================================================================ get / set / range 覆盖模式表的每个键

TEST(config_access_get_matches_template_for_every_key) {
    const std::string tmpl = template_text();
    const Config def;
    const auto keys = template_keys(tmpl);
    CHECK_EQ(keys.size(), 84);   // general 6 + mask 11 + dots 10 + trigger 17 + detect 3 + hotkeys 12 + capture 21 + ui 4
    for (const auto& k : keys) {
        const auto v = config_get_value(def, k.section, k.key);
        CHECK_TRUE(v.has_value());
        if (v) CHECK_EQ(wide_to_utf8(*v), k.value);   // 默认配置的写法 = 模板里的写法
    }
    // [hotkeys] 共 12 个键都可访问，包括新增的 open_settings
    CHECK_EQ(get_or(def, "hotkeys", "open_settings"), L"None");
    CHECK_EQ(get_or(def, "ui", "show_advanced"), L"0");
    CHECK_EQ(get_or(def, "ui", "x"), L"");
    CHECK_EQ(get_or(def, "ui", "y"), L"");
    // 不认识的键
    CHECK_FALSE(config_get_value(def, "mask", "glow").has_value());
    CHECK_FALSE(config_get_value(def, "nosuch", "x").has_value());
    CHECK_FALSE(config_get_value(def, "hotkeys", "frobnicate").has_value());
    CHECK_FALSE(config_get_value(def, "calibration", "cs2.exe").has_value());
}

TEST(config_access_set_get_roundtrip_every_key) {
    const std::string tmpl = template_text();
    for (const auto& k : template_keys(tmpl)) {
        Config c;
        const std::wstring v = get_or(c, k.section, k.key);
        CHECK_TRUE(config_set_value(c, k.section, k.key, v));   // 写回自己的写法：配置不变
        CHECK_TRUE(c == Config{});
        CHECK_EQ(get_or(c, k.section, k.key), v);
    }
    // 不认识的键：拒绝，不改配置
    Config c;
    CHECK_FALSE(config_set_value(c, "mask", "glow", L"1"));
    CHECK_FALSE(config_set_value(c, "hotkeys", "frobnicate", L"Ctrl+Alt+F3"));
    CHECK_TRUE(c == Config{});
}

TEST(config_access_numeric_range_and_clamp_every_key) {
    // 每个数值键：range 与 load_config 的截断范围相同（同一份定义）；set 越界截断、非数字拒绝。
    const std::string tmpl = template_text();
    int numeric = 0;
    for (const auto& k : template_keys(tmpl)) {
        const auto r = config_key_range(k.section, k.key);
        const bool is_ui_pos = k.section == "ui" && (k.key == "x" || k.key == "y");
        if (!r) continue;
        ++numeric;
        CHECK(r->lo < r->hi);
        // 默认值在范围内
        if (!is_ui_pos) {
            const double dv = std::stod(k.value);
            CHECK(dv >= r->lo && dv <= r->hi);
        }
        // set 越界截断到 range 的边界，与 load_config 的截断相同（阈值键除外：load_config 在 v_start ≥ v_full 时
        // 把整档恢复默认，单键越界必然触发这条规则）
        const std::string hi_text = format_ini_number(r->hi + 1000.0, 0);
        const std::string lo_text = format_ini_number(r->lo - 1000.0, 0);
        Config hi_set, lo_set;
        CHECK_TRUE(config_set_value(hi_set, k.section, k.key, utf8_to_wide(hi_text)));
        CHECK_TRUE(config_set_value(lo_set, k.section, k.key, utf8_to_wide(lo_text)));
        const bool threshold =
            (k.section == "trigger" && (k.key.rfind("v_start_", 0) == 0 || k.key.rfind("v_full_", 0) == 0)) ||
            (k.section == "capture" && (k.key.rfind("zoom_", 0) == 0 || k.key.rfind("roll_", 0) == 0));
        if (!threshold) {
            std::vector<std::string> w;
            const Config hi_cfg = load_config("[" + k.section + "]\n" + k.key + "=" + hi_text + "\n", &w);
            const Config lo_cfg = load_config("[" + k.section + "]\n" + k.key + "=" + lo_text + "\n", &w);
            CHECK_EQ(w.size(), 2);
            CHECK_EQ(get_or(hi_set, k.section, k.key), get_or(hi_cfg, k.section, k.key));
            CHECK_EQ(get_or(lo_set, k.section, k.key), get_or(lo_cfg, k.section, k.key));
        }
        CHECK_NEAR(std::stod(wide_to_utf8(get_or(hi_set, k.section, k.key))), r->hi, 1e-9);
        CHECK_NEAR(std::stod(wide_to_utf8(get_or(lo_set, k.section, k.key))), r->lo, 1e-9);
        // 非数字：拒绝且不改
        Config bad;
        CHECK_FALSE(config_set_value(bad, k.section, k.key, L"abc"));
        CHECK_TRUE(bad == Config{});
    }
    CHECK_EQ(numeric, 48);   // [mask] 10 + [dots] 5 + [trigger] 11 + [capture] 20 + [ui] x / y
    // 变更 5：前进 / 后退的灵敏度与基线
    const auto r_gain = config_key_range("capture", "forward_gain");
    CHECK_TRUE(r_gain.has_value());
    if (r_gain) {
        CHECK_NEAR(r_gain->lo, 0.5, 0);
        CHECK_NEAR(r_gain->hi, 3.0, 0);
    }
    const auto r_base = config_key_range("capture", "baseline_ms");
    CHECK_TRUE(r_base.has_value());
    if (r_base) {
        CHECK_NEAR(r_base->lo, 0, 0);
        CHECK_NEAR(r_base->hi, 150, 0);
    }
    CHECK_FALSE(config_key_range("capture", "detect_roll").has_value());   // 布尔键没有数值范围
    // 抽查（configuration「值校验与回退」与变更 3 的截屏键范围）
    const auto r1 = config_key_range("mask", "release_ms");
    CHECK_TRUE(r1.has_value());
    if (r1) {
        CHECK_NEAR(r1->lo, 10, 0);
        CHECK_NEAR(r1->hi, 5000, 0);
    }
    const auto r2 = config_key_range("capture", "min_correlation");
    CHECK_TRUE(r2.has_value());
    if (r2) {
        CHECK_NEAR(r2->lo, 0.10, 0);
        CHECK_NEAR(r2->hi, 0.99, 0);
    }
    const auto r3 = config_key_range("trigger", "default_counts_per_degree");
    CHECK_TRUE(r3.has_value());
    if (r3) {
        CHECK_NEAR(r3->lo, 1, 0);
        CHECK_NEAR(r3->hi, 1000, 0);
    }
    // 非数值键与不认识的键
    CHECK_FALSE(config_key_range("general", "mask_level").has_value());
    CHECK_FALSE(config_key_range("mask", "color").has_value());
    CHECK_FALSE(config_key_range("detect", "exclude").has_value());
    CHECK_FALSE(config_key_range("hotkeys", "toggle").has_value());
    CHECK_FALSE(config_key_range("ui", "show_advanced").has_value());
    CHECK_FALSE(config_key_range("mask", "glow").has_value());
}

TEST(config_access_set_value_types) {
    Config c;
    // 数值：设置窗口的写法；整数键四舍五入；不在步长上的值原样保留
    CHECK_TRUE(config_set_value(c, "mask", "feather", L"0.355"));
    CHECK_EQ(get_or(c, "mask", "feather"), L"0.355");
    CHECK_TRUE(config_set_value(c, "mask", "attack_ms", L" 150.4 "));
    CHECK_EQ(c.mask.attack_ms, 150);
    CHECK_TRUE(config_set_value(c, "mask", "release_ms", L"9000"));   // 越界截断
    CHECK_EQ(c.mask.release_ms, 5000);
    CHECK_TRUE(config_set_value(c, "mask", "max_opacity_strong", L"0.5"));
    CHECK_EQ(get_or(c, "mask", "max_opacity_strong"), L"0.50");          // 补零到默认值的小数位
    // 布尔只接受 0 / 1
    CHECK_TRUE(config_set_value(c, "general", "enabled", L"0"));
    CHECK_FALSE(c.enabled);
    CHECK_FALSE(config_set_value(c, "general", "enabled", L"2"));
    CHECK_FALSE(config_set_value(c, "general", "enabled", L"true"));
    CHECK_FALSE(c.enabled);
    // 枚举不区分大小写，写回规范小写
    CHECK_TRUE(config_set_value(c, "general", "mask_level", L"Strong"));
    CHECK_TRUE(c.mask.level == MaskLevel::Strong);
    CHECK_EQ(get_or(c, "general", "mask_level"), L"strong");
    CHECK_FALSE(config_set_value(c, "general", "mask_level", L"extreme"));
    CHECK_TRUE(c.mask.level == MaskLevel::Strong);
    CHECK_TRUE(config_set_value(c, "general", "hide_from_capture", L"never"));
    CHECK_TRUE(c.hide_from_capture == HideFromCapture::Never);
    CHECK_TRUE(config_set_value(c, "trigger", "capture_sensitivity", L"low"));
    CHECK_TRUE(c.trigger.capture_sensitivity == Sensitivity::Low);
    // 颜色 RRGGBB（大小写都可，写回大写）
    CHECK_TRUE(config_set_value(c, "mask", "color", L"20a0ff"));
    CHECK_EQ(get_or(c, "mask", "color"), L"20A0FF");
    CHECK_FALSE(config_set_value(c, "mask", "color", L"12345"));
    CHECK_FALSE(config_set_value(c, "mask", "color", L"GGGGGG"));
    // 列表：; 切分、去空白、忽略大小写去重
    CHECK_TRUE(config_set_value(c, "detect", "force_game", L"MyGame.exe; other.exe;mygame.EXE;"));
    CHECK_EQ(get_or(c, "detect", "force_game"), L"MyGame.exe;other.exe");
    CHECK_TRUE(config_set_value(c, "detect", "known_games", L""));
    CHECK_TRUE(c.detect.known_games.empty());
    // [hotkeys]：捕获产生的规范串、None；非法串拒绝
    CHECK_TRUE(config_set_value(c, "hotkeys", "exit", L"Ctrl+Shift+End"));
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Exit)], L"Ctrl+Shift+End");
    CHECK_TRUE(config_set_value(c, "hotkeys", "toggle", L"None"));
    CHECK_EQ(get_or(c, "hotkeys", "toggle"), L"None");
    CHECK_FALSE(config_set_value(c, "hotkeys", "toggle", L"Ctrl+Alt+F99"));
    CHECK_EQ(get_or(c, "hotkeys", "toggle"), L"None");
    CHECK_TRUE(config_set_value(c, "hotkeys", "open_settings", L"Ctrl+Alt+F12"));
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::OpenSettings)], L"Ctrl+Alt+F12");
    // get 返回用户原串（可能非法）
    Config raw = load_config("[hotkeys]\ntoggle=alt + ctrl + f9\ndebug_bar=Ctrl+Alt+F99\n");
    CHECK_EQ(get_or(raw, "hotkeys", "toggle"), L"alt + ctrl + f9");
    CHECK_EQ(get_or(raw, "hotkeys", "debug_bar"), L"Ctrl+Alt+F99");
}

TEST(config_access_thresholds_ordered) {
    Config c;
    CHECK_TRUE(config_thresholds_ordered(c));
    CHECK_TRUE(config_set_value(c, "trigger", "v_start_medium", L"400"));   // 单键写入不检查顺序
    CHECK_FALSE(config_thresholds_ordered(c));
    CHECK_TRUE(config_set_value(c, "trigger", "v_full_medium", L"500"));
    CHECK_TRUE(config_thresholds_ordered(c));
    // 变更 5：前进 / 倾斜各自的阈值表也要求开始 < 完全
    CHECK_TRUE(config_set_value(c, "capture", "zoom_start_high", L"90"));   // 满强度 83
    CHECK_FALSE(config_thresholds_ordered(c));
    CHECK_TRUE(config_set_value(c, "capture", "zoom_start_high", L"10"));
    CHECK_TRUE(config_thresholds_ordered(c));
    CHECK_TRUE(config_set_value(c, "capture", "roll_full_low", L"30"));     // 开始 30
    CHECK_FALSE(config_thresholds_ordered(c));
}

// ================================================================ [ui] 节

TEST(config_ui_section) {
    const std::string tmpl = template_text();
    CHECK(has(tmpl, "\r\n\r\n[ui]"));
    CHECK(has(tmpl, "\r\nshow_advanced=0\r\n"));
    CHECK(has(tmpl, "\r\nx=\r\ny=\r\n"));
    const Config def = load_config(tmpl);
    CHECK_FALSE(def.ui.show_advanced);
    CHECK_FALSE(def.ui.x.has_value());
    CHECK_FALSE(def.ui.y.has_value());

    Config c;
    CHECK_TRUE(config_set_value(c, "ui", "show_advanced", L"1"));
    CHECK_TRUE(config_set_value(c, "ui", "x", L"-1200"));
    CHECK_TRUE(config_set_value(c, "ui", "y", L"300"));
    const std::string saved = save_config_text(c, tmpl);
    CHECK(has(saved, "\r\nshow_advanced=1\r\n"));
    CHECK(has(saved, "\r\nx=-1200\r\ny=300\r\n"));
    std::vector<std::string> w;
    const Config back = load_config(saved, &w);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(back == c);
    CHECK_TRUE(back.ui.x == -1200);
    // 清空 = 未设置；保存回到模板
    CHECK_TRUE(config_set_value(c, "ui", "x", L""));
    CHECK_TRUE(config_set_value(c, "ui", "y", L""));
    CHECK_TRUE(config_set_value(c, "ui", "show_advanced", L"0"));
    CHECK_TRUE(save_config_text(c, tmpl) == tmpl);
    CHECK_FALSE(config_set_value(c, "ui", "x", L"left"));
    // INI 中写坏：warn 并当作未设置；越界截断
    std::vector<std::string> w2;
    const Config bad = load_config("[ui]\nx=left\ny=9999999\n", &w2);
    CHECK_EQ(w2.size(), 2);
    CHECK_FALSE(bad.ui.x.has_value());
    CHECK_TRUE(bad.ui.y == 100000);
    // 旧 INI 没有 [ui]：默认值、不记 warn，保存后出现 [ui]
    std::string old = tmpl;
    old.erase(old.find("\r\n\r\n[ui]") + 2);
    std::vector<std::string> w3;
    const Config o = load_config(old, &w3);
    CHECK_EQ(w3.size(), 0);
    CHECK_TRUE(o == Config{});
    CHECK_TRUE(save_config_text(o, tmpl) == tmpl);
}

// ================================================================ [hotkeys] open_settings

TEST(config_old_ini_without_open_settings) {
    // 由更早版本生成的 INI 没有 open_settings：不报 warn、不注册；保存时在 toggle_edge_dots 之后出现 open_settings=None。
    const std::string tmpl = template_text();
    std::string old = tmpl;
    const std::string line = "; 打开设置窗口（托盘\"设置…\"或双击托盘图标也可以打开）\r\nopen_settings=None\r\n";
    const std::size_t pos = old.find(line);
    CHECK(pos != std::string::npos);
    if (pos != std::string::npos) old.erase(pos, line.size());
    std::vector<std::string> w;
    Config c = load_config(old, &w);
    CHECK_EQ(w.size(), 0);
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::OpenSettings)], L"None");
    CHECK_EQ(resolve_bindings(c.hotkeys).active.size(), 6);
    c.dots.center_dot = false;
    const std::string saved = save_config_text(c, tmpl);
    CHECK(has(saved, "\r\ntoggle_edge_dots=None\r\n" + line + "; 把当前程序视为游戏\r\n"));
}

// ================================================================ [calibration] 日期

TEST(config_calibration_dates_read_write) {
    const std::string tmpl = template_text();
    std::vector<std::string> w;
    const Config c = load_config("[calibration]\nCS2.exe=18.4\nvalorant.exe=35.714;2026-11-01\napex.exe=15.0;昨天\n", &w);
    CHECK_EQ(w.size(), 0);   // 旧格式与损坏的日期都不记 warn
    const std::map<std::wstring, double> want{{L"apex.exe", 15.0}, {L"cs2.exe", 18.4}, {L"valorant.exe", 35.714}};
    CHECK_TRUE(c.calibration == want);
    const std::map<std::wstring, std::wstring> dates{{L"apex.exe", L"昨天"}, {L"valorant.exe", L"2026-11-01"}};
    CHECK_TRUE(c.calibration_dates == dates);
    const std::string saved = save_config_text(c, tmpl);
    CHECK(has(saved, "\r\napex.exe=15.000;昨天\r\ncs2.exe=18.400\r\nvalorant.exe=35.714;2026-11-01\r\n\r\n[capture]"));
    // 往返不变
    const Config back = load_config(saved);
    CHECK_TRUE(back == c);
    CHECK_TRUE(save_config_text(back, tmpl) == saved);
    // get 的写法与 INI 相同
    CHECK_EQ(get_or(c, "calibration", "valorant.exe"), L"35.714;2026-11-01");
    CHECK_EQ(get_or(c, "calibration", "CS2.EXE"), L"18.400");
    CHECK_EQ(get_or(c, "calibration", "apex.exe"), L"15.000;昨天");
}

TEST(config_calibration_dates_invalid_and_override) {
    std::vector<std::string> w;
    // 非法值整条丢弃（连同日期）；日期部分不影响数值校验
    const Config c = load_config("[calibration]\nbad.exe=abc;2026-11-01\nhuge.exe=2000;2026-11-01\ntiny.exe=1.0\n", &w);
    CHECK_EQ(w.size(), 3);
    CHECK_TRUE(c.calibration.empty());
    CHECK_TRUE(c.calibration_dates.empty());
    // 同名条目以最后一条为准，连同日期（后一条没有日期 → 日期为"无"）
    const Config d = load_config("[calibration]\ncs2.exe=18.2;2026-10-01\nCS2.exe=20\n");
    CHECK_EQ(d.calibration.size(), 1);
    CHECK_TRUE(d.calibration_dates.empty());
    // 空日期（"18.4;"）与带空白的日期
    const Config e = load_config("[calibration]\na.exe=18.4;\nb.exe=18.4 ; 注释\nc.exe=18.4; 2026-11-02\n");
    CHECK_EQ(e.calibration.size(), 3);
    CHECK_FALSE(e.calibration_dates.count(L"a.exe"));
    CHECK_FALSE(e.calibration_dates.count(L"b.exe"));   // " ; 注释" 是行尾注释
    CHECK_TRUE(e.calibration_dates.count(L"c.exe") && e.calibration_dates.at(L"c.exe") == L"2026-11-02");
}

TEST(config_calibration_set_and_delete) {
    const std::string tmpl = template_text();
    Config c;
    CHECK_TRUE(config_set_value(c, "calibration", "CS2.exe", L"18.4;2026-11-01"));
    CHECK_EQ(get_or(c, "calibration", "cs2.exe"), L"18.400;2026-11-01");
    // 结算写入：值 3 位小数 + 当天日期，已有条目连同旧日期覆盖
    CHECK_TRUE(config_set_value(c, "calibration", "cs2.exe",
                                utf8_to_wide(calib_format_cpd(18.2)) + L";" + calib_format_date(2026, 11, 3)));
    CHECK(has(save_config_text(c, tmpl), "\r\ncs2.exe=18.200;2026-11-03\r\n"));
    // 非法值拒绝、不改
    CHECK_FALSE(config_set_value(c, "calibration", "cs2.exe", L"abc"));
    CHECK_FALSE(config_set_value(c, "calibration", "cs2.exe", L"1.0"));
    CHECK_FALSE(config_set_value(c, "calibration", "", L"18.2"));
    CHECK_FALSE(config_set_value(c, "calibration", "a=b.exe", L"18.2"));
    CHECK_EQ(get_or(c, "calibration", "cs2.exe"), L"18.200;2026-11-03");
    // 空串删除；删除后回落默认值
    CHECK_TRUE(config_set_value(c, "calibration", "cs2.exe", L""));
    CHECK_TRUE(c.calibration.empty());
    CHECK_TRUE(c.calibration_dates.empty());
    CHECK_NEAR(counts_per_degree_for(L"cs2.exe", c.calibration, c.trigger.default_counts_per_degree), 18.2, 0);
    CHECK_TRUE(save_config_text(c, tmpl) == tmpl);
}

// ================================================================ 恢复默认 / 清空已知游戏

TEST(config_restore_defaults_keeps_calibration_and_lists) {
    // configuration「恢复默认设置」场景：保留校准与名单
    Config c;
    c.mask.level = MaskLevel::Strong;
    c.hotkeys[hotkey_index(HotkeyAction::Toggle)] = L"Ctrl+Alt+K";
    c.detect.exclude.push_back(L"tool.exe");
    c.detect.force_game = {L"mygame.exe"};
    c.detect.known_games = {L"newgame.exe"};
    c.calibration[L"cs2.exe"] = 18.4;
    c.calibration_dates[L"cs2.exe"] = L"2026-11-01";
    c.autostart = true;
    c.enabled = false;
    c.log_level = log::Level::Debug;
    c.dots.edge_dot_margin_px = 40;
    c.trigger.capture = false;
    c.capture.grid_w = 200;
    c.ui.show_advanced = true;
    c.ui.x = 10;
    c.extras.emplace_back("mask.glow", "1");

    const Config d = restore_defaults(c);
    const Config def;
    CHECK_TRUE(d.mask.level == MaskLevel::Medium);
    CHECK_EQ(d.hotkeys[hotkey_index(HotkeyAction::Toggle)], L"Ctrl+Alt+F9");
    CHECK_EQ(d.detect.exclude.size(), 22);
    CHECK_TRUE(d.detect.exclude == def.detect.exclude);
    CHECK_TRUE(d.enabled);
    CHECK_TRUE(d.log_level == log::Level::Info);
    CHECK_TRUE(d.dots == def.dots);
    CHECK_TRUE(d.trigger == def.trigger);
    CHECK_TRUE(d.capture == def.capture);
    // 保留项
    CHECK_TRUE(d.detect.force_game == c.detect.force_game);
    CHECK_TRUE(d.detect.known_games == c.detect.known_games);
    CHECK_TRUE(d.calibration == c.calibration);
    CHECK_TRUE(d.calibration_dates == c.calibration_dates);
    CHECK_TRUE(d.autostart);
    CHECK_TRUE(d.ui == c.ui);
    CHECK_TRUE(d.extras == c.extras);
    const std::string saved = save_config_text(d, template_text());
    CHECK(has(saved, "\r\ncs2.exe=18.400;2026-11-01\r\n"));
    CHECK(has(saved, "\r\nforce_game=mygame.exe\r\n"));
    CHECK(has(saved, "\r\nautostart=1\r\n"));
}

TEST(config_clear_known_games_only) {
    Config c;
    c.detect.known_games = {L"newgame.exe", L"other.exe"};
    c.detect.force_game = {L"mygame.exe"};
    clear_known_games(c);
    CHECK_TRUE(c.detect.known_games.empty());
    CHECK_TRUE(c.detect.force_game == std::vector<std::wstring>{L"mygame.exe"});
    CHECK_EQ(c.detect.exclude.size(), 22);
    CHECK(has(save_config_text(c, template_text()), "\r\nknown_games=\r\n"));
}

// ================================================================ 变更 5：[capture] forward_gain / detect_roll / baseline_ms

TEST(config_access_forward_and_roll_keys) {
    const Config def;
    CHECK_EQ(get_or(def, "capture", "forward_gain"), L"1.0");
    CHECK_EQ(get_or(def, "capture", "detect_roll"), L"1");
    CHECK_EQ(get_or(def, "capture", "baseline_ms"), L"66");
    Config c;
    CHECK_TRUE(config_set_value(c, "capture", "forward_gain", L"2.3"));
    CHECK_NEAR(c.trigger.forward_gain, 2.3, 1e-12);
    CHECK_EQ(get_or(c, "capture", "forward_gain"), L"2.3");
    CHECK_TRUE(config_set_value(c, "capture", "forward_gain", L"9"));   // 越界截断
    CHECK_NEAR(c.trigger.forward_gain, 3.0, 0);
    CHECK_EQ(get_or(c, "capture", "forward_gain"), L"3.0");
    CHECK_TRUE(config_set_value(c, "capture", "detect_roll", L"0"));
    CHECK_FALSE(c.capture.detect_roll);
    CHECK_FALSE(config_set_value(c, "capture", "detect_roll", L"yes"));
    CHECK_FALSE(c.capture.detect_roll);
    CHECK_TRUE(config_set_value(c, "capture", "baseline_ms", L"200"));
    CHECK_EQ(c.capture.baseline_ms, 150);
    // 前进 / 倾斜各自的阈值（[capture] zoom_* / roll_*）
    const std::pair<const char*, const wchar_t*> th_defaults[] = {
        {"zoom_start_low", L"40"},    {"zoom_full_low", L"250"},  {"zoom_start_medium", L"20"},
        {"zoom_full_medium", L"150"}, {"zoom_start_high", L"10"}, {"zoom_full_high", L"83"},
        {"roll_start_low", L"30"},    {"roll_full_low", L"150"},  {"roll_start_medium", L"15"},
        {"roll_full_medium", L"90"},  {"roll_start_high", L"8"},  {"roll_full_high", L"50"},
    };
    for (const auto& [key, want] : th_defaults) {
        CHECK_EQ(get_or(def, "capture", key), want);
        const auto r = config_key_range("capture", key);
        CHECK_TRUE(r.has_value());
        if (r) {
            CHECK_NEAR(r->lo, 1, 0);
            CHECK_NEAR(r->hi, 5000, 0);
        }
    }
    CHECK_TRUE(config_set_value(c, "capture", "zoom_start_medium", L"25.5"));
    CHECK_NEAR(c.trigger.zoom_thresholds[1].v_start, 25.5, 1e-12);
    CHECK_EQ(get_or(c, "capture", "zoom_start_medium"), L"25.5");
    CHECK_TRUE(config_set_value(c, "capture", "roll_full_high", L"9000"));   // 越界截断
    CHECK_NEAR(c.trigger.roll_thresholds[2].v_full, 5000, 0);
    CHECK_FALSE(config_set_value(c, "capture", "roll_start_low", L"fast"));
    CHECK_NEAR(c.trigger.roll_thresholds[0].v_start, 30, 0);
    // 恢复默认设置覆盖这些键
    const Config d = restore_defaults(c);
    CHECK_NEAR(d.trigger.forward_gain, 1.0, 0);
    CHECK_TRUE(d.capture.detect_roll);
    CHECK_EQ(d.capture.baseline_ms, 66);
    CHECK_TRUE(d.trigger.zoom_thresholds == Config{}.trigger.zoom_thresholds);
    CHECK_TRUE(d.trigger.roll_thresholds == Config{}.trigger.roll_thresholds);
}
