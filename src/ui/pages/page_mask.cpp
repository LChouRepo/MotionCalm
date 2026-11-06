// page_mask.cpp — "遮罩"页：预览强度（不保存，窗口打开期间作为强度下限）、启用、强度档位（每档一句说明）；高级：颜色、3 档 × 最深程度 / 清晰区大小、形状、边缘柔和度、出现 / 淡出速度。
#include <algorithm>
#include <cmath>

#include "res/resource.h"
#include "ui/pages/page.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

class MaskPage final : public Page {
public:
    using Page::Page;

protected:
    void build() override {
        add_description(tr(S::MaskDesc));

        add_heading(tr(S::MaskHeadPreview));
        HWND lbl = make_label(tr(S::MaskPreviewLabel));
        preview_ = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_BOTH | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, IDC_MASK_PREVIEW);
        preview_readout_ = make(L"Static", L"0%", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | WS_GROUP, IDC_MASK_PREVIEW_VAL);
        setup_slider(preview_, preview_readout_, 100);
        add_row(lbl, {{preview_, layout::slider_w()}, {preview_readout_, layout::readout_w(), 20, 0}}, layout::kSliderRowH);
        add_helper(tr(S::MaskPreviewHelp));

        add_heading(tr(S::MaskHeadStrength));
        add_check(tr(S::MaskEnabledLabel), IDC_MASK_ENABLED, tr(S::MaskEnabledCheck), {"general", "enabled"});
        add_radio_group(tr(S::MaskLevelLabel), {IDC_MASK_LEVEL_LIGHT, IDC_MASK_LEVEL_MEDIUM, IDC_MASK_LEVEL_STRONG},
                        {"general", "mask_level"},
                        {{tr(S::LevelLight), L"light", tr(S::MaskNoteLight)},
                         {tr(S::LevelMedium), L"medium", tr(S::MaskNoteMedium)},
                         {tr(S::LevelStrong), L"strong", tr(S::MaskNoteStrong)}});

        add_heading(tr(S::MaskHeadLook), true);
        add_color(tr(S::MaskColorLabel), IDC_MASK_COLOR, IDC_MASK_COLOR_SWATCH, {"mask", "color"}, true);
        add_helper(tr(S::MaskColorHelp), true);
        add_slider(tr(S::MaskOpacityLight), IDC_MASK_OPACITY_L, IDC_MASK_OPACITY_L_VAL, {"mask", "max_opacity_light"}, 0.01,
                   2, Readout::Percent, true);
        add_slider(tr(S::MaskClearLight), IDC_MASK_CLEAR_L, IDC_MASK_CLEAR_L_VAL, {"mask", "clear_height_light"}, 0.01,
                   2, Readout::ScreenHeight, true);
        add_slider(tr(S::MaskOpacityMedium), IDC_MASK_OPACITY_M, IDC_MASK_OPACITY_M_VAL, {"mask", "max_opacity_medium"},
                   0.01, 2, Readout::Percent, true);
        add_slider(tr(S::MaskClearMedium), IDC_MASK_CLEAR_M, IDC_MASK_CLEAR_M_VAL, {"mask", "clear_height_medium"},
                   0.01, 2, Readout::ScreenHeight, true);
        add_slider(tr(S::MaskOpacityStrong), IDC_MASK_OPACITY_S, IDC_MASK_OPACITY_S_VAL, {"mask", "max_opacity_strong"},
                   0.01, 2, Readout::Percent, true);
        add_slider(tr(S::MaskClearStrong), IDC_MASK_CLEAR_S, IDC_MASK_CLEAR_S_VAL, {"mask", "clear_height_strong"},
                   0.01, 2, Readout::ScreenHeight, true);
        add_helper(tr(S::MaskLevelsHelp), true);
        add_slider(tr(S::MaskShapeLabel), IDC_MASK_ELLIPSE, IDC_MASK_ELLIPSE_VAL, {"mask", "ellipse_ratio"}, 0.05,
                   2, Readout::Times, true);
        add_helper(tr(S::MaskShapeHelp), true);
        add_slider(tr(S::MaskFeatherLabel), IDC_MASK_FEATHER, IDC_MASK_FEATHER_VAL, {"mask", "feather"}, 0.01, 2,
                   Readout::Percent, true);
        add_helper(tr(S::MaskFeatherHelp), true);

        add_heading(tr(S::MaskHeadSpeed), true);
        add_slider(tr(S::MaskAttackLabel), IDC_MASK_ATTACK, IDC_MASK_ATTACK_VAL, {"mask", "attack_ms"}, 10, 0,
                   Readout::Millis, true);
        add_helper(tr(S::MaskAttackHelp), true);
        add_slider(tr(S::MaskReleaseLabel), IDC_MASK_RELEASE, IDC_MASK_RELEASE_VAL, {"mask", "release_ms"}, 10, 0,
                   Readout::Millis, true);
        add_helper(tr(S::MaskReleaseHelp), true);

        add_advanced_toggle();
    }

    void load_extra() override {
        const int pct = static_cast<int>(std::lround(std::clamp(site_.preview(), 0.0f, 1.0f) * 100.0f));
        if (static_cast<int>(::SendMessageW(preview_, TBM_GETPOS, 0, 0)) != pct) {
            ::SendMessageW(preview_, TBM_SETPOS, TRUE, pct);
        }
        set_text(preview_readout_, format_readout(pct / 100.0, Readout::Percent));
    }

    bool hscroll(HWND ctl) override {
        if (ctl != preview_) return false;
        const int pct = std::clamp(static_cast<int>(::SendMessageW(ctl, TBM_GETPOS, 0, 0)), 0, 100);
        set_text(preview_readout_, format_readout(pct / 100.0, Readout::Percent));
        site_.set_preview(static_cast<float>(pct) / 100.0f);
        return true;
    }

private:
    HWND preview_ = nullptr;
    HWND preview_readout_ = nullptr;
};

}  // namespace

std::unique_ptr<Page> make_mask_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<MaskPage>(host, site);
}

}  // namespace mc::ui
