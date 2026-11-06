// calibration.cpp — calib/calibration 的实现：按 design D2 的转移表推进状态、计数与结算规则、提示 / toast 文案、[calibration] 值的格式化与查表（纯逻辑，不含 Win32）。
#include "calib/calibration.h"

#include "i18n/i18n.h"

#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

#include "util/strings.h"

namespace mc {
namespace {

long long abs_ll(long long v) { return v < 0 ? -v : v; }

std::string fixed3(double v) {
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 3);
    return ec == std::errc{} ? std::string(buf, ptr) : std::string("0.000");
}

}  // namespace

const char* calib_state_name(CalibState s) {
    switch (s) {
        case CalibState::Idle: return "Idle";
        case CalibState::Run1: return "Run1";
        case CalibState::Between: return "Between";
        case CalibState::Run2: return "Run2";
        case CalibState::Done: return "Done";
    }
    return "?";
}

// ---------------------------------------------------------------- 规则（自由函数）

bool calib_count_valid(long long counts_360) { return counts_360 >= kCalibMinCounts && counts_360 <= kCalibMaxCounts; }

bool calib_runs_consistent(long long c1, long long c2) {
    // |c1 − c2| / ((c1 + c2) / 2) ≤ 0.15  ⇔  |c1 − c2| ≤ 0.075·(c1 + c2)  ⇔  40·|c1 − c2| ≤ 3·(c1 + c2)
    if (c1 < 0 || c2 < 0) return false;
    const long long sum = c1 + c2;
    if (sum == 0) return false;
    return 40 * abs_ll(c1 - c2) <= 3 * sum;
}

double calib_counts_per_degree(long long c1, long long c2) {
    return (static_cast<double>(c1) + static_cast<double>(c2)) / 2.0 / 360.0;
}

std::wstring calib_result_toast(double counts_per_degree) {
    return i18n::format(i18n::StrId::CalibDone, {{L"cpd", std::format(L"{:.1f}", counts_per_degree)}});
}

bool calib_cpd_valid(double counts_per_degree) {
    if (!std::isfinite(counts_per_degree)) return false;
    const double counts = counts_per_degree * 360.0;
    return counts >= static_cast<double>(kCalibMinCounts) && counts <= static_cast<double>(kCalibMaxCounts);
}

std::wstring calib_format_date(int year, int month, int day) {
    return std::format(L"{:04}-{:02}-{:02}", year, month, day);
}

std::string calib_format_cpd(double counts_per_degree) {
    std::string s = fixed3(counts_per_degree);
    double back = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), back);
    if (ec == std::errc{} && ptr == s.data() + s.size() && calib_cpd_valid(counts_per_degree) && !calib_cpd_valid(back)) {
        // 四舍五入把合法值推出了范围（800 / 360 = 2.2222… → "2.222"，× 360 = 799.92）：向范围内取整。
        const double lo = static_cast<double>(kCalibMinCounts) / 360.0;
        const double hi = static_cast<double>(kCalibMaxCounts) / 360.0;
        const double fixed = back * 360.0 < static_cast<double>(kCalibMinCounts) ? std::ceil(lo * 1000.0) / 1000.0
                                                                                 : std::floor(hi * 1000.0) / 1000.0;
        s = fixed3(fixed);
    }
    return s;
}

double counts_per_degree_for(std::wstring_view exe, const std::map<std::wstring, double>& table, double fallback) {
    if (exe.empty() || table.empty()) return fallback;
    const auto it = table.find(to_lower_ascii(exe));
    if (it == table.end() || !calib_cpd_valid(it->second)) return fallback;
    return it->second;
}

// ---------------------------------------------------------------- 状态机

long long Calibration::current_abs_count() const { return abs_ll(sum_dx_); }

void Calibration::enter(CalibState s, double now_s) {
    state_ = s;
    t_enter_ = now_s;
}

void Calibration::reset() {
    state_ = CalibState::Idle;
    target_.clear();
    sum_dx_ = 0;
    c1_ = 0;
    t_enter_ = 0;
}

void Calibration::cancel() { reset(); }

void Calibration::on_mouse_dx(long long dx) {
    if (state_ == CalibState::Run1 || state_ == CalibState::Run2) sum_dx_ += dx;
}

CalibEffect Calibration::check_abort(double now_s, const CalibContext& ctx) {
    CalibEffect e;
    if (state_ == CalibState::Idle) return e;
    if (ctx.exe.empty() || !iequals(std::wstring_view(ctx.exe), std::wstring_view(target_))) {
        reset();
        e.toast = i18n::tr(i18n::StrId::CalibLeftGame);
    } else if (now_s - t_enter_ > kCalibStateTimeoutS) {
        reset();
        e.toast = i18n::tr(i18n::StrId::CalibTimeout);
    }
    return e;
}

CalibEffect Calibration::update(double now_s, const CalibContext& ctx) { return check_abort(now_s, ctx); }

CalibEffect Calibration::on_hotkey(double now_s, const CalibContext& ctx) {
    CalibEffect e;
    if (state_ == CalibState::Idle) {
        if (!ctx.in_game || !ctx.cursor_locked || ctx.exe.empty()) {
            e.toast = i18n::tr(i18n::StrId::CalibNotReady);
            return e;
        }
        reset();
        target_ = to_lower_ascii(std::wstring_view(ctx.exe));
        enter(CalibState::Run1, now_s);
        return e;
    }

    e = check_abort(now_s, ctx);   // 热键路径上先检查"离开游戏"（与超时），再执行其他转移
    if (!e.empty()) return e;

    switch (state_) {
        case CalibState::Run1: {
            const long long n = current_abs_count();
            if (!calib_count_valid(n)) {
                reset();
                e.toast = i18n::tr(i18n::StrId::CalibBadCount);
                return e;
            }
            c1_ = n;
            sum_dx_ = 0;
            enter(CalibState::Between, now_s);
            return e;
        }
        case CalibState::Between:
            sum_dx_ = 0;
            enter(CalibState::Run2, now_s);
            return e;
        case CalibState::Run2: {
            const long long c2 = current_abs_count();
            if (!calib_count_valid(c2)) {
                sum_dx_ = 0;   // 保留第 1 次的结果，回到 Between 重测第 2 次
                enter(CalibState::Between, now_s);
                e.toast = i18n::tr(i18n::StrId::CalibBadCount);
                return e;
            }
            if (!calib_runs_consistent(c1_, c2)) {
                reset();
                e.toast = i18n::tr(i18n::StrId::CalibInconsistent);
                return e;
            }
            enter(CalibState::Done, now_s);   // 瞬时：结算后立即回到 Idle
            const double cpd = calib_counts_per_degree(c1_, c2);
            e.save = CalibSave{target_, cpd, c1_, c2};
            e.toast = calib_result_toast(cpd);
            reset();
            return e;
        }
        case CalibState::Idle:
        case CalibState::Done: break;
    }
    return e;
}

std::wstring Calibration::message(std::wstring_view hotkey_text) const {
    const std::wstring hk(hotkey_text);
    switch (state_) {
        case CalibState::Run1:
            return i18n::format(i18n::StrId::CalibRun1, {{L"hk", hk}, {L"n", std::to_wstring(current_abs_count())}});
        case CalibState::Between:
            return i18n::format(i18n::StrId::CalibBetween, {{L"hk", hk}, {L"n", std::to_wstring(c1_)}});
        case CalibState::Run2:
            return i18n::format(i18n::StrId::CalibRun2, {{L"hk", hk}, {L"n", std::to_wstring(current_abs_count())}});
        case CalibState::Idle:
        case CalibState::Done: break;
    }
    return {};
}

}  // namespace mc
