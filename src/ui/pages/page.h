// page.h — 设置窗口分页的基类：子对话框容器 + 版式引擎（docs/settings-ui.md §2 的 8 px 节奏、168 px 标签栏、按 DPI 缩放、超出时页内滚动、"显示高级选项"渐进披露）+ 控件 ⇄ INI 键绑定引擎；以及 6 个分页的工厂。
//
// 各页在 build() 里按视觉顺序（从上到下、从左到右）创建控件：创建顺序 = Z 序 = Tab 顺序，标签总在它标注的
// 控件之前。数值范围由宿主按配置模式表提供，页内只登记步长、显示格式与单位。
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "i18n/i18n.h"
#include "model/intensity.h"
#include "ui/settings_host.h"
#include "ui/settings_logic.h"

namespace mc::ui {

// 版式常量：96 DPI 下的像素（docs/settings-ui.md §2），使用时按 UiStyle::px 缩放。
namespace layout {
inline constexpr int kMargin = 16;       // 页内边距
inline constexpr int kGroupGap = 20;     // 分组之间
inline constexpr int kRowGap = 8;        // 组内行距
inline constexpr int kLabelGap = 12;     // 标签与控件
inline constexpr int kRowH = 28;         // 普通行高
inline constexpr int kSliderRowH = 32;   // 滑块行高
inline constexpr int kLabelW = 168;      // 左栏标签宽（所有页相同；zh-CN / zh-TW）
inline constexpr int kSliderW = 260;     // 滑块宽（zh-CN）
inline constexpr int kReadoutW = 76;     // 数值读数宽（规范写 64；"屏高的 100%" 约 69 px，放宽到 76 避免截断）
// localization：译文更长时按当前界面语言放宽标签栏 / 读数、相应收窄滑块（总宽不超过分页可用宽度）。
// zh-TW："螢幕高度的 100%" 约 100 px；en：标签最长约 190 px（"Fo&rward/back sensitivity"），"100% of screen height" 约 125 px。
inline int label_w() { return i18n::current() == i18n::Lang::En ? 196 : kLabelW; }
inline int slider_w() {
    switch (i18n::current()) {
        case i18n::Lang::En: return 208;
        case i18n::Lang::ZhTW: return 232;
        case i18n::Lang::ZhCN: break;
    }
    return kSliderW;
}
inline int readout_w() {
    switch (i18n::current()) {
        case i18n::Lang::En: return 132;
        case i18n::Lang::ZhTW: return 112;
        case i18n::Lang::ZhCN: break;
    }
    return kReadoutW;
}
inline constexpr int kHelperGap = 2;     // 辅助文字与它说明的控件之间
inline constexpr int kWheelStep = 48;    // 滚轮一格滚动的距离
}  // namespace layout

// 文字颜色（白底对比度 ≥ 4.5:1，docs/settings-ui.md §7）。
inline constexpr COLORREF kHelperTextColor = RGB(0x5C, 0x5C, 0x5C);
inline constexpr COLORREF kErrorTextColor = RGB(0xC4, 0x2B, 0x1C);
inline constexpr COLORREF kWarningTextColor = RGB(0x9D, 0x5D, 0x00);

// 当前 DPI 下的字体与缩放（外壳持有，DPI 变化时重建）。
struct UiStyle {
    int dpi = 96;
    HFONT body = nullptr;      // 正文 9 pt
    HFONT heading = nullptr;   // 分组标题 10.5 pt 半粗体
    int px(int v) const { return ::MulDiv(v, dpi, 96); }
};

// 分页与外壳窗口之间的回调（SettingsWindow 实现）。
class PageSite {
public:
    virtual ~PageSite() = default;
    virtual HINSTANCE instance() const = 0;
    virtual const UiStyle& style() const = 0;
    // 合并后重新载入全部分页（PostMessage，下一轮消息循环执行）。
    virtual void request_refresh() = 0;
    // 预览强度（0..1，不保存）。
    virtual float preview() const = 0;
    virtual void set_preview(float p) = 0;
    // 破坏性操作的确认框（任务对话框，动词按钮 +"取消"，带模态循环兜底）；返回是否确认。
    virtual bool confirm(const ConfirmText& text) = 0;
    // 在底部状态栏显示一条操作结果（例如"已删除 cs2.exe 的校准"）。
    virtual void notice(const std::wstring& text) = 0;
    // "显示高级选项"（全窗口共用一个状态，存 [ui] show_advanced）。
    virtual bool show_advanced() const = 0;
    virtual void set_show_advanced(bool on) = 0;
};

class Page {
public:
    Page(SettingsHost& host, PageSite& site) : host_(host), site_(site) {}
    virtual ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // 由模板创建子对话框容器（不可见），父窗口为外壳对话框；随后 build() 创建全部控件。
    bool create(HWND parent, int template_id);
    HWND hwnd() const { return hwnd_; }
    // 从宿主重新载入全部控件，再重新排版；有键盘焦点且有未提交输入的编辑框保持用户正在输入的内容。
    void load();
    // 按当前 DPI / 高级选项 / 滚动位置重新摆放全部控件。
    void relayout();
    // 字体或 DPI 变了：重设全部控件的字体，再排版。
    void apply_style();
    // 提交有键盘焦点的编辑框（Enter、切页、关闭窗口前）。
    void commit_focus();
    // 键盘导航把焦点移到了滚动区域之外时，把它滚进可见区域。
    void ensure_visible(HWND ctl);
    // 页内滚动（演示 / 截图用）：滚到 y 像素处；返回可滚动的最大值。
    int scroll_to(int y);
    // 当前页内滚动位置（像素）。
    int scroll_pos() const { return scroll_y_; }
    // 分页被隐藏、窗口失去激活或即将关闭（快捷键页借此恢复热键）。
    virtual void on_deactivate() {}
    // 这一页有没有"高级"条目（有才显示"显示高级选项"复选框）。
    bool has_advanced() const;

protected:
    struct Choice {
        std::wstring_view label;   // 单选框 / 下拉项文字
        std::wstring_view value;   // INI 写法
        std::wstring_view note;    // 单选框右侧的一句说明（可空）
    };
    struct Part {
        HWND hwnd = nullptr;
        int width = 0;    // 96 DPI 像素；-1 = 填满到右边距
        int height = 0;   // 96 DPI 像素；0 = 按控件类型的默认高度
        int gap = 8;      // 与下一个控件的间距（96 DPI 像素）
    };

