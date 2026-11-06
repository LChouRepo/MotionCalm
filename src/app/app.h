// app.h — 应用装配：命令行、日志、配置加载 / 监视 / 保存、host 窗口、主循环（design D1）、tick、托盘 / 热键共用的动作（含变更 2 的游戏内校准、变更 3 的截屏运动检测）、开机自启、有序退出，以及变更 4 的设置窗口宿主（ui::SettingsHost 的实现在 app_settings.cpp）。
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "calib/calibration.h"
#include "capture/desktop_dup.h"
#include "config/config.h"
#include "detect/game_detect.h"
#include "input/gamepad.h"
#include "input/raw_input.h"
#include "model/intensity.h"
#include "overlay/overlay.h"
#include "tray/hotkeys.h"
#include "tray/tray.h"
#include "ui/settings_dlg.h"
#include "ui/settings_host.h"
#include "util/win.h"

namespace mc {

// host 窗口的自定义消息。
inline constexpr UINT WM_APP_TRAY = WM_APP + 1;              // 托盘回调
inline constexpr UINT WM_APP_ALREADY_RUNNING = WM_APP + 2;   // 第二个实例的通知
// host 窗口的计时器 id。
inline constexpr UINT_PTR kModalTimerId = 1;        // 托盘菜单 / 信息框 / 设置窗口模态循环期间用 WM_TIMER 驱动 tick
inline constexpr UINT_PTR kSaveTimerId = 2;         // 设置窗口改动的 300 ms 防抖保存
inline constexpr UINT_PTR kTrayClickTimerId = 3;    // 托盘左键单击：等过双击间隔再弹菜单
inline constexpr UINT kSettingsSaveDelayMs = 300;
// host 窗口类名（第二个实例用 FindWindowW 找它）与单实例互斥量名。
inline constexpr wchar_t kHostClassName[] = L"MotionCalm.Host";
inline constexpr wchar_t kInstanceMutexName[] = L"Local\\MotionCalm.Instance";

// 命令行参数（configuration spec "命令行参数"）。
struct CommandLine {
    std::wstring config_path;          // --config <path>；空 = exe 目录下的 MotionCalm.ini
    bool verbose = false;              // --verbose：本次运行日志级别为 debug
    bool no_capture = false;           // --no-capture：本次运行不启动截屏运动检测（不建截屏线程，不改 INI）
    bool force_game = false;           // --force-game：测试模式
    bool portable = false;             // --portable：默认行为
    double exit_after_s = 0;           // --exit-after <秒>；≤ 0 = 不自动退出
    std::vector<std::wstring> unknown; // 未知参数（启动后逐个记 warn）
    std::vector<std::wstring> errors;  // 缺值 / 非法值（启动后逐个记 warn）
};

// 解析参数（不含 argv[0]）。纯函数。
CommandLine parse_command_line(std::span<const std::wstring> args);

// 调试条的截屏字段（overlay-rendering「调试条」）：user_on（未带 --no-capture 且 [trigger] capture=1）为假或
// 状态为 Off → "关"；Running → "{°/s 取整}°/s c{置信度两位小数}"，zoom / roll ≥ 0 时（变更 5）再接
// " · 前进 {zoom 取整}°/s · 倾斜 {roll 取整}°/s"；Paused / Recovering → "暂停"；Failed → "失败"。纯函数。
std::wstring format_capture_debug_field(bool user_on, CaptureState state, double deg_per_s, double confidence,
                                        double zoom_deg_per_s = -1.0, double roll_deg_per_s = -1.0);

// App 自己就是设置窗口的宿主（私有继承：SettingsHost 的方法只给 settings_ 调用）。
class App final : private ui::SettingsHost {
public:
    explicit App(HINSTANCE instance);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // 运行到退出，返回进程退出码。
    int run();

private:
    // ---- 启动 / 退出
    bool init();
    void shutdown();
    bool create_host_window();
    void request_quit(const char* reason);

    // ---- 主循环与 tick
    void loop();
    void pump_messages();
    void tick();
    void arm_frame_timer();
    void write_frame_log(double now, const GameContext& ctx, double mouse_deg, bool moving, double pad_deg,
                         double capture_deg, double zoom_deg, double roll_deg, float preview);
    void write_perf_log(double now, double tick_ms, bool flush = false);

    // ---- host 窗口
    static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle_host_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK foreground_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object,
                                               LONG id_child, DWORD thread, DWORD time_ms);
    void on_foreground_changed();
    void on_tray_message(WPARAM wp, LPARAM lp);
    void show_tray_menu(POINT pt);
    void enter_modal();
    void leave_modal();

