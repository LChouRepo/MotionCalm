// page_common.cpp — Page 基类：子对话框过程、白底与文字颜色、版式引擎（按 DPI 缩放的 8 px 节奏、两栏对齐、辅助文字在控件下方、高级条目折叠、页内滚动）、控件 ⇄ INI 键绑定（design D7）与气泡提示。
#include "ui/pages/page.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>

#include "res/resource.h"
#include "util/strings.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

constexpr UINT_PTR kWheelSubclassId = 0x4D435753;   // 'MCWS'

// 滑块没有焦点时把滚轮交给页（否则在页上滚动时会顺手改掉滑块的值）。
LRESULT CALLBACK slider_wheel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_MOUSEWHEEL && ::GetFocus() != hwnd) {
        return ::SendMessageW(reinterpret_cast<HWND>(ref), WM_MOUSEWHEEL, wp, lp);
    }
    if (msg == WM_NCDESTROY) ::RemoveWindowSubclass(hwnd, slider_wheel_proc, id);
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

std::wstring class_of(HWND h) {
    wchar_t buf[64] = {};
    ::GetClassNameW(h, buf, 64);
    return buf;
}

}  // namespace

Page::~Page() {
    for (auto& b : bindings_) {
        if (b.brush != nullptr) ::DeleteObject(b.brush);
    }
    if (hwnd_ != nullptr) {
        HWND h = hwnd_;
        ::SetWindowLongPtrW(h, DWLP_USER, 0);
        hwnd_ = nullptr;
        ::DestroyWindow(h);
    }
}

bool Page::create(HWND parent, int template_id) {
    hwnd_ = ::CreateDialogParamW(site_.instance(), MAKEINTRESOURCEW(template_id), parent, &Page::proc,
                                 reinterpret_cast<LPARAM>(this));
    return hwnd_ != nullptr;
}

INT_PTR CALLBACK Page::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Page* self = nullptr;
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<Page*>(lp);
        ::SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Page*>(::GetWindowLongPtrW(hwnd, DWLP_USER));
    }
    if (self == nullptr) return FALSE;
    if (msg == WM_NCDESTROY) {   // 外壳销毁时子页随之销毁；之后 Page 对象不再碰这个句柄
        ::SetWindowLongPtrW(hwnd, DWLP_USER, 0);
        self->hwnd_ = nullptr;
        return FALSE;
    }
    return self->handle(msg, wp, lp);
}

INT_PTR Page::handle(UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR custom = 0;
    if (msg != WM_INITDIALOG && message(msg, wp, lp, &custom)) {
        ::SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, custom);
        return TRUE;
    }
    switch (msg) {
        case WM_INITDIALOG:
            build();
            load();
            return FALSE;   // 焦点由外壳决定
        case WM_SIZE:
            if (!in_layout_) relayout();
            return FALSE;
        case WM_VSCROLL:
            on_vscroll(LOWORD(wp));
            return TRUE;
        case WM_MOUSEWHEEL:
            if (scroll_shown_) {
                const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                scroll_to(scroll_y_ - notches * px(layout::kWheelStep));
            }
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (id == IDC_SHOW_ADVANCED && code == BN_CLICKED) {
                site_.set_show_advanced(::SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
                return TRUE;
            }
            if (command(id, code, ctl)) return TRUE;
            if (Binding* b = find(id)) {
                const bool is_edit = (b->kind == Kind::Number || b->kind == Kind::Color) && b->id == id;
                if (is_edit) {
                    if (code == EN_CHANGE && !loading_) b->dirty = true;
                    if (code == EN_KILLFOCUS) commit_edit(*b);
                    return TRUE;
                }
                if (code == BN_CLICKED || code == CBN_SELCHANGE) on_click(*b, id);
                return TRUE;
            }
            return FALSE;
        }
        case WM_HSCROLL: {
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl == nullptr) return FALSE;
            if (Binding* b = find(::GetDlgCtrlID(ctl)); b != nullptr && b->kind == Kind::Slider) {
                on_slider(*b);
                return TRUE;
            }
            return hscroll(ctl) ? TRUE : FALSE;
        }
        case WM_NOTIFY: {
            LRESULT r = 0;
            if (notify(reinterpret_cast<NMHDR*>(lp), &r)) {
                ::SetWindowLongPtrW(hwnd_, DWLP_MSGRESULT, r);
                return TRUE;
            }
            return FALSE;
        }
        case WM_CTLCOLORDLG:
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_WINDOW));
        case WM_CTLCOLORBTN:
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_WINDOW));
        case WM_CTLCOLORSTATIC: {
            // 页面统一白底；静态文字、复选框、单选框、滑块与只读编辑框都从这里取底色。
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            const int id = ::GetDlgCtrlID(ctl);
            for (auto& b : bindings_) {
                if (b.kind == Kind::Color && b.id2 == id && b.brush != nullptr) {
                    ::SetBkColor(dc, RGB(b.color.r, b.color.g, b.color.b));
                    return reinterpret_cast<INT_PTR>(b.brush);
                }
            }
            ::SetBkColor(dc, ::GetSysColor(COLOR_WINDOW));
            COLORREF color = ::GetSysColor(COLOR_WINDOWTEXT);
            if (helpers_.count(ctl) != 0) color = kHelperTextColor;
            text_color(ctl, &color);
            ::SetTextColor(dc, color);
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_WINDOW));
        }
        default: return FALSE;
    }
}

