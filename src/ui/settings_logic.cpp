// settings_logic.cpp — ui/settings_logic 的实现（纯逻辑）。
#include "ui/settings_logic.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

#include "i18n/i18n.h"
#include "util/strings.h"

namespace mc::ui {
namespace {

using i18n::StrId;

double pow10i(int d) {
    double p = 1.0;
    for (int i = 0; i < d; ++i) p *= 10.0;
    return p;
}

std::wstring ascii_to_wide(std::string_view s) { return std::wstring(s.begin(), s.end()); }

// 最短往返的定点写法（"0.35"、"400"、"0.355"）。
std::string shortest_fixed(double v) {
    if (v == 0.0) v = 0.0;   // -0 → 0
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    if (ec != std::errc{}) return "0";
    return std::string(buf, ptr);
}

std::string pad_decimals(std::string s, int min_decimals) {
    if (min_decimals <= 0) return s;
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
    return s;
}

int hex_value(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

}  // namespace

// ---------------------------------------------------------------- 滑块

int slider_max_pos(double lo, double hi, double step) {
    if (!(step > 0.0) || !(hi > lo)) return 0;
    return static_cast<int>(std::lround((hi - lo) / step));
}

int slider_pos_for(double value, double lo, double hi, double step) {
    const int n = slider_max_pos(lo, hi, step);
    if (n == 0 || !std::isfinite(value)) return 0;
    const long pos = std::lround((value - lo) / step);
    return static_cast<int>(std::clamp<long>(pos, 0, n));
}

double slider_value_at(int pos, double lo, double hi, double step, int decimals) {
    const int n = slider_max_pos(lo, hi, step);
    pos = std::clamp(pos, 0, n);
    const double v = round_decimals(lo + static_cast<double>(pos) * step, decimals);
    return std::clamp(v, lo, hi);
}

// ---------------------------------------------------------------- 数值文本

double round_decimals(double v, int decimals) {
    if (!std::isfinite(v)) return v;
    const double p = pow10i(std::max(decimals, 0));
    const double scaled = v * p;
    // 相对容差：0.355 × 100 = 35.4999…，按十进制字面意思应进位。
    const double nudged = scaled + std::copysign(std::max(1e-9, std::fabs(scaled) * 1e-12), scaled);
    const double r = std::round(nudged) / p;
    return r == 0.0 ? 0.0 : r;
}

std::wstring format_fixed(double v, int decimals) {
    v = round_decimals(v, decimals);
    char buf[64];
    const auto [ptr, ec] =
        std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, std::max(decimals, 0));
    if (ec != std::errc{}) return L"0";
    return ascii_to_wide(std::string_view(buf, static_cast<std::size_t>(ptr - buf)));
}

std::wstring format_number(double v, int max_decimals, int min_decimals) {
    return ascii_to_wide(pad_decimals(shortest_fixed(round_decimals(v, max_decimals)), min_decimals));
}

std::optional<double> parse_number_text(std::wstring_view text) {
    std::string ascii;
    for (const wchar_t raw : trim(text)) {
        wchar_t c = raw;
        if (c >= 0xFF10 && c <= 0xFF19) c = static_cast<wchar_t>(L'0' + (c - 0xFF10));   // 全角数字
        if (c == 0xFF0E || c == 0x3002) c = L'.';                                         // 全角句点 / 句号
        if (c == 0xFF0D || c == 0x2212) c = L'-';
        if (c == 0xFF0B) c = L'+';
        if (!((c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'+')) return std::nullopt;
        ascii += static_cast<char>(c);
    }
    std::string_view s = ascii;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        if (s.size() == 1) return std::nullopt;
    }
    const bool negative = !s.empty() && s[0] == '-';
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) s.remove_prefix(1);
    // 只允许 digits[.digits] 或 .digits
    int digits = 0, dots = 0;
    for (const char c : s) {
        if (c == '.') {
            ++dots;
        } else if (c >= '0' && c <= '9') {
            ++digits;
        } else {
            return std::nullopt;   // 中间的正负号
        }
    }
    if (digits == 0 || dots > 1) return std::nullopt;
    double v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, std::chars_format::fixed);
    if (ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
    v = negative ? -v : v;
    return v == 0.0 ? 0.0 : v;
}

