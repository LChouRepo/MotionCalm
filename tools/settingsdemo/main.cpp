// main.cpp — settingsdemo：不依赖 App，用一个记录每次调用的假 SettingsHost 打开真正的设置窗口（src/ui），供手动试用与逐页截图。
//
//   settingsdemo [--page N] [--exit-after S] [--screenshots DIR] [--conflicts] [--no-capture] [--capture-failed]
//                [--verbose] [--calib-busy] [--fail-autostart] [--empty-calib] [--advanced] [--dpi N] [--focus-hotkey N]
//
//   --page N           打开后切到第 N 页（0 遮罩 … 5 通用）
//   --exit-after S     S 秒后像点"关闭"一样关闭窗口并退出
//   --screenshots DIR  依次切到 6 个分页，各截一张 PNG 到 DIR\settings_<N>_<名>.png（页内需要滚动时
//                      再截一张滚到底的 settings_<N>_<名>_end.png），然后关闭退出
//   --conflicts        预置冲突示例：debug_bar=Ctrl+Alt+F9（与 toggle 重复）、toggle_center_dot=F11（无修饰键）、
//                      exit=Ctrl+Alt+K（假装被其他程序占用）、cycle_sensitivity=Ctrl+Alt+F99（写法非法）
//   --no-capture / --capture-failed / --verbose / --calib-busy   假的运行期状态
//   --fail-autostart   勾选"开机自动启动"时假装写注册表失败（回滚并刷新）
//   --empty-calib      没有校准记录
//   --advanced         [ui] show_advanced=1（显示高级选项）
//   --dpi N            用 N DPI 排版与选字号（模拟 150% 用 144；系统绘制的复选框方块、标题栏不变）
//   --focus-hotkey N   截图前把焦点放到快捷键页第 N 行的捕获框（看占位提示）
//   --selftest         在进程内用消息驱动真实窗口（SetKeyboardState 模拟修饰键，不产生真实输入），检查宿主收到的值，
//                      打印 PASS / FAIL；退出码 = 失败数
//   --lang L           界面语言 auto | zh-CN | zh-TW | en（默认 zh-CN；同时作为假宿主的 [ui] language）
//   --printwindow      截图改用 PrintWindow(PW_RENDERFULLCONTENT)（默认：窗口置顶后从屏幕 BitBlt 它的可见边框区域；
//                      PrintWindow 对刚滚动 / 刚显示的子控件常截到空白，不可靠）
// 假宿主模拟"最后一次改动 300 ms 后保存"，保存时调用 SettingsWindow::config_saved()（状态栏"上次保存"）。
// 默认值取自内嵌的 MotionCalm.default.ini 模板（与 app 相同的 app.rc）；模板里还没有的变更 3 键用规格默认值补上。
// 退出码：0 正常；1 参数错误；2 窗口创建失败。
#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "config/ini.h"
#include "i18n/i18n.h"
#include "res/resource.h"
#include "tray/hotkeys.h"
#include "ui/settings_dlg.h"
#include "ui/settings_host.h"
#include "util/strings.h"

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

using mc::ui::CalibEntry;
using mc::ui::KeyRange;
using mc::ui::SettingKey;
using mc::ui::SettingsStatus;
using mc::ui::SettingValue;

std::vector<std::wstring> g_log;   // 宿主调用记录（自检用）

void out(const std::wstring& line) {
    g_log.push_back(line);
    const std::string u = mc::wide_to_utf8(line + L"\n");
    std::fwrite(u.data(), 1, u.size(), stdout);
    std::fflush(stdout);
}

std::string key_name(SettingKey k) { return std::string(k.section) + "." + std::string(k.key); }
std::wstring wkey(SettingKey k) { return mc::utf8_to_wide(key_name(k)); }

// 规格里的合法范围（configuration「值校验与回退」+ 变更 3 的截屏键）。只给演示用：app 里由配置模式表提供。
const std::map<std::string, KeyRange>& demo_ranges() {
    static const std::map<std::string, KeyRange> r = {
        {"mask.max_opacity_light", {0, 1}},       {"mask.max_opacity_medium", {0, 1}},
        {"mask.max_opacity_strong", {0, 1}},      {"mask.clear_height_light", {0.05, 1}},
        {"mask.clear_height_medium", {0.05, 1}},  {"mask.clear_height_strong", {0.05, 1}},
        {"mask.ellipse_ratio", {0.5, 3}},         {"mask.feather", {0.05, 1}},
        {"mask.attack_ms", {10, 2000}},           {"mask.release_ms", {10, 5000}},
        {"dots.center_dot_radius_px", {1, 20}},   {"dots.edge_dot_radius_px", {1, 20}},
        {"dots.edge_dot_min_alpha", {0, 1}},      {"dots.edge_dot_max_alpha", {0, 1}},
        {"dots.edge_dot_margin_px", {0, 400}},    {"trigger.default_counts_per_degree", {1, 1000}},
        {"trigger.v_start_low", {1, 5000}},       {"trigger.v_full_low", {1, 5000}},
        {"trigger.v_start_medium", {1, 5000}},    {"trigger.v_full_medium", {1, 5000}},
        {"trigger.v_start_high", {1, 5000}},      {"trigger.v_full_high", {1, 5000}},
        {"trigger.move_intensity", {0, 1}},       {"trigger.gamepad_max_deg_per_s", {10, 2000}},
        {"trigger.gamepad_deadzone", {0, 0.9}},   {"trigger.velocity_window_ms", {10, 500}},
        {"capture.grid_w", {64, 320}},            {"capture.grid_h", {36, 180}},
        {"capture.max_shift", {2, 40}},           {"capture.min_correlation", {0.10, 0.99}},
        {"capture.assumed_hfov_deg", {30, 170}},  {"capture.hold_ms", {0, 1000}},
        {"capture.forward_gain", {0.5, 3.0}},     {"capture.baseline_ms", {0, 150}},
        {"capture.zoom_start_low", {1, 5000}},    {"capture.zoom_full_low", {1, 5000}},
        {"capture.zoom_start_medium", {1, 5000}}, {"capture.zoom_full_medium", {1, 5000}},
        {"capture.zoom_start_high", {1, 5000}},   {"capture.zoom_full_high", {1, 5000}},
        {"capture.roll_start_low", {1, 5000}},    {"capture.roll_full_low", {1, 5000}},
        {"capture.roll_start_medium", {1, 5000}}, {"capture.roll_full_medium", {1, 5000}},
        {"capture.roll_start_high", {1, 5000}},   {"capture.roll_full_high", {1, 5000}},
    };
    return r;
}

