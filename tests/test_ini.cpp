// test_ini.cpp — util/strings、util/log、config/ini、config/config 的单元测试（configuration spec 的 INI 语法、值类型、校验、按模板保存、文件层）。
#include "test_framework.h"

#include <windows.h>

#include <algorithm>
#include <clocale>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.h"
#include "config/ini.h"
#include "util/log.h"
#include "util/strings.h"
#include "util/win.h"

using namespace mc;

namespace {

// 模板路径：由本文件的 __FILE__（build_tests.cmd 传给 cl 的是绝对路径）推出 ..\src\res\MotionCalm.default.ini。
std::wstring template_path() {
    std::wstring f = utf8_to_wide(__FILE__);
    const auto slash = f.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring(L".") : f.substr(0, slash);
    return dir + L"\\..\\src\\res\\MotionCalm.default.ini";
}

std::string load_template() {
    std::string text;
    const bool ok = read_file(template_path(), &text);
    CHECK_TRUE(ok);   // 找不到模板算失败
    return text;
}

std::string crlf_to_lf(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out += s[i];
    }
    return out;
}

std::string strip_bom(std::string s) {
    if (s.rfind(std::string(ini::kUtf8Bom), 0) == 0) s.erase(0, ini::kUtf8Bom.size());
    return s;
}

bool contains(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

// "key=value" 用空格补到第 col 列后接 comment（模板里行尾注释都对齐在第 31 列）。
std::string padded(std::string_view head, std::size_t col, std::string_view comment) {
    std::string s(head);
    s.append(col > s.size() ? col - s.size() : 1, ' ');
    s += comment;
    return s;
}

// 模板中以 prefix 开头的那一行替换为 replacement（保留 CRLF）。
std::string replace_line(std::string text, std::string_view prefix, std::string_view replacement) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::size_t line_end = end == std::string::npos ? text.size() : end;
        std::size_t body_end = line_end;
        if (body_end > pos && text[body_end - 1] == '\r') --body_end;
        if (std::string_view(text).substr(pos, body_end - pos).rfind(prefix, 0) == 0) {
            text.replace(pos, body_end - pos, replacement);
            return text;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    const bool prefix_found = false;
    CHECK_TRUE(prefix_found);
    return text;
}

std::uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return ~0ull;
    return (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

bool file_exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// 每个测试一个独立的临时目录，析构时删除其中的文件和目录本身。
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        wchar_t base[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH, base);
        path_ = std::wstring(base) + L"MotionCalm-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(++counter) + L"-" + std::to_wstring(GetTickCount64());
        CreateDirectoryW(path_.c_str(), nullptr);
    }
    ~TempDir() {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((path_ + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = fd.cFileName;
                if (name != L"." && name != L"..") DeleteFileW((path_ + L"\\" + name).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path_.c_str());
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::wstring file(std::wstring_view name) const { return path_ + L"\\" + std::wstring(name); }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

// 文本中的节标题（小写节名，按出现顺序；包括没有任何条目的节）。
std::vector<std::string> section_headers(std::string_view text) {
    std::vector<std::string> out;
    for (const std::string_view line : split(text, '\n')) {
        const std::string_view t = trim(line);
        if (t.empty() || t[0] != '[') continue;
        const std::size_t close = t.find(']');
        if (close != std::string_view::npos) out.push_back(to_lower_ascii(trim(t.substr(1, close - 1))));
    }
    return out;
}

// 把若干行插到模板 [calibration] 注释块之后、[capture] 之前的空行前面（= save_config_text 写校准条目的位置）。
std::string with_calibration_lines(const std::string& tmpl, std::string_view lines) {
    const std::size_t pos = tmpl.find("\r\n\r\n[capture]");
    CHECK(pos != std::string::npos);
    if (pos == std::string::npos) return tmpl;
    std::string out = tmpl;
    out.insert(pos + 2, lines);
    return out;
}

// 删除以 prefix 开头的那一整行（含 CRLF）；找不到算失败。
std::string remove_line(std::string text, std::string_view prefix) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find("\r\n", pos);
        const std::size_t line_end = end == std::string::npos ? text.size() : end + 2;
        if (std::string_view(text).substr(pos).rfind(prefix, 0) == 0) {
            text.erase(pos, line_end - pos);
            return text;
        }
        if (end == std::string::npos) break;
        pos = end + 2;
    }
    const bool prefix_found = false;
    CHECK_TRUE(prefix_found);
    return text;
}

std::vector<std::string> split_crlf_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(pos, end - pos));
        pos = end + 2;
    }
    return lines;
}

}  // namespace

// ================================================================ strings

TEST(strings_utf8_roundtrip_chinese) {
    const std::string utf8 = "满强度时清晰区直径 / 屏幕高度 \xF0\x9F\x98\x80";   // 含一个 BMP 外字符
    const std::wstring wide = utf8_to_wide(utf8);
    CHECK_EQ(wide, L"满强度时清晰区直径 / 屏幕高度 \U0001F600");
    CHECK_EQ(wide_to_utf8(wide), utf8);
    CHECK_EQ(utf8_to_wide(""), L"");
    CHECK_EQ(utf8_to_wide("\xFF"), L"\xFFFD");          // 非法前导字节
    CHECK_EQ(utf8_to_wide("a\xE4\xB8"), L"a\xFFFD");    // 截断
    CHECK_EQ(utf8_to_wide("\xC0\xAF"), L"\xFFFD");      // 过长编码
    CHECK_EQ(utf8_to_wide("\xED\xA0\x80"), L"\xFFFD");  // 代理区
    CHECK_EQ(wide_to_utf8(std::wstring(1, static_cast<wchar_t>(0xD800))), "\xEF\xBF\xBD");   // 孤立代理项
}

TEST(strings_trim_split) {
    CHECK_EQ(trim("  a b \t\r\n"), "a b");
    CHECK_EQ(trim(L" x "), L"x");
    CHECK_EQ(trim("   "), "");
    const auto parts = split("a;;b", ';');
    CHECK_EQ(parts.size(), 3);
    if (parts.size() == 3) {
        CHECK_EQ(parts[0], "a");
        CHECK_EQ(parts[1], "");
        CHECK_EQ(parts[2], "b");
    }
    CHECK_EQ(split("", ';').size(), 1);
    CHECK_EQ(split(L"x+y", L'+').size(), 2);
    CHECK_EQ(to_lower_ascii("MyGame.EXE"), "mygame.exe");
    CHECK_EQ(to_lower_ascii(L"ÄBC.Exe"), L"Äbc.exe");   // 只转 ASCII
}

TEST(strings_iequals) {
    CHECK_TRUE(iequals("PotPlayerMini64.exe", "potplayermini64.exe"));
    CHECK_TRUE(iequals(L"Ctrl", L"CTRL"));
    CHECK_FALSE(iequals("a", "ab"));
    CHECK_FALSE(iequals("a.exe", "b.exe"));
    CHECK_TRUE(ends_with_ci(L"Game.EXE", L".exe"));
    CHECK_FALSE(ends_with_ci(L"exe", L".exe"));
    CHECK_TRUE(starts_with_ci("VK_OEM_3", "vk_"));
}

TEST(strings_util_win_raii) {
    // util/win.h：UniqueHandle / UniqueHwnd / ScopeExit。
    UniqueHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    CHECK_TRUE(ev.valid());
    UniqueHandle moved(std::move(ev));
    CHECK_FALSE(ev.valid());
    CHECK_TRUE(moved.valid());
    HANDLE raw = moved.release();
    CHECK_FALSE(moved.valid());
    moved.reset(raw);
    CHECK_TRUE(moved.valid());
    moved.reset();
    CHECK_FALSE(moved.valid());
    CHECK_FALSE(UniqueHandle(INVALID_HANDLE_VALUE).valid());

    HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    CHECK_TRUE(w != nullptr);
    {
        UniqueHwnd owned(w);
        CHECK_TRUE(IsWindow(w));
    }
    CHECK_FALSE(IsWindow(w));

    int runs = 0;
    {
        auto guard = ScopeExit([&] { ++runs; });
    }
    {
        auto guard = ScopeExit([&] { ++runs; });
        guard.dismiss();
    }
    CHECK_EQ(runs, 1);
    ComPtr<IUnknown> none;
    CHECK_TRUE(none == nullptr);
}

// ================================================================ log

