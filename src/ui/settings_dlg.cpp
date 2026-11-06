// settings_dlg.cpp — ui/settings_dlg 的实现（settings-window design D3 / D9 / D13 + docs/settings-ui.md §1 / §4 / §7）。
#include "ui/settings_dlg.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cwchar>

#include "i18n/i18n.h"
#include "res/resource.h"
#include "util/strings.h"

namespace mc::ui {
namespace {

constexpr UINT WM_APP_REFRESH = WM_APP + 31;    // 合并后的全页刷新
constexpr UINT_PTR kNoticeTimer = 1;            // 状态栏操作结果的显示时长
constexpr UINT kNoticeMs = 8000;

using S = i18n::StrId;
using i18n::tr;

constexpr S kTabNames[SettingsWindow::kPageCount] = {S::TabMask,  S::TabDots,    S::TabTrigger,
                                                     S::TabCalib, S::TabHotkeys, S::TabGeneral};
constexpr int kPageTemplates[SettingsWindow::kPageCount] = {IDD_PAGE_MASK,  IDD_PAGE_DOTS,    IDD_PAGE_TRIGGER,
                                                            IDD_PAGE_CALIB, IDD_PAGE_HOTKEYS, IDD_PAGE_GENERAL};

// 外壳版式（96 DPI 像素）：窗口客户区 644 × 600，左右边距 12，状态栏高 52。
constexpr int kClientW = 644;
constexpr int kClientH = 600;
constexpr int kShellMargin = 12;
constexpr int kTabTop = 10;
constexpr int kStatusH = 52;
constexpr COLORREF kFrameColor = RGB(0xD9, 0xD9, 0xD9);

constexpr SettingKey kShowAdvanced{"ui", "show_advanced"};
constexpr SettingKey kPosX{"ui", "x"};
constexpr SettingKey kPosY{"ui", "y"};

// comctl32 v6 / 较新 user32 的函数动态取：tests.exe 没有 v6 manifest，静态导入会让它加载失败。
using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
using SetDialogDpiChangeBehaviorFn = BOOL(WINAPI*)(HWND, int, int);
constexpr int kDdcDisableAll = 0x0001;   // DDC_DISABLE_ALL：DPI 变化完全由本窗口自己处理（控件全由代码排版）

template <class Fn>
Fn comctl_proc(const char* name) {
    HMODULE m = ::GetModuleHandleW(L"comctl32.dll");
    return m != nullptr ? reinterpret_cast<Fn>(::GetProcAddress(m, name)) : nullptr;
}

void disable_dialog_dpi_handling(HWND h) {
    static SetDialogDpiChangeBehaviorFn fn = [] {
        HMODULE m = ::GetModuleHandleW(L"user32.dll");
        return m != nullptr ? reinterpret_cast<SetDialogDpiChangeBehaviorFn>(::GetProcAddress(m, "SetDialogDpiChangeBehavior"))
                            : nullptr;
    }();
    if (fn != nullptr) fn(h, kDdcDisableAll, kDdcDisableAll);
}

HFONT create_ui_font(int dpi, int tenth_points, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(tenth_points, dpi, 720);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, tr(S::FontUi));   // 按界面语言：雅黑 / 正黑 / Segoe UI
    return ::CreateFontIndirectW(&lf);
}

}  // namespace

int SettingsWindow::s_last_page_ = 0;

SettingsWindow::SettingsWindow(SettingsHost& host) : host_(host) {}

SettingsWindow::~SettingsWindow() {
    close();
    free_fonts();
}

bool SettingsWindow::open(HINSTANCE instance) {
    if (hwnd_ != nullptr) {
        if (::IsIconic(hwnd_)) ::ShowWindow(hwnd_, SW_RESTORE);
        ::SetForegroundWindow(hwnd_);
        return true;
    }
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                                             ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    ::InitCommonControlsEx(&icc);
    instance_ = instance;
    preview_ = 0.0f;
    closing_ = false;
    notice_.clear();
    HWND h = ::CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, &SettingsWindow::proc,
                                  reinterpret_cast<LPARAM>(this));
    if (h == nullptr) return false;
    place_on_open();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    return true;
}

