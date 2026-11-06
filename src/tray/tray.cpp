// tray.cpp — tray/tray 的实现：NOTIFYICONDATAW（uID=1，版本 4）、TaskbarCreated 重新添加、按 tray-and-hotkeys spec 的结构动态构建菜单（含变更 2 的校准项与"校准列表"子菜单、变更 3 的"截屏运动检测"、变更 4 的默认项"设置…"）。
#include "tray/tray.h"

#include <cwchar>
#include <format>

#include "i18n/i18n.h"
#include "res/resource.h"
#include "util/log.h"

namespace mc {

std::wstring_view level_name_zh(MaskLevel level) {
    switch (level) {
        case MaskLevel::Light: return L"轻";
        case MaskLevel::Medium: return L"中";
        case MaskLevel::Strong: return L"强";
    }
    return L"中";
}

std::wstring_view sensitivity_name_zh(Sensitivity s) {
    switch (s) {
        case Sensitivity::Low: return L"低";
        case Sensitivity::Medium: return L"中";
        case Sensitivity::High: return L"高";
    }
    return L"中";
}

std::wstring_view level_name(MaskLevel level) {
    switch (level) {
        case MaskLevel::Light: return i18n::tr(i18n::StrId::LevelLight);
        case MaskLevel::Medium: return i18n::tr(i18n::StrId::LevelMedium);
        case MaskLevel::Strong: return i18n::tr(i18n::StrId::LevelStrong);
    }
    return i18n::tr(i18n::StrId::LevelMedium);
}

std::wstring_view sensitivity_name(Sensitivity s) {
    switch (s) {
        case Sensitivity::Low: return i18n::tr(i18n::StrId::SensLow);
        case Sensitivity::Medium: return i18n::tr(i18n::StrId::SensMedium);
        case Sensitivity::High: return i18n::tr(i18n::StrId::SensHigh);
    }
    return i18n::tr(i18n::StrId::SensMedium);
}

std::wstring format_status_line(bool has_game, std::wstring_view exe, bool locked, bool enabled, MaskLevel level) {
    using i18n::StrId;
    const std::wstring mask(enabled ? level_name(level) : std::wstring_view(i18n::tr(StrId::TrayMaskOff)));
    std::wstring game;
    if (has_game && !exe.empty()) {
        game = i18n::format(locked ? StrId::TrayGameLocked : StrId::TrayGameUnlocked, {{L"exe", std::wstring(exe)}});
    } else {
        game = i18n::tr(StrId::None);
    }
    return i18n::format(StrId::TrayStatus, {{L"game", game}, {L"mask", mask}});
}

std::wstring format_capture_item(bool cli_disabled, bool failed) {
    std::wstring s = i18n::tr(i18n::StrId::CaptureName);
    if (cli_disabled) {
        s += i18n::tr(i18n::StrId::CaptureSuffixCli);
    } else if (failed) {
        s += i18n::tr(i18n::StrId::CaptureSuffixFailed);
    }
    return s;
}

std::wstring format_calibration_item(std::wstring_view exe, double counts_per_degree) {
    return i18n::format(i18n::StrId::TrayCalibItem,
                        {{L"exe", std::wstring(exe)}, {L"cpd", std::format(L"{:.1f}", counts_per_degree)}});
}

UINT Tray::taskbar_created_message() { return ::RegisterWindowMessageW(L"TaskbarCreated"); }

NOTIFYICONDATAW Tray::make_data() const {
    NOTIFYICONDATAW d{};
    d.cbSize = sizeof d;
    d.hWnd = owner_;
    d.uID = 1;
    d.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    d.uCallbackMessage = callback_;
    d.hIcon = icon_;
    wcscpy_s(d.szTip, L"MotionCalm");
    return d;
}

bool Tray::add(HWND owner, UINT callback_message, HICON icon) {
    owner_ = owner;
    callback_ = callback_message;
    icon_ = icon;
    NOTIFYICONDATAW d = make_data();
    if (!::Shell_NotifyIconW(NIM_ADD, &d)) {
        log::warn("Shell_NotifyIcon(NIM_ADD) failed; will retry when the taskbar is (re)created");
        added_ = false;
        return false;
    }
    d.uVersion = NOTIFYICON_VERSION_4;
    if (!::Shell_NotifyIconW(NIM_SETVERSION, &d)) log::warn("Shell_NotifyIcon(NIM_SETVERSION 4) failed");
    added_ = true;
    return true;
}

bool Tray::readd() {
    if (owner_ == nullptr) return false;
    if (added_) {
        NOTIFYICONDATAW d = make_data();
        ::Shell_NotifyIconW(NIM_DELETE, &d);   // 新的资源管理器里本来就没有，失败无妨
        added_ = false;
    }
    const bool ok = add(owner_, callback_, icon_);
    log::info("tray icon re-added after TaskbarCreated: {}", ok ? "ok" : "failed");
    return ok;
}

void Tray::remove() {
    if (!added_) return;
    NOTIFYICONDATAW d = make_data();
    ::Shell_NotifyIconW(NIM_DELETE, &d);
    added_ = false;
}

UINT Tray::show_menu(const TrayMenuState& st, POINT pt) {
    using i18n::StrId;
    using i18n::tr;
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return kTrayCmdNone;
    // 菜单文字：标签 + \t + 热键规范串（未绑定时不显示热键列）。
    auto label = [&](std::wstring_view text, HotkeyAction a, std::wstring_view suffix = {}) {
        std::wstring s(text);
        const std::wstring& hk = st.hotkey_text[hotkey_index(a)];
        if (!hk.empty()) {
            s += L'\t';
            s += hk;
            s += suffix;
        }
        return s;
    };
    auto checked = [](bool on) -> UINT { return on ? MF_CHECKED : MF_UNCHECKED; };

    const std::wstring title = std::format(L"MotionCalm v{}.{}", MC_VERSION_MAJOR, MC_VERSION_MINOR);
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 1, title.c_str());
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 2, st.status_line.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(menu, MF_STRING | checked(st.enabled), kTrayCmdToggle,
                  label(tr(StrId::TrayEnable), HotkeyAction::Toggle).c_str());