TEST(log_line_format) {
    CHECK_EQ(log::format_line(2026, 1, 2, 3, 4, 5, 6, log::Level::Info, "hello"), "2026-01-02 03:04:05.006 [info] hello");
    CHECK_EQ(log::format_line(2026, 12, 31, 23, 59, 59, 999, log::Level::Warn, "x"),
             "2026-12-31 23:59:59.999 [warn] x");
    CHECK_TRUE(log::parse_level("DEBUG") == log::Level::Debug);
    CHECK_FALSE(log::parse_level("verbose").has_value());

    TempDir dir;
    const std::wstring path = dir.file(L"MotionCalm.log");
    CHECK_TRUE(log::init(path, log::Level::Debug));
    log::debug("调试 {}", 1);
    log::info("value={:.3f}", 0.5);
    log::warn(L"宽字符 {}", std::wstring(L"cs2.exe"));
    log::set_level(log::Level::Info);
    log::debug("这一行不该出现");
    CHECK_FALSE(log::enabled(log::Level::Debug));

    // 运行中可以被其他程序打开阅读（共享读写删除）。
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_TRUE(reader != INVALID_HANDLE_VALUE);
    if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    log::info("after reader");
    log::shutdown();
    log::set_level(log::Level::Info);

    std::string text;
    CHECK_TRUE(read_file(path, &text));
    const auto lines = split_crlf_lines(text);
    CHECK_EQ(lines.size(), 4);
    const std::regex re(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \[(debug|info|warn)\] .*$)");
    for (const auto& l : lines) CHECK(std::regex_match(l, re));
    if (lines.size() == 4) {
        CHECK(contains(lines[0], "[debug] 调试 1"));
        CHECK(contains(lines[1], "[info] value=0.500"));
        CHECK(contains(lines[2], "[warn] 宽字符 cs2.exe"));
        CHECK(contains(lines[3], "[info] after reader"));
    }
}

TEST(log_rotates_at_1mb) {
    TempDir dir;
    const std::wstring path = dir.file(L"MotionCalm.log");
    const std::wstring old_path = path + L".1";
    CHECK_TRUE(save_file_atomic(old_path, "stale previous rotation\r\n"));   // 旧的 .1 应被覆盖

    CHECK_TRUE(log::init(path, log::Level::Info));
    log::write(log::Level::Info, std::string(1100 * 1024, 'x'));   // 写入 1.1 MB
    log::info("first line after rotation");                         // 写之前发现 ≥ 1 MB → 轮转
    log::shutdown();

    CHECK_TRUE(file_exists(old_path));
    CHECK(file_size(old_path) >= 1100ull * 1024ull);
    CHECK(file_size(path) < 1024ull);
    std::string fresh;
    CHECK_TRUE(read_file(path, &fresh));
    CHECK(contains(fresh, "first line after rotation"));
    CHECK_FALSE(file_exists(path + L".2"));
}

// ================================================================ ini 文本层

TEST(ini_parse_entries_and_line_numbers) {
    const auto entries = ini::parse_ini("a=1\n[S]   ; note\n  k = v ; c\nnot a pair\n=x\n[broken\nz=2\n");
    CHECK_EQ(entries.size(), 3);
    if (entries.size() == 3) {
        CHECK_EQ(entries[0].section, "");
        CHECK_EQ(entries[0].key, "a");
        CHECK_EQ(entries[0].line, 1);
        CHECK_EQ(entries[1].section, "s");
        CHECK_EQ(entries[1].section_text, "S");
        CHECK_EQ(entries[1].key, "k");
        CHECK_EQ(entries[1].value, "v ; c");   // 文本层不剥离行尾注释
        CHECK_EQ(entries[1].line, 3);
        CHECK_EQ(entries[2].section, "s");      // "[broken" 不是合法节标题，被忽略
        CHECK_EQ(entries[2].key, "z");
    }
    CHECK_EQ(ini::strip_inline_comment("strong              ; light | medium | strong"), "strong");
    CHECK_EQ(ini::strip_inline_comment("a;b"), "a;b");
    CHECK_EQ(ini::strip_inline_comment("x\t# y"), "x");
}

TEST(ini_template_matches_defaults) {
    const std::string tmpl = load_template();
    CHECK(tmpl.rfind(std::string(ini::kUtf8Bom), 0) == 0);   // UTF-8 BOM
    CHECK_FALSE(contains(crlf_to_lf(tmpl), "\r"));            // 只有 CRLF 换行
    CHECK_EQ(crlf_to_lf(tmpl).size() + static_cast<std::size_t>(std::count(tmpl.begin(), tmpl.end(), '\n')),
             tmpl.size());

    std::vector<std::string> warnings;
    const Config cfg = load_config(tmpl, &warnings);
    CHECK_EQ(warnings.size(), 0);
    CHECK_TRUE(cfg == Config{});
    CHECK_TRUE(cfg.extras.empty());

    // 节顺序：general、mask、dots、trigger、detect、hotkeys、calibration、capture、ui，没有 [extra]；[calibration] 只有注释。
    const std::vector<std::string> expected{"general", "mask",        "dots",    "trigger", "detect",
                                            "hotkeys", "calibration", "capture", "ui"};
    CHECK_TRUE(section_headers(tmpl) == expected);
    std::vector<std::string> sections_with_keys;
    for (const auto& e : ini::parse_ini(tmpl)) {
        if (sections_with_keys.empty() || sections_with_keys.back() != e.section) sections_with_keys.push_back(e.section);
    }
    const std::vector<std::string> keyed{"general", "mask", "dots", "trigger", "detect", "hotkeys", "capture", "ui"};
    CHECK_TRUE(sections_with_keys == keyed);   // [calibration] 没有条目
    CHECK_TRUE(cfg.calibration.empty());

    // [hotkeys] 的 12 个键与默认值（tray-and-hotkeys 动作表顺序）。
    std::vector<std::string> hotkey_lines;
    for (const auto& e : ini::parse_ini(tmpl)) {
        if (e.section == "hotkeys") hotkey_lines.push_back(e.key + "=" + e.value);
    }
    const std::vector<std::string> want_hotkeys{
        "toggle=Ctrl+Alt+F9",       "cycle_level=Ctrl+Alt+F10", "toggle_center_dot=Ctrl+Alt+F11",
        "debug_bar=Ctrl+Alt+F8",    "calibrate=Ctrl+Alt+F7",    "toggle_capture=Ctrl+Alt+F6",
        "cycle_sensitivity=None",   "toggle_edge_dots=None",    "open_settings=None",
        "mark_game=None",           "exclude_app=None",         "exit=None"};
    CHECK_TRUE(hotkey_lines == want_hotkeys);

    // [trigger] 中 capture / capture_sensitivity 紧跟在 gamepad 之后；[capture] 的九个键（configuration「[capture] 节」，含变更 5 的三个）。
    std::vector<std::string> trigger_keys, capture_lines;
    for (const auto& e : ini::parse_ini(tmpl)) {
        if (e.section == "trigger") trigger_keys.push_back(e.key);
        if (e.section == "capture") capture_lines.push_back(e.key + "=" + e.value);
    }
    CHECK(trigger_keys.size() >= 5);
    if (trigger_keys.size() >= 5) {
        CHECK_EQ(trigger_keys[2], "gamepad");
        CHECK_EQ(trigger_keys[3], "capture");
        CHECK_EQ(trigger_keys[4], "capture_sensitivity");
    }
    const std::vector<std::string> want_capture{"grid_w=160",           "grid_h=90",
                                                "max_shift=20",         "min_correlation=0.60",
                                                "assumed_hfov_deg=90",  "hold_ms=100",
                                                "forward_gain=1.0",     "detect_roll=1",   // 变更 5
                                                "baseline_ms=66",
                                                "zoom_start_low=40",    "zoom_full_low=250",
                                                "zoom_start_medium=20", "zoom_full_medium=150",
                                                "zoom_start_high=10",   "zoom_full_high=83",
                                                "roll_start_low=30",    "roll_full_low=150",
                                                "roll_start_medium=15", "roll_full_medium=90",
                                                "roll_start_high=8",    "roll_full_high=50"};
    CHECK_TRUE(capture_lines == want_capture);
    CHECK(contains(tmpl, "\r\ncapture=1                      ; 截屏运动检测 0 | 1\r\n"));
    CHECK(contains(tmpl, "\r\ncapture_sensitivity=medium     ; low | medium | high\r\n"));
    // 每个 [capture] 键前都有一行中文注释
    const std::vector<std::string> lines = split_crlf_lines(tmpl);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        for (const char* key : {"grid_w=", "grid_h=", "max_shift=", "min_correlation=", "assumed_hfov_deg=", "hold_ms=",
                                "capture=", "capture_sensitivity=", "toggle_capture=", "forward_gain=", "detect_roll=",
                                "baseline_ms=", "zoom_start_", "zoom_full_", "roll_start_", "roll_full_"}) {
            if (lines[i].rfind(key, 0) == 0) CHECK(i > 0 && lines[i - 1].rfind("; ", 0) == 0);
        }
    }

    // 默认值抽查（configuration spec 各节的"默认值"场景）。
    CHECK_TRUE(cfg.enabled);
    CHECK_TRUE(cfg.mask.level == MaskLevel::Medium);
    CHECK_TRUE(cfg.trigger.sensitivity == Sensitivity::Medium);
    CHECK_FALSE(cfg.autostart);
    CHECK_TRUE(cfg.hide_from_capture == HideFromCapture::Auto);
    CHECK_TRUE(cfg.log_level == log::Level::Info);
    CHECK_NEAR(cfg.trigger.thresholds[0].v_start, 120, 0);
    CHECK_NEAR(cfg.trigger.thresholds[0].v_full, 600, 0);
    CHECK_NEAR(cfg.trigger.thresholds[1].v_start, 60, 0);
    CHECK_NEAR(cfg.trigger.thresholds[1].v_full, 360, 0);
    CHECK_NEAR(cfg.trigger.thresholds[2].v_start, 30, 0);
    CHECK_NEAR(cfg.trigger.thresholds[2].v_full, 200, 0);
    CHECK_NEAR(cfg.trigger.default_counts_per_degree, 18.2, 0);
    CHECK_TRUE(cfg.dots.center_dot && cfg.dots.center_dot_outline && cfg.dots.edge_dots);
    CHECK_EQ(cfg.dots.center_dot_radius_px, 3);
    CHECK_TRUE(cfg.dots.center_dot_color == (Rgb{255, 255, 255}));
    CHECK_TRUE(cfg.dots.edge_dot_color == (Rgb{0xC8, 0xC8, 0xC8}));
    CHECK_NEAR(cfg.dots.edge_dot_min_alpha, 0.15, 0);
    CHECK_NEAR(cfg.dots.edge_dot_max_alpha, 0.75, 0);
    CHECK_EQ(cfg.dots.edge_dot_margin_px, 24);
    CHECK_EQ(cfg.detect.exclude.size(), 22);
    if (!cfg.detect.exclude.empty()) {
        CHECK_EQ(cfg.detect.exclude.front(), L"chrome.exe");
        CHECK_EQ(cfg.detect.exclude.back(), L"obs64.exe");
    }
    CHECK_TRUE(cfg.detect.force_game.empty() && cfg.detect.known_games.empty());
    CHECK_EQ(cfg.hotkeys[0], L"Ctrl+Alt+F9");
    CHECK_EQ(cfg.hotkeys[3], L"Ctrl+Alt+F8");
    CHECK_EQ(cfg.hotkeys[4], L"Ctrl+Alt+F7");
    CHECK_EQ(cfg.hotkeys[hotkey_index(HotkeyAction::Calibrate)], L"Ctrl+Alt+F7");
    CHECK_EQ(cfg.hotkeys[5], L"Ctrl+Alt+F6");
    CHECK_EQ(cfg.hotkeys[hotkey_index(HotkeyAction::ToggleCapture)], L"Ctrl+Alt+F6");
    CHECK_EQ(cfg.hotkeys[10], L"None");
    // 截屏（configuration「[trigger] 节」「[capture] 节」的默认值场景）
    CHECK_TRUE(cfg.trigger.capture);
    CHECK_TRUE(cfg.trigger.capture_sensitivity == Sensitivity::Medium);
    CHECK_EQ(cfg.capture.grid_w, 160);
    CHECK_EQ(cfg.capture.grid_h, 90);
    CHECK_EQ(cfg.capture.max_shift, 20);
    CHECK_NEAR(cfg.capture.min_correlation, 0.60, 0);
    CHECK_NEAR(cfg.capture.assumed_hfov_deg, 90, 0);
    CHECK_EQ(cfg.capture.hold_ms, 100);
    CHECK_TRUE(cfg.capture == CaptureParams{});   // 与截屏服务的默认参数一致
}