// ---------------------------------------------------------------- 默认钩子

bool Page::command(int, int, HWND) { return false; }
bool Page::notify(NMHDR*, LRESULT*) { return false; }
bool Page::hscroll(HWND) { return false; }
bool Page::text_color(HWND, COLORREF*) { return false; }
bool Page::message(UINT, WPARAM, LPARAM, INT_PTR*) { return false; }
bool Page::commit_custom(HWND) { return false; }
std::optional<std::wstring> Page::validate(SettingKey, double) { return std::nullopt; }
std::vector<SettingValue> Page::expand(SettingKey key, std::wstring value) { return {{key, std::move(value)}}; }
bool Page::control_enabled(int) const { return true; }

// ---------------------------------------------------------------- 创建控件

HWND Page::make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex_style) {
    HWND h = ::CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), site_.instance(), nullptr);
    if (h != nullptr) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(style_font_body()), FALSE);
    return h;
}

HWND Page::make_label(const wchar_t* text) {
    return make(L"Static", text, SS_LEFT | SS_CENTERIMAGE | WS_GROUP);
}

HWND Page::make_text(const wchar_t* text, bool helper_color) {
    HWND h = make(L"Static", text, SS_LEFT | SS_NOPREFIX | WS_GROUP);
    if (helper_color) helpers_.insert(h);
    return h;
}

HWND Page::make_button(const wchar_t* text, int id) {
    return make(L"Button", text, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, id);
}

// ---------------------------------------------------------------- 版式条目

int Page::add_description(const wchar_t* text) {
    Item it;
    it.kind = ItemKind::Text;
    it.text = make_text(text, true);
    items_.push_back(std::move(it));
    return static_cast<int>(items_.size()) - 1;
}

int Page::add_heading(const wchar_t* text, bool advanced) {
    Item it;
    it.kind = ItemKind::Heading;
    it.advanced = advanced;
    it.text = make(L"Static", text, SS_LEFT | SS_NOPREFIX | WS_GROUP);
    headings_.insert(it.text);
    ::SendMessageW(it.text, WM_SETFONT, reinterpret_cast<WPARAM>(style().heading), FALSE);
    items_.push_back(std::move(it));
    return static_cast<int>(items_.size()) - 1;
}

int Page::add_row(HWND label, std::vector<Part> parts, int height, bool advanced, bool full_width, bool group_start) {
    Item it;
    it.kind = ItemKind::Row;
    it.label = label;
    it.parts = std::move(parts);
    it.height = height;
    it.advanced = advanced;
    it.full_width = full_width;
    it.group_start = group_start;
    items_.push_back(std::move(it));
    return static_cast<int>(items_.size()) - 1;
}

int Page::add_helper(const wchar_t* text, bool advanced, bool full_width) {
    Item it;
    it.kind = ItemKind::Text;
    it.helper = true;
    it.advanced = advanced;
    it.full_width = full_width;
    it.text = make_text(text, true);
    items_.push_back(std::move(it));
    return static_cast<int>(items_.size()) - 1;
}

HWND Page::item_text(int item) const {
    return item >= 0 && static_cast<std::size_t>(item) < items_.size() ? items_[static_cast<std::size_t>(item)].text
                                                                       : nullptr;
}

void Page::set_item_group_start(int item) {
    if (item >= 0 && static_cast<std::size_t>(item) < items_.size()) items_[static_cast<std::size_t>(item)].group_start = true;
}

void Page::set_item_tight(int item) {
    if (item >= 0 && static_cast<std::size_t>(item) < items_.size()) items_[static_cast<std::size_t>(item)].tight = true;
}