    HMENU level = ::CreatePopupMenu();
    ::AppendMenuW(level, MF_STRING, kTrayCmdLevelLight, tr(StrId::LevelLight));
    ::AppendMenuW(level, MF_STRING, kTrayCmdLevelMedium, tr(StrId::LevelMedium));
    ::AppendMenuW(level, MF_STRING, kTrayCmdLevelStrong, tr(StrId::LevelStrong));
    ::CheckMenuRadioItem(level, kTrayCmdLevelLight, kTrayCmdLevelStrong,
                         kTrayCmdLevelLight + static_cast<UINT>(st.level), MF_BYCOMMAND);
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(level),
                  label(tr(StrId::TrayLevel), HotkeyAction::CycleLevel, tr(StrId::TrayCycleSuffix)).c_str());

    HMENU sens = ::CreatePopupMenu();
    ::AppendMenuW(sens, MF_STRING, kTrayCmdSensLow, tr(StrId::SensLow));
    ::AppendMenuW(sens, MF_STRING, kTrayCmdSensMedium, tr(StrId::SensMedium));
    ::AppendMenuW(sens, MF_STRING, kTrayCmdSensHigh, tr(StrId::SensHigh));
    ::CheckMenuRadioItem(sens, kTrayCmdSensLow, kTrayCmdSensHigh, kTrayCmdSensLow + static_cast<UINT>(st.sensitivity),
                         MF_BYCOMMAND);
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sens), tr(StrId::TraySensitivity));

    ::AppendMenuW(menu, MF_STRING | checked(st.center_dot), kTrayCmdCenterDot,
                  label(tr(StrId::TrayCenterDot), HotkeyAction::ToggleCenterDot).c_str());
    ::AppendMenuW(menu, MF_STRING | checked(st.edge_dots), kTrayCmdEdgeDots,
                  label(tr(StrId::TrayEdgeDots), HotkeyAction::ToggleEdgeDots).c_str());
    // 变更 3：--no-capture 时灰显（热键也只弹 toast），所以不显示热键列；勾选状态始终是 INI 的值。
    const std::wstring capture_text = format_capture_item(st.capture_cli_disabled, st.capture_failed);
    const UINT capture_grayed = st.capture_cli_disabled ? static_cast<UINT>(MF_GRAYED) : 0u;
    ::AppendMenuW(menu, MF_STRING | checked(st.capture) | capture_grayed, kTrayCmdCapture,
                  st.capture_cli_disabled ? capture_text.c_str()
                                          : label(capture_text, HotkeyAction::ToggleCapture).c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const UINT program = st.has_current_program ? 0u : static_cast<UINT>(MF_GRAYED);
    ::AppendMenuW(menu, MF_STRING | program, kTrayCmdMarkGame,
                  label(tr(StrId::TrayMarkGame), HotkeyAction::MarkGame).c_str());
    ::AppendMenuW(menu, MF_STRING | program, kTrayCmdExcludeApp,
                  label(tr(StrId::TrayExcludeApp), HotkeyAction::ExcludeApp).c_str());
    ::AppendMenuW(menu, MF_STRING, kTrayCmdCalibrate, label(tr(StrId::TrayCalibrate), HotkeyAction::Calibrate).c_str());
    HMENU calib = ::CreatePopupMenu();
    if (st.calibrations.empty()) {
        ::AppendMenuW(calib, MF_STRING | MF_GRAYED, kTrayCmdCalibEmpty, tr(StrId::TrayCalibEmpty));
    } else {
        const std::size_t max_items = kTrayCmdCalibDeleteLast - kTrayCmdCalibDeleteBase + 1;
        for (std::size_t i = 0; i < st.calibrations.size() && i < max_items; ++i) {
            const auto& [exe, cpd] = st.calibrations[i];
            ::AppendMenuW(calib, MF_STRING, kTrayCmdCalibDeleteBase + static_cast<UINT>(i),
                          format_calibration_item(exe, cpd).c_str());
        }
    }
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(calib), tr(StrId::TrayCalibList));
    // 变更 4："设置…"是菜单默认项（加粗）；双击托盘图标执行的也是它。
    ::AppendMenuW(menu, MF_STRING, kTrayCmdSettings, label(tr(StrId::TraySettings), HotkeyAction::OpenSettings).c_str());
    ::SetMenuDefaultItem(menu, kTrayCmdSettings, FALSE);
    ::AppendMenuW(menu, MF_STRING, kTrayCmdOpenConfig, tr(StrId::TrayOpenConfig));
    ::AppendMenuW(menu, MF_STRING, kTrayCmdReload, tr(StrId::TrayReload));
    ::AppendMenuW(menu, MF_STRING | checked(st.debug_bar), kTrayCmdDebugBar,
                  label(tr(StrId::TrayDebug), HotkeyAction::DebugBar).c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(menu, MF_STRING | checked(st.autostart), kTrayCmdAutostart, tr(StrId::TrayAutostart));
    ::AppendMenuW(menu, MF_STRING, kTrayCmdOpenLog, tr(StrId::TrayOpenLog));
    ::AppendMenuW(menu, MF_STRING, kTrayCmdAbout, tr(StrId::TrayAbout));
    ::AppendMenuW(menu, MF_STRING, kTrayCmdExit, label(tr(StrId::TrayExit), HotkeyAction::Exit).c_str());

    // 没有这一步，点菜单外面时菜单不会关闭（design D11）。
    ::SetForegroundWindow(owner_);
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN;
    flags |= ::GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const BOOL cmd = ::TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner_, nullptr);
    ::PostMessageW(owner_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);   // 连同子菜单一起销毁
    return cmd > 0 ? static_cast<UINT>(cmd) : static_cast<UINT>(kTrayCmdNone);
}

}  // namespace mc
