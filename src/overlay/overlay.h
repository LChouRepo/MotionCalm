// overlay.h — 点击穿透、永不激活的置顶 overlay 窗口：显示器跟随（物理像素 + DPI）、显示 / 隐藏、截屏排除、置顶维持、按帧键差异 Present。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "config/config.h"
#include "model/intensity.h"
#include "overlay/renderer.h"

namespace mc {

// 渲染样式直接取配置（DESIGN §3.3 的 MaskStyle / DotStyle）。
using MaskStyle = MaskConfig;
using DotStyle = DotConfig;

// 一个 tick 要显示的内容（DESIGN §3.3）。scene = false 时为"仅文字"模式：不画遮罩和参照点。
struct OverlayFrame {
    bool scene = false;                    // enabled && in_game：画遮罩与参照点
    float intensity = 0;                   // 平滑后的强度 0..1
    MaskLevel level = MaskLevel::Medium;
    bool show_center_dot = true, show_edge_dots = true;
    bool debug_bar = false;
    std::wstring debug_text;
    float debug_target = 0, debug_value = 0;
    std::wstring toast;                    // 空 = 没有 toast
    double toast_age_s = 0;                // toast 已显示的时间
    std::wstring message;                  // 变更 2：校准提示（多行，按 L'\n' 分行；空 = 没有），画在屏幕中上方

    // 是否有需要显示的文字（toast 未过期、调试条或提示）。
    bool has_text() const;
};

class Overlay {
public:
    static constexpr double kToastTotalS = 1.5;   // toast 总时长
    static constexpr double kToastFadeS = 0.3;    // 最后 0.3 s 线性淡出
    static constexpr double kRetryS = 2.0;        // 渲染设备重建失败后的重试间隔
    static constexpr double kMonitorRecheckS = 1.0;   // 同一显示器的兜底重新查询间隔

    // toast 的不透明度：前 1.2 s 为 1，之后线性降到 1.5 s 时为 0。
    static float toast_opacity(double age_s);
    // 由帧与样式算出着色器常量与文字参数（纯函数，供测试与 render 共用）。遮罩在整个屏幕上都为 0 时
    // （强度低到内边界退出屏幕角之外），着色器的遮罩强度给 0。
    static RenderParams make_params(const OverlayFrame& f, const MaskStyle& mask, const DotStyle& dots, UINT width,
                                    UINT height, UINT dpi);
    // 帧键比较（design D8）：两帧在屏幕上是否一样。边缘点 alpha 按 8 位输出比较，其余字段逐位比较；
    // 强度"是否为 0"不同时总算不同（回到 0 的那一帧必须 Present）。纯函数。
    static bool same_visible(const RenderParams& a, const RenderParams& b);

    Overlay() = default;
    ~Overlay() { shutdown(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // 注册窗口类、创建隐藏的 overlay 窗口（覆盖主显示器）并初始化渲染器。窗口创建失败返回 false；
    // 渲染器失败只记 warn，之后每 2 s 重试。
    bool init(HINSTANCE instance, bool gpu_timing);
    // 释放渲染资源并销毁窗口。
    void shutdown();

    // 贴合显示器的 rcMonitor（物理像素）；尺寸变化时 ResizeBuffers，并更新 DPI 与刷新率。没有变化时什么都不做。
    void follow_monitor(HMONITOR monitor);
    // 期望的可见性。变为可见时不立即显示：下一次 render 先 Present 首帧再 SW_SHOWNOACTIVATE。变为不可见时立即 SW_HIDE。
    void set_visible(bool visible);
    // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE / WDA_NONE)；不支持时记 warn。
    void set_capture_excluded(bool excluded);
    // 重新置顶（HWND_TOPMOST，不移动、不改尺寸、不激活）。
    void keep_topmost();
    // 1 s 兜底检查用：只有窗口丢了 WS_EX_TOPMOST，或 Z 序里有可见窗口压在它上面并与它重叠时才 keep_topmost()。
    // （SetWindowPos 一次约 0.7 ms CPU，而沿 Z 序往上看几个窗口只要几微秒。）
    void recheck_topmost();
    // 与上一次 Present 的帧键不同时才画并 Present；返回本次是否 Present 了。
    bool render(const OverlayFrame& frame, const MaskStyle& mask, const DotStyle& dots);

    // 窗口当前是否显示着。
    bool visible() const { return shown_; }
    // 有尚未成功 Present 的内容（例如交换链还没准备好），需要保持活动节拍。
    bool dirty() const { return dirty_; }
    HWND hwnd() const { return hwnd_; }
    HMONITOR monitor() const { return monitor_; }
    // 当前显示器的刷新率（Hz，取不到时 60）。
    double refresh_hz() const { return refresh_hz_; }
    // 交换链的 frame-latency 可等待对象；渲染器不可用时为 nullptr。
    HANDLE frame_latency_handle() const { return renderer_.frame_latency_handle(); }
    // 主循环在 frame-latency 对象上等到了信号。
    void note_frame_ready() { renderer_.note_frame_ready(); }
    // 累计 Present 次数。
    std::uint64_t presents() const { return renderer_.presents(); }
    // GPU 计时统计（--verbose），每 5 s 一行；flush = true 时立即输出当前（不满 5 s 的）窗口。
    void poll_stats(double now_s, bool flush = false) { renderer_.poll_gpu_stats(now_s, flush); }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_message(UINT msg, WPARAM wp, LPARAM lp);
    bool ensure_renderer();
    void apply_capture_affinity();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool class_registered_ = false;
    Renderer renderer_;
    bool gpu_timing_ = false;
    double retry_at_ = 0;          // 渲染器不可用时下次重建的时刻

    HMONITOR monitor_ = nullptr;
    RECT rect_{};
    UINT dpi_ = 96;
    double refresh_hz_ = 60.0;
    bool display_changed_ = false;  // WM_DISPLAYCHANGE / WM_DPICHANGED 之后强制重新查询
    double monitor_checked_t_ = -1e9;   // 上一次查询显示器信息的时刻

    bool want_visible_ = false;
    bool shown_ = false;
    bool dirty_ = false;
    bool have_last_ = false;
    RenderParams last_;

    bool capture_excluded_ = false;
    bool capture_known_ = false;
};

}  // namespace mc
