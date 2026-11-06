// page_trigger.cpp — "触发"页：触发灵敏度、四个触发来源；高级：截屏检测灵敏度、鼠标锁定判定、移动强度、手柄死区 / 最快转速、转速计算时间、3 档 × 开始变暗 / 完全变暗的转速（开始 < 完全）、截屏检测细节（前进 / 后退的灵敏度、识别画面倾斜与原有六项）。
#include <string>

#include "res/resource.h"
#include "ui/pages/page.h"

namespace mc::ui {
namespace {

using S = i18n::StrId;
using i18n::tr;

struct ThresholdPair {
    SettingKey start;
    SettingKey full;
};
constexpr ThresholdPair kThresholds[] = {
    {{"trigger", "v_start_low"}, {"trigger", "v_full_low"}},
    {{"trigger", "v_start_medium"}, {"trigger", "v_full_medium"}},
    {{"trigger", "v_start_high"}, {"trigger", "v_full_high"}},
};
// 变更 5：前进 / 后退与倾斜各自的三档阈值（[capture] zoom_* / roll_*）
constexpr ThresholdPair kZoomThresholds[] = {
    {{"capture", "zoom_start_low"}, {"capture", "zoom_full_low"}},
    {{"capture", "zoom_start_medium"}, {"capture", "zoom_full_medium"}},
    {{"capture", "zoom_start_high"}, {"capture", "zoom_full_high"}},
};
constexpr ThresholdPair kRollThresholds[] = {
    {{"capture", "roll_start_low"}, {"capture", "roll_full_low"}},
    {{"capture", "roll_start_medium"}, {"capture", "roll_full_medium"}},
    {{"capture", "roll_start_high"}, {"capture", "roll_full_high"}},
};

constexpr SettingKey kCapture{"trigger", "capture"};

class TriggerPage final : public Page {
public:
    using Page::Page;

protected:
    void build() override {
        add_description(tr(S::TrigDesc));

        add_heading(tr(S::TrigHeadSens));
        const std::vector<Choice> levels{
            {tr(S::SensLow), L"low", L""}, {tr(S::SensMedium), L"medium", L""}, {tr(S::SensHigh), L"high", L""}};
        add_combo(tr(S::TrigSensLabel), IDC_TRIG_SENS, {"general", "sensitivity"}, levels, 120);
        add_helper(tr(S::TrigSensHelp));

        add_heading(tr(S::TrigHeadSources));
        HWND src_label = make_label(tr(S::TrigSourcesLabel));
        HWND mouse = make(L"Button", tr(S::TrigMouse), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, IDC_TRIG_MOUSE);
        HWND keys = make(L"Button", tr(S::TrigKeys), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, IDC_TRIG_KEYBOARD);
        HWND pad = make(L"Button", tr(S::TrigPad), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, IDC_TRIG_GAMEPAD);
        HWND cap = make(L"Button", tr(S::TrigCapture), BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, IDC_TRIG_CAPTURE);
        add_row(src_label, {{mouse, -1}});
        add_row(nullptr, {{keys, -1}});
        add_row(nullptr, {{pad, -1}});
        add_row(nullptr, {{cap, -1}});
        add_helper(tr(S::TrigCaptureHelp));
        bind_check(IDC_TRIG_MOUSE, {"trigger", "mouse"});
        bind_check(IDC_TRIG_KEYBOARD, {"trigger", "keyboard"});
        bind_check(IDC_TRIG_GAMEPAD, {"trigger", "gamepad"});
        bind_check(IDC_TRIG_CAPTURE, kCapture);

        add_heading(tr(S::TrigHeadFine), true);
        add_combo(tr(S::TrigCapSensLabel), IDC_TRIG_CAPTURE_SENS, {"trigger", "capture_sensitivity"}, levels, 120, true);
        add_combo(tr(S::TrigLockRuleLabel), IDC_TRIG_LOCK_RULE, {"trigger", "mouse_lock_rule"},
                  {{tr(S::TrigLockStrict), L"strict", L""}, {tr(S::TrigLockHidden), L"hidden", L""}}, 160, true);
        add_helper(tr(S::TrigLockHelp), true);
        add_slider(tr(S::TrigMoveLabel), IDC_TRIG_MOVE, IDC_TRIG_MOVE_VAL, {"trigger", "move_intensity"}, 0.05, 2,
                   Readout::Percent, true);
        add_helper(tr(S::TrigMoveHelp), true);
        add_slider(tr(S::TrigPadDzLabel), IDC_TRIG_PAD_DZ, IDC_TRIG_PAD_DZ_VAL, {"trigger", "gamepad_deadzone"}, 0.01, 2,
                   Readout::Percent, true);
        add_slider(tr(S::TrigPadMaxLabel), IDC_TRIG_PAD_MAX, IDC_TRIG_PAD_MAX_VAL, {"trigger", "gamepad_max_deg_per_s"},
                   10, 0, Readout::DegPerSec, true);
        add_helper(tr(S::TrigPadMaxHelp), true);
        add_slider(tr(S::TrigVwinLabel), IDC_TRIG_VWIN, IDC_TRIG_VWIN_VAL, {"trigger", "velocity_window_ms"}, 5, 0,
                   Readout::Millis, true);
        add_helper(tr(S::TrigVwinHelp), true);

        add_heading(tr(S::TrigHeadThresholds), true);
        const int start_ids[] = {IDC_TRIG_VSTART_LOW, IDC_TRIG_VSTART_MEDIUM, IDC_TRIG_VSTART_HIGH};
        const int full_ids[] = {IDC_TRIG_VFULL_LOW, IDC_TRIG_VFULL_MEDIUM, IDC_TRIG_VFULL_HIGH};
        add_threshold_table(tr(S::TrigThStartHead), tr(S::TrigThFullHead), L'1', start_ids, full_ids, kThresholds);
        add_helper(tr(S::TrigThHelp), true);

        add_heading(tr(S::TrigHeadCapture), true);
        add_slider(tr(S::TrigFwdGainLabel), IDC_TRIG_FWD_GAIN, IDC_TRIG_FWD_GAIN_VAL, {"capture", "forward_gain"}, 0.1, 1,
                   Readout::Gain, true);
        add_helper(tr(S::TrigFwdGainHelp), true);
        add_check(tr(S::TrigRollLabel), IDC_TRIG_ROLL, tr(S::TrigRollCheck), {"capture", "detect_roll"}, true);
        add_helper(tr(S::TrigRollHelp), true);
        {
            int zs[3], zf[3], rs[3], rf[3];
            for (int k = 0; k < 3; ++k) {
                zs[k] = IDC_TRIG_ZOOM_TH_BASE + 2 * k;
                zf[k] = zs[k] + 1;
                rs[k] = IDC_TRIG_ROLL_TH_BASE + 2 * k;
                rf[k] = rs[k] + 1;
            }
            add_threshold_table(tr(S::TrigZoomStartHead), tr(S::TrigZoomFullHead), L'4', zs, zf, kZoomThresholds);
            add_threshold_table(tr(S::TrigRollStartHead), tr(S::TrigRollFullHead), L'7', rs, rf, kRollThresholds);
            add_helper(tr(S::TrigZoomRollHelp), true);
        }
        add_slider(tr(S::TrigHfovLabel), IDC_TRIG_HFOV, IDC_TRIG_HFOV_VAL, {"capture", "assumed_hfov_deg"}, 1, 0,
                   Readout::Degrees, true);
        add_helper(tr(S::TrigHfovHelp), true);
        add_slider(tr(S::TrigMinCorrLabel), IDC_TRIG_MINCORR, IDC_TRIG_MINCORR_VAL, {"capture", "min_correlation"}, 0.01,
                   2, Readout::Percent, true);
        add_helper(tr(S::TrigMinCorrHelp), true);
        add_slider(tr(S::TrigHoldLabel), IDC_TRIG_HOLD, IDC_TRIG_HOLD_VAL, {"capture", "hold_ms"}, 10, 0, Readout::Millis,
                   true);
        add_number(tr(S::TrigMaxShiftLabel), IDC_TRIG_MAXSHIFT, {"capture", "max_shift"}, 0, 0, tr(S::UnitCells), true);
        HWND grid_label = make_label(tr(S::TrigGridLabel));
        HWND gw = make(L"Edit", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP | WS_GROUP, IDC_TRIG_GRIDW, WS_EX_CLIENTEDGE);
        HWND times = make_text(L"×", false);
        HWND gh = make(L"Edit", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP | WS_GROUP, IDC_TRIG_GRIDH, WS_EX_CLIENTEDGE);
        HWND unit = make_text(tr(S::TrigGridUnit), false);
        add_row(grid_label, {{gw, 72, 0, 8}, {times, 12, 20, 8}, {gh, 72, 0, 8}, {unit, -1, 20, 0}}, layout::kRowH, true);
        bind_number(IDC_TRIG_GRIDW, {"capture", "grid_w"}, 0, 0);
        bind_number(IDC_TRIG_GRIDH, {"capture", "grid_h"}, 0, 0);

        add_advanced_toggle();
    }