struct Options {
    int page = -1;
    double exit_after = 0;
    std::wstring screenshots;
    bool conflicts = false;
    bool no_capture = false;
    bool capture_failed = false;
    bool verbose = false;
    bool calib_busy = false;
    bool fail_autostart = false;
    bool empty_calib = false;
    bool advanced = false;
    int dpi = 0;
    int focus_hotkey = -1;
    bool selftest = false;
    std::wstring lang;   // --lang（空 = zh-CN）
};

class FakeHost;
FakeHost* g_host = nullptr;
bool g_bitblt = true;   // 默认从屏幕 BitBlt（CAPTUREBLT）截取窗口区域；--printwindow 改用 PrintWindow
void CALLBACK save_timer_proc(HWND, UINT, UINT_PTR, DWORD);

class FakeHost final : public mc::ui::SettingsHost {
public:
    FakeHost(HINSTANCE inst, const Options& o) : opt_(o) {
        template_ = load_template(inst);
        seed_defaults();
        if (!o.empty_calib) {
            calib_ = {{L"apex.exe", 15.0, L"昨天"}, {L"cs2.exe", 18.4, L""}, {L"valorant.exe", 35.714, L"2026-11-01"}};
        }
        values_["detect.known_games"] = L"newgame.exe;other.exe;tool.exe";
        values_["detect.force_game"] = L"mygame.exe";
        if (o.conflicts) {
            values_["hotkeys.debug_bar"] = L"Ctrl+Alt+F9";
            values_["hotkeys.toggle_center_dot"] = L"F11";
            values_["hotkeys.exit"] = L"Ctrl+Alt+K";
            values_["hotkeys.cycle_sensitivity"] = L"Ctrl+Alt+F99";
        }
        occupied_.insert(L"Ctrl+Alt+K");
        values_["ui.show_advanced"] = o.advanced ? L"1" : L"0";
        values_["ui.language"] = o.lang.empty() ? L"zh-CN" : o.lang;
        values_["ui.x"] = L"";
        values_["ui.y"] = L"";
    }

    // 防抖保存到期（或 flush）：假装写了 INI。
    void save_now(const wchar_t* why) {
        if (save_timer_ != 0) {
            ::KillTimer(nullptr, save_timer_);
            save_timer_ = 0;
        }
        out(std::wstring(L"[host] INI saved (") + why + L")");
        if (window_ != nullptr) window_->config_saved();
    }
    void on_save_timer() {
        if (save_timer_ != 0) save_now(L"debounced 300 ms");
    }

    void attach(mc::ui::SettingsWindow* w) { window_ = w; }
    std::wstring value(const char* name) const {
        const auto it = values_.find(name);
        return it != values_.end() ? it->second : std::wstring(L"<missing>");
    }