NumberCommit commit_number(std::wstring_view text, double lo, double hi, int max_decimals) {
    const auto parsed = parse_number_text(text);
    if (!parsed) return {};
    NumberCommit r;
    r.kind = NumberCommitKind::Ok;
    r.value = round_decimals(*parsed, max_decimals);
    if (r.value < lo || r.value > hi) {
        r.value = std::clamp(r.value, lo, hi);
        r.kind = NumberCommitKind::Clamped;
    }
    return r;
}

std::wstring range_text(double lo, double hi, int min_decimals) {
    return i18n::format(StrId::RangeText,
                        {{L"lo", format_number(lo, 6, min_decimals)}, {L"hi", format_number(hi, 6, min_decimals)}});
}

// ---------------------------------------------------------------- 数值读数

std::wstring format_readout(double v, Readout kind) {
    switch (kind) {
        case Readout::Percent: return format_number(v * 100.0, 1, 0) + L"%";
        case Readout::ScreenHeight: return i18n::format(StrId::ReadoutScreenHeight, {{L"v", format_number(v * 100.0, 1, 0)}});
        case Readout::Millis: return i18n::format(StrId::ReadoutMillis, {{L"v", format_number(v, 0, 0)}});
        case Readout::DegPerSec: return i18n::format(StrId::ReadoutDegPerSec, {{L"v", format_number(v, 1, 0)}});
        case Readout::Pixels: return i18n::format(StrId::ReadoutPixels, {{L"v", format_number(v, 0, 0)}});
        case Readout::Degrees: return format_number(v, 1, 0) + L"°";
        case Readout::Times: return i18n::format(StrId::ReadoutTimes, {{L"v", format_number(v, 3, 2)}});
        case Readout::Gain: return i18n::format(StrId::ReadoutTimes, {{L"v", format_number(v, 2, 1)}});
    }
    return format_number(v, 3, 0);
}

// ---------------------------------------------------------------- 颜色

std::optional<Rgb> parse_color_text(std::wstring_view text) {
    const std::wstring_view s = trim(text);
    if (s.size() != 6) return std::nullopt;
    int v[6];
    for (std::size_t k = 0; k < 6; ++k) {
        v[k] = hex_value(s[k]);
        if (v[k] < 0) return std::nullopt;
    }
    return Rgb{static_cast<std::uint8_t>(v[0] * 16 + v[1]), static_cast<std::uint8_t>(v[2] * 16 + v[3]),
               static_cast<std::uint8_t>(v[4] * 16 + v[5])};
}

std::wstring format_color_text(Rgb c) {
    static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring s(6, L'0');
    const std::uint8_t parts[3] = {c.r, c.g, c.b};
    for (std::size_t k = 0; k < 3; ++k) {
        s[2 * k] = kHex[parts[k] >> 4];
        s[2 * k + 1] = kHex[parts[k] & 0xF];
    }
    return s;
}

// ---------------------------------------------------------------- 联动规则

bool thresholds_ordered(double v_start, double v_full) { return v_start < v_full; }

AlphaPair push_alpha_pair(double min_alpha, double max_alpha, bool min_changed) {
    AlphaPair p{min_alpha, max_alpha};
    if (min_changed) {
        if (p.min_alpha > p.max_alpha) p.max_alpha = p.min_alpha;
    } else {
        if (p.max_alpha < p.min_alpha) p.min_alpha = p.max_alpha;
    }
    return p;
}

// ---------------------------------------------------------------- 程序名单

std::vector<std::wstring> parse_name_list(std::wstring_view text) {
    std::vector<std::wstring> out;
    for (const std::wstring_view part : split(text, L';')) {
        const std::wstring_view item = trim(part);
        if (item.empty()) continue;
        bool dup = false;
        for (const auto& e : out) {
            if (iequals(e, item)) {
                dup = true;
                break;
            }
        }
        if (!dup) out.emplace_back(item);
    }
    return out;
}

std::wstring join_name_list(const std::vector<std::wstring>& list) {
    std::wstring out;
    for (std::size_t k = 0; k < list.size(); ++k) {
        if (k > 0) out += L';';
        out += list[k];
    }
    return out;
}

NameCheck check_list_name(std::wstring_view raw, const std::vector<std::wstring>& list, std::wstring* trimmed) {
    const std::wstring_view name = trim(raw);
    if (trimmed != nullptr) trimmed->assign(name);
    if (name.empty()) return NameCheck::Empty;
    if (name.find(L';') != std::wstring_view::npos || name.find(L'=') != std::wstring_view::npos) {
        return NameCheck::BadChar;
    }
    for (const auto& e : list) {
        if (iequals(e, name)) return NameCheck::Duplicate;
    }
    return NameCheck::Ok;
}