    // ---- 配置
    bool load_template();
    void load_initial_config();
    void apply_text(const std::string& text, bool initial);
    void apply_config(bool initial);
    void apply_language();   // localization：按 [ui] language 设定当前界面语言（变化时刷新设置窗口）
    // 立即保存（并清掉设置窗口挂起的 300 ms 防抖）；成功后通知设置窗口"上次保存"。from_settings = 设置窗口的
    // 防抖 / flush 写入（不必再让窗口刷新）；其他来源保存后让设置窗口刷新。
    void save_config(bool from_settings = false);
    bool start_watcher();
    bool issue_watch();
    void stop_watcher();
    void on_watcher_signaled();
    void check_external_reload();
    void reload_from_menu();

    // ---- 热键、自启、截屏可见性与截屏运动检测（变更 3）
    // from_settings：由设置窗口发起（改键、恢复默认、捕获结束后恢复），注册失败 / 重复只写 warn，不弹 toast。
    // 按键捕获暂停期间只更新 bindings_，不注册（恢复时再注册）。
    void register_hotkeys(bool from_settings = false);
    bool sync_autostart(bool want);
    void apply_autostart_from_config();
    // cap_on = 截屏检测处于开启状态（capture_user_on() 且未"采集失败"）；按 hide_from_capture 设置排除，仅在结果变化时调用
    // SetWindowDisplayAffinity；never 且 cap_on 从关变开时写一条 warn。
    void apply_capture_visibility(bool cap_on);
    // 未带 --no-capture 且 [trigger] capture=1（不含"采集失败"，见 desktop_dup.h）。
    bool capture_user_on() const { return !cmd_.no_capture && cfg_.trigger.capture; }
    bool capture_on() const { return capture_user_on() && capture_.state() != CaptureState::Failed; }
    void start_capture();

    // ---- 动作（托盘与热键共用）
    void do_hotkey_action(HotkeyAction action, const char* source);
    void do_tray_command(UINT cmd);
    void action_toggle();
    void action_set_level(MaskLevel level);
    void action_set_sensitivity(Sensitivity s);
    void action_toggle_center_dot();
    void action_toggle_edge_dots();
    void action_toggle_debug_bar();
    void action_mark_game();
    void action_exclude_app();
    void action_toggle_autostart();
    void action_toggle_capture();
    // ---- 校准（变更 2，in-game-calibration design D3 / D6）
    void action_calibrate(const char* source);
    CalibContext make_calib_context(const GameContext& ctx) const;
    void apply_calib_effect(const CalibEffect& effect, CalibState from, long long count_before);
    void log_calib_progress(double now);
    std::wstring calibrate_hotkey_text() const;   // calibrate 当前生效绑定的规范字符串；未绑定为空
    void show_calibrate_help();
    // 删除校准记录。托盘：立即保存并 toast；设置窗口：按防抖保存、不 toast（turn-calibration「在设置窗口删除校准记录」）。
    void remove_calibration(const std::wstring& exe, bool from_settings);
    void open_file(const std::wstring& path);
    void show_about();
    void show_toast(std::wstring text);
    TrayMenuState make_menu_state() const;
    std::wstring make_debug_text(const GameContext& ctx, double mouse_deg, bool moving, double pad_deg,
                                 double capture_deg, double zoom_deg, double roll_deg) const;

    // ---- 设置窗口（变更 4，settings-window）：app_settings.cpp
    void open_settings(const char* source);
    // 设置窗口的一次改动已写进 cfg_（before = 改动前）：按差异重新配置模型、渲染、热键、截屏、游戏判定、自启等。
    void apply_settings_change(const Config& before);
    void schedule_settings_save();   // 最后一次改动 300 ms 后保存（宿主窗口计时器）
    void on_save_timer();
    // ui::SettingsHost
    std::optional<std::wstring> get(ui::SettingKey key) const override;
    bool set(std::span<const ui::SettingValue> values) override;
    std::optional<ui::KeyRange> range(ui::SettingKey key) const override;
    void flush_config() override;
    void restore_defaults() override;
    void clear_known_games() override;
    void open_config_file() override;
    ui::SettingsStatus status() const override;
    std::vector<ui::CalibEntry> calibrations() const override;
    void delete_calibration(const std::wstring& exe) override;
    void pause_hotkeys(bool paused) override;
    bool hotkey_available(HotkeyAction action, const Hotkey& hotkey) override;
    void set_preview_floor(float p) override;
    void modal_begin() override;
    void modal_end() override;
    void settings_closed() override;

    // ---- 成员
    HINSTANCE instance_ = nullptr;
    CommandLine cmd_;
    std::wstring cmdline_text_;
    double t0_ = 0;                    // 启动时刻（QPC 秒）
    double exit_deadline_ = 0;         // --exit-after 的退出时刻；0 = 无
    std::wstring exe_path_, exe_dir_, config_path_, config_dir_, config_name_, log_path_;

