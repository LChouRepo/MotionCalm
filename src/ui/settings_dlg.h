// settings_dlg.h — 设置窗口外壳 SettingsWindow：非模态、单实例（打开 / 激活 / 关闭）、Tab + 6 个分页、底部状态栏（"所有更改立即生效并自动保存 · 上次保存 …"、操作结果、"恢复默认…"、"关闭"）、Ctrl+Tab 切页、记住上次分页与窗口位置、pre_translate（IsDialogMessage）、预览强度、配置变化后的合并刷新、模态循环兜底、PMv2 DPI 缩放、标题栏随系统深 / 浅色。
//
// 宿主（App）的接入方式：
//   ui::SettingsWindow settings_{host};             // host 实现 ui::SettingsHost
//   托盘"设置…"、双击托盘、热键 open_settings  →  settings_.open(instance_)
//   主循环 PeekMessage 之后：if (settings_.pre_translate(msg)) continue;   再 Translate / Dispatch
//   任何来源改了配置 / 校准状态 / 截屏状态  →  settings_.refresh()
//   每次真正把 INI 写到磁盘之后（任何来源）  →  settings_.config_saved()
//   界面语言变了（i18n::set_language）  →  settings_.refresh()（窗口在下一次刷新时按新语言重建分页）
//   退出前：settings_.close()
#pragma once

#include <windows.h>

#include <array>
#include <memory>
#include <string>

#include "i18n/i18n.h"
#include "ui/pages/page.h"
#include "ui/settings_host.h"

namespace mc::ui {

class SettingsWindow final : private PageSite {
public:
    static constexpr int kPageCount = 6;

    explicit SettingsWindow(SettingsHost& host);
    ~SettingsWindow() override;
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // 窗口不存在时创建并显示：放在上次的位置（[ui] x / y，且在鼠标指针所在显示器上时），否则居中于鼠标指针
    // 所在显示器的工作区；已存在时恢复（若最小化）并激活。instance 必须包含 IDD_SETTINGS 等对话框模板（app.rc）。
    bool open(HINSTANCE instance);
    // 关闭：提交焦点编辑框 → flush_config → 预览清零 → 恢复热键 → 记住位置 → 销毁窗口 → settings_closed。未打开时无操作。
    void close();
    bool is_open() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    // 主循环在 PeekMessage 之后、TranslateMessage 之前调用；返回 true 表示消息已处理（不要再分发）。
    bool pre_translate(MSG& msg);
    // 配置或运行期状态因其他来源（托盘、热键、重载、校准、已知游戏追加…）改变：合并后在下一轮消息循环重新载入
    // 全部分页（正在输入的编辑框除外）。窗口未打开时无操作。
    void refresh();
    // 宿主刚把 INI 写到磁盘：状态栏"上次保存"更新为当前本地时间。
    void config_saved();
    // 当前预览强度 0..1（窗口关闭后为 0）。
    float preview() const override { return preview_; }
    // 切到第 index 页（0 遮罩 … 5 通用）。
    void select_page(int index);
    int current_page() const { return current_; }

    // ---- 演示 / 截图用
    // 用指定 DPI 排版与选字号（0 = 跟随显示器）。须在 open 之前调用；系统绘制的部件（复选框方块、标题栏）不受影响。
    void set_dpi_override(int dpi) { dpi_override_ = dpi; }
    // 当前页滚到 y 像素处；返回可滚动的最大值（0 = 不需要滚动）。
    int scroll_current_page(int y);

private:
    // PageSite
    HINSTANCE instance() const override { return instance_; }
    const UiStyle& style() const override { return style_; }
    void request_refresh() override { refresh(); }
    void set_preview(float p) override;
    bool confirm(const ConfirmText& text) override;
    void notice(const std::wstring& text) override;
    bool show_advanced() const override;
    void set_show_advanced(bool on) override;

    static INT_PTR CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR handle(UINT msg, WPARAM wp, LPARAM lp);
    void on_init();
    UiStyle make_fonts(int dpi);   // 返回旧字体
    void free_fonts();
    void size_window();
    void layout_shell();
    void place_on_open();
    void fill_tabs();             // 分页标签文字（按当前界面语言）
    void set_shell_texts();       // 标题、"恢复默认…"、"关闭"
    void create_pages();
    void rebuild_for_language();  // 界面语言变化后重建分页（保留当前分页与滚动位置）
    void save_position();
    void update_status();
    void apply_title_theme();
    void load_icons();
    void free_icons();
    RECT page_frame() const;

    SettingsHost& host_;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tab_ = nullptr;
    HWND status_ = nullptr;
    HWND restore_ = nullptr;
    HWND close_ = nullptr;
    std::array<std::unique_ptr<Page>, kPageCount> pages_{};
    int current_ = 0;
    float preview_ = 0.0f;
    bool refresh_pending_ = false;
    bool in_size_move_ = false;
    bool closing_ = false;
    bool advanced_local_ = false;     // 宿主不认识 [ui] show_advanced 时的本次运行状态
    int dpi_override_ = 0;
    i18n::Lang built_lang_ = i18n::Lang::ZhCN;   // 当前分页按哪种界面语言创建
    UiStyle style_;
    std::wstring notice_;
    std::wstring last_saved_;          // "HH:MM:SS"；空 = 本次还没保存过
    HICON icon_small_ = nullptr;
    HICON icon_big_ = nullptr;

    static int s_last_page_;   // 同一次运行内记住上次的分页（不写 INI）
};

}  // namespace mc::ui