std::wstring_view name_check_message(NameCheck c) {
    switch (c) {
        case NameCheck::Ok: return L"";
        case NameCheck::Empty: return i18n::tr(StrId::GenNameEmpty);
        case NameCheck::BadChar: return i18n::tr(StrId::GenNameBadChar);
        case NameCheck::Duplicate: return i18n::tr(StrId::GenNameDuplicate);
    }
    return L"";
}

std::wstring known_games_label(std::size_t count) {
    return i18n::format(StrId::GenKnownCount, {{L"n", std::to_wstring(count)}});
}

// ---------------------------------------------------------------- 校准页

bool is_calendar_date(std::wstring_view t) {
    if (t.size() != 10 || t[4] != L'-' || t[7] != L'-') return false;
    auto num = [&](std::size_t pos, std::size_t len, int* out) {
        int v = 0;
        for (std::size_t k = pos; k < pos + len; ++k) {
            if (t[k] < L'0' || t[k] > L'9') return false;
            v = v * 10 + (t[k] - L'0');
        }
        *out = v;
        return true;
    };
    int y = 0, m = 0, d = 0;
    if (!num(0, 4, &y) || !num(5, 2, &m) || !num(8, 2, &d)) return false;
    if (y < 1900 || m < 1 || m > 12 || d < 1) return false;
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int dim = (m == 2 && is_leap(y)) ? 29 : kDays[m - 1];
    return d <= dim;
}

std::wstring format_cpd_cell(double counts_per_degree) { return format_fixed(counts_per_degree, 2); }

std::wstring format_date_cell(std::wstring_view date) {
    return is_calendar_date(date) ? std::wstring(date) : std::wstring(L"—");
}

std::wstring recalibrate_hint(std::wstring_view exe, std::wstring_view hotkey_text) {
    if (hotkey_text.empty()) return i18n::format(StrId::CalRecalHintNoHotkey, {{L"exe", std::wstring(exe)}});
    return i18n::format(StrId::CalRecalHint, {{L"exe", std::wstring(exe)}, {L"hk", std::wstring(hotkey_text)}});
}

std::wstring calibration_empty_text(std::wstring_view hotkey_text) {
    if (hotkey_text.empty()) return i18n::tr(StrId::CalEmptyNoHotkey);
    return i18n::format(StrId::CalEmpty, {{L"hk", std::wstring(hotkey_text)}});
}

// ---------------------------------------------------------------- 确认框文案

ConfirmText restore_defaults_confirm() {
    return {i18n::tr(StrId::ConfirmRestoreInstr), i18n::tr(StrId::ConfirmRestoreBody), i18n::tr(StrId::ConfirmRestoreVerb)};
}

ConfirmText clear_known_games_confirm(std::size_t count) {
    return {i18n::tr(StrId::ConfirmClearKnownInstr),
            i18n::format(StrId::ConfirmClearKnownBody, {{L"n", std::to_wstring(count)}}),
            i18n::tr(StrId::ConfirmClearKnownVerb)};
}

ConfirmText delete_calibration_confirm(std::wstring_view exe, double counts_per_degree) {
    const std::wstring e(exe);
    return {i18n::format(StrId::ConfirmDeleteCalibInstr, {{L"exe", e}}),
            i18n::format(StrId::ConfirmDeleteCalibBody, {{L"exe", e}, {L"cpd", format_fixed(counts_per_degree, 1)}}),
            i18n::tr(StrId::ConfirmDeleteCalibVerb)};
}

// ---------------------------------------------------------------- 其他

std::wstring capture_checkbox_label(bool disabled_by_cmdline, bool capture_failed) {
    std::wstring s = i18n::tr(StrId::CaptureName);
    return s + capture_label_suffix(disabled_by_cmdline, capture_failed);
}

std::wstring capture_label_suffix(bool disabled_by_cmdline, bool capture_failed) {
    if (disabled_by_cmdline) return i18n::tr(StrId::CaptureSuffixCli);
    if (capture_failed) return i18n::tr(StrId::CaptureSuffixFailed);
    return {};
}

}  // namespace mc::ui
