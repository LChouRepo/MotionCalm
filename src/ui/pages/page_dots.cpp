// page_dots.cpp — "参照物"页：中心点 / 边缘点开关；高级：大小、颜色、描边、边缘点最淡 / 最浓亮度（互推）、离屏幕边缘的距离。关掉某类点时它的细调控件灰显。
#include <array>

#include "res/resource.h"
#include "ui/pages/page.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

constexpr SettingKey kCenter{"dots", "center_dot"};
constexpr SettingKey kEdge{"dots", "edge_dots"};
constexpr SettingKey kMinAlpha{"dots", "edge_dot_min_alpha"};
constexpr SettingKey kMaxAlpha{"dots", "edge_dot_max_alpha"};

constexpr std::array kCenterDependents{
    IDC_DOTS_CENTER_RADIUS, IDC_DOTS_CENTER_RADIUS_VAL, IDC_DOTS_CENTER_COLOR, IDC_DOTS_CENTER_SWATCH,
    IDC_DOTS_CENTER_OUTLINE,
};
constexpr std::array kEdgeDependents{
    IDC_DOTS_EDGE_RADIUS, IDC_DOTS_EDGE_RADIUS_VAL, IDC_DOTS_EDGE_COLOR,  IDC_DOTS_EDGE_SWATCH,
    IDC_DOTS_EDGE_MIN,    IDC_DOTS_EDGE_MIN_VAL,    IDC_DOTS_EDGE_MAX,    IDC_DOTS_EDGE_MAX_VAL,
    IDC_DOTS_EDGE_MARGIN, IDC_DOTS_EDGE_MARGIN_VAL,
};

template <std::size_t N>
bool contains(const std::array<int, N>& ids, int id) {
    for (int x : ids) {
        if (x == id) return true;
    }
    return false;
}

class DotsPage final : public Page {
public:
    using Page::Page;

protected:
    void build() override {
        add_description(tr(S::DotsDesc));

        add_heading(tr(S::DotsHeadCenter));
        add_check(tr(S::DotsCenterLabel), IDC_DOTS_CENTER, tr(S::DotsCenterCheck), kCenter);
        add_helper(tr(S::DotsCenterHelp));
        add_slider(tr(S::DotsCenterSize), IDC_DOTS_CENTER_RADIUS, IDC_DOTS_CENTER_RADIUS_VAL, {"dots", "center_dot_radius_px"},
                   1, 0, Readout::Pixels, true);
        add_color(tr(S::DotsCenterColor), IDC_DOTS_CENTER_COLOR, IDC_DOTS_CENTER_SWATCH, {"dots", "center_dot_color"}, true);
        add_check(tr(S::DotsOutlineLabel), IDC_DOTS_CENTER_OUTLINE, tr(S::DotsOutlineCheck), {"dots", "center_dot_outline"}, true);
        add_helper(tr(S::DotsOutlineHelp), true);

        add_heading(tr(S::DotsHeadEdge));
        add_check(tr(S::DotsEdgeLabel), IDC_DOTS_EDGE, tr(S::DotsEdgeCheck), kEdge);
        add_helper(tr(S::DotsEdgeHelp));
        add_slider(tr(S::DotsEdgeSize), IDC_DOTS_EDGE_RADIUS, IDC_DOTS_EDGE_RADIUS_VAL, {"dots", "edge_dot_radius_px"}, 1,
                   0, Readout::Pixels, true);
        add_color(tr(S::DotsEdgeColor), IDC_DOTS_EDGE_COLOR, IDC_DOTS_EDGE_SWATCH, {"dots", "edge_dot_color"}, true);
        add_slider(tr(S::DotsMinLabel), IDC_DOTS_EDGE_MIN, IDC_DOTS_EDGE_MIN_VAL, kMinAlpha, 0.01, 2, Readout::Percent, true);
        add_helper(tr(S::DotsMinHelp), true);
        add_slider(tr(S::DotsMaxLabel), IDC_DOTS_EDGE_MAX, IDC_DOTS_EDGE_MAX_VAL, kMaxAlpha, 0.01, 2, Readout::Percent, true);
        add_helper(tr(S::DotsMaxHelp), true);
        add_slider(tr(S::DotsMarginLabel), IDC_DOTS_EDGE_MARGIN, IDC_DOTS_EDGE_MARGIN_VAL, {"dots", "edge_dot_margin_px"}, 1, 0,
                   Readout::Pixels, true);

        add_advanced_toggle();
    }

    bool control_enabled(int id) const override {
        if (contains(kCenterDependents, id)) return get_bool(kCenter);
        if (contains(kEdgeDependents, id)) return get_bool(kEdge);
        return true;
    }

    // 最淡不超过最浓：改一个时把另一个推到相同值，两键在同一次改动里写入。
    std::vector<SettingValue> expand(SettingKey key, std::wstring value) override {
        const bool is_min = key == kMinAlpha;
        if (!is_min && key != kMaxAlpha) return Page::expand(key, std::move(value));
        const auto v = parse_number_text(value);
        const auto other = get_number(is_min ? kMaxAlpha : kMinAlpha);
        if (!v || !other) return Page::expand(key, std::move(value));
        const AlphaPair p = is_min ? push_alpha_pair(*v, *other, true) : push_alpha_pair(*other, *v, false);
        std::vector<SettingValue> out{{key, value}};
        if (is_min && p.max_alpha != *other) out.push_back({kMaxAlpha, format_fixed(p.max_alpha, 2)});
        if (!is_min && p.min_alpha != *other) out.push_back({kMinAlpha, format_fixed(p.min_alpha, 2)});
        return out;
    }
};

}  // namespace

std::unique_ptr<Page> make_dots_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<DotsPage>(host, site);
}

}  // namespace mc::ui