void Page::set_item_collapsed(int item, bool collapsed) {
    if (item < 0 || static_cast<std::size_t>(item) >= items_.size()) return;
    items_[static_cast<std::size_t>(item)].collapsed = collapsed;
}

bool Page::has_advanced() const {
    for (const auto& it : items_) {
        if (it.advanced) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 绑定 + 控件

void Page::add_check(const wchar_t* label, int id, const wchar_t* text, SettingKey key, bool advanced) {
    HWND lbl = label != nullptr ? make_label(label) : nullptr;
    HWND cb = make(L"Button", text, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, id);
    add_row(lbl, {{cb, -1}}, layout::kRowH, advanced);
    bind_check(id, key);
}

void Page::bind_check(int id, SettingKey key) {
    Binding b;
    b.kind = Kind::Check;
    b.key = key;
    b.id = id;
    bindings_.push_back(std::move(b));
}

void Page::add_radio_group(const wchar_t* label, std::initializer_list<int> ids, SettingKey key,
                           std::vector<Choice> choices, bool advanced) {
    HWND lbl = make_label(label);
    std::vector<HWND> radios;
    std::size_t k = 0;
    for (const int id : ids) {
        const std::wstring text(k < choices.size() ? choices[k].label : L"");
        const DWORD style = BS_AUTORADIOBUTTON | (k == 0 ? (WS_GROUP | WS_TABSTOP) : 0);
        radios.push_back(make(L"Button", text.c_str(), style, id));
        ++k;
    }
    // 说明文字在全部单选框之后创建：不打断单选组的方向键导航，并以 WS_GROUP 结束这个组。
    std::vector<HWND> notes;
    for (std::size_t n = 0; n < radios.size(); ++n) {
        const std::wstring note(n < choices.size() ? choices[n].note : L"");
        notes.push_back(note.empty() ? nullptr : make_text(note.c_str(), true));
    }
    for (std::size_t n = 0; n < radios.size(); ++n) {
        std::vector<Part> parts{{radios[n], 64, 0, 4}};
        if (notes[n] != nullptr) parts.push_back({notes[n], -1, 20, 0});
        add_row(n == 0 ? lbl : nullptr, std::move(parts), layout::kRowH, advanced);
    }
    Binding b;
    b.kind = Kind::Radio;
    b.key = key;
    b.id = *ids.begin();
    b.radio_ids.assign(ids.begin(), ids.end());
    b.choices = std::move(choices);
    bindings_.push_back(std::move(b));
}

void Page::add_combo(const wchar_t* label, int id, SettingKey key, std::vector<Choice> choices, int width,
                     bool advanced) {
    HWND lbl = make_label(label);
    HWND combo = make(L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, id);
    for (const auto& c : choices) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::wstring(c.label).c_str()));
    }
    add_row(lbl, {{combo, width}}, layout::kRowH, advanced);
    bind_combo(id, key, std::move(choices));
}

void Page::bind_combo(int id, SettingKey key, std::vector<Choice> choices) {
    Binding b;
    b.kind = Kind::Combo;
    b.key = key;
    b.id = id;
    b.choices = std::move(choices);
    bindings_.push_back(std::move(b));
}

void Page::set_part_hidden(HWND part, bool hidden) {
    if (part == nullptr) return;
    if (hidden) {
        hidden_.insert(part);
    } else {
        hidden_.erase(part);
    }
}

void Page::setup_slider(HWND slider, HWND readout, int max_pos) {
    ::SendMessageW(slider, TBM_SETRANGEMIN, FALSE, 0);
    ::SendMessageW(slider, TBM_SETRANGEMAX, TRUE, max_pos);
    ::SendMessageW(slider, TBM_SETLINESIZE, 0, 1);    // 方向键：一个步进
    ::SendMessageW(slider, TBM_SETPAGESIZE, 0, 5);    // PageUp / PageDown：五个步进
    if (readout != nullptr) ::SendMessageW(slider, TBM_SETBUDDY, FALSE, reinterpret_cast<LPARAM>(readout));
    ::SetWindowSubclass(slider, slider_wheel_proc, kWheelSubclassId, reinterpret_cast<DWORD_PTR>(hwnd_));
}