TEST(ini_save_defaults_is_identity) {
    const std::string tmpl = load_template();
    const std::string saved = save_config_text(Config{}, tmpl);
    CHECK_EQ(saved.size(), tmpl.size());
    CHECK_TRUE(saved == tmpl);
}

TEST(ini_inline_comment_scalar) {
    std::vector<std::string> w;
    const Config c = load_config(
        "[general]\nmask_level=strong              ; light | medium | strong\n[mask]\nfeather=0.5 # note\n", &w);
    CHECK_TRUE(c.mask.level == MaskLevel::Strong);
    CHECK_NEAR(c.mask.feather, 0.5, 0);
    CHECK_EQ(w.size(), 0);
}

TEST(ini_list_ignores_comment_chars) {
    const Config c = load_config("[detect]\nexclude=chrome.exe;msedge.exe; my game.exe ;;\nknown_games=a.exe ; # b.exe\n");
    const std::vector<std::wstring> expected{L"chrome.exe", L"msedge.exe", L"my game.exe"};
    CHECK_TRUE(c.detect.exclude == expected);
    const std::vector<std::wstring> known{L"a.exe", L"# b.exe"};   // 列表键不识别行尾注释
    CHECK_TRUE(c.detect.known_games == known);
}

TEST(ini_bom_and_lf_equivalent) {
    const std::string tmpl = load_template();
    const std::string modified =
        replace_line(replace_line(tmpl, "mask_level=", "mask_level=light"), "known_games=", "known_games=x.exe;y.exe");
    const std::string plain_lf = crlf_to_lf(strip_bom(modified));
    CHECK_FALSE(plain_lf == modified);

    const auto a = ini::parse_ini(modified);
    const auto b = ini::parse_ini(plain_lf);
    CHECK_EQ(a.size(), b.size());
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
        same = a[i].section == b[i].section && a[i].key == b[i].key && a[i].value == b[i].value && a[i].line == b[i].line;
    }
    CHECK_TRUE(same);
    CHECK_TRUE(load_config(modified) == load_config(plain_lf));
    CHECK_TRUE(load_config(plain_lf).mask.level == MaskLevel::Light);
}

TEST(ini_section_comment_and_key_case) {
    std::vector<std::string> w;
    const Config c = load_config(
        "; 满强度时清晰区直径 / 屏幕高度\n[Hotkeys]   ; 格式说明\nTOGGLE=Shift+F2\n[GENERAL]\nMask_Level=Light\n", &w);
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Toggle)], L"Shift+F2");
    CHECK_TRUE(c.mask.level == MaskLevel::Light);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(c.extras.empty());

    const Config d = load_config("[Hotkeys]   ; 格式说明\nTOGGLE=Ctrl+Alt+F9\n");
    CHECK_EQ(d.hotkeys[0], L"Ctrl+Alt+F9");
    CHECK_EQ(ini::parse_ini("; 满强度时清晰区直径 / 屏幕高度\n# 另一种注释\n").size(), 0);
}

TEST(ini_duplicate_key_last_wins) {
    const Config c = load_config("[mask]\nfeather=0.5\nfeather=0.6\n[general]\nmask_level=light\nmask_level=strong\n");
    CHECK_NEAR(c.mask.feather, 0.6, 0);
    CHECK_TRUE(c.mask.level == MaskLevel::Strong);
}

TEST(ini_list_dedup_case_insensitive) {
    const Config c = load_config("[detect]\nforce_game=Game.exe;game.exe;GAME.EXE;other.exe\n");
    const std::vector<std::wstring> expected{L"Game.exe", L"other.exe"};
    CHECK_TRUE(c.detect.force_game == expected);
}

TEST(ini_list_write_back) {
    const std::string tmpl = load_template();
    Config c;
    c.detect.known_games.push_back(L"a.exe");
    c.detect.known_games.push_back(L"b.exe");
    const std::string saved = save_config_text(c, tmpl);
    CHECK(contains(saved, "\r\nknown_games=a.exe;b.exe\r\n"));
    CHECK_TRUE(load_config(saved).detect.known_games == c.detect.known_games);
}

TEST(ini_locale_independent_numbers) {
    const char* set = std::setlocale(LC_ALL, "de-DE");
    if (set != nullptr) CHECK_EQ(std::string(std::localeconv()->decimal_point), ",");   // 区域设置确实生效
    const Config c = load_config("[mask]\nfeather=0.35\n");
    const std::string saved = save_config_text(c, load_template());
    std::setlocale(LC_ALL, "C");
    CHECK_NEAR(c.mask.feather, 0.35, 0);
    CHECK(contains(saved, "\r\nfeather=0.35\r\n"));
    CHECK_EQ(format_ini_number(0.35, 2), "0.35");
}

