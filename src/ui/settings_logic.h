// settings_logic.h — 设置窗口的纯逻辑（不含窗口代码，可单测）：滑块位置 ⇄ 值、数值文本解析 / 钳制 / 格式化、颜色文本、阈值顺序、边缘点最淡 / 最浓互推、程序名单的解析 / 校验、校准列表的显示文字与重测指引、几处随状态变化的标签。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "model/intensity.h"   // Rgb

namespace mc::ui {

// ---------------------------------------------------------------- 滑块
// 滑块位置 0..n 对应值 lo + pos × step（settings-window design D7）。

// n = round((hi − lo) / step)，至少 0。
int slider_max_pos(double lo, double hi, double step);
// 值 → 最接近的位置（钳制到 0..n）。不在步长上的值停在最近的位置，不改动值本身。
int slider_pos_for(double value, double lo, double hi, double step);
// 位置 → 值：lo + pos × step，四舍五入到 decimals 位小数，再钳制到 [lo, hi]。
double slider_value_at(int pos, double lo, double hi, double step, int decimals);

// ---------------------------------------------------------------- 数值文本

// 四舍五入到 decimals 位小数（远离 0 取整，带一点相对容差，使 0.355 → 0.36）。
double round_decimals(double v, int decimals);
// 定点 decimals 位小数，'.' 为小数点（与区域设置无关），不输出 "-0"。
std::wstring format_fixed(double v, int decimals);
// 先四舍五入到 max_decimals 位，再写成最短往返形式并补零到至少 min_decimals 位：
// format_number(400, 1, 0) = "400"，format_number(60.25, 1, 0) = "60.3"，format_number(9, 1, 1) = "9.0"。
std::wstring format_number(double v, int max_decimals, int min_decimals);
// 解析用户输入的数字：去首尾空白；全角数字 / 句点 / 正负号按半角处理；只接受 [+-]digits[.digits]（或 .digits）。
std::optional<double> parse_number_text(std::wstring_view text);

enum class NumberCommitKind { Invalid, Ok, Clamped };
struct NumberCommit {
    NumberCommitKind kind = NumberCommitKind::Invalid;
    double value = 0;
};
// 编辑框提交：解析失败 → Invalid；否则四舍五入到 max_decimals 位后钳制到 [lo, hi]，被钳制时为 Clamped。
NumberCommit commit_number(std::wstring_view text, double lo, double hi, int max_decimals);
// 气泡提示里的范围："范围 1–5000"、"范围 1.0–1000.0"、"范围 0.05–1.00"（数值按 min_decimals 补零；按当前界面语言）。
std::wstring range_text(double lo, double hi, int min_decimals);

// ---------------------------------------------------------------- 滑块右侧的数值读数（docs/settings-ui.md §2：带单位、不显示内部小数）

enum class Readout {
    Percent,        // 0.75 → "75%"
    ScreenHeight,   // 0.60 → "屏高的 60%"
    Millis,         // 100 → "100 毫秒"
    DegPerSec,      // 60 → "60°/秒"
    Pixels,         // 3 → "3 像素"
    Degrees,        // 90 → "90°"
    Times,          // 1.25 → "1.25 倍"
    Gain,           // 1.5 → "1.5 倍"（一位小数，变更 5 的前进灵敏度）
};
// 读数显示实际值：不在步长上的值照实显示（feather=0.355 → "35.5%"）。单位按当前界面语言（zh-TW "螢幕高度的 60%"，en "60% of screen height"、"100 ms"、"60°/s"、"3 px"、"1.25×"）。
std::wstring format_readout(double value, Readout kind);

// ---------------------------------------------------------------- 颜色

// 恰好 6 位十六进制（不区分大小写，可有首尾空白）→ 颜色；否则 nullopt。
std::optional<Rgb> parse_color_text(std::wstring_view text);
// 大写 RRGGBB。
std::wstring format_color_text(Rgb c);

// ---------------------------------------------------------------- 联动规则

// 同一档阈值：开始出现 < 满强度。
bool thresholds_ordered(double v_start, double v_full);
// 边缘点最淡 / 最浓互推：改的是最淡且超过最浓 → 最浓跟到最淡；改的是最浓且低于最淡 → 最淡跟到最浓。
struct AlphaPair {
    double min_alpha = 0;
    double max_alpha = 0;
};
AlphaPair push_alpha_pair(double min_alpha, double max_alpha, bool min_changed);

// ---------------------------------------------------------------- 程序名单（[detect] exclude / force_game / known_games）

// "a.exe;b.exe" → 各项去首尾空白、丢弃空项、按 ASCII 大小写不敏感去重（保留第一次出现的写法）。
std::vector<std::wstring> parse_name_list(std::wstring_view text);
// 用 ';' 连接（与 INI 写法相同）。
std::wstring join_name_list(const std::vector<std::wstring>& list);

enum class NameCheck { Ok, Empty, BadChar, Duplicate };
// 添加前的校验：去首尾空白后为空 → Empty；含 ';' 或 '=' → BadChar；已在 list 中（不区分大小写）→ Duplicate。
// trimmed 非空时写入去空白后的名称。
NameCheck check_list_name(std::wstring_view raw, const std::vector<std::wstring>& list, std::wstring* trimmed = nullptr);
// 校验失败时的气泡文字；Ok 返回空。
std::wstring_view name_check_message(NameCheck c);
// "已知游戏：N 个"。
std::wstring known_games_label(std::size_t count);

// ---------------------------------------------------------------- 校准页

// 形如 YYYY-MM-DD 的合法公历日期（年 1900–9999，按月份 / 闰年检查日）。
bool is_calendar_date(std::wstring_view text);
// "每度计数"列：两位小数，例如 18.4 → "18.40"。
std::wstring format_cpd_cell(double counts_per_degree);
// "校准日期"列：合法日期原样显示，否则（含空）显示 "—"。
std::wstring format_date_cell(std::wstring_view date);
// "重测"指引。hotkey_text 为 calibrate 当前绑定的规范字符串，空 = 未绑定（None 或写法非法）。
std::wstring recalibrate_hint(std::wstring_view exe, std::wstring_view hotkey_text);
// 列表为空时列表区域中央的说明（快捷键文字随当前绑定变化）。
std::wstring calibration_empty_text(std::wstring_view hotkey_text);

// ---------------------------------------------------------------- 确认框文案（docs/settings-ui.md §4）

struct ConfirmText {
    std::wstring instruction;   // 主说明（一句话问题）
    std::wstring content;       // 会发生什么、保留什么
    std::wstring verb;          // 确认按钮的动词（另一个按钮固定为"取消"）
};
ConfirmText restore_defaults_confirm();
ConfirmText clear_known_games_confirm(std::size_t count);
ConfirmText delete_calibration_confirm(std::wstring_view exe, double counts_per_degree);

// ---------------------------------------------------------------- 其他随状态变化的文字

// 触发页"截屏运动检测"复选框的文字（当前界面语言，不含访问键）。
std::wstring capture_checkbox_label(bool disabled_by_cmdline, bool capture_failed);
// 上面文字的状态后缀（"（已由 --no-capture 禁用）"、"（采集失败）"或空）；触发页把它接在带访问键的名字后面。
std::wstring capture_label_suffix(bool disabled_by_cmdline, bool capture_failed);

}  // namespace mc::ui