void Page::add_slider(const wchar_t* label, int id, int readout_id, SettingKey key, double step, int decimals,
                      Readout readout, bool advanced) {
    HWND lbl = make_label(label);
    HWND sl = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, id);
    HWND ro = make(L"Static", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | WS_GROUP, readout_id);
    setup_slider(sl, ro, 0);
    add_row(lbl, {{sl, layout::slider_w()}, {ro, layout::readout_w(), 20, 0}}, layout::kSliderRowH, advanced);
    Binding b;
    b.kind = Kind::Slider;
    b.key = key;
    b.id = id;
    b.id2 = readout_id;
    b.step = step;
    b.max_dec = decimals;
    b.min_dec = decimals;
    b.readout = readout;
    bindings_.push_back(std::move(b));
}

void Page::add_number(const wchar_t* label, int id, SettingKey key, int max_decimals, int min_decimals,
                      const wchar_t* unit, bool advanced) {
    HWND lbl = make_label(label);
    HWND edit = make(L"Edit", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP | WS_GROUP, id, WS_EX_CLIENTEDGE);
    std::vector<Part> parts{{edit, 96}};
    if (unit != nullptr) parts.push_back({make_text(unit, false), -1, 20, 0});
    add_row(lbl, std::move(parts), layout::kRowH, advanced);
    bind_number(id, key, max_decimals, min_decimals);
}

void Page::bind_number(int edit_id, SettingKey key, int max_decimals, int min_decimals) {
    Binding b;
    b.kind = Kind::Number;
    b.key = key;
    b.id = edit_id;
    b.max_dec = max_decimals;
    b.min_dec = min_decimals;
    bindings_.push_back(std::move(b));
}

void Page::add_color(const wchar_t* label, int id, int swatch_id, SettingKey key, bool advanced) {
    HWND lbl = make_label(label);
    HWND edit = make(L"Edit", L"", ES_UPPERCASE | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, id, WS_EX_CLIENTEDGE);
    ::SendMessageW(edit, EM_LIMITTEXT, 6, 0);
    HWND swatch = make(L"Static", L"", SS_LEFT | WS_BORDER | WS_GROUP, swatch_id);
    add_row(lbl, {{edit, 96}, {swatch, 32, 24, 0}}, layout::kRowH, advanced);
    Binding b;
    b.kind = Kind::Color;
    b.key = key;
    b.id = id;
    b.id2 = swatch_id;
    bindings_.push_back(std::move(b));
}

void Page::add_advanced_toggle() {
    advanced_toggle_ = make(L"Button", tr(S::ShowAdvanced), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, IDC_SHOW_ADVANCED);
    add_row(nullptr, {{advanced_toggle_, -1}}, layout::kRowH, false, true, true);
}

Page::Binding* Page::find(int id) {
    for (auto& b : bindings_) {
        if (b.id == id) return &b;
        for (const int r : b.radio_ids) {
            if (r == id) return &b;
        }
    }
    return nullptr;
}

