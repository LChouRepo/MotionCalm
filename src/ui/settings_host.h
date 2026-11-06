// settings_host.h — 设置窗口与宿主（App）之间的唯一接口：按 INI 节 / 键读写配置、数值范围、运行期状态、校准记录、热键暂停 / 试注册、预览强度下限、模态循环兜底。设置窗口不直接接触 Config 结构。
//
// 设计说明（settings-window design D4 的"窗口侧"）：
// - 所有可编辑的值都按 INI 键访问，值是该键在 INI 中的写法（UTF-16）。这样窗口与 Config 的字段布局无关：
//   变更 3 新增的 [capture] 键、[trigger] capture / capture_sensitivity 与其他键走同一条路，宿主一侧用配置
//   模式表（config.cpp 的 kSchema：解析 / 校验 / 格式化）实现 get / set / range 即可，界面范围与 INI 校验范围
//   天然是同一份定义（configuration「界面范围与校验范围一致」）。
// - 宿主不认识的键（get / range 返回 nullopt）对应的控件自动灰显，不会崩溃。
// - 界面自己的状态也走这条路（docs/settings-ui.md）：[ui] show_advanced（0/1，"显示高级选项"）、[ui] x / [ui] y
//   （窗口左上角的屏幕坐标，整数；取不到或不在鼠标所在显示器上时居中）。宿主不认识这些键时窗口退化为
//   本次运行内记住，不报错。
// - 宿主每次真正把 INI 写到磁盘之后调用 SettingsWindow::config_saved()（状态栏"上次保存 HH:MM:SS"）；
//   配置或运行期状态因任何来源变化之后调用 SettingsWindow::refresh()。
// - 全部调用都在主线程（窗口线程）上发生。
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tray/hotkeys.h"

namespace mc::ui {

// 一个 INI 键：小写节名 + 小写键名，例如 {"mask", "feather"}、{"hotkeys", "toggle"}、{"capture", "grid_w"}。
struct SettingKey {
    std::string_view section;
    std::string_view key;
    bool operator==(const SettingKey&) const = default;
};

// 一次写入的一项：value 是该键的 INI 写法，例如 L"0.36"、L"strong"、L"1"、L"200000"、L"a.exe;b.exe"、
// L"Ctrl+Shift+F10"、L"None"。
struct SettingValue {
    SettingKey key;
    std::wstring value;
};

// 数值键（整数 / 小数）的合法范围，取自配置模式表（与 INI 读取时的校验 / 截断范围相同）。
struct KeyRange {
    double lo = 0;
    double hi = 0;
};

// [calibration] 的一条记录。
struct CalibEntry {
    std::wstring exe;                // 小写 exe
    double counts_per_degree = 0;
    std::wstring date;               // 记录里 ';' 之后的日期文本（原样，可能不是合法日期）；没有时为空
    bool operator==(const CalibEntry&) const = default;
};

// 运行期状态（不属于 INI）。设置窗口每次刷新时读取。
struct SettingsStatus {
    bool calibration_idle = true;              // 校准状态机为 Idle（"重测"可用）
    bool capture_disabled_by_cmdline = false;  // 以 --no-capture 启动
    bool capture_failed = false;               // 截屏检测处于"采集失败"
    bool verbose = false;                      // 以 --verbose 启动（实际日志级别为调试）
};

class SettingsHost {
public:
    virtual ~SettingsHost() = default;

    // ---- 配置
    // 当前生效配置中该键的 INI 写法（[hotkeys] 返回用户原串，可能非法）；宿主不认识的键返回 nullopt。
    virtual std::optional<std::wstring> get(SettingKey key) const = 0;
    // 一次原子改动（来源 = 设置窗口）：按模式表解析 / 校验 / 截断后替换配置、发出一次变更通知（受影响的节
    // 由键推出）、立即应用到模型 / 渲染 / 热键 / 截屏 / 游戏判定，并按"最后一次改动后 300 ms"防抖保存。
    // 有任何键不认识或值无法解析时整次拒绝并返回 false。[general] autostart 由宿主同步注册表，失败时宿主
    // 回滚该键、弹 toast "无法设置开机自启"并调用 SettingsWindow::refresh()。
    virtual bool set(std::span<const SettingValue> values) = 0;
    // 数值键的合法范围；非数值键或不认识的键返回 nullopt。
    virtual std::optional<KeyRange> range(SettingKey key) const = 0;
    // 立即写出尚未落盘的设置窗口改动（关闭窗口时调用）。
    virtual void flush_config() = 0;
    // "恢复默认设置"（已确认）：按 configuration「恢复默认设置」重置并立即保存、重新注册热键。
    virtual void restore_defaults() = 0;
    // "清空已知游戏列表"（已确认）：known_games 置空并立即保存。
    virtual void clear_known_games() = 0;
    // "打开配置文件"：先 flush，再用系统默认程序打开 MotionCalm.ini。
    virtual void open_config_file() = 0;

    // ---- 运行期状态
    virtual SettingsStatus status() const = 0;

    // ---- 校准记录（[calibration]）
    virtual std::vector<CalibEntry> calibrations() const = 0;
    // 删除该 exe 的记录（已确认）；按设置窗口改动的保存时机写入，写一条 info 日志，不弹 toast。
    virtual void delete_calibration(const std::wstring& exe) = 0;

    // ---- 热键
    // 按键捕获框获得 / 失去焦点（设置窗口保证成对调用，不会连续两次 true）：true = 注销本程序全部全局热键，
    // false = 立即按当前配置重新注册全部热键（注册失败只写 warn，不弹 toast）。
    virtual void pause_hotkeys(bool paused) = 0;
    // 该组合当前能否被注册（试注册一次再立即注销）。本程序自己正注册着的组合视为可用。
    // 返回 false = 被其他程序或系统占用（快捷键页标红"已被其他程序占用"）。
    virtual bool hotkey_available(HotkeyAction action, const Hotkey& hotkey) = 0;

    // ---- overlay 预览、模态循环兜底、关闭
    // 预览强度下限 p ∈ [0, 1]；0 = 关闭预览模式（overlay-rendering「设置窗口预览强度」）。
    virtual void set_preview_floor(float p) = 0;
    // 进入 / 离开系统模态循环（拖动窗口标题栏、确认框）：宿主在这期间用计时器兜底 tick（design D9）。成对调用。
    virtual void modal_begin() = 0;
    virtual void modal_end() = 0;
    // 设置窗口已销毁（关闭流程的最后一步）。
    virtual void settings_closed() {}
};

}  // namespace mc::ui