    bool control_enabled(int id) const override {
        if (id == IDC_TRIG_CAPTURE) return !host_.status().capture_disabled_by_cmdline;
        return true;
    }

    void load_extra() override {
        const SettingsStatus st = host_.status();
        // 带访问键的名字（"截屏运动检测(&C)"）+ 状态后缀（"（采集失败）"等），按当前界面语言。
        set_text(item(IDC_TRIG_CAPTURE),
                 std::wstring(tr(S::TrigCapture)) + capture_label_suffix(st.capture_disabled_by_cmdline, st.capture_failed));
    }

    // 一张"3 档 × 开始 / 完全"的阈值表（高级区）：表头一行 + 每档一行两个编辑框。
    // 行标签"低 / 中 / 高灵敏度"的访问键依次为 first_key、first_key + 1、first_key + 2。
    void add_threshold_table(const wchar_t* start_head, const wchar_t* full_head, wchar_t first_key, const int* start_ids,
                             const int* full_ids, const ThresholdPair* keys) {
        HWND h1 = make_text(start_head, false);
        HWND h2 = make_text(full_head, false);
        add_row(nullptr, {{h1, 136, 20, 16}, {h2, 136, 20, 0}}, 20, true);
        constexpr S kRowIds[] = {S::TrigRowLow, S::TrigRowMedium, S::TrigRowHigh};
        for (int k = 0; k < 3; ++k) {
            const std::wstring row = i18n::format(kRowIds[k], {{L"key", std::wstring(1, static_cast<wchar_t>(first_key + k))}});
            HWND lbl = make_label(row.c_str());
            HWND e1 = make(L"Edit", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP | WS_GROUP, start_ids[k], WS_EX_CLIENTEDGE);
            HWND u1 = make_text(tr(S::UnitDegPerSec), false);
            HWND e2 = make(L"Edit", L"", ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP | WS_GROUP, full_ids[k], WS_EX_CLIENTEDGE);
            HWND u2 = make_text(tr(S::UnitDegPerSec), false);
            add_row(lbl, {{e1, 84, 0, 4}, {u1, 48, 20, 0}, {e2, 84, 0, 4}, {u2, 48, 20, 0}}, layout::kRowH, true);
            bind_number(start_ids[k], keys[k].start, 1, 0);   // 最多一位小数
            bind_number(full_ids[k], keys[k].full, 1, 0);
        }
    }

    std::optional<std::wstring> validate(SettingKey key, double value) override {
        for (const ThresholdPair* table : {kThresholds, kZoomThresholds, kRollThresholds})
        for (int k = 0; k < 3; ++k) {
            const ThresholdPair& t = table[k];
            if (key == t.start) {
                const auto full = get_number(t.full);
                if (full && !thresholds_ordered(value, *full)) return tr(S::TrigThOrderError);
            } else if (key == t.full) {
                const auto start = get_number(t.start);
                if (start && !thresholds_ordered(*start, value)) return tr(S::TrigThOrderError);
            }
        }
        return std::nullopt;
    }
};

}  // namespace

std::unique_ptr<Page> make_trigger_page(SettingsHost& host, PageSite& site) {
    return std::make_unique<TriggerPage>(host, site);
}

}  // namespace mc::ui
