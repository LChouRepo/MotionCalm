// config.h — 带类型的 Config（默认值写在成员初始化器里，与 configuration spec 一致）、INI 文本 ⇄ Config（纯逻辑），以及配置文件 I/O。
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture/capture_policy.h"
#include "detect/game_detect.h"
#include "i18n/i18n.h"
#include "model/intensity.h"
#include "tray/hotkeys.h"
#include "util/log.h"

namespace mc {

// [general] hide_from_capture。
enum class HideFromCapture { Auto = 0, Always = 1, Never = 2 };

// [dots] 节。
struct DotConfig {
    bool center_dot = true;
    int center_dot_radius_px = 3;
    Rgb center_dot_color{0xFF, 0xFF, 0xFF};
    bool center_dot_outline = true;
    bool edge_dots = true;
    int edge_dot_radius_px = 3;
    Rgb edge_dot_color{0xC8, 0xC8, 0xC8};
    double edge_dot_min_alpha = 0.15;
    double edge_dot_max_alpha = 0.75;
    int edge_dot_margin_px = 24;
    bool operator==(const DotConfig&) const = default;
};

// [capture] 节（变更 3 / 5）：除 forward_gain（在 TriggerConfig）外的键与截屏服务的参数一一对应，范围见 configuration spec「截屏配置值的校验」。
using CaptureConfig = CaptureParams;

// [ui] 节（变更 4）：界面语言（localization）与设置窗口自己的界面状态，由设置窗口维护（docs/settings-ui.md）。
struct UiConfig {
    i18n::LangSetting language = i18n::LangSetting::Auto;   // 界面语言（localization）：auto 跟随系统
    bool show_advanced = false;   // 各页是否显示"高级选项"
    std::optional<int> x;         // 设置窗口左上角的屏幕坐标；空 = 居中于鼠标所在显示器
    std::optional<int> y;
    bool operator==(const UiConfig&) const = default;
};

// MotionCalm.ini 的全部持久化设置。字段按"使用方"分组，与 INI 节的对应关系见各注释。
struct Config {
    // [general]（mask_level 在 mask.level，sensitivity 在 trigger.sensitivity）
    bool enabled = true;
    bool autostart = false;
    HideFromCapture hide_from_capture = HideFromCapture::Auto;
    log::Level log_level = log::Level::Info;
    // [mask] + [general] mask_level
    MaskConfig mask;
    // [dots]
    DotConfig dots;
    // [trigger] + [general] sensitivity（mouse_lock_rule 单独放在下面；含变更 3 的 capture / capture_sensitivity）
    TriggerConfig trigger;
    // [trigger] mouse_lock_rule
    MouseLockRule mouse_lock_rule = MouseLockRule::Strict;
    // [detect]
    DetectLists detect = default_detect_lists();
    // [hotkeys]：按 kHotkeyActions 顺序保存用户写的原始字符串（包括非法串），保存时原样写回
    std::array<std::wstring, kHotkeyActionCount> hotkeys = default_hotkey_strings();
    // [calibration]：小写 exe → counts_per_degree（只含合法条目；std::map 使写回天然按字母序）
    std::map<std::wstring, double> calibration;
    // [calibration] 条目值中 ';' 之后的日期文本（原样保存以便往返，可能不是合法日期）；键是 calibration 的键，
    // 没有日期的条目不在表中（变更 4，configuration「[calibration] 节」）
    std::map<std::wstring, std::wstring> calibration_dates;
    // [capture]（变更 3）
    CaptureConfig capture;
    // [ui]（变更 4）
    UiConfig ui;
    // 未知键与 [extra] 中的键：("原节名.键" 或 [extra] 里的原键名, 原始值)，UTF-8，保存时写到 [extra]
    std::vector<std::pair<std::string, std::string>> extras;