void SettingsWindow::close() {
    if (hwnd_ == nullptr || closing_) return;
    closing_ = true;
    if (Page* p = pages_[static_cast<std::size_t>(current_)].get()) p->commit_focus();
    save_position();
    host_.flush_config();
    preview_ = 0.0f;
    host_.set_preview_floor(0.0f);
    for (auto& p : pages_) {
        if (p) p->on_deactivate();   // 快捷键页：如全局热键处于暂停则恢复
    }
    if (in_size_move_) {
        in_size_move_ = false;
        host_.modal_end();
    }
    s_last_page_ = current_;
    ::KillTimer(hwnd_, kNoticeTimer);
    ::DestroyWindow(hwnd_);   // 子页随之销毁（WM_NCDESTROY 把各自的句柄置空）
    hwnd_ = nullptr;
    tab_ = status_ = restore_ = close_ = nullptr;
    for (auto& p : pages_) p.reset();
    free_icons();
    refresh_pending_ = false;
    closing_ = false;
    host_.settings_closed();
}

bool SettingsWindow::pre_translate(MSG& msg) {
    if (hwnd_ == nullptr) return false;
    if (msg.hwnd != hwnd_ && !::IsChild(hwnd_, msg.hwnd)) return false;
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && ::GetKeyState(VK_CONTROL) < 0 &&
        ::GetKeyState(VK_MENU) >= 0) {
        const bool back = ::GetKeyState(VK_SHIFT) < 0;
        select_page((current_ + (back ? kPageCount - 1 : 1)) % kPageCount);
        return true;
    }
    const bool handled = ::IsDialogMessageW(hwnd_, &msg) != FALSE;
    if (handled && (msg.message == WM_KEYDOWN || msg.message == WM_SYSCHAR || msg.message == WM_SYSKEYDOWN)) {
        // Tab / 访问键把焦点移到了滚动区域之外：滚进可见区域。
        if (Page* p = pages_[static_cast<std::size_t>(current_)].get()) p->ensure_visible(::GetFocus());
    }
    return handled;
}

void SettingsWindow::refresh() {
    if (hwnd_ == nullptr || refresh_pending_) return;
    refresh_pending_ = ::PostMessageW(hwnd_, WM_APP_REFRESH, 0, 0) != FALSE;
}

void SettingsWindow::config_saved() {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    last_saved_ = buf;
    update_status();
}

void SettingsWindow::select_page(int index) {
    if (hwnd_ == nullptr) return;
    index = std::clamp(index, 0, kPageCount - 1);
    HWND focus = ::GetFocus();
    const int old = current_;
    current_ = index;
    if (static_cast<int>(::SendMessageW(tab_, TCM_GETCURSEL, 0, 0)) != index) {
        ::SendMessageW(tab_, TCM_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    for (int k = 0; k < kPageCount; ++k) {
        Page* p = pages_[static_cast<std::size_t>(k)].get();
        if (p == nullptr || p->hwnd() == nullptr) continue;
        if (k == index) {
            ::ShowWindow(p->hwnd(), SW_SHOWNA);
            p->relayout();
        } else if (::IsWindowVisible(p->hwnd())) {
            if (k == old && focus != nullptr && ::IsChild(p->hwnd(), focus)) {
                p->commit_focus();
                ::SetFocus(tab_);   // 焦点不能留在隐藏的页上
            }
            ::ShowWindow(p->hwnd(), SW_HIDE);
            p->on_deactivate();
        }
    }
}

int SettingsWindow::scroll_current_page(int y) {
    Page* p = hwnd_ != nullptr ? pages_[static_cast<std::size_t>(current_)].get() : nullptr;
    return p != nullptr ? p->scroll_to(y) : 0;
}

// ---------------------------------------------------------------- PageSite

void SettingsWindow::set_preview(float p) {
    preview_ = std::clamp(p, 0.0f, 1.0f);
    host_.set_preview_floor(preview_);
}

bool SettingsWindow::confirm(const ConfirmText& text) {
    static TaskDialogIndirectFn task_dialog = comctl_proc<TaskDialogIndirectFn>("TaskDialogIndirect");
    host_.modal_begin();
    bool yes = false;
    if (task_dialog != nullptr) {
        constexpr int kVerbId = 1001;
        TASKDIALOG_BUTTON verb{kVerbId, text.verb.c_str()};
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof cfg;
        cfg.hwndParent = hwnd_;
        cfg.hInstance = instance_;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        cfg.pszWindowTitle = tr(S::SetTitle);
        cfg.pszMainIcon = TD_WARNING_ICON;
        cfg.pszMainInstruction = text.instruction.c_str();
        cfg.pszContent = text.content.c_str();
        cfg.cButtons = 1;
        cfg.pButtons = &verb;
        cfg.nDefaultButton = IDCANCEL;   // 破坏性操作：默认按钮是"取消"
        int pressed = 0;
        yes = SUCCEEDED(task_dialog(&cfg, &pressed, nullptr, nullptr)) && pressed == kVerbId;
    } else {
        const std::wstring body = text.instruction + L"\n\n" + text.content;
        yes = ::MessageBoxW(hwnd_, body.c_str(), tr(S::SetTitle), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }
    host_.modal_end();
    return yes;
}

void SettingsWindow::notice(const std::wstring& text) {
    notice_ = text;
    update_status();
    if (hwnd_ != nullptr) ::SetTimer(hwnd_, kNoticeTimer, kNoticeMs, nullptr);
}

bool SettingsWindow::show_advanced() const {
    if (const auto v = host_.get(kShowAdvanced)) return trim(std::wstring_view(*v)) == L"1";
    return advanced_local_;
}

void SettingsWindow::set_show_advanced(bool on) {
    advanced_local_ = on;
    const SettingValue v{kShowAdvanced, on ? L"1" : L"0"};
    if (host_.get(kShowAdvanced)) host_.set(std::span<const SettingValue>(&v, 1));
    refresh();
}

// ---------------------------------------------------------------- 窗口过程

INT_PTR CALLBACK SettingsWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<SettingsWindow*>(lp);
        ::SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
    }
    if (self == nullptr) return FALSE;
    if (msg == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, DWLP_USER, 0);
        self->hwnd_ = nullptr;
        return FALSE;
    }
    return self->handle(msg, wp, lp);
}

