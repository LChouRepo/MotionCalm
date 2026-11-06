// overlay.cpp — overlay/overlay 的实现：WS_EX_NOREDIRECTIONBITMAP|TRANSPARENT|LAYERED|TOPMOST|TOOLWINDOW|NOACTIVATE 窗口、显示器跟随、帧键比较与按需 Present。
#include "overlay/overlay.h"

#include <ShellScalingApi.h>

#include <algorithm>
#include <cmath>

#include "util/clock.h"
#include "util/log.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace mc {
namespace {

constexpr wchar_t kOverlayClass[] = L"MotionCalm.Overlay";
constexpr DWORD kOverlayExStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST |
                                  WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

float unit(std::uint8_t c) { return static_cast<float>(c) / 255.0f; }

}  // namespace

bool OverlayFrame::has_text() const {
    return (!toast.empty() && toast_age_s < Overlay::kToastTotalS) || debug_bar || !message.empty();
}

bool Overlay::same_visible(const RenderParams& a, const RenderParams& b) {
    // 强度回到 0 的那一帧 MUST Present（spec"按需 Present"），即使它与上一帧只差不到一个灰阶。
    if ((a.intensity == 0.0f) != (b.intensity == 0.0f)) return false;
    // 边缘点 alpha 只按 8 位输出比较：淡出尾段（遮罩已经整片为 0）它每帧只变零点几个灰阶，屏幕上看不出来。
    // 其余字段（遮罩强度、几何、开关、文字…）逐位比较。
    auto edge_level = [](const OverlayConstants& c) {
        return c.edge[2] > 0.5f ? std::lround(static_cast<double>(c.edge[3]) * 255.0) : -1L;
    };
    if (edge_level(a.constants) != edge_level(b.constants)) return false;
    OverlayConstants ca = a.constants;
    OverlayConstants cb = b.constants;
    ca.edge[3] = 0.0f;
    cb.edge[3] = 0.0f;
    return ca == cb && a.dpi_scale == b.dpi_scale && a.toast == b.toast && a.toast_opacity == b.toast_opacity &&
           a.debug_bar == b.debug_bar && a.debug_text == b.debug_text && a.debug_target == b.debug_target &&
           a.debug_value == b.debug_value && a.message == b.message;
}

float Overlay::toast_opacity(double age_s) {
    if (age_s < 0) age_s = 0;
    if (age_s >= kToastTotalS) return 0.0f;
    const double fade_start = kToastTotalS - kToastFadeS;
    if (age_s <= fade_start) return 1.0f;
    return static_cast<float>((kToastTotalS - age_s) / kToastFadeS);
}

RenderParams Overlay::make_params(const OverlayFrame& f, const MaskStyle& mask, const DotStyle& dots, UINT width,
                                  UINT height, UINT dpi) {
    RenderParams p;
    OverlayConstants& c = p.constants;
    const float scale = static_cast<float>(dpi == 0 ? 96 : dpi) / 96.0f;
    const auto idx = static_cast<std::size_t>(std::clamp(static_cast<int>(f.level), 0, 2));
    const double w = static_cast<double>(std::max<UINT>(1, width));
    const double h = static_cast<double>(std::max<UINT>(1, height));
    // spec（遮罩几何与不透明度）：ry = clear_height·H/2，rx = ry·ellipse_ratio，R = |(W/2, H/2) / (rx, ry)|
    const double ry = std::max(0.5, mask.clear_height[idx] * h / 2.0);
    const double rx = std::max(0.5, ry * mask.ellipse_ratio);
    const double R = std::hypot(w / 2.0 / rx, h / 2.0 / ry);
    const double i = f.scene ? std::clamp(static_cast<double>(f.intensity), 0.0, 1.0) : 0.0;

    // 遮罩内边界 r_in = lerp(R + feather, 1, i)。r_in ≥ R 时屏幕上所有像素（d < R）都在内边界以内，遮罩处处为 0：
    // 着色器的遮罩强度直接给 0（输出不变），这样淡出尾段只剩边缘点 alpha 在变，帧键才能识别出"看不出变化"。
    const double r_in = (R + mask.feather) + (1.0 - (R + mask.feather)) * i;
    const double mask_i = r_in >= R + 1e-4 ? 0.0 : i;

    c.size_radii[0] = static_cast<float>(w);
    c.size_radii[1] = static_cast<float>(h);
    c.size_radii[2] = static_cast<float>(rx);
    c.size_radii[3] = static_cast<float>(ry);
    c.mask[0] = static_cast<float>(R);
    c.mask[1] = static_cast<float>(mask.feather);
    c.mask[2] = static_cast<float>(mask_i);
    c.mask[3] = static_cast<float>(mask.max_opacity[idx]);
    c.mask_rgb[0] = unit(mask.color.r);
    c.mask_rgb[1] = unit(mask.color.g);
    c.mask_rgb[2] = unit(mask.color.b);

    c.center[0] = static_cast<float>(dots.center_dot_radius_px) * scale;
    c.center[1] = 1.0f * scale;   // 描边 1 px（按 DPI 缩放）
    c.center[2] = (f.scene && f.show_center_dot) ? 1.0f : 0.0f;
    c.center[3] = dots.center_dot_outline ? 1.0f : 0.0f;
    c.center_rgba[0] = unit(dots.center_dot_color.r);
    c.center_rgba[1] = unit(dots.center_dot_color.g);
    c.center_rgba[2] = unit(dots.center_dot_color.b);
    c.center_rgba[3] = 0.9f;

    c.edge[0] = static_cast<float>(dots.edge_dot_radius_px) * scale;
    c.edge[1] = static_cast<float>(dots.edge_dot_margin_px) * scale;
    c.edge[2] = (f.scene && f.show_edge_dots) ? 1.0f : 0.0f;
    c.edge[3] = static_cast<float>(dots.edge_dot_min_alpha + (dots.edge_dot_max_alpha - dots.edge_dot_min_alpha) * i);
    c.edge_rgb[0] = unit(dots.edge_dot_color.r);
    c.edge_rgb[1] = unit(dots.edge_dot_color.g);
    c.edge_rgb[2] = unit(dots.edge_dot_color.b);

    p.intensity = static_cast<float>(i);
    p.dpi_scale = scale;
    const float toast_a = f.toast.empty() ? 0.0f : toast_opacity(f.toast_age_s);
    if (toast_a > 0.0f) {
        p.toast = f.toast;
        p.toast_opacity = toast_a;
    }
    p.debug_bar = f.debug_bar;
    if (f.debug_bar) {
        p.debug_text = f.debug_text;
        p.debug_target = f.debug_target;
        p.debug_value = f.debug_value;
    }
    p.message = f.message;
    return p;
}