TEST(ini_color_parse_and_upper) {
    std::vector<std::string> w;
    Config c = load_config("[dots]\nedge_dot_color=c8c8c8\ncenter_dot_color=#FFFFFF\n", &w);
    CHECK_TRUE(c.dots.edge_dot_color == (Rgb{200, 200, 200}));
    CHECK_TRUE(c.dots.center_dot_color == (Rgb{255, 255, 255}));   // 非法 → 默认
    CHECK_EQ(w.size(), 1);
    c.mask.color = Rgb{0xAB, 0xCD, 0xEF};
    c.dots.edge_dot_color = Rgb{0x0a, 0x0b, 0x0c};
    const std::string saved = save_config_text(c, load_template());
    CHECK(contains(saved, "\r\ncolor=ABCDEF\r\n"));
    CHECK(contains(saved, "\r\nedge_dot_color=0A0B0C\r\n"));
}

TEST(ini_bool_only_01) {
    std::vector<std::string> w;
    const Config c = load_config("[general]\nenabled=yes\nautostart=true\n[dots]\ncenter_dot=0\n", &w);
    CHECK_TRUE(c.enabled);      // 默认值 1
    CHECK_FALSE(c.autostart);   // 默认值 0
    CHECK_FALSE(c.dots.center_dot);
    CHECK_EQ(w.size(), 2);
}

TEST(ini_out_of_range_clamped) {
    std::vector<std::string> w;
    const Config c = load_config("[mask]\nmax_opacity_strong=1.5\n", &w);
    CHECK_NEAR(c.mask.max_opacity[2], 1.0, 0);
    CHECK_EQ(w.size(), 1);

    std::vector<std::string> w2;
    const Config d = load_config(
        "[mask]\nattack_ms=5\nrelease_ms=99999\nclear_height_light=0\n[trigger]\ngamepad_deadzone=0.95\n"
        "velocity_window_ms=49.6\n",
        &w2);
    CHECK_EQ(d.mask.attack_ms, 10);
    CHECK_EQ(d.mask.release_ms, 5000);
    CHECK_NEAR(d.mask.clear_height[0], 0.05, 0);
    CHECK_NEAR(d.trigger.gamepad_deadzone, 0.9, 0);
    CHECK_EQ(d.trigger.velocity_window_ms, 50);   // 整数键四舍五入，不算越界
    CHECK_EQ(w2.size(), 4);
}

TEST(ini_bad_enum_default) {
    std::vector<std::string> w;
    const Config c = load_config("[general]\nmask_level=extreme\nsensitivity=HIGH\nhide_from_capture=Always\n", &w);
    CHECK_TRUE(c.mask.level == MaskLevel::Medium);
    CHECK_TRUE(c.trigger.sensitivity == Sensitivity::High);   // 读取时不区分大小写
    CHECK_TRUE(c.hide_from_capture == HideFromCapture::Always);
    CHECK_EQ(w.size(), 1);
    // 保存时写小写。
    const std::string saved = save_config_text(c, load_template());
    CHECK(contains(saved, "\r\n" + padded("sensitivity=high", 31, "; low | medium | high") + "\r\n"));
}

TEST(ini_bad_number_default) {
    std::vector<std::string> w;
    const Config c = load_config("[mask]\nattack_ms=abc\nfeather=0,35\nellipse_ratio=\n[trigger]\nmove_intensity=nan\n", &w);
    CHECK_EQ(c.mask.attack_ms, 100);
    CHECK_NEAR(c.mask.feather, 0.35, 0);
    CHECK_NEAR(c.mask.ellipse_ratio, 1.25, 0);
    CHECK_NEAR(c.trigger.move_intensity, 0.35, 0);
    CHECK_EQ(w.size(), 4);
}

TEST(ini_threshold_inverted_resets_level) {
    std::vector<std::string> w;
    const Config c = load_config("[trigger]\nv_start_high=300\nv_full_high=200\nv_start_low=100\nv_full_low=700\n", &w);
    CHECK_NEAR(c.trigger.thresholds[2].v_start, 30, 0);
    CHECK_NEAR(c.trigger.thresholds[2].v_full, 200, 0);
    CHECK_NEAR(c.trigger.thresholds[0].v_start, 100, 0);
    CHECK_NEAR(c.trigger.thresholds[0].v_full, 700, 0);
    CHECK_EQ(w.size(), 1);

    const Config d = load_config("[trigger]\nv_start_medium=360\n");   // 相等也算颠倒
    CHECK_NEAR(d.trigger.thresholds[1].v_start, 60, 0);
    CHECK_NEAR(d.trigger.thresholds[1].v_full, 360, 0);
}

TEST(ini_unknown_key_to_extra) {
    const std::string tmpl = load_template();
    const std::string user = replace_line(tmpl, "[mask]", "[mask]\r\nglow=1");
    const Config c = load_config(user);
    CHECK_EQ(c.extras.size(), 1);
    if (c.extras.size() == 1) {
        CHECK_EQ(c.extras[0].first, "mask.glow");
        CHECK_EQ(c.extras[0].second, "1");
    }
    const std::string saved = save_config_text(c, tmpl);
    CHECK_FALSE(contains(saved, "\r\nglow=1"));
    const std::string tail = "\r\n[extra]\r\nmask.glow=1\r\n";
    CHECK(saved.size() > tail.size() && saved.compare(saved.size() - tail.size(), tail.size(), tail) == 0);
    CHECK(saved.compare(0, tmpl.size(), tmpl) == 0);   // 前面与模板完全相同
    // 再读再存一次，逐字节相同。
    const std::string again = save_config_text(load_config(saved), tmpl);
    CHECK_TRUE(again == saved);
}

TEST(ini_unknown_section_to_extra) {
    const std::string tmpl = load_template();
    const Config c = load_config(tmpl + "\r\n[foo]\r\nbar=1\r\n[hotkeys]\r\nfrobnicate=Ctrl+Alt+F3\r\n");
    CHECK_EQ(c.extras.size(), 2);
    const std::string saved = save_config_text(c, tmpl);
    CHECK_FALSE(contains(saved, "[foo]"));
    CHECK(contains(saved, "\r\n[extra]\r\nfoo.bar=1\r\nhotkeys.frobnicate=Ctrl+Alt+F3\r\n"));
    CHECK_TRUE(save_config_text(load_config(saved), tmpl) == saved);
    // [extra] 在文件末尾（最后一节 [ui] 之后，中间一个空行）
    const std::vector<std::string> want{"general", "mask",        "dots",    "trigger", "detect",
                                        "hotkeys", "calibration", "capture", "ui",      "extra"};
    CHECK(contains(saved, "\r\ny=\r\n\r\n[extra]\r\n"));
    CHECK_TRUE(section_headers(saved) == want);
}

TEST(ini_value_replaced_keeps_comment_column) {
    const std::string tmpl = load_template();
    Config c;
    c.mask.level = MaskLevel::Strong;
    c.hide_from_capture = HideFromCapture::Always;
    const std::string saved = save_config_text(c, tmpl);
    CHECK(contains(saved, "\r\nmask_level=strong              ; light | medium | strong\r\n"));
    CHECK(contains(saved, "\r\n" + padded("hide_from_capture=always", 31, "; auto | always | never") + "\r\n"));
    CHECK_EQ(padded("mask_level=strong", 31, "; light | medium | strong"),
             "mask_level=strong              ; light | medium | strong");   // spec 原文
    CHECK(contains(saved, "\r\n; 遮罩强度档位：light（轻）、medium（中）、strong（强）\r\n"));   // 其余注释原样
    // 新值比原注释列还长时至少留一个空格。
    const std::string out = ini::regenerate(
        "k=v    ; c\n", [](std::string_view, std::string_view key) -> std::optional<std::string> {
            if (key == "k") return std::string("verylongvalue");
            return std::nullopt;
        },
        {});
    CHECK_EQ(out, "k=verylongvalue ; c\n");
}

TEST(ini_hotkey_string_preserved) {
    const std::string tmpl = load_template();
    Config c = load_config(replace_line(tmpl, "toggle=", "toggle=alt + ctrl + f9"));
    CHECK_EQ(c.hotkeys[0], L"alt + ctrl + f9");
    c.hotkeys[hotkey_index(HotkeyAction::Exit)] = L"Ctrl+Alt+F99";   // 非法串也原样写回
    c.mask.level = MaskLevel::Light;                                  // "因为其他改动保存"
    const std::string saved = save_config_text(c, tmpl);
    CHECK(contains(saved, "\r\ntoggle=alt + ctrl + f9\r\n"));
    CHECK(contains(saved, "\r\nexit=Ctrl+Alt+F99\r\n"));
}