UiStyle SettingsWindow::make_fonts(int dpi) {
    const UiStyle old = style_;   // 由调用方在控件换上新字体之后删除
    style_.dpi = dpi;
    style_.body = create_ui_font(dpi, 90, FW_NORMAL);
    style_.heading = create_ui_font(dpi, 105, FW_SEMIBOLD);
    return old;
}

void SettingsWindow::free_fonts() {
    if (style_.body != nullptr) ::DeleteObject(style_.body);
    if (style_.heading != nullptr) ::DeleteObject(style_.heading);
    style_.body = style_.heading = nullptr;
}

void SettingsWindow::load_icons() {
    free_icons();
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    icon_small_ = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                  ::GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                  ::GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
    icon_big_ = static_cast<HICON>(::LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                ::GetSystemMetricsForDpi(SM_CXICON, dpi),
                                                ::GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
    if (icon_small_ != nullptr) ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));
    if (icon_big_ != nullptr) ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
}

void SettingsWindow::free_icons() {
    if (icon_small_ != nullptr) ::DestroyIcon(icon_small_);
    if (icon_big_ != nullptr) ::DestroyIcon(icon_big_);
    icon_small_ = icon_big_ = nullptr;
}

void SettingsWindow::apply_title_theme() {
    // 标题栏跟随系统"应用"深 / 浅色；客户区保持系统标准浅色（原生控件不支持完整深色）。
    DWORD light = 1;
    DWORD size = sizeof light;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) {
        light = 1;
    }
    const BOOL dark = light == 0 ? TRUE : FALSE;
    constexpr DWORD kImmersiveDarkMode = 20;      // DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD kImmersiveDarkModeOld = 19;   // Windows 10 20H1 之前的值
    if (FAILED(::DwmSetWindowAttribute(hwnd_, kImmersiveDarkMode, &dark, sizeof dark))) {
        ::DwmSetWindowAttribute(hwnd_, kImmersiveDarkModeOld, &dark, sizeof dark);
    }
}