    std::optional<std::wstring> get(SettingKey key) const override {
        const auto it = values_.find(key_name(key));
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    bool set(std::span<const SettingValue> values) override {
        std::wstring line = L"[host] set";
        for (const auto& v : values) {
            if (values_.find(key_name(v.key)) == values_.end()) {
                out(L"[host] set REJECTED (unknown key " + wkey(v.key) + L")");
                return false;
            }
            line += L"  " + wkey(v.key) + L"=" + v.value;
        }
        out(line);
        for (const auto& v : values) {
            values_[key_name(v.key)] = v.value;
            mc::i18n::LangSetting lang{};
            if (key_name(v.key) == "ui.language" && mc::i18n::parse_lang_setting(v.value, &lang)) {
                // 与 App::apply_language 相同：立即切换；设置窗口在下一次刷新时按新语言重建分页
                mc::i18n::set_language(mc::i18n::resolve(lang));
                out(std::wstring(L"[host] ui language -> ") + mc::i18n::lang_code(mc::i18n::current()));
                if (window_ != nullptr) window_->refresh();
            }
        }
        if (save_timer_ != 0) ::KillTimer(nullptr, save_timer_);
        save_timer_ = ::SetTimer(nullptr, 0, 300, save_timer_proc);   // 最后一次改动 300 ms 后保存
        for (const auto& v : values) {
            if (key_name(v.key) == "general.autostart" && v.value == L"1" && opt_.fail_autostart) {
                out(L"[host] autostart: RegSetValueEx failed (simulated) -> rollback autostart=0, toast 无法设置开机自启");
                values_["general.autostart"] = L"0";
                if (window_ != nullptr) window_->refresh();
            }
        }
        return true;
    }

    std::optional<KeyRange> range(SettingKey key) const override {
        const auto it = demo_ranges().find(key_name(key));
        if (it == demo_ranges().end()) return std::nullopt;
        return it->second;
    }

    void flush_config() override {
        out(L"[host] flush_config");
        if (save_timer_ != 0) save_now(L"flush");
    }

    void restore_defaults() override {
        out(L"[host] restore_defaults");
        const auto keep_force = values_["detect.force_game"];
        const auto keep_known = values_["detect.known_games"];
        const auto keep_autostart = values_["general.autostart"];
        const auto keep_language = values_["ui.language"];   // [ui] 整节保留（与 app 的 restore_defaults 相同）
        seed_defaults();
        values_["ui.language"] = keep_language;
        values_["detect.force_game"] = keep_force;
        values_["detect.known_games"] = keep_known;
        values_["general.autostart"] = keep_autostart;
        save_now(L"restore defaults");
    }

    void clear_known_games() override {
        out(L"[host] clear_known_games");
        values_["detect.known_games"] = L"";
        save_now(L"clear known games");
    }

    void open_config_file() override { out(L"[host] open_config_file (flush, then ShellExecute MotionCalm.ini)"); }

    SettingsStatus status() const override {
        SettingsStatus s;
        s.calibration_idle = !opt_.calib_busy;
        s.capture_disabled_by_cmdline = opt_.no_capture;
        s.capture_failed = opt_.capture_failed;
        s.verbose = opt_.verbose;
        return s;
    }

    std::vector<CalibEntry> calibrations() const override { return calib_; }

    void delete_calibration(const std::wstring& exe) override {
        out(L"[host] delete_calibration " + exe);
        std::erase_if(calib_, [&](const CalibEntry& e) { return e.exe == exe; });
        if (save_timer_ != 0) ::KillTimer(nullptr, save_timer_);
        save_timer_ = ::SetTimer(nullptr, 0, 300, save_timer_proc);
    }

    void pause_hotkeys(bool paused) override { out(paused ? L"[host] pause_hotkeys(true)" : L"[host] pause_hotkeys(false)"); }

    bool hotkey_available(mc::HotkeyAction action, const mc::Hotkey& hk) override {
        const std::wstring text = mc::format_hotkey(hk);
        bool ok = occupied_.count(text) == 0;
        if (ok) {   // 真的试注册一次（线程热键），能发现被其他程序占用的组合
            constexpr int kTrialId = 0xBFF0;
            ok = ::RegisterHotKey(nullptr, kTrialId, hk.mods | MOD_NOREPEAT, hk.vk) != FALSE;
            if (ok) ::UnregisterHotKey(nullptr, kTrialId);
        }
        out(L"[host] hotkey_available " + mc::utf8_to_wide(std::string(mc::hotkey_info(action).ini_key)) + L" " + text +
            (ok ? L" -> yes" : L" -> NO (occupied)"));
        return ok;
    }

    void set_preview_floor(float p) override {
        if (p == last_preview_) return;
        last_preview_ = p;
        wchar_t buf[64];
        std::swprintf(buf, 64, L"[host] set_preview_floor(%.2f)", static_cast<double>(p));
        out(buf);
    }

    void modal_begin() override { out(L"[host] modal_begin"); }
    void modal_end() override { out(L"[host] modal_end"); }
    void settings_closed() override {
        out(L"[host] settings_closed");
        ::PostQuitMessage(0);
    }

private:
    static std::string load_template(HINSTANCE inst) {
        const HRSRC res = ::FindResourceW(inst, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA);
        if (res == nullptr) return {};
        const HGLOBAL g = ::LoadResource(inst, res);
        const void* p = g != nullptr ? ::LockResource(g) : nullptr;
        const DWORD n = ::SizeofResource(inst, res);
        return p != nullptr ? std::string(static_cast<const char*>(p), n) : std::string();
    }

    void seed_defaults() {
        static const std::set<std::string> kLists = {"detect.exclude", "detect.force_game", "detect.known_games"};
        for (const auto& e : mc::ini::parse_ini(template_)) {
            if (e.section == "calibration" || e.section == "extra" || e.section.empty()) continue;
            const std::string name = e.section + "." + e.key;
            const std::string_view v = kLists.count(name) ? std::string_view(e.value) : mc::ini::strip_inline_comment(e.value);
            values_[name] = mc::utf8_to_wide(v);
        }
        // 变更 3 / 4 在本工作树的模板里可能还没有的键：按规格默认值补上。
        const std::pair<const char*, const wchar_t*> extra[] = {
            {"trigger.capture", L"1"},           {"trigger.capture_sensitivity", L"medium"},
            {"capture.grid_w", L"160"},          {"capture.grid_h", L"90"},
            {"capture.max_shift", L"20"},        {"capture.min_correlation", L"0.60"},
            {"capture.assumed_hfov_deg", L"90"}, {"capture.hold_ms", L"100"},
            {"capture.forward_gain", L"1.0"},    {"capture.detect_roll", L"1"},
            {"capture.baseline_ms", L"66"},
        };
        for (const auto& [k, v] : extra) values_.try_emplace(k, v);
        for (const auto& a : mc::kHotkeyActions) {
            values_.try_emplace("hotkeys." + std::string(a.ini_key), std::wstring(a.default_binding));
        }
    }

    Options opt_;
    std::string template_;
    std::map<std::string, std::wstring> values_;
    std::vector<CalibEntry> calib_;
    std::set<std::wstring> occupied_;
    mc::ui::SettingsWindow* window_ = nullptr;
    float last_preview_ = -1.0f;
    UINT_PTR save_timer_ = 0;
};

void CALLBACK save_timer_proc(HWND, UINT, UINT_PTR, DWORD) {
    if (g_host != nullptr) g_host->on_save_timer();
}

bool parse_args(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == L"--page") {
            const wchar_t* v = next();
            if (v == nullptr) return false;
            o->page = _wtoi(v);
        } else if (a == L"--exit-after") {
            const wchar_t* v = next();
            if (v == nullptr) return false;
            o->exit_after = _wtof(v);
        } else if (a == L"--screenshots") {
            const wchar_t* v = next();
            if (v == nullptr) return false;
            o->screenshots = v;
        } else if (a == L"--conflicts") {
            o->conflicts = true;
        } else if (a == L"--no-capture") {
            o->no_capture = true;
        } else if (a == L"--capture-failed") {
            o->capture_failed = true;
        } else if (a == L"--verbose") {
            o->verbose = true;
        } else if (a == L"--calib-busy") {
            o->calib_busy = true;
        } else if (a == L"--fail-autostart") {
            o->fail_autostart = true;
        } else if (a == L"--empty-calib") {
            o->empty_calib = true;
        } else if (a == L"--advanced") {
            o->advanced = true;
        } else if (a == L"--printwindow") {
            g_bitblt = false;
        } else if (a == L"--dpi") {
            const wchar_t* v = next();
            if (v == nullptr) return false;
            o->dpi = _wtoi(v);
        } else if (a == L"--focus-hotkey") {
            const wchar_t* v = next();
            if (v == nullptr) return false;
            o->focus_hotkey = _wtoi(v);
        } else if (a == L"--selftest") {
            o->selftest = true;
        } else if (a == L"--lang") {
            const wchar_t* v = next();
            mc::i18n::LangSetting ls{};
            if (v == nullptr || !mc::i18n::parse_lang_setting(v, &ls)) return false;
            o->lang = v;
        } else {
            return false;
        }
    }
    return true;
}