TEST(ini_min_decimals) {
    const std::string tmpl = load_template();
    Config c;
    c.mask.max_opacity[0] = 0.6;
    c.trigger.default_counts_per_degree = 18.25;
    c.trigger.gamepad_max_deg_per_s = 250.5;
    const std::string saved = save_config_text(c, tmpl);
    CHECK(contains(saved, "\r\nmax_opacity_light=0.60\r\n"));
    CHECK(contains(saved, "\r\ndefault_counts_per_degree=18.25\r\n"));
    CHECK(contains(saved, "\r\ngamepad_max_deg_per_s=250.5\r\n"));
    CHECK_EQ(format_ini_number(300, 0), "300");
    CHECK_EQ(format_ini_number(0.1, 2), "0.10");
    CHECK_EQ(format_ini_number(1e-7, 2), "0.0000001");
    CHECK_EQ(format_ini_number(-0.0, 2), "0.00");
}

TEST(ini_regenerate_dynamic_section) {
    ini::DynamicSection cal;
    cal.section = "calibration";
    cal.entries = {{"b.exe", "2"}, {"a.exe", "1"}};
    const std::string out = ini::regenerate("[calibration]\n; c\n\n[capture]\nx=1\n", nullptr, {}, {cal});
    CHECK_EQ(out, "[calibration]\n; c\na.exe=1\nb.exe=2\n\n[capture]\nx=1\n");
    const std::string at_end = ini::regenerate("[calibration]", nullptr, {}, {cal});
    CHECK_EQ(at_end, "[calibration]\r\na.exe=1\r\nb.exe=2\r\n");
}

// ================================================================ [calibration]（变更 2）

TEST(ini_calibration_read_lowercase) {
    std::vector<std::string> w;
    const Config c = load_config("[calibration]\nCS2.exe=18.4\nvalorant.exe=35.714\n", &w);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(c.extras.empty());
    CHECK_EQ(c.calibration.size(), 2);
    const std::map<std::wstring, double> want{{L"cs2.exe", 18.4}, {L"valorant.exe", 35.714}};
    CHECK_TRUE(c.calibration == want);
    // 同名条目以最后一条为准（大小写不同也算同名）；行尾注释被剥离
    const Config d = load_config("[Calibration]   ; note\ncs2.exe=19\nCS2.EXE=20.5 ; 手动改过\n", &w);
    CHECK_EQ(w.size(), 0);
    CHECK_EQ(d.calibration.size(), 1);
    if (d.calibration.size() == 1) {
        CHECK_EQ(d.calibration.begin()->first, L"cs2.exe");
        CHECK_NEAR(d.calibration.begin()->second, 20.5, 0);
    }
}

TEST(ini_calibration_write_sorted_3dp) {
    const std::string tmpl = load_template();
    Config c;
    c.calibration[L"valorant.exe"] = 35.714;
    c.calibration[L"cs2.exe"] = 18.4;
    const std::string saved = save_config_text(c, tmpl);
    // 条目紧跟在 [calibration] 的注释块之后（[capture] 之前），按 exe 字母序，固定 3 位小数
    CHECK_TRUE(saved == with_calibration_lines(tmpl, "cs2.exe=18.400\r\nvalorant.exe=35.714\r\n"));
    const std::vector<std::string> want{"general", "mask",    "dots",        "trigger",
                                        "detect",  "hotkeys", "calibration", "capture", "ui"};
    CHECK_TRUE(section_headers(saved) == want);
    // 往返：读回相同的表，再存逐字节相同
    const Config back = load_config(saved);
    CHECK_TRUE(back.calibration == c.calibration);
    CHECK_TRUE(back == c);
    CHECK_TRUE(save_config_text(back, tmpl) == saved);
    // 覆盖旧值：只剩一条
    Config d = back;
    d.calibration[L"cs2.exe"] = 20.0;
    const std::string again = save_config_text(d, tmpl);
    CHECK(contains(again, "\r\ncs2.exe=20.000\r\nvalorant.exe=35.714\r\n"));
    CHECK_FALSE(contains(again, "\r\ncs2.exe=18.400\r\n"));   // 模板注释里的示例 "例如 cs2.exe=18.400；" 不算
    // 删除后回到模板
    d.calibration.clear();
    CHECK_TRUE(save_config_text(d, tmpl) == tmpl);
}

TEST(ini_calibration_invalid_dropped) {
    const std::string tmpl = load_template();
    std::vector<std::string> w;
    // 紧接在 [calibration] 注释块后面的行属于该节
    const Config c =
        load_config(with_calibration_lines(tmpl, "bad.exe=abc\r\ntiny.exe=1.0\r\nhuge.exe=2000\r\nok.exe=18.2\r\n"), &w);
    CHECK_EQ(w.size(), 3);
    for (const auto& msg : w) CHECK(contains(msg, "[calibration]"));
    if (w.size() == 3) {
        CHECK(contains(w[0], "bad.exe=abc"));
        CHECK(contains(w[1], "tiny.exe=1.0"));
        CHECK(contains(w[2], "huge.exe=2000"));
    }
    CHECK_EQ(c.calibration.size(), 1);
    CHECK_TRUE(c.extras.empty());
    const std::string saved = save_config_text(c, tmpl);
    CHECK_FALSE(contains(saved, "bad.exe"));
    CHECK_FALSE(contains(saved, "tiny.exe"));
    CHECK_FALSE(contains(saved, "huge.exe"));
    CHECK_TRUE(saved == with_calibration_lines(tmpl, "ok.exe=18.200\r\n"));
    // 下界附近：800 / 360 = 2.2222… 合法，写成 2.223（读回仍合法）；1111.111 × 360 = 399999.96 合法
    std::vector<std::string> w2;
    const Config e = load_config("[calibration]\nlo.exe=2.2222222222222223\nhi.exe=1111.111\nx.exe=-5\n", &w2);
    CHECK_EQ(e.calibration.size(), 2);
    CHECK_EQ(w2.size(), 1);
    const std::string s2 = save_config_text(e, tmpl);
    CHECK(contains(s2, "\r\nhi.exe=1111.111\r\nlo.exe=2.223\r\n"));
    CHECK_EQ(load_config(s2).calibration.size(), 2);
}

TEST(ini_calibration_not_extra) {
    const std::string tmpl = load_template();
    const std::string user = with_calibration_lines(replace_line(tmpl, "[mask]", "[mask]\r\nglow=1"), "cs2.exe=18.4\r\n");
    const Config c = load_config(user);
    CHECK_EQ(c.calibration.size(), 1);
    CHECK_EQ(c.extras.size(), 1);   // 只有 mask.glow
    for (const auto& [k, v] : c.extras) CHECK_FALSE(contains(k, "calibration"));
    const std::string saved = save_config_text(c, tmpl);
    // 条目在 [calibration] 注释之后、[capture] 之前；[extra] 仍在文件末尾
    CHECK(contains(saved, "\r\ncs2.exe=18.400\r\n\r\n[capture]"));
    const std::string tail = "\r\ny=\r\n\r\n[extra]\r\nmask.glow=1\r\n";
    CHECK(saved.size() > tail.size() && saved.compare(saved.size() - tail.size(), tail.size(), tail) == 0);
    CHECK_TRUE(save_config_text(load_config(saved), tmpl) == saved);
}