void SettingsWindow::on_init() {
    disable_dialog_dpi_handling(hwnd_);
    built_lang_ = i18n::current();
    const UiStyle old = make_fonts(dpi_override_ > 0 ? dpi_override_ : static_cast<int>(::GetDpiForWindow(hwnd_)));
    if (old.body != nullptr) ::DeleteObject(old.body);   // 上一次打开窗口时的字体
    if (old.heading != nullptr) ::DeleteObject(old.heading);
    load_icons();
    apply_title_theme();

    // 创建顺序 = Z 序 = Tab 顺序：分页标签 → 分页内容 → 状态栏按钮（与视觉顺序一致）。
    tab_ = ::CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                             hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), instance_, nullptr);
    ::SendMessageW(tab_, WM_SETFONT, reinterpret_cast<WPARAM>(style_.body), FALSE);
    ::SendMessageW(tab_, TCM_SETPADDING, 0, MAKELPARAM(style_.px(14), style_.px(5)));
    fill_tabs();
    size_window();
    create_pages();

    status_ = ::CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX |
                                                       SS_ENDELLIPSIS,
                                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_TEXT)),
                                instance_, nullptr);
    restore_ = ::CreateWindowExW(0, L"Button", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                                 hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESTORE_DEFAULTS)), instance_,
                                 nullptr);
    close_ = ::CreateWindowExW(0, L"Button", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                               hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance_, nullptr);
    for (HWND h : {status_, restore_, close_}) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(style_.body), FALSE);
    set_shell_texts();
    layout_shell();
    update_status();

    current_ = -1;
    select_page(s_last_page_);
    if (current_ < 0) select_page(0);
}

void SettingsWindow::fill_tabs() {
    ::SendMessageW(tab_, TCM_DELETEALLITEMS, 0, 0);
    for (int k = 0; k < kPageCount; ++k) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tr(kTabNames[k]));
        ::SendMessageW(tab_, TCM_INSERTITEMW, static_cast<WPARAM>(k), reinterpret_cast<LPARAM>(&item));
    }
}

void SettingsWindow::set_shell_texts() {
    ::SetWindowTextW(hwnd_, tr(S::SetTitle));
    if (restore_ != nullptr) ::SetWindowTextW(restore_, tr(S::BtnRestoreDefaults));
    if (close_ != nullptr) ::SetWindowTextW(close_, tr(S::BtnClose));
}

void SettingsWindow::create_pages() {
    using Factory = std::unique_ptr<Page> (*)(SettingsHost&, PageSite&);
    constexpr Factory kFactories[kPageCount] = {make_mask_page,  make_dots_page,    make_trigger_page,
                                                make_calib_page, make_hotkeys_page, make_general_page};
    const RECT frame = page_frame();
    HWND after = tab_;   // Z 序：分页紧跟在分页标签之后、状态栏按钮之前（Tab 顺序 = 视觉顺序）
    for (int k = 0; k < kPageCount; ++k) {
        auto page = kFactories[k](host_, *this);
        if (page->create(hwnd_, kPageTemplates[k])) {
            disable_dialog_dpi_handling(page->hwnd());
            ::SetWindowPos(page->hwnd(), after, frame.left + 1, frame.top + 1, frame.right - frame.left - 2,
                           frame.bottom - frame.top - 2, SWP_NOACTIVATE);
            after = page->hwnd();
            pages_[static_cast<std::size_t>(k)] = std::move(page);
        }
    }
}