Page::Binding* Page::find_edit(HWND edit) {
    if (edit == nullptr) return nullptr;
    for (auto& b : bindings_) {
        if ((b.kind == Kind::Number || b.kind == Kind::Color) && item(b.id) == edit) return &b;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 载入

void Page::load() {
    if (hwnd_ == nullptr) return;
    loading_ = true;
    HWND focus = ::GetFocus();
    for (auto& b : bindings_) load_binding(b, focus);
    if (advanced_toggle_ != nullptr) {
        const bool on = site_.show_advanced();
        if ((::SendMessageW(advanced_toggle_, BM_GETCHECK, 0, 0) == BST_CHECKED) != on) {
            ::SendMessageW(advanced_toggle_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }
    loading_ = false;
    load_extra();
    // 行标签跟着它标注的控件灰显（禁用态清晰可辨）。
    for (const auto& it : items_) {
        if (it.kind != ItemKind::Row || it.label == nullptr) continue;
        bool any_input = false, any_enabled = false;
        for (const auto& p : it.parts) {
            if (p.hwnd == nullptr || iequals(class_of(p.hwnd), L"Static")) continue;
            any_input = true;
            any_enabled = any_enabled || ::IsWindowEnabled(p.hwnd) != FALSE;
        }
        if (any_input) set_enabled(it.label, any_enabled);
    }
    // 只有影响版式的东西变了（高级选项、折叠、隐藏、说明文字）才重排；隐藏的页在切换显示时再排。
    if (::IsWindowVisible(hwnd_) && layout_signature() != last_signature_) relayout();
}

std::wstring Page::layout_signature() const {
    std::wstring sig = site_.show_advanced() ? L"A" : L"a";
    for (const auto& it : items_) {
        sig += it.collapsed ? L'1' : L'0';
        if (it.kind == ItemKind::Text) {
            sig += text_of(it.text);
            sig += L'|';
        }
    }
    for (HWND h : hidden_) sig += std::to_wstring(reinterpret_cast<std::uintptr_t>(h)) + L",";
    return sig;
}

void Page::load_binding(Binding& b, HWND focus) {
    const auto v = host_.get(b.key);
    b.present = v.has_value();
    if ((b.kind == Kind::Slider || b.kind == Kind::Number) && !b.range) b.range = host_.range(b.key);
    const bool usable = b.present && ((b.kind != Kind::Slider && b.kind != Kind::Number) || b.range.has_value());

    std::vector<int> ids = b.radio_ids;
    if (ids.empty()) ids.push_back(b.id);
    if (b.id2 != 0) ids.push_back(b.id2);
    for (const int id : ids) set_enabled(id, usable && control_enabled(id));
    if (!usable) return;

    b.value = std::wstring(trim(std::wstring_view(*v)));
    HWND edit = (b.kind == Kind::Number || b.kind == Kind::Color) ? item(b.id) : nullptr;
    const bool keep_typing = edit != nullptr && edit == focus && b.dirty;
    switch (b.kind) {
        case Kind::Check: {
            const bool on = b.value == L"1";
            if ((::SendMessageW(item(b.id), BM_GETCHECK, 0, 0) == BST_CHECKED) != on) {
                ::SendMessageW(item(b.id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            break;
        }
        case Kind::Radio:
            for (std::size_t k = 0; k < b.radio_ids.size(); ++k) {
                const bool on = k < b.choices.size() && iequals(b.choices[k].value, b.value);
                HWND r = item(b.radio_ids[k]);
                if ((::SendMessageW(r, BM_GETCHECK, 0, 0) == BST_CHECKED) != on) {
                    ::SendMessageW(r, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            break;
        case Kind::Combo: {
            int sel = -1;
            for (std::size_t k = 0; k < b.choices.size(); ++k) {
                if (iequals(b.choices[k].value, b.value)) sel = static_cast<int>(k);
            }
            if (static_cast<int>(::SendMessageW(item(b.id), CB_GETCURSEL, 0, 0)) != sel) {
                ::SendMessageW(item(b.id), CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
            }
            break;
        }
        case Kind::Slider: {
            HWND sl = item(b.id);
            const KeyRange r = *b.range;
            const int n = slider_max_pos(r.lo, r.hi, b.step);
            if (static_cast<int>(::SendMessageW(sl, TBM_GETRANGEMAX, 0, 0)) != n) {
                ::SendMessageW(sl, TBM_SETRANGEMAX, TRUE, n);
            }
            const auto num = parse_number_text(b.value);
            const int pos = num ? slider_pos_for(*num, r.lo, r.hi, b.step) : 0;
            b.pos = pos;
            if (static_cast<int>(::SendMessageW(sl, TBM_GETPOS, 0, 0)) != pos) ::SendMessageW(sl, TBM_SETPOS, TRUE, pos);
            if (num) update_readout(b, *num);   // 读数显示实际值（不在步长上的值照实显示）
            break;
        }
        case Kind::Number:
            if (!keep_typing) {
                set_text(edit, b.value);
                b.dirty = false;
            }
            break;
        case Kind::Color:
            if (!keep_typing) {
                set_text(edit, b.value);
                b.dirty = false;
            }
            if (const auto c = parse_color_text(b.value)) update_swatch(b, *c);
            break;
    }
}

void Page::update_swatch(Binding& b, Rgb c) {
    if (b.brush != nullptr && b.color == c) return;
    if (b.brush != nullptr) ::DeleteObject(b.brush);
    b.color = c;
    b.brush = ::CreateSolidBrush(RGB(c.r, c.g, c.b));
    ::InvalidateRect(item(b.id2), nullptr, TRUE);
}

void Page::update_readout(Binding& b, double v) { set_text(item(b.id2), format_readout(v, b.readout)); }

// ---------------------------------------------------------------- 改动

void Page::on_click(Binding& b, int id) {
    if (loading_ || !b.present) return;
    std::wstring value;
    switch (b.kind) {
        case Kind::Check:
            value = ::SendMessageW(item(b.id), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
            break;
        case Kind::Radio:
            if (::SendMessageW(item(id), BM_GETCHECK, 0, 0) != BST_CHECKED) return;
            for (std::size_t k = 0; k < b.radio_ids.size() && k < b.choices.size(); ++k) {
                if (b.radio_ids[k] == id) value = b.choices[k].value;
            }
            break;
        case Kind::Combo: {
            const auto sel = static_cast<int>(::SendMessageW(item(b.id), CB_GETCURSEL, 0, 0));
            if (sel < 0 || static_cast<std::size_t>(sel) >= b.choices.size()) return;
            value = b.choices[static_cast<std::size_t>(sel)].value;
            break;
        }
        default: return;
    }
    if (value.empty() || iequals(value, b.value)) return;
    b.value = value;
    write(expand(b.key, value));
}

void Page::on_slider(Binding& b) {
    if (loading_ || !b.present || !b.range) return;
    const int pos = static_cast<int>(::SendMessageW(item(b.id), TBM_GETPOS, 0, 0));
    if (pos == b.pos) return;   // 只有真正移动了才写回：不在步长上的 INI 值不会被悄悄改掉
    b.pos = pos;
    const double v = slider_value_at(pos, b.range->lo, b.range->hi, b.step, b.max_dec);
    const std::wstring text = format_fixed(v, b.max_dec);
    b.value = text;
    update_readout(b, v);
    write(expand(b.key, text));
}

void Page::restore_edit(Binding& b) {
    loading_ = true;
    set_text(item(b.id), b.value);
    loading_ = false;
    b.dirty = false;
}

void Page::commit_edit(Binding& b) {
    if (!b.dirty) return;
    b.dirty = false;
    HWND edit = item(b.id);
    const std::wstring text = text_of(edit);
    if (!b.present) return;
    if (b.kind == Kind::Color) {
        const auto c = parse_color_text(text);
        if (!c) {
            restore_edit(b);
            balloon(edit, tr(S::BalloonColorTitle), tr(S::BalloonColorBody));
            return;
        }
        const std::wstring hex = format_color_text(*c);
        loading_ = true;
        set_text(edit, hex);
        loading_ = false;
        update_swatch(b, *c);
        if (iequals(hex, b.value)) return;
        b.value = hex;
        write(expand(b.key, hex));
        return;
    }
    if (!b.range) return;
    const KeyRange r = *b.range;
    const NumberCommit nc = commit_number(text, r.lo, r.hi, b.max_dec);
    if (nc.kind == NumberCommitKind::Invalid) {
        restore_edit(b);
        balloon(edit, tr(S::BalloonNumberTitle), i18n::format(S::BalloonNumberBody, {{L"range", range_text(r.lo, r.hi, b.min_dec)}}));
        return;
    }
    if (const auto err = validate(b.key, nc.value)) {
        restore_edit(b);
        balloon(edit, tr(S::BalloonCannotApply), *err);
        return;
    }
    const std::wstring out = format_number(nc.value, b.max_dec, b.min_dec);
    loading_ = true;
    set_text(edit, out);
    loading_ = false;
    if (nc.kind == NumberCommitKind::Clamped) balloon(edit, tr(S::BalloonOutOfRange), range_text(r.lo, r.hi, b.min_dec), TTI_INFO);
    const auto old = parse_number_text(b.value);
    if (old && *old == nc.value) return;   // 值没变（例如只改了写法）
    b.value = out;
    write(expand(b.key, out));
}

void Page::commit_focus() {
    if (hwnd_ == nullptr) return;
    HWND focus = ::GetFocus();
    if (focus == nullptr || !::IsChild(hwnd_, focus)) return;
    if (Binding* b = find_edit(focus)) {
        commit_edit(*b);
        return;
    }
    commit_custom(focus);
}

// ---------------------------------------------------------------- 版式

HFONT Page::style_font_body() const { return site_.style().body; }

int Page::text_height(HWND ctl, int width) const {
    const std::wstring text = text_of(ctl);
    HDC dc = ::GetDC(hwnd_);
    HGDIOBJ old = ::SelectObject(dc, reinterpret_cast<HFONT>(::SendMessageW(ctl, WM_GETFONT, 0, 0)));
    RECT r{0, 0, std::max(width, 1), 0};
    const wchar_t* s = text.empty() ? L" " : text.c_str();
    ::DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd_, dc);
    return r.bottom - r.top;
}

int Page::default_height(HWND ctl) const {
    const std::wstring cls = class_of(ctl);
    const auto style = static_cast<DWORD>(::GetWindowLongPtrW(ctl, GWL_STYLE));
    if (iequals(cls, L"Edit") || iequals(cls, L"ComboBox")) return 24;
    if (iequals(cls, TRACKBAR_CLASSW)) return 28;
    if (iequals(cls, L"Button")) {
        const DWORD type = style & BS_TYPEMASK;
        return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) ? 28 : 24;
    }
    if (iequals(cls, L"Static")) return (style & SS_TYPEMASK) == SS_ICON ? 16 : 20;
    if (iequals(cls, L"ListBox") || iequals(cls, WC_LISTVIEWW)) return 96;
    return 24;
}

int Page::run_layout(int client_w, int client_h, bool place) {
    (void)client_h;
    using namespace layout;
    const int m = px(kMargin);
    const int x0 = m;
    const int right = client_w - m;
    const int content_w = std::max(right - x0, 1);
    const int ctrl_x = x0 + px(label_w()) + px(kLabelGap);
    const bool adv = site_.show_advanced();
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    auto show = [&](HWND h, bool on) {
        // 看控件自己的 WS_VISIBLE（IsWindowVisible 在分页隐藏时对子控件总是 FALSE，会漏掉"应隐藏"的控件）。
        const bool shown = h != nullptr && (::GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        if (h != nullptr && place && shown != on) ::ShowWindow(h, on ? SW_SHOWNA : SW_HIDE);
    };
    auto pos = [&](HWND h, int x, int y, int w, int hgt) {
        if (h != nullptr && place) ::SetWindowPos(h, nullptr, x, y - scroll_y_, w, hgt, flags);
    };

    int y = m;
    bool first = true;
    bool after_description = false;
    for (auto& it : items_) {
        const bool visible = !it.collapsed && (!it.advanced || adv);
        show(it.text, visible);
        show(it.label, visible);
        for (const auto& p : it.parts) show(p.hwnd, visible && hidden_.count(p.hwnd) == 0);
        if (!visible) continue;
        if (!first) {
            int gap = kRowGap;
            if (it.kind == ItemKind::Heading || after_description) gap = kGroupGap;
            if ((it.kind == ItemKind::Text && it.helper) || it.tight) gap = kHelperGap;
            if (it.group_start) gap = kGroupGap;
            y += px(gap);
        }
        first = false;
        switch (it.kind) {
            case ItemKind::Text: {
                const int x = (it.helper && !it.full_width) ? ctrl_x : x0;
                const int w = std::max(right - x, 1);
                const int h = text_height(it.text, w);
                pos(it.text, x, y, w, h);
                y += h;
                after_description = !it.helper;
                break;
            }
            case ItemKind::Heading: {
                const int h = text_height(it.text, content_w);
                pos(it.text, x0, y, content_w, h);
                y += h;
                after_description = false;
                break;
            }
            case ItemKind::Row: {
                int row_h = px(it.height);
                for (const auto& p : it.parts) {
                    if (p.height > 0) row_h = std::max(row_h, px(p.height));
                }
                const int line_h = std::min(px(it.height), px(kSliderRowH));
                if (it.label != nullptr) pos(it.label, x0, y, px(label_w()), line_h);
                int x = it.full_width ? x0 : ctrl_x;
                for (const auto& p : it.parts) {
                    const int w = p.width < 0 ? std::max(right - x, 1) : px(p.width);
                    const int h = p.height > 0 ? px(p.height) : px(default_height(p.hwnd));
                    const int py = h >= line_h ? y : y + (line_h - h) / 2;
                    const bool combo = iequals(class_of(p.hwnd), L"ComboBox");
                    pos(p.hwnd, x, py, w, combo ? h + px(220) : h);   // 组合框的高度含下拉列表
                    x += w + px(p.gap);
                }
                y += row_h;
                after_description = false;
                break;
            }
        }
    }
    return y + m;
}

void Page::relayout() {
    if (hwnd_ == nullptr || in_layout_) return;
    in_layout_ = true;
    struct Reset {
        bool* flag;
        ~Reset() { *flag = false; }
    } reset{&in_layout_};
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    int cw = rc.right;
    const int ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    int content = run_layout(cw, ch, false);
    const bool need = content > ch;
    if (need != scroll_shown_) {
        scroll_shown_ = need;
        ::ShowScrollBar(hwnd_, SB_VERT, need ? TRUE : FALSE);   // 会改变客户区宽度（并发来 WM_SIZE）
        ::GetClientRect(hwnd_, &rc);
        cw = rc.right;
        content = run_layout(cw, ch, false);
    }
    scroll_max_ = std::max(0, content - ch);
    scroll_y_ = std::clamp(scroll_y_, 0, scroll_max_);
    if (scroll_shown_) {
        SCROLLINFO si{};
        si.cbSize = sizeof si;
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = content - 1;
        si.nPage = static_cast<UINT>(ch);
        si.nPos = scroll_y_;
        ::SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    }
    last_signature_ = layout_signature();
    // 隐藏的页不能用 WM_SETREDRAW（它会把 WS_VISIBLE 位打开）。
    const bool visible = ::IsWindowVisible(hwnd_) != FALSE;
    if (visible) ::SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    run_layout(cw, ch, true);
    if (visible) {
        ::SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
        ::RedrawWindow(hwnd_, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    // 焦点控件被折叠时，焦点落到"显示高级选项"上。
    HWND focus = ::GetFocus();
    if (focus != nullptr && ::IsChild(hwnd_, focus) && !::IsWindowVisible(focus) && advanced_toggle_ != nullptr &&
        visible) {
        ::SetFocus(advanced_toggle_);
    }
}

void Page::apply_style() {
    if (hwnd_ == nullptr) return;
    const UiStyle& st = style();
    for (HWND c = ::GetWindow(hwnd_, GW_CHILD); c != nullptr; c = ::GetWindow(c, GW_HWNDNEXT)) {
        HFONT f = headings_.count(c) != 0 ? st.heading : st.body;
        ::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
    }
    style_changed();
    relayout();
}

int Page::scroll_to(int y) {
    const int clamped = std::clamp(y, 0, scroll_max_);
    if (clamped != scroll_y_) {
        scroll_y_ = clamped;
        relayout();
    }
    return scroll_max_;
}

void Page::on_vscroll(int code) {
    SCROLLINFO si{};
    si.cbSize = sizeof si;
    si.fMask = SIF_ALL;
    ::GetScrollInfo(hwnd_, SB_VERT, &si);
    int y = scroll_y_;
    const int line = px(layout::kWheelStep);
    switch (code) {
        case SB_LINEUP: y -= line; break;
        case SB_LINEDOWN: y += line; break;
        case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: y = si.nTrackPos; break;
        case SB_TOP: y = 0; break;
        case SB_BOTTOM: y = scroll_max_; break;
        default: return;
    }
    scroll_to(y);
}

void Page::ensure_visible(HWND ctl) {
    if (hwnd_ == nullptr || ctl == nullptr || !scroll_shown_ || !::IsChild(hwnd_, ctl)) return;
    HWND child = ctl;
    while (child != nullptr && ::GetParent(child) != hwnd_) child = ::GetParent(child);
    if (child == nullptr) return;
    RECT r{};
    ::GetWindowRect(child, &r);
    ::MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&r), 2);
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int m = px(layout::kMargin);
    if (r.top < m) {
        scroll_to(scroll_y_ + r.top - m);
    } else if (r.bottom > rc.bottom - m) {
        scroll_to(scroll_y_ + r.bottom - (rc.bottom - m));
    }
}

// ---------------------------------------------------------------- 工具

std::optional<double> Page::get_number(SettingKey key) const {
    const auto v = host_.get(key);
    if (!v) return std::nullopt;
    return parse_number_text(*v);
}

bool Page::get_bool(SettingKey key) const {
    const auto v = host_.get(key);
    return v && trim(std::wstring_view(*v)) == L"1";
}

bool Page::write(const std::vector<SettingValue>& values) {
    const bool ok = host_.set(values);
    site_.request_refresh();
    return ok;
}

void Page::balloon(HWND edit, const wchar_t* title, const std::wstring& text, INT icon) {
    if (edit == nullptr) return;
    EDITBALLOONTIP tip{};
    tip.cbStruct = sizeof tip;
    tip.pszTitle = title;
    tip.pszText = text.c_str();
    tip.ttiIcon = icon;
    ::SendMessageW(edit, EM_SHOWBALLOONTIP, 0, reinterpret_cast<LPARAM>(&tip));
}

void Page::set_text(HWND ctl, std::wstring_view text) {
    if (ctl == nullptr) return;
    if (text_of(ctl) == text) return;
    ::SetWindowTextW(ctl, std::wstring(text).c_str());
}

std::wstring Page::text_of(HWND ctl) {
    if (ctl == nullptr) return {};
    const int n = ::GetWindowTextLengthW(ctl);
    std::wstring s(static_cast<std::size_t>(std::max(n, 0)) + 1, L'\0');
    const int got = ::GetWindowTextW(ctl, s.data(), n + 1);
    s.resize(static_cast<std::size_t>(std::max(got, 0)));
    return s;
}

void Page::set_enabled(int id, bool on) { set_enabled(item(id), on); }

void Page::set_enabled(HWND h, bool on) {
    if (h != nullptr && (::IsWindowEnabled(h) != FALSE) != on) ::EnableWindow(h, on ? TRUE : FALSE);
}

}  // namespace mc::ui