namespace {

// core-overlay 版本的模板（没有 calibrate 键、没有 [calibration] 节），原样嵌入；换行统一成 CRLF 并加 BOM。
std::string core_overlay_template() {
    static const char kText[] = R"MCINI(; MotionCalm 配置文件（UTF-8 编码）。可以用记事本修改，保存后约 0.5 秒自动生效。
; 程序保存设置时会按这份模板重写整个文件：这些说明会保留，你自己加的注释不会保留。
; 规则：布尔值只认 0 / 1；颜色写 RRGGBB（6 位十六进制，不带 #）；小数一律用英文句点。
; 名单类的值（exclude、force_game、known_games）用 ; 分隔，名单行的末尾不要写注释。
; 写错的值会换成默认值，超出范围的值会被截断，原因记在 MotionCalm.log 里。

[general]
; 是否启用遮罩：1 = 启用，0 = 关闭
enabled=1
; 遮罩强度档位：light（轻）、medium（中）、strong（强）
mask_level=medium              ; light | medium | strong
; 触发灵敏度：low（低）、medium（中）、high（高）
sensitivity=medium             ; low | medium | high
; 开机自动启动：1 = 是，0 = 否
autostart=0
; 遮罩是否对截屏 / 录屏 / 屏幕共享隐藏：auto（随截屏运动检测）、always（总是隐藏）、never（从不隐藏）
hide_from_capture=auto         ; auto | always | never
; 日志级别：debug、info、warn
log_level=info                 ; debug | info | warn

[mask]
; 遮罩颜色 RRGGBB，默认纯黑 000000
color=000000
; 轻档满强度时屏幕边缘的最深不透明度，范围 0–1
max_opacity_light=0.60
; 中档满强度时屏幕边缘的最深不透明度，范围 0–1
max_opacity_medium=0.75
; 强档满强度时屏幕边缘的最深不透明度，范围 0–1
max_opacity_strong=0.85
; 满强度时清晰区直径 / 屏幕高度（轻档），范围 0.05–1
clear_height_light=0.65        ; 满强度时清晰区直径 / 屏幕高度
; 满强度时清晰区直径 / 屏幕高度（中档），范围 0.05–1
clear_height_medium=0.60
; 满强度时清晰区直径 / 屏幕高度（强档），范围 0.05–1
clear_height_strong=0.55
; 清晰区椭圆的横向半径 / 纵向半径，范围 0.5–3（大于 1 时横向更宽）
ellipse_ratio=1.25             ; 横向半径 / 纵向半径
; 遮罩边缘的羽化宽度（相对清晰区半径），范围 0.05–1
feather=0.35
; 遮罩出现所需时间（毫秒，整数），范围 10–2000
attack_ms=100
; 遮罩淡出所需时间（毫秒，整数），范围 10–5000
release_ms=500

[dots]
; 中心参照点：1 = 显示，0 = 不显示
center_dot=1
; 中心点半径（100% 缩放下的像素，整数），范围 1–20
center_dot_radius_px=3
; 中心点颜色 RRGGBB
center_dot_color=FFFFFF
; 中心点外圈的深色描边：1 = 有，0 = 无
center_dot_outline=1
; 边缘参照点（四角加四边中点，共 8 个）：1 = 显示，0 = 不显示
edge_dots=1
; 边缘点半径（100% 缩放下的像素，整数），范围 1–20
edge_dot_radius_px=3
; 边缘点颜色 RRGGBB
edge_dot_color=C8C8C8
; 强度为 0 时边缘点的不透明度，范围 0–1
edge_dot_min_alpha=0.15
; 满强度时边缘点的不透明度，范围 0–1
edge_dot_max_alpha=0.75
; 边缘点离屏幕边缘的距离（100% 缩放下的像素，整数），范围 0–400
edge_dot_margin_px=24

[trigger]
; 鼠标转视角时触发遮罩：1 = 是，0 = 否
mouse=1
; 按 WASD / 方向键移动时触发遮罩：1 = 是，0 = 否
keyboard=1
; 手柄操作时触发遮罩：1 = 是，0 = 否
gamepad=1
; 未校准时每转 1 度对应的鼠标计数（约为 800 DPI 加常见灵敏度），范围 1–1000
default_counts_per_degree=18.2
; 低灵敏度：开始出现遮罩的转速（度/秒），范围 1–5000
v_start_low=120
; 低灵敏度：遮罩达到满强度的转速（度/秒），范围 1–5000，必须大于 v_start_low
v_full_low=600
; 中灵敏度：开始出现遮罩的转速（度/秒），范围 1–5000
v_start_medium=60
; 中灵敏度：遮罩达到满强度的转速（度/秒），范围 1–5000，必须大于 v_start_medium
v_full_medium=360
; 高灵敏度：开始出现遮罩的转速（度/秒），范围 1–5000
v_start_high=30
; 高灵敏度：遮罩达到满强度的转速（度/秒），范围 1–5000，必须大于 v_start_high
v_full_high=200
; 只移动、不转视角时的遮罩强度，范围 0–1
move_intensity=0.35
; 手柄右摇杆推满时相当于的转速（度/秒），范围 10–2000
gamepad_max_deg_per_s=300
; 手柄摇杆的径向死区，范围 0–0.9
gamepad_deadzone=0.20
; 判断鼠标被游戏锁住的规则：strict（光标隐藏，并且被限制在窗口内或每帧回中）、hidden（光标隐藏即可）
mouse_lock_rule=strict         ; strict | hidden
; 计算鼠标转速用的时间窗口（毫秒，整数），范围 10–500
velocity_window_ms=50

[detect]
; 排除名单：这些程序永远不触发遮罩（不区分大小写，用 ; 分隔）
exclude=chrome.exe;msedge.exe;firefox.exe;brave.exe;opera.exe;opera_gx.exe;vivaldi.exe;arc.exe;vlc.exe;PotPlayerMini64.exe;PotPlayer64.exe;PotPlayerMini.exe;mpc-hc64.exe;mpc-hc.exe;mpv.exe;wmplayer.exe;Video.UI.exe;Netflix.exe;Spotify.exe;Discord.exe;explorer.exe;obs64.exe
; 强制视为游戏的程序，托盘"把当前程序视为游戏"会加到这里（用 ; 分隔）
force_game=
; 已知游戏：程序第一次锁住鼠标时自动加入（用 ; 分隔，可以清空）
known_games=

[hotkeys]                      ; 格式 Ctrl+Alt+Shift+Win+<Key>，None 表示不绑定
; 可用按键：F1–F24、A–Z、0–9、Space、Tab、Esc、Pause、Insert、Delete、Home、End、PgUp、PgDn、
; Up、Down、Left、Right、Num0–Num9、VK_OEM_1、VK_OEM_PLUS、VK_OEM_COMMA、VK_OEM_MINUS、
; VK_OEM_PERIOD、VK_OEM_2 … VK_OEM_8、VK_OEM_102；不带修饰键的单键会吃掉游戏里的这个键。
; 启用 / 关闭遮罩
toggle=Ctrl+Alt+F9
; 循环遮罩强度（轻 → 中 → 强）
cycle_level=Ctrl+Alt+F10
; 中心参照点开关
toggle_center_dot=Ctrl+Alt+F11
; 显示调试信息
debug_bar=Ctrl+Alt+F8
; 循环触发灵敏度（低 → 中 → 高）
cycle_sensitivity=None
; 边缘参照点开关
toggle_edge_dots=None
; 把当前程序视为游戏
mark_game=None
; 把当前程序加入排除名单
exclude_app=None
; 退出
exit=None
)MCINI";
    std::string out(ini::kUtf8Bom);
    for (const char ch : std::string_view(kText)) {
        if (ch == '\r') continue;
        if (ch == '\n') out += '\r';
        out += ch;
    }
    return out;
}

}  // namespace

TEST(ini_old_ini_gains_calibration_section) {
    const std::string tmpl = load_template();
    const std::string old_tmpl = core_overlay_template();
    CHECK_FALSE(contains(old_tmpl, "calibrat"));
    CHECK(contains(old_tmpl, "\r\nexit=None\r\n"));
    // 旧版程序生成、用户改过几个值的 INI
    std::string old_ini = replace_line(old_tmpl, "mask_level=", "mask_level=strong              ; light | medium | strong");
    old_ini = replace_line(old_ini, "known_games=", "known_games=cs2.exe;fakegame.exe");
    old_ini = replace_line(old_ini, "toggle=", "toggle=Shift+F2");
    old_ini = replace_line(old_ini, "edge_dot_margin_px=", "edge_dot_margin_px=40");

    std::vector<std::string> w;
    Config c = load_config(old_ini, &w);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(c.extras.empty());
    CHECK_TRUE(c.calibration.empty());
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Calibrate)], L"Ctrl+Alt+F7");   // 缺键 → 默认值
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::Toggle)], L"Shift+F2");

    c.dots.center_dot = false;   // "因任一改动保存"
    const std::string saved = save_config_text(c, tmpl);

    // [hotkeys] 之后出现带注释的 [calibration]（再之后是变更 3 的 [capture]）；debug_bar 之后出现 calibrate=Ctrl+Alt+F7
    const std::vector<std::string> want{"general", "mask",    "dots",        "trigger",
                                        "detect",  "hotkeys", "calibration", "capture", "ui"};
    CHECK_TRUE(section_headers(saved) == want);
    CHECK(contains(saved, "\r\ndebug_bar=Ctrl+Alt+F8\r\n; 校准当前游戏（游戏内转一圈 ×2）\r\ncalibrate=Ctrl+Alt+F7\r\n"));
    const std::size_t cal = saved.find("\r\n[calibration]");
    CHECK(cal != std::string::npos && cal > saved.find("\r\nexit=None\r\n"));
    // 其余用户值保持不变
    CHECK(contains(saved, "\r\nmask_level=strong              ; light | medium | strong\r\n"));
    CHECK(contains(saved, "\r\nknown_games=cs2.exe;fakegame.exe\r\n"));
    CHECK(contains(saved, "\r\ntoggle=Shift+F2\r\n"));
    CHECK(contains(saved, "\r\nedge_dot_margin_px=40\r\n"));
    CHECK(contains(saved, "\r\ncenter_dot=0\r\n"));
    // 与"新模板 + 同样的值"逐字节相同，且读回后值不变
    Config expect;
    expect.mask.level = MaskLevel::Strong;
    expect.detect.known_games = {L"cs2.exe", L"fakegame.exe"};
    expect.hotkeys[hotkey_index(HotkeyAction::Toggle)] = L"Shift+F2";
    expect.dots.edge_dot_margin_px = 40;
    expect.dots.center_dot = false;
    CHECK_TRUE(c == expect);
    CHECK_TRUE(saved == save_config_text(expect, tmpl));
    CHECK_TRUE(load_config(saved) == c);
}