    std::string template_;             // 内嵌 INI 模板（RCDATA）
    Config cfg_;
    bool config_writable_ = true;
    std::uint64_t last_hash_ = 0;      // 最后一次自己写入或读取的内容哈希

    // 目录监视（ReadDirectoryChangesW + OVERLAPPED）
    UniqueHandle dir_handle_;
    UniqueHandle dir_event_;
    OVERLAPPED dir_ov_{};
    std::array<DWORD, 2048> dir_buf_{};   // 8 KB，DWORD 对齐
    bool dir_pending_ = false;
    double reload_deadline_ = 0;       // 防抖截止时刻；0 = 无

    HWND host_ = nullptr;
    bool host_class_registered_ = false;
    UINT taskbar_created_msg_ = 0;
    HICON tray_icon_ = nullptr;
    HWINEVENTHOOK fg_hook_ = nullptr;

    RawInput raw_;
    Gamepad pad_;
    GameDetector detector_;
    IntensityModel model_;
    Overlay overlay_;
    Tray tray_;
    HotkeyManager hotkeys_;
    ResolvedHotkeys bindings_;
    Calibration calib_;
    CaptureService capture_;                      // 变更 3：--no-capture 时从不 start()
    HMONITOR capture_mon_ = nullptr;              // 最近一次交给 set_target 的显示器
    bool capture_wanted_ = false;                 // 最近一次交给 set_target 的 wanted
    bool capture_vis_on_ = false;                 // 上一次 apply_capture_visibility 时是否"never 且截屏检测开启"
    bool capture_vis_known_ = false;
    std::vector<std::wstring> menu_calib_exes_;   // 最近一次弹出的"校准列表"子菜单：下标 → exe
    double calib_log_t_ = 0;                      // 校准计数 debug 行的节流
    long long calib_log_count_ = -1;
    std::wstring cpd_exe_;                        // 上一个 tick 用的 counts_per_degree 及其 exe（变化时记一行）
    double cpd_used_ = 0;

    // 运行期状态
    bool debug_bar_ = false;           // 不写入 INI
    std::wstring toast_;
    double toast_t_ = -1e9;
    bool quit_ = false;
    std::string quit_reason_ = "exit-after";
    bool shut_down_ = false;
    bool force_sample_ = true;
    bool tick_now_ = true;
    double last_tick_t_ = 0;
    double next_idle_tick_ = 0;
    bool active_ = false;              // 活动节拍（按刷新率）还是空闲 30 ms 轮询
    bool raw_wake_ = false;            // 主循环是否为原始输入事件醒来（见 RawInput::wants_event_wake）
    bool last_presented_ = false;
    UniqueHandle frame_timer_;
    bool timer_armed_ = false;
    double last_topmost_t_ = 0;
    HWND last_fg_ = nullptr;
    int modal_depth_ = 0;
    bool in_menu_ = false;
    // 托盘左键：单击延迟到双击间隔之后再弹菜单，双击打开设置（tray-and-hotkeys「托盘图标」）
    bool tray_click_pending_ = false;
    POINT tray_click_pt_{};
    double tray_ignore_select_until_ = 0;

    // 设置窗口（变更 4）
    bool save_pending_ = false;        // 设置窗口的改动尚未落盘（300 ms 防抖计时器在跑）
    bool hotkeys_paused_ = false;      // 按键捕获框有焦点：本程序的全局热键已全部注销
    float preview_floor_ = 0.0f;       // 预览强度下限 0..1（只作用于渲染）
    bool language_applied_ = false;   // apply_language 至少执行过一次
    bool capture_failed_seen_ = false; // 上一个 tick 的"采集失败"状态（变化时刷新设置窗口）

    // frame 日志
    struct FrameLog {
        bool valid = false;
        double t = 0;
        double target = 0, value = 0, preview = 0;
        bool in_game = false, locked = false;
        std::wstring exe;
    } frame_log_;

    // perf 日志（--verbose，每 5 s）
    double perf_t0_ = 0;
    std::uint32_t perf_ticks_ = 0;
    double perf_sum_ms_ = 0, perf_max_ms_ = 0;
    std::uint64_t perf_presents0_ = 0;
    std::uint64_t wakeups_ = 0, perf_wakeups0_ = 0;      // 主循环被唤醒的次数
    std::uint64_t perf_inputs0_ = 0;                     // 相对鼠标事件累计数（RawInput::mouse_events）
    std::uint64_t perf_cycles0_ = 0;                     // 进程 CPU 周期（QueryProcessCycleTime）
    std::uint64_t perf_main_cycles0_ = 0;                // 主线程 CPU 周期（QueryThreadCycleTime）

    static App* s_instance_;   // WinEvent 回调用

    // 最后声明：最先析构（析构时窗口已在 shutdown 中关闭；它引用的 App 成员都还在）。
    ui::SettingsWindow settings_{*this};
};

}  // namespace mc
