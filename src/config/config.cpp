// config.cpp — config/config 的实现：静态模式表（节、键、类型、范围、最少小数位）驱动的加载 / 校验 / 写回，以及原子文件 I/O。
#include "config/config.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <system_error>

#include "calib/calibration.h"
#include "config/ini.h"
#include "util/strings.h"

namespace mc {
namespace {

// ---------------------------------------------------------------- 模式表

enum class KeyType { Bool, Int, Double, Enum, Color, List, OptInt };   // OptInt：可留空的整数（[ui] x / y）

constexpr std::string_view kMaskLevelNames[] = {"light", "medium", "strong"};
constexpr std::string_view kSensitivityNames[] = {"low", "medium", "high"};
constexpr std::string_view kHideNames[] = {"auto", "always", "never"};
constexpr std::string_view kLogLevelNames[] = {"debug", "info", "warn"};
constexpr std::string_view kLockRuleNames[] = {"strict", "hidden"};
constexpr std::string_view kLanguageNames[] = {"auto", "zh-CN", "zh-TW", "en"};   // 下标 = i18n::LangSetting

struct KeySpec {
    std::string_view section;
    std::string_view key;
    KeyType type;
    double lo = 0;              // Int / Double 的合法范围
    double hi = 0;
    int min_decimals = 0;       // Double：写回时最少的小数位（= 默认值字面量的小数位）
    std::span<const std::string_view> names{};   // Enum：下标即枚举值
    bool* (*b)(Config&) = nullptr;
    int* (*i)(Config&) = nullptr;
    double* (*d)(Config&) = nullptr;
    int (*eget)(const Config&) = nullptr;
    void (*eset)(Config&, int) = nullptr;
    Rgb* (*c)(Config&) = nullptr;
    std::vector<std::wstring>* (*l)(Config&) = nullptr;
    std::optional<int>* (*oi)(Config&) = nullptr;
};

#define MC_BOOL(sec, k, field) \
    KeySpec { .section = sec, .key = k, .type = KeyType::Bool, .b = [](Config& c) { return &c.field; } }
#define MC_INT(sec, k, field, lo_, hi_)                                                   \
    KeySpec {                                                                             \
        .section = sec, .key = k, .type = KeyType::Int, .lo = lo_, .hi = hi_,             \
        .i = [](Config& c) { return &c.field; }                                           \
    }
#define MC_DBL(sec, k, field, lo_, hi_, dec)                                              \
    KeySpec {                                                                             \
        .section = sec, .key = k, .type = KeyType::Double, .lo = lo_, .hi = hi_,          \
        .min_decimals = dec, .d = [](Config& c) { return &c.field; }                      \
    }
#define MC_ENUM(sec, k, field, Type, names_)                                              \
    KeySpec {                                                                             \
        .section = sec, .key = k, .type = KeyType::Enum, .names = names_,                 \
        .eget = [](const Config& c) { return static_cast<int>(c.field); },                \
        .eset = [](Config& c, int v) { c.field = static_cast<Type>(v); }                  \
    }
#define MC_COLOR(sec, k, field) \
    KeySpec { .section = sec, .key = k, .type = KeyType::Color, .c = [](Config& c) { return &c.field; } }
#define MC_LIST(sec, k, field) \
    KeySpec { .section = sec, .key = k, .type = KeyType::List, .l = [](Config& c) { return &c.field; } }
#define MC_OPTINT(sec, k, field, lo_, hi_)                                                \
    KeySpec {                                                                             \
        .section = sec, .key = k, .type = KeyType::OptInt, .lo = lo_, .hi = hi_,          \
        .oi = [](Config& c) { return &c.field; }                                          \
    }

// INI 顺序。[hotkeys] 不在表里：它的键由 tray/hotkeys.h 的动作表生成（见 load_config / save_config_text）。
constexpr KeySpec kSchema[] = {
    MC_BOOL("general", "enabled", enabled),
    MC_ENUM("general", "mask_level", mask.level, MaskLevel, kMaskLevelNames),
    MC_ENUM("general", "sensitivity", trigger.sensitivity, Sensitivity, kSensitivityNames),
    MC_BOOL("general", "autostart", autostart),
    MC_ENUM("general", "hide_from_capture", hide_from_capture, HideFromCapture, kHideNames),
    MC_ENUM("general", "log_level", log_level, log::Level, kLogLevelNames),

    MC_COLOR("mask", "color", mask.color),
    MC_DBL("mask", "max_opacity_light", mask.max_opacity[0], 0, 1, 2),
    MC_DBL("mask", "max_opacity_medium", mask.max_opacity[1], 0, 1, 2),
    MC_DBL("mask", "max_opacity_strong", mask.max_opacity[2], 0, 1, 2),
    MC_DBL("mask", "clear_height_light", mask.clear_height[0], 0.05, 1, 2),
    MC_DBL("mask", "clear_height_medium", mask.clear_height[1], 0.05, 1, 2),
    MC_DBL("mask", "clear_height_strong", mask.clear_height[2], 0.05, 1, 2),
    MC_DBL("mask", "ellipse_ratio", mask.ellipse_ratio, 0.5, 3, 2),
    MC_DBL("mask", "feather", mask.feather, 0.05, 1, 2),
    MC_INT("mask", "attack_ms", mask.attack_ms, 10, 2000),
    MC_INT("mask", "release_ms", mask.release_ms, 10, 5000),

    MC_BOOL("dots", "center_dot", dots.center_dot),
    MC_INT("dots", "center_dot_radius_px", dots.center_dot_radius_px, 1, 20),
    MC_COLOR("dots", "center_dot_color", dots.center_dot_color),
    MC_BOOL("dots", "center_dot_outline", dots.center_dot_outline),
    MC_BOOL("dots", "edge_dots", dots.edge_dots),
    MC_INT("dots", "edge_dot_radius_px", dots.edge_dot_radius_px, 1, 20),
    MC_COLOR("dots", "edge_dot_color", dots.edge_dot_color),
    MC_DBL("dots", "edge_dot_min_alpha", dots.edge_dot_min_alpha, 0, 1, 2),
    MC_DBL("dots", "edge_dot_max_alpha", dots.edge_dot_max_alpha, 0, 1, 2),
    MC_INT("dots", "edge_dot_margin_px", dots.edge_dot_margin_px, 0, 400),

    MC_BOOL("trigger", "mouse", trigger.mouse),
    MC_BOOL("trigger", "keyboard", trigger.keyboard),
    MC_BOOL("trigger", "gamepad", trigger.gamepad),
    MC_BOOL("trigger", "capture", trigger.capture),
    MC_ENUM("trigger", "capture_sensitivity", trigger.capture_sensitivity, Sensitivity, kSensitivityNames),
    MC_DBL("trigger", "default_counts_per_degree", trigger.default_counts_per_degree, 1, 1000, 1),
    MC_DBL("trigger", "v_start_low", trigger.thresholds[0].v_start, 1, 5000, 0),
    MC_DBL("trigger", "v_full_low", trigger.thresholds[0].v_full, 1, 5000, 0),
    MC_DBL("trigger", "v_start_medium", trigger.thresholds[1].v_start, 1, 5000, 0),
    MC_DBL("trigger", "v_full_medium", trigger.thresholds[1].v_full, 1, 5000, 0),
    MC_DBL("trigger", "v_start_high", trigger.thresholds[2].v_start, 1, 5000, 0),
    MC_DBL("trigger", "v_full_high", trigger.thresholds[2].v_full, 1, 5000, 0),
    MC_DBL("trigger", "move_intensity", trigger.move_intensity, 0, 1, 2),
    MC_DBL("trigger", "gamepad_max_deg_per_s", trigger.gamepad_max_deg_per_s, 10, 2000, 0),
    MC_DBL("trigger", "gamepad_deadzone", trigger.gamepad_deadzone, 0, 0.9, 2),
    MC_ENUM("trigger", "mouse_lock_rule", mouse_lock_rule, MouseLockRule, kLockRuleNames),
    MC_INT("trigger", "velocity_window_ms", trigger.velocity_window_ms, 10, 500),

    MC_LIST("detect", "exclude", detect.exclude),
    MC_LIST("detect", "force_game", detect.force_game),
    MC_LIST("detect", "known_games", detect.known_games),

    // [capture]（变更 3）：在 [hotkeys]、[calibration] 之后
    MC_INT("capture", "grid_w", capture.grid_w, 64, 320),
    MC_INT("capture", "grid_h", capture.grid_h, 36, 180),
    MC_INT("capture", "max_shift", capture.max_shift, 2, 40),
    MC_DBL("capture", "min_correlation", capture.min_correlation, 0.10, 0.99, 2),
    MC_DBL("capture", "assumed_hfov_deg", capture.assumed_hfov_deg, 30, 170, 0),
    MC_INT("capture", "hold_ms", capture.hold_ms, 0, 1000),
    // 变更 5：前进 / 倾斜检测
    MC_DBL("capture", "forward_gain", trigger.forward_gain, 0.5, 3.0, 1),
    MC_BOOL("capture", "detect_roll", capture.detect_roll),
    MC_INT("capture", "baseline_ms", capture.baseline_ms, 0, 150),
    MC_DBL("capture", "zoom_start_low", trigger.zoom_thresholds[0].v_start, 1, 5000, 0),
    MC_DBL("capture", "zoom_full_low", trigger.zoom_thresholds[0].v_full, 1, 5000, 0),
    MC_DBL("capture", "zoom_start_medium", trigger.zoom_thresholds[1].v_start, 1, 5000, 0),
    MC_DBL("capture", "zoom_full_medium", trigger.zoom_thresholds[1].v_full, 1, 5000, 0),
    MC_DBL("capture", "zoom_start_high", trigger.zoom_thresholds[2].v_start, 1, 5000, 0),
    MC_DBL("capture", "zoom_full_high", trigger.zoom_thresholds[2].v_full, 1, 5000, 0),
    MC_DBL("capture", "roll_start_low", trigger.roll_thresholds[0].v_start, 1, 5000, 0),
    MC_DBL("capture", "roll_full_low", trigger.roll_thresholds[0].v_full, 1, 5000, 0),
    MC_DBL("capture", "roll_start_medium", trigger.roll_thresholds[1].v_start, 1, 5000, 0),
    MC_DBL("capture", "roll_full_medium", trigger.roll_thresholds[1].v_full, 1, 5000, 0),
    MC_DBL("capture", "roll_start_high", trigger.roll_thresholds[2].v_start, 1, 5000, 0),
    MC_DBL("capture", "roll_full_high", trigger.roll_thresholds[2].v_full, 1, 5000, 0),

    // [ui]（变更 4）：设置窗口自己的界面状态，在 [capture] 之后
    MC_ENUM("ui", "language", ui.language, i18n::LangSetting, kLanguageNames),   // localization
    MC_BOOL("ui", "show_advanced", ui.show_advanced),
    MC_OPTINT("ui", "x", ui.x, -100000, 100000),
    MC_OPTINT("ui", "y", ui.y, -100000, 100000),
};

#undef MC_BOOL
#undef MC_INT
#undef MC_DBL
#undef MC_ENUM
#undef MC_COLOR
#undef MC_LIST
#undef MC_OPTINT

constexpr std::string_view kHotkeysSection = "hotkeys";
constexpr std::string_view kCalibrationSection = "calibration";   // 变更 2：动态节，键为 exe
constexpr std::string_view kExtraSection = "extra";
constexpr std::string_view kSensitivityZh[] = {"低", "中", "高"};

const KeySpec* find_spec(std::string_view section, std::string_view key) {
    for (const KeySpec& s : kSchema) {
        if (s.section == section && s.key == key) return &s;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 值的解析与格式化

std::optional<double> parse_number(std::string_view s) {
    if (s.empty()) return std::nullopt;
    double v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<Rgb> parse_color(std::string_view s) {
    if (s.size() != 6) return std::nullopt;
    int v[6];
    for (int k = 0; k < 6; ++k) {
        v[k] = hex_digit(s[static_cast<std::size_t>(k)]);
        if (v[k] < 0) return std::nullopt;
    }
    return Rgb{static_cast<std::uint8_t>(v[0] * 16 + v[1]), static_cast<std::uint8_t>(v[2] * 16 + v[3]),
               static_cast<std::uint8_t>(v[4] * 16 + v[5])};
}

std::string format_color(Rgb c) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s(6, '0');
    const std::uint8_t parts[3] = {c.r, c.g, c.b};
    for (int k = 0; k < 3; ++k) {
        s[static_cast<std::size_t>(2 * k)] = kHex[parts[k] >> 4];
        s[static_cast<std::size_t>(2 * k + 1)] = kHex[parts[k] & 0xF];
    }
    return s;
}

std::vector<std::wstring> parse_list(std::string_view raw) {
    std::vector<std::wstring> out;
    for (const std::string_view part : split(raw, ';')) {
        const std::string_view item = trim(part);
        if (item.empty()) continue;
        list_add_unique(out, utf8_to_wide(item));   // 忽略大小写去重，保留第一次出现的写法
    }
    return out;
}

std::string join_list(const std::vector<std::wstring>& list) {
    std::string out;
    for (std::size_t k = 0; k < list.size(); ++k) {
        if (k > 0) out += ';';
        out += wide_to_utf8(list[k]);
    }
    return out;
}

std::string enum_choices(const KeySpec& s) {
    std::string out;
    for (std::size_t k = 0; k < s.names.size(); ++k) {
        if (k > 0) out += " | ";
        out += s.names[k];
    }
    return out;
}

// 字段当前值的 INI 写法。
std::string format_value(const KeySpec& s, Config& c) {
    switch (s.type) {
        case KeyType::Bool: return *s.b(c) ? "1" : "0";
        case KeyType::Int: return std::to_string(*s.i(c));
        case KeyType::Double: return format_ini_number(*s.d(c), s.min_decimals);
        case KeyType::Enum: {
            const int v = s.eget(c);
            return v >= 0 && static_cast<std::size_t>(v) < s.names.size() ? std::string(s.names[static_cast<std::size_t>(v)])
                                                                            : std::string(s.names[0]);
        }
        case KeyType::Color: return format_color(*s.c(c));
        case KeyType::List: return join_list(*s.l(c));
        case KeyType::OptInt: {
            const std::optional<int>& v = *s.oi(c);
            return v ? std::to_string(*v) : std::string();
        }
    }
    return {};
}

std::string where(const KeySpec& s, std::string_view raw) {
    return "[" + std::string(s.section) + "] " + std::string(s.key) + "=" + std::string(raw);
}

// 把 raw 解析进 cfg 的对应字段；非法 → 默认值，越界 → 截断，并记 warning。
void apply_value(const KeySpec& s, std::string_view raw_value, Config& cfg, Config& defaults,
                 std::vector<std::string>* warnings) {
    auto warn = [&](std::string msg) {
        if (warnings) warnings->push_back(std::move(msg));
    };
    if (s.type == KeyType::List) {
        *s.l(cfg) = parse_list(raw_value);   // 列表键不识别行尾注释
        return;
    }
    const std::string_view v = ini::strip_inline_comment(raw_value);
    switch (s.type) {
        case KeyType::Bool: {
            if (v == "0" || v == "1") {
                *s.b(cfg) = v == "1";
            } else {
                *s.b(cfg) = *s.b(defaults);
                warn(where(s, v) + " 不是 0 或 1，改用默认值 " + format_value(s, defaults));
            }
            return;
        }
        case KeyType::Int:
        case KeyType::Double: {
            const auto num = parse_number(v);
            if (!num) {
                if (s.type == KeyType::Int) {
                    *s.i(cfg) = *s.i(defaults);
                } else {
                    *s.d(cfg) = *s.d(defaults);
                }
                warn(where(s, v) + " 不是数字，改用默认值 " + format_value(s, defaults));
                return;
            }
            double x = *num;
            if (s.type == KeyType::Int) x = std::round(x);   // 整数键写成小数时四舍五入
            if (x < s.lo || x > s.hi) {
                x = x < s.lo ? s.lo : s.hi;
                warn(where(s, v) + " 超出范围 [" + format_ini_number(s.lo, 0) + ", " + format_ini_number(s.hi, 0) +
                     "]，已截断为 " + format_ini_number(x, 0));
            }
            if (s.type == KeyType::Int) {
                *s.i(cfg) = static_cast<int>(x);
            } else {
                *s.d(cfg) = x == 0.0 ? 0.0 : x;   // 去掉 -0
            }
            return;
        }
        case KeyType::Enum: {
            for (std::size_t k = 0; k < s.names.size(); ++k) {
                if (iequals(v, s.names[k])) {
                    s.eset(cfg, static_cast<int>(k));
                    return;
                }
            }
            s.eset(cfg, s.eget(defaults));
            warn(where(s, v) + " 不是合法取值（" + enum_choices(s) + "），改用默认值 " + format_value(s, defaults));
            return;
        }
        case KeyType::Color: {
            if (const auto color = parse_color(v)) {
                *s.c(cfg) = *color;
            } else {
                *s.c(cfg) = *s.c(defaults);
                warn(where(s, v) + " 不是 RRGGBB 颜色，改用默认值 " + format_value(s, defaults));
            }
            return;
        }
        case KeyType::List: return;
        case KeyType::OptInt: {
            if (v.empty()) {
                *s.oi(cfg) = std::nullopt;
                return;
            }
            const auto num = parse_number(v);
            if (!num) {
                *s.oi(cfg) = *s.oi(defaults);
                warn(where(s, v) + " 不是整数，改为不设置");
                return;
            }
            const double x = std::round(*num);
            const double clamped = std::clamp(x, s.lo, s.hi);
            if (clamped != x) {
                warn(where(s, v) + " 超出范围 [" + format_ini_number(s.lo, 0) + ", " + format_ini_number(s.hi, 0) +
                     "]，已截断为 " + format_ini_number(clamped, 0));
            }
            *s.oi(cfg) = static_cast<int>(clamped);
            return;
        }
    }
}

// [calibration] 条目的值 "<counts_per_degree>[;<日期文本>]"（已剥离行尾注释）。合法时返回 cpd，并把 trim 后的日期文本
// （可能为空）写进 *date；否则返回 nullopt 并把原因写进 *error。
std::optional<double> parse_calibration_value(std::string_view v, std::string* date, std::string* error) {
    const std::size_t semi = v.find(';');
    const std::string_view num_text = trim(v.substr(0, semi));
    *date = semi == std::string_view::npos ? std::string() : std::string(trim(v.substr(semi + 1)));
    const auto num = parse_number(num_text);
    if (!num) {
        *error = " 不是数字，已忽略该条目";
        return std::nullopt;
    }
    if (!calib_cpd_valid(*num)) {
        *error = " 超出范围（每度计数 × 360 须在 " + std::to_string(kCalibMinCounts) + "–" +
                 std::to_string(kCalibMaxCounts) + " 之间，约 2.222–1111.111），已忽略该条目";
        return std::nullopt;
    }
    return *num;
}

void set_calibration(Config& cfg, const std::wstring& exe, double cpd, const std::string& date) {
    cfg.calibration[exe] = cpd;
    if (date.empty()) {
        cfg.calibration_dates.erase(exe);
    } else {
        cfg.calibration_dates[exe] = utf8_to_wide(date);
    }
}

// [calibration] 的一条：<exe>=<counts_per_degree>[;<日期>]。合法则写入（同名覆盖，连同日期），否则记 warning 并忽略。
void apply_calibration_entry(const ini::Entry& e, Config& cfg, std::vector<std::string>* warnings) {
    const std::string_view v = ini::strip_inline_comment(e.value);
    std::string date, error;
    const auto cpd = parse_calibration_value(v, &date, &error);
    if (!cpd) {
        if (warnings) warnings->push_back("[calibration] " + e.key_text + "=" + std::string(v) + error);
        return;
    }
    set_calibration(cfg, utf8_to_wide(e.key), *cpd, date);   // e.key 已转 ASCII 小写
}

// 设置窗口写入一个模式表键：解析失败返回 false（不改 cfg）；数值越界截断，范围与 apply_value 相同。
bool set_schema_value(const KeySpec& s, std::string_view v, Config& cfg) {
    switch (s.type) {
        case KeyType::Bool:
            if (v != "0" && v != "1") return false;
            *s.b(cfg) = v == "1";
            return true;
        case KeyType::Int:
        case KeyType::Double: {
            const auto num = parse_number(v);
            if (!num) return false;
            double x = s.type == KeyType::Int ? std::round(*num) : *num;
            x = std::clamp(x, s.lo, s.hi);
            if (s.type == KeyType::Int) {
                *s.i(cfg) = static_cast<int>(x);
            } else {
                *s.d(cfg) = x == 0.0 ? 0.0 : x;
            }
            return true;
        }
        case KeyType::Enum:
            for (std::size_t k = 0; k < s.names.size(); ++k) {
                if (iequals(v, s.names[k])) {
                    s.eset(cfg, static_cast<int>(k));
                    return true;
                }
            }
            return false;
        case KeyType::Color: {
            const auto color = parse_color(v);
            if (!color) return false;
            *s.c(cfg) = *color;
            return true;
        }
        case KeyType::List: *s.l(cfg) = parse_list(v); return true;
        case KeyType::OptInt: {
            if (v.empty()) {
                *s.oi(cfg) = std::nullopt;
                return true;
            }
            const auto num = parse_number(v);
            if (!num) return false;
            *s.oi(cfg) = static_cast<int>(std::clamp(std::round(*num), s.lo, s.hi));
            return true;
        }
    }
    return false;
}

void add_extra(Config& cfg, std::string name, std::string value) {
    for (auto& [k, v] : cfg.extras) {
        if (iequals(k, name)) {   // 重复键：以最后一次为准，位置保持第一次出现的地方
            v = std::move(value);
            return;
        }
    }
    cfg.extras.emplace_back(std::move(name), std::move(value));
}

}  // namespace

std::string format_ini_number(double v, int min_decimals) {
    if (v == 0.0) v = 0.0;   // -0 → 0
    char buf[400];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    std::string s = ec == std::errc{} ? std::string(buf, ptr) : std::string("0");
    if (min_decimals > 0) {
        std::size_t dot = s.find('.');
        if (dot == std::string::npos) {
            s += '.';
            dot = s.size() - 1;
        }
        std::size_t decimals = s.size() - dot - 1;
        while (decimals < static_cast<std::size_t>(min_decimals)) {
            s += '0';
            ++decimals;
        }
    }
    return s;
}

Config load_config(std::string_view utf8_text, std::vector<std::string>* warnings) {
    Config cfg;
    Config defaults;
    for (const ini::Entry& e : ini::parse_ini(utf8_text)) {
        if (e.section == kExtraSection) {
            add_extra(cfg, e.key_text, e.value);
            continue;
        }
        if (e.section == kCalibrationSection) {   // 键不固定：不会进入 [extra]
            apply_calibration_entry(e, cfg, warnings);
            continue;
        }
        if (e.section == kHotkeysSection) {
            if (const auto action = hotkey_action_from_key(e.key)) {
                cfg.hotkeys[hotkey_index(*action)] = utf8_to_wide(ini::strip_inline_comment(e.value));
                continue;
            }
        } else if (const KeySpec* spec = find_spec(e.section, e.key)) {
            apply_value(*spec, e.value, cfg, defaults, warnings);
            continue;
        }
        add_extra(cfg, e.section_text.empty() ? e.key_text : e.section_text + "." + e.key_text, e.value);
    }
    // 某档开始值 ≥ 满强度值：该档两个阈值都恢复默认（鼠标 [trigger] v_*，变更 5 的 [capture] zoom_* / roll_*）。
    struct Table {
        std::array<Thresholds, 3>* cur;
        const std::array<Thresholds, 3>* def;
        const char* section;
        const char* start;
        const char* full;
        const char* what;
    };
    const Table tables[] = {
        {&cfg.trigger.thresholds, &defaults.trigger.thresholds, "trigger", "v_start_", "v_full_", ""},
        {&cfg.trigger.zoom_thresholds, &defaults.trigger.zoom_thresholds, "capture", "zoom_start_", "zoom_full_", "前进 / 后退"},
        {&cfg.trigger.roll_thresholds, &defaults.trigger.roll_thresholds, "capture", "roll_start_", "roll_full_", "倾斜"},
    };
    for (const Table& tb : tables) {
        for (std::size_t k = 0; k < tb.cur->size(); ++k) {
            Thresholds& th = (*tb.cur)[k];
            if (th.v_start >= th.v_full) {
                const Thresholds def = (*tb.def)[k];
                if (warnings) {
                    const std::string level(kSensitivityNames[k]);
                    warnings->push_back("[" + std::string(tb.section) + "] " + tb.start + level + "=" +
                                        format_ini_number(th.v_start, 0) + " 不小于 " + tb.full + level + "=" +
                                        format_ini_number(th.v_full, 0) + "，" + tb.what +
                                        std::string(kSensitivityZh[k]) + "档阈值恢复默认 " +
                                        format_ini_number(def.v_start, 0) + " / " + format_ini_number(def.v_full, 0));
                }
                th = def;
            }
        }
    }
    return cfg;
}

std::string save_config_text(const Config& cfg_in, std::string_view template_text) {
    Config cfg = cfg_in;   // 访问器需要非 const 引用；只读不写
    const ini::Lookup lookup = [&cfg](std::string_view section, std::string_view key) -> std::optional<std::string> {
        if (section == kHotkeysSection) {
            if (const auto action = hotkey_action_from_key(key)) return wide_to_utf8(cfg.hotkeys[hotkey_index(*action)]);
            return std::nullopt;
        }
        if (const KeySpec* spec = find_spec(section, key)) return format_value(*spec, cfg);
        return std::nullopt;
    };
    // [calibration]：条目写在节标题及其注释块之后（ini::regenerate 按键名字母序展开）；没有条目时只剩注释，
    // 所以默认配置的输出仍与模板逐字节相同。
    ini::DynamicSection calibration;
    calibration.section = std::string(kCalibrationSection);
    for (const auto& [exe, cpd] : cfg.calibration) {
        if (exe.empty() || !calib_cpd_valid(cpd)) continue;
        std::string value = calib_format_cpd(cpd);
        if (const auto d = cfg.calibration_dates.find(exe); d != cfg.calibration_dates.end() && !d->second.empty()) {
            value += ';';
            value += wide_to_utf8(d->second);
        }
        calibration.entries.emplace_back(to_lower_ascii(wide_to_utf8(exe)), std::move(value));
    }
    return ini::regenerate(template_text, lookup, cfg.extras, {calibration});
}

// ---------------------------------------------------------------- 按 INI 键访问（设置窗口）

std::optional<std::wstring> config_get_value(const Config& cfg_in, std::string_view section, std::string_view key) {
    if (section == kHotkeysSection) {
        if (const auto action = hotkey_action_from_key(key)) return cfg_in.hotkeys[hotkey_index(*action)];
        return std::nullopt;
    }
    if (section == kCalibrationSection) {
        const std::wstring exe = utf8_to_wide(to_lower_ascii(key));
        const auto it = cfg_in.calibration.find(exe);
        if (it == cfg_in.calibration.end()) return std::nullopt;
        std::wstring v = utf8_to_wide(calib_format_cpd(it->second));
        if (const auto d = cfg_in.calibration_dates.find(exe); d != cfg_in.calibration_dates.end() && !d->second.empty()) {
            v += L';';
            v += d->second;
        }
        return v;
    }
    const KeySpec* spec = find_spec(section, key);
    if (spec == nullptr) return std::nullopt;
    Config cfg = cfg_in;   // 访问器需要非 const 引用；只读不写
    return utf8_to_wide(format_value(*spec, cfg));
}

bool config_set_value(Config& cfg, std::string_view section, std::string_view key, std::wstring_view text) {
    const std::string v = wide_to_utf8(trim(text));
    if (section == kHotkeysSection) {
        const auto action = hotkey_action_from_key(key);
        if (!action || !parse_hotkey(utf8_to_wide(v)).ok) return false;
        cfg.hotkeys[hotkey_index(*action)] = utf8_to_wide(v);
        return true;
    }
    if (section == kCalibrationSection) {
        const std::string exe_utf8 = to_lower_ascii(trim(key));
        if (exe_utf8.empty() || exe_utf8.find_first_of(";=[]") != std::string::npos) return false;
        const std::wstring exe = utf8_to_wide(exe_utf8);
        if (v.empty()) {
            cfg.calibration.erase(exe);
            cfg.calibration_dates.erase(exe);
            return true;
        }
        std::string date, error;
        const auto cpd = parse_calibration_value(v, &date, &error);
        if (!cpd) return false;
        set_calibration(cfg, exe, *cpd, date);
        return true;
    }
    const KeySpec* spec = find_spec(section, key);
    if (spec == nullptr) return false;
    Config next = cfg;
    if (!set_schema_value(*spec, v, next)) return false;
    cfg = std::move(next);
    return true;
}

std::optional<ConfigKeyRange> config_key_range(std::string_view section, std::string_view key) {
    const KeySpec* spec = find_spec(section, key);
    if (spec == nullptr) return std::nullopt;
    if (spec->type != KeyType::Int && spec->type != KeyType::Double && spec->type != KeyType::OptInt) {
        return std::nullopt;
    }
    return ConfigKeyRange{spec->lo, spec->hi};
}

bool config_thresholds_ordered(const Config& cfg) {
    for (const auto* table : {&cfg.trigger.thresholds, &cfg.trigger.zoom_thresholds, &cfg.trigger.roll_thresholds}) {
        for (const Thresholds& th : *table) {
            if (!(th.v_start < th.v_full)) return false;
        }
    }
    return true;
}

Config restore_defaults(const Config& cfg) {
    Config d;
    d.calibration = cfg.calibration;
    d.calibration_dates = cfg.calibration_dates;
    d.detect.force_game = cfg.detect.force_game;
    d.detect.known_games = cfg.detect.known_games;
    d.autostart = cfg.autostart;
    d.ui = cfg.ui;
    d.extras = cfg.extras;
    return d;
}

void clear_known_games(Config& cfg) { cfg.detect.known_games.clear(); }

// ---------------------------------------------------------------- 文件层

bool read_file(const std::wstring& path, std::string* out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30);
    std::string data;
    if (ok) {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t got = 0;
        while (got < data.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - got, 1u << 24));
            if (!::ReadFile(h, data.data() + got, want, &n, nullptr) || n == 0) break;
            got += n;
        }
        data.resize(got);
    }
    ::CloseHandle(h);
    if (ok) *out = std::move(data);
    return ok;
}

bool save_file_atomic(const std::wstring& path, std::string_view bytes) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 24));
        ok = ::WriteFile(h, bytes.data() + done, want, &n, nullptr) && n > 0;
        done += n;
    }
    ok = ok && ::FlushFileBuffers(h);
    ::CloseHandle(h);
    if (ok) ok = ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) ::DeleteFileW(tmp.c_str());
    return ok;
}

LoadedFile load_or_create(const std::wstring& path, std::string_view template_text) {
    LoadedFile r;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            if (!save_file_atomic(path, template_text)) {
                r.text.assign(template_text);
                r.writable = false;
                return r;
            }
            r.created = true;
        }
    }
    if (read_file(path, &r.text)) {
        r.from_disk = true;
        return r;
    }
    r.text.assign(template_text);
    r.writable = false;
    return r;
}

std::uint64_t content_hash(std::string_view bytes) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (const char ch : bytes) {
        h ^= static_cast<unsigned char>(ch);
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace mc