// ================================================================ [capture] 与截屏键（变更 3）

namespace {

// in-game-calibration 版本的模板：从当前模板删掉变更 3 加入的键、注释与 [capture] 节。
std::string calibration_version_template() {
    std::string t = load_template();
    t = remove_line(t, "; 截屏运动检测：");
    t = remove_line(t, "capture=1");
    t = remove_line(t, "; 截屏运动检测的灵敏度");
    t = remove_line(t, "capture_sensitivity=");
    t = remove_line(t, "; 截屏运动检测开关");
    t = remove_line(t, "toggle_capture=");
    t = remove_line(t, "; 打开设置窗口");   // 变更 4 的键与 [ui] 节（[ui] 在 [capture] 之后，随下面一起删掉）
    t = remove_line(t, "open_settings=");
    const std::size_t cap = t.find("\r\n\r\n[capture]");
    CHECK(cap != std::string::npos);
    if (cap != std::string::npos) t.erase(cap + 2);
    return t;
}

}  // namespace

TEST(ini_capture_missing_uses_defaults) {
    // 旧文件缺少 [capture]，[trigger] 中也没有 capture / capture_sensitivity：全部默认值、不记 warn。
    std::vector<std::string> w;
    const Config c = load_config("[trigger]\nmouse=1\nkeyboard=1\ngamepad=1\n[detect]\nknown_games=\n", &w);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(c.trigger.capture);
    CHECK_TRUE(c.trigger.capture_sensitivity == Sensitivity::Medium);
    CHECK_TRUE(c.capture == CaptureParams{});
    CHECK_TRUE(c.extras.empty());
}

TEST(ini_capture_values_and_clamping) {
    std::vector<std::string> w;
    const Config c = load_config(
        "[trigger]\ncapture=0\ncapture_sensitivity=HIGH\n"
        "[capture]\ngrid_w=192\ngrid_h=108\nmax_shift=12\nmin_correlation=0.7\nassumed_hfov_deg=103.5\nhold_ms=0\n",
        &w);
    CHECK_EQ(w.size(), 0);
    CHECK_FALSE(c.trigger.capture);
    CHECK_TRUE(c.trigger.capture_sensitivity == Sensitivity::High);
    CHECK_EQ(c.capture.grid_w, 192);
    CHECK_EQ(c.capture.grid_h, 108);
    CHECK_EQ(c.capture.max_shift, 12);
    CHECK_NEAR(c.capture.min_correlation, 0.7, 0);
    CHECK_NEAR(c.capture.assumed_hfov_deg, 103.5, 0);
    CHECK_EQ(c.capture.hold_ms, 0);

    // configuration「截屏配置值的校验」：越界截断（每键一条 warn）
    std::vector<std::string> w2;
    const Config d = load_config("[capture]\nmax_shift=100\nmin_correlation=1.5\n", &w2);
    CHECK_EQ(d.capture.max_shift, 40);
    CHECK_NEAR(d.capture.min_correlation, 0.99, 0);
    CHECK_EQ(w2.size(), 2);
    if (w2.size() == 2) {
        CHECK(contains(w2[0], "[capture] max_shift=100"));
        CHECK(contains(w2[1], "[capture] min_correlation=1.5"));
    }
    std::vector<std::string> w3;
    const Config e = load_config(
        "[capture]\ngrid_w=10\ngrid_h=999\nmax_shift=1\nmin_correlation=0.01\nassumed_hfov_deg=200\nhold_ms=-5\n", &w3);
    CHECK_EQ(w3.size(), 6);
    CHECK_EQ(e.capture.grid_w, 64);
    CHECK_EQ(e.capture.grid_h, 180);
    CHECK_EQ(e.capture.max_shift, 2);
    CHECK_NEAR(e.capture.min_correlation, 0.10, 0);
    CHECK_NEAR(e.capture.assumed_hfov_deg, 170, 0);
    CHECK_EQ(e.capture.hold_ms, 0);
    std::vector<std::string> w3b;
    const Config e2 = load_config("[capture]\ngrid_w=999\nhold_ms=5000\nassumed_hfov_deg=1\n", &w3b);
    CHECK_EQ(w3b.size(), 3);
    CHECK_EQ(e2.capture.grid_w, 320);
    CHECK_EQ(e2.capture.hold_ms, 1000);
    CHECK_NEAR(e2.capture.assumed_hfov_deg, 30, 0);

    // 非法值 → 默认值 + warn
    std::vector<std::string> w4;
    const Config f = load_config(
        "[trigger]\ncapture=yes\ncapture_sensitivity=extreme\n[capture]\ngrid_w=abc\nmin_correlation=x\n", &w4);
    CHECK_EQ(w4.size(), 4);
    CHECK_TRUE(f.trigger.capture);
    CHECK_TRUE(f.trigger.capture_sensitivity == Sensitivity::Medium);
    CHECK_EQ(f.capture.grid_w, 160);
    CHECK_NEAR(f.capture.min_correlation, 0.60, 0);
    bool sens_warned = false;
    for (const auto& msg : w4) sens_warned = sens_warned || contains(msg, "capture_sensitivity=extreme");
    CHECK_TRUE(sens_warned);
}

TEST(ini_capture_write_back) {
    const std::string tmpl = load_template();
    Config c;
    c.trigger.capture = false;
    c.trigger.capture_sensitivity = Sensitivity::Low;
    c.capture.grid_w = 192;
    c.capture.grid_h = 108;
    c.capture.min_correlation = 0.7;
    c.capture.assumed_hfov_deg = 103.5;
    const std::string saved = save_config_text(c, tmpl);
    CHECK(contains(saved, "\r\ncapture=0                      ; 截屏运动检测 0 | 1\r\n"));
    CHECK(contains(saved, "\r\ncapture_sensitivity=low        ; low | medium | high\r\n"));
    CHECK(contains(saved, "\r\ngrid_w=192\r\n"));
    CHECK(contains(saved, "\r\ngrid_h=108\r\n"));
    CHECK(contains(saved, "\r\nmin_correlation=0.70\r\n"));   // 至少 2 位小数（同默认值字面量）
    CHECK(contains(saved, "\r\nassumed_hfov_deg=103.5\r\n"));
    CHECK_TRUE(load_config(saved) == c);
    CHECK_TRUE(save_config_text(load_config(saved), tmpl) == saved);
}