    // ---- 派生类钩子
    virtual void build() = 0;                                  // 按视觉顺序创建控件与版式条目
    virtual void load_extra() {}                               // 通用载入之后的页内刷新
    virtual void style_changed() {}                            // 字体 / DPI 变化之后（图标、列宽…）
    virtual bool command(int id, int code, HWND ctl);          // 返回 true = 已处理
    virtual bool notify(NMHDR* nm, LRESULT* result);
    virtual bool hscroll(HWND ctl);                            // 未绑定的滑块
    // WM_CTLCOLORSTATIC：设置 dc 的文字颜色并返回 true 表示已处理（背景由基类统一为白色）。
    virtual bool text_color(HWND ctl, COLORREF* color);
    virtual bool message(UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result);
    virtual bool commit_custom(HWND focus);                    // 未绑定的编辑框按 Enter / 切页 / 关闭
    // 数值提交前的额外校验（例如阈值顺序）；返回错误文字 = 拒绝并恢复原值。
    virtual std::optional<std::wstring> validate(SettingKey key, double value);
    // 一次改动要写入的全部键（例如最淡 / 最浓互推）；默认只有这一个键。
    virtual std::vector<SettingValue> expand(SettingKey key, std::wstring value);
    // 控件是否可用（键存在之外的页内依赖，例如关掉中心点后相关控件灰显）。
    virtual bool control_enabled(int id) const;

    // ---- 创建控件（字体自动设为正文字体；所有控件都带 WS_GROUP，单选组内除第一个外由调用方去掉）
    HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id = -1, DWORD ex_style = 0);
    HWND make_label(const wchar_t* text);                       // 左栏标签（可带 & 访问键）
    HWND make_text(const wchar_t* text, bool helper_color);     // 单行静态文字（不解析 &）
    HWND make_button(const wchar_t* text, int id);

    // ---- 版式条目（返回条目序号）
    int add_description(const wchar_t* text);                   // 页顶一行灰色页面说明
    int add_heading(const wchar_t* text, bool advanced = false);
    int add_row(HWND label, std::vector<Part> parts, int height = layout::kRowH, bool advanced = false,
                bool full_width = false, bool group_start = false);
    // 灰色辅助说明：放在上一行控件的下方、与控件左对齐（full_width = 与标签左对齐）。
    int add_helper(const wchar_t* text, bool advanced = false, bool full_width = false);
    HWND item_text(int item) const;
    void set_item_collapsed(int item, bool collapsed);          // 折叠 = 不显示也不占位
    void set_item_tight(int item);                              // 紧贴上一行（间距同辅助文字）
    void set_item_group_start(int item);                        // 与上一项之间留分组间距
    void set_part_hidden(HWND part, bool hidden);               // 隐藏但保留位置

    // ---- 绑定 + 控件（一行：左栏标签 + 右栏控件）
    void add_check(const wchar_t* label, int id, const wchar_t* text, SettingKey key, bool advanced = false);
    void add_radio_group(const wchar_t* label, std::initializer_list<int> ids, SettingKey key,
                         std::vector<Choice> choices, bool advanced = false);
    void add_combo(const wchar_t* label, int id, SettingKey key, std::vector<Choice> choices, int width,
                   bool advanced = false);
    void add_slider(const wchar_t* label, int id, int readout_id, SettingKey key, double step, int decimals,
                    Readout readout, bool advanced = false);
    void add_number(const wchar_t* label, int id, SettingKey key, int max_decimals, int min_decimals,
                    const wchar_t* unit, bool advanced = false);
    void add_color(const wchar_t* label, int id, int swatch_id, SettingKey key, bool advanced = false);
    // 页底"显示高级选项"复选框（只在有高级条目的页上调用）。
    void add_advanced_toggle();
    // 只登记绑定（控件由页自己创建并放进行里）。
    void bind_check(int id, SettingKey key);
    void bind_combo(int id, SettingKey key, std::vector<Choice> choices);   // 下拉项须已由页添加
    void bind_number(int edit_id, SettingKey key, int max_decimals, int min_decimals);
    // 页自己创建的滑块的公共设置（范围 0..n、方向键 1 步、PageUp/PageDown 5 步、读数为右侧伙伴、滚轮转交给页）。
    void setup_slider(HWND slider, HWND readout, int max_pos);