    bool operator==(const Config&) const = default;
};

// 解析 INI 文本（UTF-8，可带 BOM）→ Config。从默认值出发：标量键剥离行尾注释，列表键按 ; 切分并去重；
// 非法值改用默认值、越界截断、某档开始值 ≥ 满强度值时该档恢复默认（v_* / zoom_* / roll_*），每个问题向 warnings 追加一条（UTF-8）。
// [calibration] 的每个键（转小写）是一个 exe，值为 "<counts_per_degree>[;<日期文本>]"：第一个 ; 之前不是数字或 × 360
// 不在 [800, 400000] 内时忽略该条目并记一条 warning；日期文本（trim 后非空）原样存进 calibration_dates，不校验、
// 不记 warning；同名条目以最后一条为准（连同日期）。未知节 / 未知键收进 extras。缺失的键保持默认值。
Config load_config(std::string_view utf8_text, std::vector<std::string>* warnings = nullptr);

// 以模板文本为骨架生成完整的 INI（见 ini::regenerate）。数值写最短往返形式并补零到默认值的小数位，
// 颜色写大写 RRGGBB，列表用 ; 连接，热键字符串原样写回，[calibration] 条目按 exe 字母序写在该节注释之后
// （固定 3 位小数，有日期文本时后接 ;<日期文本>），[ui] x / y 未设置时写空，extras 写到末尾 [extra]。
std::string save_config_text(const Config& cfg, std::string_view template_text);

// 数值的 INI 写法：std::to_chars 定点最短往返形式，再补零到至少 min_decimals 位小数（与区域设置无关）。
std::string format_ini_number(double v, int min_decimals);

// ---------------------------------------------------------------- 按 INI 键访问（设置窗口用，变更 4）
// 节名 / 键名为小写 ASCII（与 INI 相同）。全部由模式表驱动，界面范围与 INI 校验范围是同一份定义
// （configuration「界面范围与校验范围一致」）。

// 数值键的合法范围（与 load_config 截断用的范围相同）。
struct ConfigKeyRange {
    double lo = 0;
    double hi = 0;
};

// 当前值的 INI 写法（与 save_config_text 写出的相同）：[hotkeys] 返回用户原串（可能非法）；[ui] x / y 未设置时为空串；
// [calibration] 的键是 exe，值为 "<3 位小数>[;<日期文本>]"。不认识的键返回 nullopt。
std::optional<std::wstring> config_get_value(const Config& cfg, std::string_view section, std::string_view key);
// 按模式表解析 text 并写入 cfg：布尔只接受 0 / 1、枚举不区分大小写、颜色为 RRGGBB、数值越界截断（整数键四舍五入）、
// 列表按 ; 切分去重、[hotkeys] 须能被 parse_hotkey 解析（原样保存 text 去掉首尾空白）、[ui] x / y 为空串表示未设置。
// [calibration]：text 为 "<counts_per_degree>[;<日期文本>]"（规则同 load_config），空串表示删除该条目。
// 不认识的键或无法解析的值返回 false 且不修改 cfg。不检查阈值顺序（见 config_thresholds_ordered）。
bool config_set_value(Config& cfg, std::string_view section, std::string_view key, std::wstring_view text);
// 数值键（整数 / 小数）的合法范围；非数值键或不认识的键返回 nullopt。
std::optional<ConfigKeyRange> config_key_range(std::string_view section, std::string_view key);
// 鼠标、前进 / 后退（zoom_*）、倾斜（roll_*）三张阈值表的每一档都满足开始值 < 满强度值。
bool config_thresholds_ordered(const Config& cfg);

// configuration「恢复默认设置」：返回默认配置，但保留 cfg 的 [calibration]（含日期）、[detect] force_game、
// [detect] known_games、[general] autostart、[ui] 与 [extra]。
Config restore_defaults(const Config& cfg);
// configuration「清空已知游戏列表」：known_games 置空（force_game 与排除名单不变）。
void clear_known_games(Config& cfg);

// ---------------------------------------------------------------- 文件层（Win32）

struct LoadedFile {
    std::string text;         // 文件内容；无法读取 / 创建时为模板文本
    bool created = false;     // 本次因文件不存在而由模板新建
    bool from_disk = false;   // text 确实来自磁盘上的文件
    bool writable = true;     // false：文件无法创建或读取，调用方应只用默认值运行并禁用保存
};

// 读取配置文件；不存在时把模板原样（逐字节）写到 path 再读回。目录须已存在。
LoadedFile load_or_create(const std::wstring& path, std::string_view template_text);
// 读整个文件（共享读 / 写 / 删除打开）。
bool read_file(const std::wstring& path, std::string* out);
// 原子写：写 <path>.tmp → FlushFileBuffers → MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)；失败时删除 .tmp。
bool save_file_atomic(const std::wstring& path, std::string_view bytes);
// 内容哈希（FNV-1a 64），用于判断目录监视到的变化是否就是自己刚写入 / 读取的内容。
std::uint64_t content_hash(std::string_view bytes);

}  // namespace mc