void SettingsWindow::rebuild_for_language() {
    // 界面语言变了（通用页的语言下拉框、托盘之外的其他来源、外部改 INI 后重载）：按新语言重建全部分页，
    // 保留当前分页与页内滚动位置；焦点原来在语言下拉框上时放回新的下拉框。
    built_lang_ = i18n::current();
    const int page = current_;
    Page* cur = page >= 0 ? pages_[static_cast<std::size_t>(page)].get() : nullptr;
    const int scroll = cur != nullptr ? cur->scroll_pos() : 0;
    HWND focus = ::GetFocus();
    const bool focus_on_language = focus != nullptr && ::GetDlgCtrlID(focus) == IDC_GEN_LANGUAGE;
    ::SetFocus(tab_);
    for (auto& p : pages_) {
        if (p) p->on_deactivate();
        p.reset();   // 销毁子页窗口
    }
    const UiStyle old = make_fonts(style_.dpi);   // 字体随语言变化
    for (HWND h : {tab_, status_, restore_, close_}) {
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(style_.body), FALSE);
    }
    if (old.body != nullptr) ::DeleteObject(old.body);
    if (old.heading != nullptr) ::DeleteObject(old.heading);
    fill_tabs();
    set_shell_texts();
    layout_shell();
    create_pages();
    current_ = -1;
    select_page(page);
    if (Page* p = pages_[static_cast<std::size_t>(current_)].get()) {
        if (scroll > 0) p->scroll_to(scroll);
        if (focus_on_language) {
            if (HWND combo = ::GetDlgItem(p->hwnd(), IDC_GEN_LANGUAGE)) ::SetFocus(combo);
        }
    }
    update_status();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::size_window() {
    const UINT real_dpi = ::GetDpiForWindow(hwnd_);
    RECT rc{0, 0, style_.px(kClientW), style_.px(kClientH)};
    const auto st = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const auto ex = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    ::AdjustWindowRectExForDpi(&rc, st, FALSE, ex, real_dpi);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT SettingsWindow::page_frame() const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    RECT item{};
    ::SendMessageW(tab_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
    const int tab_bottom = style_.px(kTabTop) + item.bottom + style_.px(1);
    return RECT{style_.px(kShellMargin), tab_bottom, client.right - style_.px(kShellMargin),
                client.bottom - style_.px(kStatusH)};
}

void SettingsWindow::layout_shell() {
    if (tab_ == nullptr) return;
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const UiStyle& s = style_;
    RECT item{};
    ::SendMessageW(tab_, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
    // 分页标签只占一条标签带，分页内容在它下方（不与 Tab 控件重叠：Z 序与 Tab 顺序都按视觉顺序）。
    ::SetWindowPos(tab_, nullptr, s.px(kShellMargin), s.px(kTabTop), client.right - 2 * s.px(kShellMargin),
                   item.bottom + s.px(2), SWP_NOZORDER | SWP_NOACTIVATE);
    const RECT frame = page_frame();
    for (auto& p : pages_) {
        if (!p || p->hwnd() == nullptr) continue;
        ::SetWindowPos(p->hwnd(), nullptr, frame.left + 1, frame.top + 1, frame.right - frame.left - 2,
                       frame.bottom - frame.top - 2, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // 状态栏：左侧状态文字，右侧"恢复默认…""关闭"。
    const int bar_top = client.bottom - s.px(kStatusH);
    const int btn_h = s.px(28);
    const int btn_y = bar_top + (s.px(kStatusH) - btn_h) / 2;
    const int close_w = s.px(88), restore_w = s.px(112);
    const int close_x = client.right - s.px(16) - close_w;
    const int restore_x = close_x - s.px(8) - restore_w;
    ::SetWindowPos(close_, nullptr, close_x, btn_y, close_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(restore_, nullptr, restore_x, btn_y, restore_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(status_, nullptr, s.px(16), bar_top, std::max(restore_x - s.px(28), 1), s.px(kStatusH),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void SettingsWindow::update_status() {
    if (status_ == nullptr) return;
    std::wstring s = notice_.empty() ? std::wstring(tr(S::StatusIdle)) : notice_;
    if (!last_saved_.empty()) s += i18n::format(S::StatusLastSaved, {{L"time", last_saved_}});
    ::SetWindowTextW(status_, s.c_str());
}

void SettingsWindow::place_on_open() {
    POINT cursor{};
    ::GetCursorPos(&cursor);
    HMONITOR mon = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!::GetMonitorInfoW(mon, &mi)) return;
    const RECT& work = mi.rcWork;
    // 上次的位置（[ui] x / y）在鼠标所在显示器上就用它，否则居中于该显示器（spec：出现在鼠标指针所在显示器上）。
    std::optional<POINT> saved;
    const auto sx = host_.get(kPosX), sy = host_.get(kPosY);
    if (sx && sy) {
        const auto x = parse_number_text(*sx), y = parse_number_text(*sy);
        if (x && y) {
            const POINT pt{static_cast<LONG>(*x), static_cast<LONG>(*y)};
            if (::MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == mon) saved = pt;
        }
    }
    // 两遍：挪到 DPI 不同的显示器时系统会发 WM_DPICHANGED 并改变尺寸，第二遍按新尺寸摆放。
    for (int pass = 0; pass < 2; ++pass) {
        RECT wr{};
        ::GetWindowRect(hwnd_, &wr);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int x = saved ? saved->x : work.left + ((work.right - work.left) - w) / 2;
        int y = saved ? saved->y : work.top + ((work.bottom - work.top) - h) / 2;
        x = std::clamp(x, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - w));
        y = std::clamp(y, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - h));
        ::SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void SettingsWindow::save_position() {
    if (hwnd_ == nullptr || ::IsIconic(hwnd_) || !host_.get(kPosX) || !host_.get(kPosY)) return;
    RECT wr{};
    ::GetWindowRect(hwnd_, &wr);
    const std::vector<SettingValue> v{{kPosX, std::to_wstring(wr.left)}, {kPosY, std::to_wstring(wr.top)}};
    if (host_.get(kPosX) == v[0].value && host_.get(kPosY) == v[1].value) return;
    host_.set(v);
}

INT_PTR SettingsWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            on_init();
            ::SetFocus(tab_);
            return FALSE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDCANCEL:   // "关闭"按钮、Esc（焦点不在捕获框时）
                    close();
                    return TRUE;
                case IDOK:       // Enter：只提交当前焦点编辑框，不关窗口
                    if (Page* p = pages_[static_cast<std::size_t>(current_)].get()) p->commit_focus();
                    return TRUE;
                case IDC_RESTORE_DEFAULTS:
                    if (HIWORD(wp) == BN_CLICKED && confirm(restore_defaults_confirm())) {
                        host_.restore_defaults();
                        notice(tr(S::NoticeRestored));
                        refresh();
                    }
                    return TRUE;
                default: return FALSE;
            }
        case WM_CLOSE:   // 标题栏关闭按钮、Alt+F4
            close();
            return TRUE;
        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->hwndFrom == tab_ && nm->code == TCN_SELCHANGE) {
                select_page(static_cast<int>(::SendMessageW(tab_, TCM_GETCURSEL, 0, 0)));
                return TRUE;
            }
            return FALSE;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd_, &ps);
            const RECT frame = page_frame();
            HBRUSH br = ::CreateSolidBrush(kFrameColor);
            ::FrameRect(dc, &frame, br);   // 分页内容区的浅灰细边
            ::DeleteObject(br);
            ::EndPaint(hwnd_, &ps);
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kNoticeTimer) {
                ::KillTimer(hwnd_, kNoticeTimer);
                notice_.clear();
                update_status();
                return TRUE;
            }
            return FALSE;
        case WM_APP_REFRESH:
            refresh_pending_ = false;
            if (i18n::current() != built_lang_) {   // localization：界面语言变了 → 按新语言重建
                rebuild_for_language();
                return TRUE;
            }
            for (auto& p : pages_) {
                if (p) p->load();
            }
            return TRUE;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                // 兜底：窗口失去激活时恢复全局热键（重新激活后焦点回到捕获框会再次暂停）。
                for (auto& p : pages_) {
                    if (p) p->on_deactivate();
                }
            }
            return FALSE;
        case WM_ENTERSIZEMOVE:   // 拖动标题栏进入系统模态循环：宿主用计时器兜底 tick
            if (!in_size_move_) {
                in_size_move_ = true;
                host_.modal_begin();
            }
            return FALSE;
        case WM_EXITSIZEMOVE:
            if (in_size_move_) {
                in_size_move_ = false;
                host_.modal_end();
            }
            save_position();
            return FALSE;
        case WM_SETTINGCHANGE:
            if (lp != 0 && std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                apply_title_theme();
                ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return FALSE;
        case WM_DPICHANGED: {
            // 控件全由代码按像素排版：自己处理 DPI 变化（对话框管理器的自动缩放已关掉）。
            UiStyle old{};
            if (dpi_override_ == 0) old = make_fonts(HIWORD(wp));
            const auto* r = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            load_icons();
            for (HWND h : {tab_, status_, restore_, close_}) {
                ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(style_.body), FALSE);
            }
            ::SendMessageW(tab_, TCM_SETPADDING, 0, MAKELPARAM(style_.px(14), style_.px(5)));
            size_window();
            layout_shell();
            for (auto& p : pages_) {
                if (p) p->apply_style();
            }
            if (old.body != nullptr) ::DeleteObject(old.body);
            if (old.heading != nullptr) ::DeleteObject(old.heading);
            return TRUE;
        }
        default: return FALSE;
    }
}

}  // namespace mc::ui