    // ---- 工具
    HWND item(int id) const { return ::GetDlgItem(hwnd_, id); }
    const UiStyle& style() const { return site_.style(); }
    int px(int v) const { return site_.style().px(v); }
    std::optional<std::wstring> get(SettingKey key) const { return host_.get(key); }
    std::optional<double> get_number(SettingKey key) const;
    bool get_bool(SettingKey key) const;
    // host.set + 请求刷新；返回 host.set 的结果。
    bool write(const std::vector<SettingValue>& values);
    void balloon(HWND edit, const wchar_t* title, const std::wstring& text, INT icon = TTI_WARNING);
    static void set_text(HWND ctl, std::wstring_view text);   // 文字不同才设置
    static std::wstring text_of(HWND ctl);
    void set_enabled(int id, bool on);
    void set_enabled(HWND h, bool on);
    bool loading() const { return loading_; }

    SettingsHost& host_;
    PageSite& site_;
    HWND hwnd_ = nullptr;

private:
    enum class Kind { Check, Radio, Combo, Slider, Number, Color };
    struct Binding {
        Kind kind = Kind::Check;
        SettingKey key;
        int id = 0;        // 复选框 / 组合框 / 滑块 / 数值框 / 颜色框
        int id2 = 0;       // 滑块的读数 / 颜色色块
        double step = 1;
        int max_dec = 0;
        int min_dec = 0;
        Readout readout = Readout::Percent;
        std::vector<int> radio_ids;
        std::vector<Choice> choices;
        // 运行期
        bool present = false;            // 宿主认识这个键
        std::optional<KeyRange> range;
        std::wstring value;              // 最近一次载入 / 写入的 INI 文本
        int pos = -1;                    // 滑块位置
        bool dirty = false;              // 编辑框有未提交的输入
        HBRUSH brush = nullptr;          // 色块画刷
        Rgb color{};
    };
    enum class ItemKind { Text, Heading, Row };
    struct Item {
        ItemKind kind = ItemKind::Row;
        bool advanced = false;
        bool collapsed = false;
        bool full_width = false;
        bool group_start = false;
        bool tight = false;
        bool helper = false;            // Text：辅助说明（否则为页面说明）
        HWND text = nullptr;            // Text / Heading
        HWND label = nullptr;           // Row：左栏标签
        std::vector<Part> parts;        // Row：右栏控件
        int height = layout::kRowH;     // Row
    };

    static INT_PTR CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR handle(UINT msg, WPARAM wp, LPARAM lp);
    Binding* find(int id);
    Binding* find_edit(HWND edit);
    void load_binding(Binding& b, HWND focus);
    void on_click(Binding& b, int id);
    void on_slider(Binding& b);
    void commit_edit(Binding& b);
    void restore_edit(Binding& b);
    void update_swatch(Binding& b, Rgb c);
    void update_readout(Binding& b, double v);
    int run_layout(int client_w, int client_h, bool place);
    HFONT style_font_body() const;
    int text_height(HWND ctl, int width) const;
    int default_height(HWND ctl) const;
    void on_vscroll(int code);
    std::wstring layout_signature() const;

    std::vector<Binding> bindings_;
    std::vector<Item> items_;
    std::set<HWND> helpers_;      // 灰色文字
    std::set<HWND> headings_;     // 半粗体
    std::set<HWND> hidden_;       // 页逻辑隐藏的控件（保留位置）
    HWND advanced_toggle_ = nullptr;
    bool loading_ = false;
    bool in_layout_ = false;
    bool scroll_shown_ = false;
    int scroll_y_ = 0;
    int scroll_max_ = 0;
    std::wstring last_signature_;   // 上次排版时的折叠 / 隐藏 / 文字状态（没变就不重排，拖滑块时不闪）
};

// 6 个分页（顺序即 Tab 顺序）。
std::unique_ptr<Page> make_mask_page(SettingsHost& host, PageSite& site);
std::unique_ptr<Page> make_dots_page(SettingsHost& host, PageSite& site);
std::unique_ptr<Page> make_trigger_page(SettingsHost& host, PageSite& site);
std::unique_ptr<Page> make_calib_page(SettingsHost& host, PageSite& site);
std::unique_ptr<Page> make_hotkeys_page(SettingsHost& host, PageSite& site);
std::unique_ptr<Page> make_general_page(SettingsHost& host, PageSite& site);

}  // namespace mc::ui
