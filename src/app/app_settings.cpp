// app_settings.cpp — App 作为设置窗口宿主（ui::SettingsHost）的实现（变更 4，settings-window design D4 / D5 / D8 / D12）：
// 按 INI 键读写配置（config_get_value / config_set_value / config_key_range，同一份模式表）、一次改动 = 一次原子编辑后
// 立即应用到模型 / 渲染 / 热键 / 截屏 / 游戏判定 / 自启、最后一次改动 300 ms 后保存；恢复默认、清空已知游戏、打开配置
// 文件、校准记录、热键暂停 / 试注册、预览强度下限、模态循环兜底，以及"打开设置"动作。
#include "app/app.h"

#include "i18n/i18n.h"

#include <algorithm>

#include "util/log.h"
#include "util/strings.h"

namespace mc {
namespace {

// 试注册（hotkey_available）用的 RegisterHotKey id：不与动作 id（1..kHotkeyActionCount）重复。
constexpr int kTrialHotkeyId = 0xBF00;

std::string key_text(const ui::SettingKey& k) { return std::string(k.section) + "." + std::string(k.key); }

}  // namespace

// ---------------------------------------------------------------- 打开设置

void App::open_settings(const char* source) {
    if (shut_down_) return;
    const bool was_open = settings_.is_open();
    if (!settings_.open(instance_)) {
        log::warn("settings window could not be created (error={})", ::GetLastError());
        return;
    }
    log::info("settings window {} via {}", was_open ? "activated" : "opened", source);
}

// ---------------------------------------------------------------- 配置

std::optional<std::wstring> App::get(ui::SettingKey key) const {
    return config_get_value(cfg_, key.section, key.key);
}

std::optional<ui::KeyRange> App::range(ui::SettingKey key) const {
    const auto r = config_key_range(key.section, key.key);
    if (!r) return std::nullopt;
    return ui::KeyRange{r->lo, r->hi};
}

bool App::set(std::span<const ui::SettingValue> values) {
    if (values.empty()) return true;
    // 先在副本上逐项解析 / 校验 / 截断：任何一项不认识或无法解析就整次拒绝（SettingsHost::set 的约定）。
    Config next = cfg_;
    std::string what;
    for (const auto& v : values) {
        if (!config_set_value(next, v.key.section, v.key.key, v.value)) {
            log::debug(L"settings change rejected: {}={}", utf8_to_wide(key_text(v.key)), v.value);
            return false;
        }
        if (!what.empty()) what += ' ';
        what += key_text(v.key) + "=" + wide_to_utf8(v.value);
    }
    if (!config_thresholds_ordered(next)) {   // 页面已先检查；这里兜底，保证生效配置始终满足 v_start < v_full
        log::debug("settings change rejected (v_start >= v_full): {}", what);
        return false;
    }
    if (next == cfg_) return true;   // 值没变（例如窗口位置相同）：不应用、不保存
    log::debug("settings change: {}", what);
    const Config before = cfg_;
    cfg_ = std::move(next);
    apply_settings_change(before);
    schedule_settings_save();
    return true;
}

void App::apply_settings_change(const Config& before) {
    // 来源 = 设置窗口的配置变更通知：只做与改动有关的工作（滑块拖动时每秒可能上百次）。
    if (cfg_.log_level != before.log_level && !cmd_.verbose) log::set_level(cfg_.log_level);
    if (cfg_.ui.language != before.ui.language) apply_language();   // 立即生效：设置窗口随后按新语言重建
    if (cfg_.enabled != before.enabled) {
        model_.reset(0.0);
        log::info("mask {} (settings window)", cfg_.enabled ? "enabled" : "disabled");
    }
    if (!(cfg_.trigger == before.trigger) || !(cfg_.mask == before.mask)) model_.configure(cfg_.trigger, cfg_.mask);
    if (cfg_.trigger.velocity_window_ms != before.trigger.velocity_window_ms) {
        raw_.set_velocity_window(static_cast<double>(cfg_.trigger.velocity_window_ms) / 1000.0);
    }
    if (!cfg_.trigger.gamepad && before.trigger.gamepad) pad_.reset();
    if (cfg_.mouse_lock_rule != before.mouse_lock_rule) detector_.set_rules(cfg_.mouse_lock_rule, cmd_.force_game);
    if (!(cfg_.detect == before.detect) || cfg_.mouse_lock_rule != before.mouse_lock_rule) {
        detector_.reevaluate(cfg_.detect);   // 下一次采样起按新名单判定
        force_sample_ = true;
    }
    if (cfg_.hotkeys != before.hotkeys) register_hotkeys(true);   // 捕获暂停期间只更新绑定，恢复时注册
    if (cfg_.autostart != before.autostart && !sync_autostart(cfg_.autostart)) {
        // 写注册表失败：回滚 autostart、toast、让窗口的复选框恢复（settings-window「通用页开机自启」）。
        cfg_.autostart = before.autostart;
        log::warn("autostart change from the settings window failed; reverted to {}", before.autostart ? 1 : 0);
        show_toast(i18n::tr(i18n::StrId::ToastAutostartFailed));
        settings_.refresh();
    }
    if (cfg_.hide_from_capture != before.hide_from_capture) apply_capture_visibility(capture_on());
    if (cfg_.trigger.capture != before.trigger.capture && !cmd_.no_capture) {
        // 与热键 toggle_capture 相同：立即告诉截屏线程（关 → 停止取帧；开 → 清零失败计数），下一个 tick 再允许取帧。
        capture_.set_target(capture_mon_, cfg_.trigger.capture, false);
        log::info("capture motion detection toggled {} (settings window)", cfg_.trigger.capture ? "on" : "off");
    }
    if (!(cfg_.capture == before.capture) || cfg_.trigger.capture != before.trigger.capture) {
        capture_.update_params(cfg_.capture);   // 未启动（--no-capture）时什么都不做
    }
    if (calib_.active() && !cfg_.calibration.contains(calib_.target_exe()) &&
        before.calibration.contains(calib_.target_exe())) {
        log::info(L"calib cancelled (entry deleted) exe={}", calib_.target_exe());
        calib_.cancel();
    }
    tick_now_ = true;   // 50 ms 内体现在 overlay 上（下一轮循环立即 tick）
}

void App::schedule_settings_save() {
    // 最后一次改动 300 ms 后保存；期间的新改动重新计时（SetTimer 同 id 即重设）。
    save_pending_ = true;
    if (host_ == nullptr || ::SetTimer(host_, kSaveTimerId, kSettingsSaveDelayMs, nullptr) == 0) save_config(true);
}

void App::on_save_timer() {
    if (host_ != nullptr) ::KillTimer(host_, kSaveTimerId);
    if (save_pending_) save_config(true);
}

void App::flush_config() {
    if (save_pending_) save_config(true);
}

void App::restore_defaults() {
    const Config before = cfg_;
    cfg_ = mc::restore_defaults(cfg_);
    log::info("settings: restored defaults (kept [calibration], force_game, known_games, autostart, [ui], [extra])");
    apply_settings_change(before);
    if (cfg_.hotkeys == before.hotkeys) register_hotkeys(true);   // 恢复默认总是先注销全部再重新注册
    save_config();   // 不可逆操作：立即保存
}

void App::clear_known_games() {
    const std::size_t n = cfg_.detect.known_games.size();
    mc::clear_known_games(cfg_);
    detector_.reevaluate(cfg_.detect);
    force_sample_ = true;
    log::info("known_games cleared ({} entries) via settings window", n);
    save_config();   // 不可逆操作：立即保存
}

void App::open_config_file() {
    flush_config();   // 打开的文件里已经是新值
    open_file(config_path_);
}

// ---------------------------------------------------------------- 运行期状态与校准记录

ui::SettingsStatus App::status() const {
    ui::SettingsStatus s;
    s.calibration_idle = !calib_.active();
    s.capture_disabled_by_cmdline = cmd_.no_capture;
    s.capture_failed = capture_.state() == CaptureState::Failed;
    s.verbose = cmd_.verbose;
    return s;
}

std::vector<ui::CalibEntry> App::calibrations() const {
    std::vector<ui::CalibEntry> out;
    out.reserve(cfg_.calibration.size());
    for (const auto& [exe, cpd] : cfg_.calibration) {   // std::map：已按 exe 升序
        const auto d = cfg_.calibration_dates.find(exe);
        out.push_back(ui::CalibEntry{exe, cpd, d != cfg_.calibration_dates.end() ? d->second : std::wstring()});
    }
    return out;
}

void App::delete_calibration(const std::wstring& exe) { remove_calibration(exe, true); }

// ---------------------------------------------------------------- 热键

void App::pause_hotkeys(bool paused) {
    if (paused == hotkeys_paused_) return;
    hotkeys_paused_ = paused;
    if (paused) {
        hotkeys_.unregister_all();   // 让按键送达捕获框（tray-and-hotkeys「按键捕获期间暂停全局热键」）
        log::debug("hotkeys paused: settings window key capture");
        return;
    }
    if (shut_down_) return;
    register_hotkeys(true);   // 按当前配置重新注册全部热键；失败只写 warn
}

bool App::hotkey_available(HotkeyAction /*action*/, const Hotkey& hotkey) {
    if (hotkey.empty()) return true;
    for (const auto& b : hotkeys_.registered()) {
        if (b.hotkey == hotkey) return true;   // 本程序自己正注册着的组合视为可用
    }
    if (host_ == nullptr) return true;
    const UINT mods = static_cast<UINT>(hotkey.mods & kModMask) | MOD_NOREPEAT;
    if (!::RegisterHotKey(host_, kTrialHotkeyId, mods, static_cast<UINT>(hotkey.vk))) {
        log::debug(L"hotkey {} is in use by another program (trial registration failed)", format_hotkey(hotkey));
        return false;
    }
    ::UnregisterHotKey(host_, kTrialHotkeyId);
    return true;
}

// ---------------------------------------------------------------- 预览、模态循环、关闭

void App::set_preview_floor(float p) {
    const float v = std::clamp(p, 0.0f, 1.0f);
    if (v == preview_floor_) return;
    if ((v > 0.0f) != (preview_floor_ > 0.0f)) log::debug("settings preview {} ({:.2f})", v > 0.0f ? "on" : "off", v);
    preview_floor_ = v;
    tick_now_ = true;
}

void App::modal_begin() { enter_modal(); }

void App::modal_end() { leave_modal(); }

void App::settings_closed() {
    preview_floor_ = 0.0f;   // 下一个 tick 恢复正常的显示条件与强度
    tick_now_ = true;
    log::info("settings window closed");
}

}  // namespace mc