// ---------------------------------------------------------------- 窗口

bool Overlay::init(HINSTANCE instance, bool gpu_timing) {
    instance_ = instance;
    gpu_timing_ = gpu_timing;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &Overlay::wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kOverlayClass;
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::warn("RegisterClassEx(MotionCalm.Overlay) failed error={}", ::GetLastError());
        return false;
    }
    class_registered_ = true;

    const HMONITOR primary = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    RECT rc{0, 0, 1920, 1080};
    if (::GetMonitorInfoW(primary, &mi)) rc = mi.rcMonitor;
    hwnd_ = ::CreateWindowExW(kOverlayExStyle, kOverlayClass, L"MotionCalm Overlay", WS_POPUP, rc.left, rc.top,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        log::warn("CreateWindowEx(MotionCalm.Overlay) failed error={}", ::GetLastError());
        return false;
    }
    // layered 窗口必须设一次属性才会显示（design D6）。
    if (!::SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA)) {
        log::warn("SetLayeredWindowAttributes failed error={}", ::GetLastError());
    }
    follow_monitor(primary);
    ensure_renderer();
    return true;
}

void Overlay::shutdown() {
    renderer_.shutdown();
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_registered_) {
        ::UnregisterClassW(kOverlayClass, instance_);
        class_registered_ = false;
    }
    shown_ = false;
    want_visible_ = false;
    have_last_ = false;
}

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        switch (msg) {
            case WM_NCHITTEST: return HTTRANSPARENT;       // 点击穿透（与 WS_EX_TRANSPARENT|LAYERED 双保险）
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_DPICHANGED:                              // 不采用系统建议的矩形：下次 follow_monitor 按物理像素重贴
                self->display_changed_ = true;
                return 0;
            case WM_DISPLAYCHANGE:
                self->display_changed_ = true;
                break;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: {
                // 内容全部来自 DComp 交换链；必须用 BeginPaint/EndPaint 清掉更新区域，
                // 否则（WS_EX_LAYERED|NOREDIRECTIONBITMAP 下 ValidateRect 不够）WM_PAINT 会被不停地重新生成。
                PAINTSTRUCT ps;
                ::BeginPaint(hwnd, &ps);
                ::EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CLOSE: return 0;                         // 退出由 host 窗口负责
            default: break;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void Overlay::follow_monitor(HMONITOR monitor) {
    if (monitor == nullptr || hwnd_ == nullptr) return;
    // 每个 tick 都会调用：同一台显示器、没有显示变化消息时，最多每 kMonitorRecheckS 才重新查询一次
    // （分辨率 / DPI 变化会先送来 WM_DISPLAYCHANGE / WM_DPICHANGED，见 wnd_proc）。
    const double now = qpc_now_s();
    if (!display_changed_ && monitor == monitor_ && now - monitor_checked_t_ < kMonitorRecheckS) return;
    monitor_checked_t_ = now;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!::GetMonitorInfoW(monitor, &mi)) return;
    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0) dpi_x = 96;
    const RECT rc = mi.rcMonitor;
    if (!display_changed_ && monitor == monitor_ && ::EqualRect(&rc, &rect_) && dpi_x == dpi_) return;
    display_changed_ = false;

    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    double hz = 60.0;
    if (::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        hz = static_cast<double>(dm.dmDisplayFrequency);
    }
    monitor_ = monitor;
    rect_ = rc;
    dpi_ = dpi_x;
    refresh_hz_ = hz;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    // 物理像素（进程是 PMv2）；不激活。
    ::SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (renderer_.ready() && !renderer_.resize(static_cast<UINT>(w), static_cast<UINT>(h))) {
        log::warn("overlay: resize failed, recreating renderer");
        renderer_.shutdown();
        have_last_ = false;
        retry_at_ = 0;
    }
    log::info(L"overlay monitor {} rect=({},{})-({},{}) dpi={} refresh={}Hz", std::wstring(mi.szDevice), rc.left,
              rc.top, rc.right, rc.bottom, dpi_, static_cast<int>(hz));
}