bool png_encoder(CLSID* clsid) {
    UINT n = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&n, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<unsigned char> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(n, size, codecs) != Gdiplus::Ok) return false;
    for (UINT k = 0; k < n; ++k) {
        if (std::wcscmp(codecs[k].MimeType, L"image/png") == 0) {
            *clsid = codecs[k].Clsid;
            return true;
        }
    }
    return false;
}

// PrintWindow(PW_RENDERFULLCONTENT) 截取整个窗口（含标题栏），即使被其他窗口挡住也能截到。
bool save_window_png(HWND hwnd, const std::wstring& path) {
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    if (g_bitblt) {   // 去掉看不见的缩放边框，只截可见的窗口
        RECT fr{};
        if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) wr = fr;
    }
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    const BOOL printed = g_bitblt ? ::BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY | CAPTUREBLT)
                                  : ::PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    ::SelectObject(mem, old);
    bool ok = false;
    if (printed) {
        CLSID clsid{};
        if (png_encoder(&clsid)) {
            Gdiplus::Bitmap image(bmp, nullptr);
            ok = image.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
        }
    }
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

// 处理消息约 ms 毫秒（让刷新 / 重绘完成）。
void pump_for(mc::ui::SettingsWindow& win, DWORD ms) {
    const ULONGLONG end = ::GetTickCount64() + ms;
    for (;;) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            if (win.pre_translate(msg)) continue;
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= end) return;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(end - now), QS_ALLINPUT);
    }
}

}  // namespace

// ---------------------------------------------------------------- --selftest

int g_failures = 0;

void expect(bool ok, const std::wstring& what) {
    out((ok ? L"[PASS] " : L"[FAIL] ") + what);
    if (!ok) ++g_failures;
}

// 外壳里含有 id 控件的那一页上的控件（IDC_SHOW_ADVANCED 各页都有：取可见页上的）。
HWND find_ctl(HWND shell, int id) {
    for (HWND c = ::GetWindow(shell, GW_CHILD); c != nullptr; c = ::GetWindow(c, GW_HWNDNEXT)) {
        HWND h = ::GetDlgItem(c, id);
        if (h != nullptr && (id != IDC_SHOW_ADVANCED || ::IsWindowVisible(c))) return h;
    }
    return ::GetDlgItem(shell, id);
}

std::wstring text_of(HWND h) {
    wchar_t buf[512] = {};
    ::GetWindowTextW(h, buf, 512);
    return buf;
}

void drag_slider(HWND slider, int pos) {
    ::SendMessageW(slider, TBM_SETPOS, TRUE, pos);
    ::SendMessageW(::GetParent(slider), WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, pos), reinterpret_cast<LPARAM>(slider));
}

void type_and_commit(HWND edit, const wchar_t* text) {
    ::SetWindowTextW(edit, text);   // 触发 EN_CHANGE → 有未提交输入
    ::SendMessageW(::GetParent(edit), WM_COMMAND, MAKEWPARAM(::GetDlgCtrlID(edit), EN_KILLFOCUS),
                   reinterpret_cast<LPARAM>(edit));
}

void click(HWND button) { ::SendMessageW(button, BM_CLICK, 0, 0); }

void set_modifiers(bool ctrl, bool alt, bool shift) {
    BYTE ks[256] = {};
    ::GetKeyboardState(ks);
    ks[VK_CONTROL] = ctrl ? 0x80 : 0;
    ks[VK_MENU] = alt ? 0x80 : 0;
    ks[VK_SHIFT] = shift ? 0x80 : 0;
    ::SetKeyboardState(ks);
}

void key(HWND edit, UINT vk, bool up = false) {
    const bool sys = ::GetKeyState(VK_MENU) < 0;
    const UINT msg = up ? (sys ? WM_SYSKEYUP : WM_KEYUP) : (sys ? WM_SYSKEYDOWN : WM_KEYDOWN);
    ::SendMessageW(edit, msg, vk, up ? static_cast<LPARAM>(0xC0000001) : 1);
}

bool logged(const std::wstring& needle, std::size_t from) {
    for (std::size_t i = from; i < g_log.size(); ++i) {
        if (g_log[i].find(needle) != std::wstring::npos) return true;
    }
    return false;
}