TEST(ini_old_ini_gains_capture_keys) {
    // in-game-calibration 版本生成的 INI（没有 capture / capture_sensitivity / toggle_capture / [capture]），带一条校准。
    const std::string tmpl = load_template();
    const std::string old_tmpl = calibration_version_template();
    CHECK_FALSE(contains(old_tmpl, "\r\ncapture="));
    CHECK_FALSE(contains(old_tmpl, "toggle_capture"));
    CHECK_FALSE(contains(old_tmpl, "[capture]"));
    CHECK(contains(old_tmpl, "hide_from_capture=auto"));   // 这个键是变更 1 的，保留
    std::string old_ini = replace_line(old_tmpl, "sensitivity=", "sensitivity=high               ; low | medium | high");
    old_ini = replace_line(old_ini, "gamepad=", "gamepad=0");
    old_ini += "cs2.exe=18.4\r\n";   // 旧模板以 [calibration] 的注释结尾

    std::vector<std::string> w;
    Config c = load_config(old_ini, &w);
    CHECK_EQ(w.size(), 0);
    CHECK_TRUE(c.extras.empty());
    CHECK_TRUE(c.trigger.capture);
    CHECK_TRUE(c.trigger.capture_sensitivity == Sensitivity::Medium);
    CHECK_TRUE(c.capture == CaptureParams{});
    CHECK_EQ(c.hotkeys[hotkey_index(HotkeyAction::ToggleCapture)], L"Ctrl+Alt+F6");
    CHECK_EQ(c.calibration.size(), 1);

    c.dots.edge_dots = false;   // 因任一改动保存
    const std::string saved = save_config_text(c, tmpl);
    const std::vector<std::string> want{"general", "mask",    "dots",        "trigger",
                                        "detect",  "hotkeys", "calibration", "capture", "ui"};
    CHECK_TRUE(section_headers(saved) == want);
    CHECK(contains(saved, "\r\ngamepad=0\r\n; 截屏运动检测："));
    CHECK(contains(saved, "\r\ncapture=1                      ; 截屏运动检测 0 | 1\r\n"));
    CHECK(contains(saved, "\r\ncalibrate=Ctrl+Alt+F7\r\n; 截屏运动检测开关\r\ntoggle_capture=Ctrl+Alt+F6\r\n"));
    CHECK(contains(saved, "\r\ncs2.exe=18.400\r\n\r\n[capture]"));
    CHECK(contains(saved, "\r\nsensitivity=high               ; low | medium | high\r\n"));
    Config expect;
    expect.trigger.sensitivity = Sensitivity::High;
    expect.trigger.gamepad = false;
    expect.dots.edge_dots = false;
    expect.calibration[L"cs2.exe"] = 18.4;
    CHECK_TRUE(c == expect);
    CHECK_TRUE(saved == save_config_text(expect, tmpl));
    CHECK_TRUE(load_config(saved) == c);
}

// ================================================================ 文件层

TEST(ini_atomic_save_replaces_file) {
    TempDir dir;
    const std::wstring path = dir.file(L"MotionCalm.ini");
    CHECK_TRUE(save_file_atomic(path, "first\r\n"));
    CHECK_TRUE(save_file_atomic(path, "second version\r\n"));
    std::string back;
    CHECK_TRUE(read_file(path, &back));
    CHECK_EQ(back, "second version\r\n");
    CHECK_FALSE(file_exists(path + L".tmp"));
    CHECK_NEAR(static_cast<double>(file_size(path)), 16, 0);
}

TEST(ini_create_from_template) {
    const std::string tmpl = load_template();
    TempDir dir;
    const std::wstring path = dir.file(L"MotionCalm.ini");
    const LoadedFile first = load_or_create(path, tmpl);
    CHECK_TRUE(first.created);
    CHECK_TRUE(first.from_disk);
    CHECK_TRUE(first.writable);
    std::string bytes;
    CHECK_TRUE(read_file(path, &bytes));
    CHECK_TRUE(bytes == tmpl);
    CHECK_TRUE(first.text == tmpl);

    const LoadedFile second = load_or_create(path, tmpl);
    CHECK_FALSE(second.created);
    CHECK_TRUE(second.text == tmpl);
    CHECK_EQ(content_hash(second.text), content_hash(tmpl));
    CHECK_FALSE(content_hash("a") == content_hash("b"));

    // 目录不存在（不可写）：用模板运行、禁用保存。
    const LoadedFile bad = load_or_create(dir.file(L"no-such-dir\\MotionCalm.ini"), tmpl);
    CHECK_FALSE(bad.writable);
    CHECK_FALSE(bad.from_disk);
    CHECK_TRUE(bad.text == tmpl);
}

// 变更 5：[capture] forward_gain / detect_roll / baseline_ms 的默认值、截断、非法值与写回
TEST(ini_forward_and_roll_keys) {
    const Config def = load_config(load_template());
    CHECK_NEAR(def.trigger.forward_gain, 1.0, 0);
    CHECK_TRUE(def.capture.detect_roll);
    CHECK_EQ(def.capture.baseline_ms, 66);
    // 旧文件没有这三个键：默认值，不记 warn
    std::vector<std::string> w0;
    const Config old = load_config("[capture]\ngrid_w=160\n", &w0);
    CHECK_EQ(w0.size(), 0);
    CHECK_NEAR(old.trigger.forward_gain, 1.0, 0);
    CHECK_TRUE(old.capture.detect_roll);
    CHECK_EQ(old.capture.baseline_ms, 66);
    // 越界截断（每键一条 warn）
    std::vector<std::string> w1;
    const Config a = load_config("[capture]\nforward_gain=5\nbaseline_ms=-3\n", &w1);
    CHECK_EQ(w1.size(), 2);
    CHECK_NEAR(a.trigger.forward_gain, 3.0, 0);
    CHECK_EQ(a.capture.baseline_ms, 0);
    std::vector<std::string> w2;
    const Config b = load_config("[capture]\nforward_gain=0.1\nbaseline_ms=999\n", &w2);
    CHECK_EQ(w2.size(), 2);
    CHECK_NEAR(b.trigger.forward_gain, 0.5, 0);
    CHECK_EQ(b.capture.baseline_ms, 150);
    // 非法值 → 默认值 + warn
    std::vector<std::string> w3;
    const Config c = load_config("[capture]\nforward_gain=fast\ndetect_roll=yes\nbaseline_ms=abc\n", &w3);
    CHECK_EQ(w3.size(), 3);
    CHECK_NEAR(c.trigger.forward_gain, 1.0, 0);
    CHECK_TRUE(c.capture.detect_roll);
    CHECK_EQ(c.capture.baseline_ms, 66);
    // 写回：保留注释，数值补到默认值的小数位
    Config d;
    d.trigger.forward_gain = 2.0;
    d.capture.detect_roll = false;
    d.capture.baseline_ms = 100;
    const std::string saved = save_config_text(d, load_template());
    CHECK(contains(saved, "\r\nforward_gain=2.0\r\n"));
    CHECK(contains(saved, "\r\ndetect_roll=0\r\n"));
    CHECK(contains(saved, "\r\nbaseline_ms=100\r\n"));
    CHECK(contains(saved, "hold_ms=100\r\n; 前进 / 后退的灵敏度"));
    const Config back = load_config(saved);
    CHECK_TRUE(back == d);
}

// 变更 5（修订）：前进 / 倾斜各自的阈值 [capture] zoom_start_* / zoom_full_* / roll_start_* / roll_full_*
TEST(ini_forward_and_roll_thresholds) {
    const Config def = load_config(load_template());
    CHECK_TRUE(def.trigger.zoom_thresholds == (std::array<Thresholds, 3>{{{40, 250}, {20, 150}, {10, 83}}}));
    CHECK_TRUE(def.trigger.roll_thresholds == (std::array<Thresholds, 3>{{{30, 150}, {15, 90}, {8, 50}}}));
    // 旧文件（v1.1 初版）没有这些键：默认值，不记 warn
    std::vector<std::string> w0;
    const Config old = load_config("[capture]\nforward_gain=1.5\n", &w0);
    CHECK_EQ(w0.size(), 0);
    CHECK_TRUE(old.trigger.zoom_thresholds == def.trigger.zoom_thresholds);
    // 越界截断、非法值
    std::vector<std::string> w1;
    const Config a = load_config("[capture]\nzoom_full_low=9000\nroll_start_high=0\nroll_full_medium=abc\n", &w1);
    CHECK_EQ(w1.size(), 3);
    CHECK_NEAR(a.trigger.zoom_thresholds[0].v_full, 5000, 0);
    CHECK_NEAR(a.trigger.roll_thresholds[2].v_start, 1, 0);
    CHECK_NEAR(a.trigger.roll_thresholds[1].v_full, 90, 0);
    // 开始 ≥ 完全：该档两个值都恢复默认，记一条 warn
    std::vector<std::string> w2;
    const Config b = load_config("[capture]\nzoom_start_medium=200\nzoom_full_medium=150\nroll_start_low=40\n", &w2);
    CHECK_EQ(w2.size(), 1);
    CHECK_TRUE(b.trigger.zoom_thresholds[1] == (Thresholds{20, 150}));
    CHECK_NEAR(b.trigger.roll_thresholds[0].v_start, 40, 0);
    if (w2.size() == 1) CHECK(contains(w2[0], "[capture] zoom_start_medium=200"));
    // 写回往返（一位小数原样保留）
    Config d;
    d.trigger.zoom_thresholds[1] = {25.5, 120};
    d.trigger.roll_thresholds[2] = {6, 40};
    const std::string saved = save_config_text(d, load_template());
    CHECK(contains(saved, "\r\nzoom_start_medium=25.5\r\n"));
    CHECK(contains(saved, "\r\nroll_full_high=40\r\n"));
    CHECK(contains(saved, "baseline_ms=66\r\n; 以下是前进 / 后退与画面倾斜自己的阈值"));
    CHECK_TRUE(load_config(saved) == d);
}