void Overlay::set_visible(bool visible) {
    want_visible_ = visible;
    if (!visible && shown_) {
        ::ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
        have_last_ = false;   // 下次显示前必须重新渲染首帧
    }
    if (!visible) dirty_ = false;
}

void Overlay::set_capture_excluded(bool excluded) {
    if (capture_known_ && excluded == capture_excluded_) return;
    capture_excluded_ = excluded;
    capture_known_ = true;
    apply_capture_affinity();
}

void Overlay::apply_capture_affinity() {
    if (hwnd_ == nullptr) return;
    if (!::SetWindowDisplayAffinity(hwnd_, capture_excluded_ ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE)) {
        log::warn("SetWindowDisplayAffinity({}) failed error={} (needs Windows 10 2004+); overlay stays visible to capture",
                  capture_excluded_ ? "WDA_EXCLUDEFROMCAPTURE" : "WDA_NONE", ::GetLastError());
        return;
    }
    log::info("overlay capture visibility: {}", capture_excluded_ ? "excluded from capture" : "visible to capture");
}

void Overlay::keep_topmost() {
    if (hwnd_ == nullptr) return;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
}

void Overlay::recheck_topmost() {
    if (hwnd_ == nullptr) return;
    bool covered = (::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
    RECT mine{};
    if (!covered && !::GetWindowRect(hwnd_, &mine)) covered = true;
    int seen = 0;
    for (HWND w = ::GetWindow(hwnd_, GW_HWNDPREV); !covered && w != nullptr; w = ::GetWindow(w, GW_HWNDPREV)) {
        if (++seen > 256) {   // Z 序异常长：保守起见直接重新置顶
            covered = true;
            break;
        }
        if (!::IsWindowVisible(w)) continue;
        RECT r{}, x{};
        if (::GetWindowRect(w, &r) && ::IntersectRect(&x, &r, &mine)) covered = true;
    }
    if (covered) keep_topmost();
}

bool Overlay::ensure_renderer() {
    if (renderer_.ready()) return true;
    const double now = qpc_now_s();
    if (now < retry_at_) return false;
    const auto w = static_cast<UINT>(std::max<LONG>(1, rect_.right - rect_.left));
    const auto h = static_cast<UINT>(std::max<LONG>(1, rect_.bottom - rect_.top));
    if (renderer_.init(hwnd_, w, h, gpu_timing_)) {
        have_last_ = false;
        return true;
    }
    retry_at_ = now + kRetryS;
    log::warn("overlay: renderer unavailable, retrying in {:.0f} s", kRetryS);
    return false;
}

bool Overlay::render(const OverlayFrame& frame, const MaskStyle& mask, const DotStyle& dots) {
    if (hwnd_ == nullptr || !want_visible_) {
        dirty_ = false;
        return false;
    }
    if (!ensure_renderer()) {
        dirty_ = true;
        return false;
    }
    RenderParams p = make_params(frame, mask, dots, renderer_.width(), renderer_.height(), dpi_);
    bool presented = false;
    if (!have_last_ || !same_visible(p, last_)) {
        switch (renderer_.render(p)) {
            case RenderResult::Presented:
                last_ = std::move(p);
                have_last_ = true;
                dirty_ = false;
                presented = true;
                break;
            case RenderResult::NotReady:
            case RenderResult::Failed:
                dirty_ = true;
                break;
            case RenderResult::DeviceLost:
                log::warn("overlay: render device lost; rebuilding all render resources");
                renderer_.shutdown();
                have_last_ = false;
                dirty_ = true;
                retry_at_ = 0;   // 下一个 tick 立即重建，失败再每 2 s 重试
                break;
        }
    } else {
        dirty_ = false;
    }
    // 先 Present 首帧再显示，避免闪出旧画面（design D6）。
    if (!shown_ && have_last_) {
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        shown_ = true;
        keep_topmost();
    }
    return presented;
}

}  // namespace mc