int row_of(std::string_view ini_key) {
    return static_cast<int>(mc::hotkey_index(*mc::hotkey_action_from_key(ini_key)));
}

int run_selftest(mc::ui::SettingsWindow& win, FakeHost& host) {
    HWND shell = win.hwnd();

    // ---- 遮罩页
    win.select_page(0);
    click(find_ctl(shell, IDC_SHOW_ADVANCED));
    pump_for(win, 100);
    expect(host.value("ui.show_advanced") == L"1", L"显示高级选项 → [ui] show_advanced=1");
    expect(::IsWindowVisible(find_ctl(shell, IDC_MASK_OPACITY_S)) != FALSE, L"高级条目随之显示");
    drag_slider(find_ctl(shell, IDC_MASK_OPACITY_S), 50);
    pump_for(win, 50);
    expect(host.value("mask.max_opacity_strong") == L"0.50",
           L"强档最深程度拖到位置 50 → 0.50（实际 " + host.value("mask.max_opacity_strong") + L"）");
    expect(text_of(find_ctl(shell, IDC_MASK_OPACITY_S_VAL)) == L"50%", L"读数显示 50%");
    std::size_t mark = g_log.size();
    drag_slider(find_ctl(shell, IDC_MASK_PREVIEW), 70);
    expect(logged(L"set_preview_floor(0.70)", mark), L"预览强度 70% → set_preview_floor(0.70)");
    expect(!logged(L"[host] set ", mark), L"预览强度不写任何键");
    type_and_commit(find_ctl(shell, IDC_MASK_COLOR), L"200000");
    expect(host.value("mask.color") == L"200000", L"颜色输入 200000 → 写入");
    type_and_commit(find_ctl(shell, IDC_MASK_COLOR), L"zz");
    expect(host.value("mask.color") == L"200000" && text_of(find_ctl(shell, IDC_MASK_COLOR)) == L"200000",
           L"非法颜色 zz → 恢复原值，配置不变");
    click(find_ctl(shell, IDC_MASK_LEVEL_STRONG));
    expect(host.value("general.mask_level") == L"strong", L"点选“强” → mask_level=strong");

    // ---- 参照物页
    win.select_page(1);
    pump_for(win, 50);
    drag_slider(find_ctl(shell, IDC_DOTS_EDGE_MIN), 80);
    pump_for(win, 50);
    expect(host.value("dots.edge_dot_min_alpha") == L"0.80" && host.value("dots.edge_dot_max_alpha") == L"0.80",
           L"最淡拖到 0.80（最浓 0.75）→ 两者都为 0.80");
    click(find_ctl(shell, IDC_DOTS_CENTER));
    pump_for(win, 50);
    expect(host.value("dots.center_dot") == L"0" && !::IsWindowEnabled(find_ctl(shell, IDC_DOTS_CENTER_RADIUS)) &&
               !::IsWindowEnabled(find_ctl(shell, IDC_DOTS_CENTER_COLOR)),
           L"取消“显示中心点” → 中心点细调控件灰显");
    click(find_ctl(shell, IDC_DOTS_CENTER));
    pump_for(win, 50);

    // ---- 触发页
    win.select_page(2);
    pump_for(win, 50);
    HWND vstart_m = find_ctl(shell, IDC_TRIG_VSTART_MEDIUM);
    type_and_commit(vstart_m, L"400");
    expect(host.value("trigger.v_start_medium") == L"60" && text_of(vstart_m) == L"60",
           L"中档开始值改为 400（满强度 360）→ 被拒，框内恢复 60");
    HWND vfull_m = find_ctl(shell, IDC_TRIG_VFULL_MEDIUM);
    type_and_commit(vfull_m, L"9000");
    expect(host.value("trigger.v_full_medium") == L"5000" && text_of(vfull_m) == L"5000", L"满强度 9000 → 钳制为 5000");
    type_and_commit(find_ctl(shell, IDC_TRIG_GRIDW), L"abc");
    expect(host.value("capture.grid_w") == L"160", L"网格宽输入 abc → 恢复原值");
    HWND lock = find_ctl(shell, IDC_TRIG_LOCK_RULE);
    ::SendMessageW(lock, CB_SETCURSEL, 1, 0);
    ::SendMessageW(::GetParent(lock), WM_COMMAND, MAKEWPARAM(IDC_TRIG_LOCK_RULE, CBN_SELCHANGE),
                   reinterpret_cast<LPARAM>(lock));
    expect(host.value("trigger.mouse_lock_rule") == L"hidden", L"鼠标锁定判定选“仅看光标隐藏” → hidden");
    // 变更 5：前进 / 后退的灵敏度（0.5–3.0，步长 0.1 → 位置 0..25）与识别画面倾斜
    HWND gain = find_ctl(shell, IDC_TRIG_FWD_GAIN);
    expect(::SendMessageW(gain, TBM_GETPOS, 0, 0) == 5 && text_of(find_ctl(shell, IDC_TRIG_FWD_GAIN_VAL)) == L"1.0 倍",
           L"前进 / 后退的灵敏度默认在位置 5，读数“1.0 倍”（实际 " + text_of(find_ctl(shell, IDC_TRIG_FWD_GAIN_VAL)) + L"）");
    drag_slider(gain, 25);
    pump_for(win, 50);
    expect(host.value("capture.forward_gain") == L"3.0" && text_of(find_ctl(shell, IDC_TRIG_FWD_GAIN_VAL)) == L"3.0 倍",
           L"前进 / 后退的灵敏度拖到最右 → forward_gain=3.0（实际 " + host.value("capture.forward_gain") + L"）");
    drag_slider(gain, 0);
    pump_for(win, 50);
    expect(host.value("capture.forward_gain") == L"0.5", L"拖到最左 → forward_gain=0.5");
    HWND roll = find_ctl(shell, IDC_TRIG_ROLL);
    expect(::SendMessageW(roll, BM_GETCHECK, 0, 0) == BST_CHECKED, L"识别画面倾斜默认勾选");
    click(roll);
    pump_for(win, 50);
    expect(host.value("capture.detect_roll") == L"0", L"取消“识别画面倾斜” → detect_roll=0");
    click(roll);
    pump_for(win, 50);
    expect(host.value("capture.detect_roll") == L"1", L"再勾选 → detect_roll=1");
    // 前进 / 倾斜各自的阈值表（[capture] zoom_* / roll_*，开始 < 完全）
    HWND zstart_m = find_ctl(shell, IDC_TRIG_ZOOM_TH_BASE + 2);
    expect(text_of(zstart_m) == L"20" && text_of(find_ctl(shell, IDC_TRIG_ZOOM_TH_BASE + 3)) == L"150",
           L"前进中档阈值默认 20 / 150（实际 " + text_of(zstart_m) + L"）");
    type_and_commit(zstart_m, L"200");
    expect(host.value("capture.zoom_start_medium") == L"20" && text_of(zstart_m) == L"20",
           L"前进中档开始值改为 200（满强度 150）→ 被拒，框内恢复 20");
    type_and_commit(zstart_m, L"30");
    expect(host.value("capture.zoom_start_medium") == L"30", L"前进中档开始值改为 30 → zoom_start_medium=30");
    HWND rfull_h = find_ctl(shell, IDC_TRIG_ROLL_TH_BASE + 5);
    expect(text_of(rfull_h) == L"50", L"倾斜高档满强度默认 50（实际 " + text_of(rfull_h) + L"）");
    type_and_commit(rfull_h, L"5");
    expect(host.value("capture.roll_full_high") == L"50", L"倾斜高档满强度改为 5（开始 8）→ 被拒");

    // ---- 快捷键页
    win.select_page(4);
    pump_for(win, 50);
    HWND cyc = find_ctl(shell, IDC_HK_EDIT_BASE + row_of("cycle_level"));
    mark = g_log.size();
    ::SetFocus(cyc);
    expect(logged(L"pause_hotkeys(true)", mark), L"捕获框获得焦点 → 暂停全局热键");
    expect(text_of(cyc).empty(), L"获得焦点后框内为空、显示占位提示");
    set_modifiers(true, false, false);
    key(cyc, VK_CONTROL);
    set_modifiers(true, false, true);
    key(cyc, VK_SHIFT);
    expect(text_of(cyc) == L"Ctrl + Shift + …", L"按住 Ctrl+Shift → 半成品 Ctrl + Shift + …");
    key(cyc, VK_F10);
    set_modifiers(false, false, false);
    expect(host.value("hotkeys.cycle_level") == L"Ctrl+Shift+F10", L"再按 F10 → cycle_level=Ctrl+Shift+F10");
    expect(text_of(cyc) == L"Ctrl + Shift + F10", L"框内显示 Ctrl + Shift + F10");
    key(cyc, VK_DELETE);
    expect(host.value("hotkeys.cycle_level") == L"None" && text_of(cyc) == L"无", L"Delete → None，框内显示“无”");
    set_modifiers(true, false, false);
    key(cyc, VK_CONTROL);
    set_modifiers(false, false, false);
    key(cyc, VK_CONTROL, true);
    expect(host.value("hotkeys.cycle_level") == L"None", L"只按 Ctrl 后松开 → 配置不变");
    key(cyc, VK_ESCAPE);
    expect(text_of(cyc) == L"无" && ::IsWindow(shell) != FALSE, L"Esc → 显示原值，窗口不关闭");
    {   // 经过 pre_translate / IsDialogMessage 的真实路径
        MSG esc{};
        esc.hwnd = cyc;
        esc.message = WM_KEYDOWN;
        esc.wParam = VK_ESCAPE;
        if (!win.pre_translate(esc)) ::DispatchMessageW(&esc);
        expect(win.is_open() && ::GetFocus() == cyc, L"捕获框里的 Esc 经 IsDialogMessage 也不关闭窗口");
        MSG tab{};
        tab.hwnd = cyc;
        tab.message = WM_KEYDOWN;
        tab.wParam = VK_TAB;
        if (!win.pre_translate(tab)) ::DispatchMessageW(&tab);
        expect(::GetFocus() == find_ctl(shell, IDC_HK_CLEAR_BASE + row_of("cycle_level")) ||
                   ::GetFocus() == find_ctl(shell, IDC_HK_EDIT_BASE + row_of("toggle_center_dot")),
               L"捕获框里的 Tab 把焦点移到下一个控件");
    }
    HWND dbg = find_ctl(shell, IDC_HK_EDIT_BASE + row_of("debug_bar"));
    ::SetFocus(dbg);
    set_modifiers(true, true, false);
    key(dbg, VK_F9);
    set_modifiers(false, false, false);
    pump_for(win, 50);
    const std::wstring dbg_status = text_of(find_ctl(shell, IDC_HK_STATUS_BASE + row_of("debug_bar")));
    const std::wstring tog_status = text_of(find_ctl(shell, IDC_HK_STATUS_BASE + row_of("toggle")));
    expect(dbg_status == L"与「启用 / 关闭遮罩」重复（未生效）", L"debug_bar=Ctrl+Alt+F9 → 状态：" + dbg_status);
    expect(tog_status == L"与「显示调试信息」重复", L"toggle 行状态：" + tog_status);
    mark = g_log.size();
    ::SetFocus(::GetDlgItem(shell, IDC_TAB));
    pump_for(win, 50);
    expect(logged(L"pause_hotkeys(false)", mark), L"焦点离开捕获框 → 恢复全局热键");
    click(find_ctl(shell, IDC_HK_CLEAR_BASE + row_of("debug_bar")));
    pump_for(win, 50);
    expect(host.value("hotkeys.debug_bar") == L"None" &&
               text_of(find_ctl(shell, IDC_HK_STATUS_BASE + row_of("toggle"))).empty(),
           L"点“清除” → debug_bar=None，toggle 行的冲突提示消失");

    // ---- 通用页
    win.select_page(5);
    pump_for(win, 50);
    HWND ex_edit = find_ctl(shell, IDC_GEN_EXCLUDE_EDIT);
    const std::wstring ex_before = host.value("detect.exclude");
    ::SetWindowTextW(ex_edit, L"Chrome.EXE");
    click(find_ctl(shell, IDC_GEN_EXCLUDE_ADD));
    expect(host.value("detect.exclude") == ex_before, L"排除名单添加 Chrome.EXE → 被拒（已在名单中）");
    ::SetWindowTextW(ex_edit, L" tool.exe ");
    click(find_ctl(shell, IDC_GEN_EXCLUDE_ADD));
    expect(host.value("detect.exclude").find(L";tool.exe") != std::wstring::npos &&
               host.value("detect.known_games").find(L"tool.exe") == std::wstring::npos,
           L"添加 tool.exe → 进排除名单，并从已知游戏中移除");
    pump_for(win, 50);
    expect(text_of(find_ctl(shell, IDC_GEN_KNOWN_COUNT)) == L"已知游戏：2 个", L"已知游戏计数刷新为 2");
    ::SetWindowTextW(find_ctl(shell, IDC_GEN_FORCE_EDIT), L"tool.exe");
    click(find_ctl(shell, IDC_GEN_FORCE_ADD));
    expect(host.value("detect.force_game").find(L"tool.exe") != std::wstring::npos &&
               host.value("detect.exclude").find(L"tool.exe") == std::wstring::npos,
           L"把 tool.exe 加入视为游戏 → 从排除名单移除");

    // ---- localization：通用页切换界面语言 → 立即按新语言重建（标题、分页标签、页内文字），留在通用页
    {
        HWND lang = find_ctl(shell, IDC_GEN_LANGUAGE);
        expect(lang != nullptr && ::SendMessageW(lang, CB_GETCOUNT, 0, 0) == 4, L"通用页有界面语言下拉框（4 项）");
        auto pick = [&](int index) {
            HWND cb = find_ctl(shell, IDC_GEN_LANGUAGE);
            ::SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            ::SendMessageW(::GetParent(cb), WM_COMMAND, MAKEWPARAM(IDC_GEN_LANGUAGE, CBN_SELCHANGE),
                           reinterpret_cast<LPARAM>(cb));
            pump_for(win, 150);
        };
        auto tab_text = [&](int k) {
            wchar_t buf[64] = {};
            TCITEMW it{};
            it.mask = TCIF_TEXT;
            it.pszText = buf;
            it.cchTextMax = 64;
            ::SendMessageW(::GetDlgItem(shell, IDC_TAB), TCM_GETITEMW, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(&it));
            return std::wstring(buf);
        };
        pick(3);   // English
        expect(host.value("ui.language") == L"en" && text_of(shell) == L"MotionCalm Settings" && tab_text(0) == L"Mask" &&
                   tab_text(4) == L"Hotkeys" && win.current_page() == 5,
               L"选 English → ui.language=en，标题 / 分页标签变为英文，仍在通用页（实际 " + text_of(shell) + L" / " +
                   tab_text(0) + L"）");
        expect(text_of(find_ctl(shell, IDC_GEN_KNOWN_CLEAR)) == L"Clear &known games…", L"通用页按钮文字为英文");
        pick(2);   // 繁體中文
        expect(host.value("ui.language") == L"zh-TW" && text_of(shell) == L"MotionCalm 設定" && tab_text(4) == L"快速鍵",
               L"选 繁體中文 → 標題「MotionCalm 設定」、分頁「快速鍵」");
        pick(1);   // 简体中文：回到源文本，后面的检查按中文断言
        expect(host.value("ui.language") == L"zh-CN" && text_of(shell) == L"MotionCalm 设置" && tab_text(5) == L"通用" &&
                   text_of(find_ctl(shell, IDC_GEN_KNOWN_COUNT)) == L"已知游戏：2 个",
               L"选 简体中文 → 恢复中文，页内状态（已知游戏 2 个）随重建保留");
    }

    // ---- Ctrl+Tab 循环切页（通用 → 遮罩）
    set_modifiers(true, false, false);
    MSG msg{};
    msg.hwnd = ::GetDlgItem(shell, IDC_TAB);
    msg.message = WM_KEYDOWN;
    msg.wParam = VK_TAB;
    const bool handled = win.pre_translate(msg);
    set_modifiers(false, false, false);
    expect(handled && win.current_page() == 0, L"Ctrl+Tab 从“通用”循环到“遮罩”");

    // ---- 其他来源改了配置（模拟托盘把档位改为轻）→ 刷新
    const SettingValue lv{{"general", "mask_level"}, L"light"};
    host.set(std::span<const SettingValue>(&lv, 1));
    win.refresh();
    pump_for(win, 100);
    expect(::SendMessageW(find_ctl(shell, IDC_MASK_LEVEL_LIGHT), BM_GETCHECK, 0, 0) == BST_CHECKED,
           L"其他来源把档位改为轻 → 单选组随之刷新");
    pump_for(win, 400);
    expect(logged(L"INI saved (debounced 300 ms)", 0), L"改动后 300 ms 防抖保存，状态栏收到 config_saved");

    // ---- 关闭：焦点不在捕获框时按 Esc
    mark = g_log.size();
    ::SetFocus(::GetDlgItem(shell, IDC_TAB));
    MSG esc{};
    esc.hwnd = ::GetDlgItem(shell, IDC_TAB);
    esc.message = WM_KEYDOWN;
    esc.wParam = VK_ESCAPE;
    if (!win.pre_translate(esc)) ::DispatchMessageW(&esc);
    expect(!win.is_open() && logged(L"flush_config", mark) && logged(L"set_preview_floor(0.00)", mark) &&
               logged(L"settings_closed", mark),
           L"Esc 关闭 → flush、预览清零、settings_closed");
    if (win.is_open()) win.close();
    out(L"[selftest] failures: " + std::to_wstring(g_failures));
    return g_failures;
}

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        std::fputs("usage: settingsdemo [--page N] [--exit-after S] [--screenshots DIR] [--conflicts] [--no-capture]\n"
                   "                    [--capture-failed] [--verbose] [--calib-busy] [--fail-autostart] [--empty-calib]\n"
                   "                    [--advanced] [--dpi N] [--focus-hotkey N] [--lang auto|zh-CN|zh-TW|en] [--selftest]\n",
                   stderr);
        return 1;
    }
    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);

    if (!opt.lang.empty()) {
        mc::i18n::LangSetting ls{};
        mc::i18n::parse_lang_setting(opt.lang, &ls);
        mc::i18n::set_language(mc::i18n::resolve(ls));
    }
    out(std::wstring(L"[demo] ui language ") + mc::i18n::lang_code(mc::i18n::current()));
    FakeHost host(inst, opt);
    g_host = &host;
    int rc = 0;
    {
        mc::ui::SettingsWindow win(host);
        host.attach(&win);
        if (opt.dpi > 0) win.set_dpi_override(opt.dpi);
        if (!win.open(inst)) {
            std::fputs("settingsdemo: cannot create the settings window\n", stderr);
            rc = 2;
        } else {
            if (opt.page >= 0) win.select_page(opt.page);
            out(L"[demo] settings window open, hwnd=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(win.hwnd())));
            if (opt.selftest) {
                pump_for(win, 300);
                rc = run_selftest(win, host);
            } else if (!opt.screenshots.empty()) {
                ::CreateDirectoryW(opt.screenshots.c_str(), nullptr);
                // 截图期间置顶，保证屏幕截取（--bitblt）拿到的是这个窗口。
                ::SetWindowPos(win.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                static const wchar_t* kNames[] = {L"mask", L"dots", L"trigger", L"calib", L"hotkeys", L"general"};
                pump_for(win, 500);
                const std::wstring suffix = opt.dpi > 0 ? L"_dpi" + std::to_wstring(opt.dpi) : std::wstring();
                for (int k = 0; k < mc::ui::SettingsWindow::kPageCount; ++k) {
                    win.select_page(k);
                    if (k == 4 && opt.focus_hotkey >= 0) {   // 快捷键页：焦点放进第 N 个捕获框
                        for (HWND c = ::GetWindow(win.hwnd(), GW_CHILD); c != nullptr; c = ::GetWindow(c, GW_HWNDNEXT)) {
                            HWND edit = ::GetDlgItem(c, IDC_HK_EDIT_BASE + opt.focus_hotkey);
                            if (edit != nullptr && ::IsWindowVisible(c)) ::SetFocus(edit);
                        }
                    }
                    pump_for(win, 400);
                    const std::wstring base =
                        opt.screenshots + L"\\settings_" + std::to_wstring(k) + L"_" + kNames[k] + suffix;
                    out((save_window_png(win.hwnd(), base + L".png") ? L"[demo] saved " : L"[demo] FAILED to save ") +
                        base + L".png");
                    const int max_scroll = win.scroll_current_page(0);
                    if (max_scroll > 0) {   // 页内需要滚动：每隔约一屏截一张（_s1、_s2…），最后截一张滚到底的
                        RECT pr{};
                        ::GetClientRect(win.hwnd(), &pr);
                        const int step = std::max(200, static_cast<int>(pr.bottom - pr.top) * 2 / 3);
                        int n = 1;
                        for (int y = step; y < max_scroll; y += step, ++n) {
                            win.scroll_current_page(y);
                            pump_for(win, 250);
                            const std::wstring mid = base + L"_s" + std::to_wstring(n) + L".png";
                            out((save_window_png(win.hwnd(), mid) ? L"[demo] saved " : L"[demo] FAILED to save ") + mid);
                        }
                        win.scroll_current_page(max_scroll);
                        pump_for(win, 300);
                        out((save_window_png(win.hwnd(), base + L"_end.png") ? L"[demo] saved "
                                                                             : L"[demo] FAILED to save ") +
                            base + L"_end.png");
                        win.scroll_current_page(0);
                    }
                }
                win.close();
            } else {
                UINT_PTR timer = 0;
                if (opt.exit_after > 0) {
                    timer = ::SetTimer(nullptr, 0, static_cast<UINT>(opt.exit_after * 1000.0), nullptr);
                }
                MSG msg;
                while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    if (msg.message == WM_TIMER && msg.hwnd == nullptr && msg.wParam == timer) {
                        out(L"[demo] exit-after reached");
                        win.close();
                        continue;
                    }
                    if (win.pre_translate(msg)) continue;
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
                if (timer != 0) ::KillTimer(nullptr, timer);
            }
        }
        host.attach(nullptr);
    }
    g_host = nullptr;
    Gdiplus::GdiplusShutdown(gdiplus);
    return rc;
}
