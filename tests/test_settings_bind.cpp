// test_settings_bind.cpp — ui/settings_logic 的数值部分：滑块位置 ⇄ 值（spec 范围与步长）、显示格式、数值框提交（越界钳制、非法输入、小数位）、范围提示文字、颜色文本、阈值顺序、边缘点最淡 / 最浓互推。
#include "test_framework.h"

#include <string>

#include "ui/settings_logic.h"

using namespace mc;
using namespace mc::ui;

TEST(settings_slider_release_ms_round_trip) {
    // release_ms：10–5000 ms，步长 10 → 位置 0..499
    CHECK_EQ(slider_max_pos(10, 5000, 10), 499);
    CHECK_EQ(slider_pos_for(5000, 10, 5000, 10), 499);
    CHECK_NEAR(slider_value_at(499, 10, 5000, 10, 0), 5000.0, 1e-12);
    CHECK_EQ(slider_pos_for(500, 10, 5000, 10), 49);
    CHECK_NEAR(slider_value_at(49, 10, 5000, 10, 0), 500.0, 1e-12);
    CHECK_EQ(format_fixed(slider_value_at(99, 10, 5000, 10, 0), 0), L"1000");
}

TEST(settings_slider_off_step_value_stays_nearest) {
    // feather：0.05–1.00，步长 0.01；INI 中 0.357 → 位置 31（显示 0.36 的位置），值本身不变
    CHECK_EQ(slider_max_pos(0.05, 1.0, 0.01), 95);
    CHECK_EQ(slider_pos_for(0.357, 0.05, 1.0, 0.01), 31);
    CHECK_EQ(slider_pos_for(0.355, 0.05, 1.0, 0.01), 31);
    CHECK_EQ(slider_pos_for(0.35, 0.05, 1.0, 0.01), 30);
    CHECK_EQ(format_fixed(slider_value_at(30, 0.05, 1.0, 0.01, 2), 2), L"0.35");
    CHECK_EQ(format_fixed(slider_value_at(31, 0.05, 1.0, 0.01, 2), 2), L"0.36");
}

TEST(settings_slider_spec_ranges_and_steps) {
    struct Case {
        double lo, hi, step;
        int decimals;
        int max_pos;
        const wchar_t* at_max;
    };
    const Case cases[] = {
        {0.0, 1.0, 0.01, 2, 100, L"1.00"},     // max_opacity_*、edge_dot_*_alpha
        {0.05, 1.0, 0.01, 2, 95, L"1.00"},     // clear_height_*、feather
        {0.5, 3.0, 0.05, 2, 50, L"3.00"},      // ellipse_ratio
        {10, 2000, 10, 0, 199, L"2000"},       // attack_ms
        {1, 20, 1, 0, 19, L"20"},              // *_radius_px
        {0, 400, 1, 0, 400, L"400"},           // edge_dot_margin_px
        {0.0, 1.0, 0.05, 2, 20, L"1.00"},      // move_intensity
        {10, 2000, 10, 0, 199, L"2000"},       // gamepad_max_deg_per_s
        {0.0, 0.9, 0.01, 2, 90, L"0.90"},      // gamepad_deadzone
        {10, 500, 5, 0, 98, L"500"},           // velocity_window_ms
        {30, 170, 1, 0, 140, L"170"},          // assumed_hfov_deg
        {0.10, 0.99, 0.01, 2, 89, L"0.99"},    // min_correlation
        {0, 1000, 10, 0, 100, L"1000"},        // hold_ms
        {0.5, 3.0, 0.1, 1, 25, L"3.0"},        // forward_gain（变更 5）
    };
    for (const Case& c : cases) {
        CHECK_EQ(slider_max_pos(c.lo, c.hi, c.step), c.max_pos);
        CHECK_EQ(format_fixed(slider_value_at(c.max_pos, c.lo, c.hi, c.step, c.decimals), c.decimals), c.at_max);
        CHECK_NEAR(slider_value_at(0, c.lo, c.hi, c.step, c.decimals), c.lo, 1e-12);
        // 每个位置的值都落在范围内，且往返到同一位置
        for (int pos = 0; pos <= c.max_pos; ++pos) {
            const double v = slider_value_at(pos, c.lo, c.hi, c.step, c.decimals);
            CHECK(v >= c.lo && v <= c.hi);
            CHECK_EQ(slider_pos_for(v, c.lo, c.hi, c.step), pos);
        }
    }
}

TEST(settings_slider_clamps_out_of_range) {
    CHECK_EQ(slider_pos_for(-5, 10, 5000, 10), 0);
    CHECK_EQ(slider_pos_for(1e9, 10, 5000, 10), 499);
    CHECK_EQ(slider_pos_for(5, 10, 2000, 10), 0);   // INI attack_ms=5 被截断为 10 → 最左端
    CHECK_NEAR(slider_value_at(-3, 10, 5000, 10, 0), 10.0, 1e-12);
    CHECK_NEAR(slider_value_at(10000, 10, 5000, 10, 0), 5000.0, 1e-12);
    CHECK_EQ(slider_max_pos(5, 5, 1), 0);
    CHECK_EQ(slider_max_pos(0, 1, 0), 0);
}

TEST(settings_round_and_format) {
    CHECK_NEAR(round_decimals(0.355, 2), 0.36, 1e-12);
    CHECK_NEAR(round_decimals(0.354, 2), 0.35, 1e-12);
    CHECK_NEAR(round_decimals(60.25, 1), 60.3, 1e-12);
    CHECK_NEAR(round_decimals(-0.004, 2), 0.0, 1e-12);
    CHECK_EQ(format_fixed(0.0000001, 2), L"0.00");
    CHECK_EQ(format_fixed(-0.001, 2), L"0.00");   // 不输出 -0.00
    CHECK_EQ(format_fixed(1.25, 2), L"1.25");
    CHECK_EQ(format_number(400, 1, 0), L"400");
    CHECK_EQ(format_number(60.25, 1, 0), L"60.3");
    CHECK_EQ(format_number(9, 1, 1), L"9.0");
    CHECK_EQ(format_number(18.2, 1, 1), L"18.2");
    CHECK_EQ(format_number(0.355, 3, 2), L"0.355");
    CHECK_EQ(format_number(0.6, 2, 2), L"0.60");
}

TEST(settings_parse_number_text) {
    CHECK_TRUE(parse_number_text(L"400").has_value());
    CHECK_NEAR(*parse_number_text(L"  400 "), 400.0, 0);
    CHECK_NEAR(*parse_number_text(L"0.35"), 0.35, 0);
    CHECK_NEAR(*parse_number_text(L".5"), 0.5, 0);
    CHECK_NEAR(*parse_number_text(L"5."), 5.0, 0);
    CHECK_NEAR(*parse_number_text(L"+7"), 7.0, 0);
    CHECK_NEAR(*parse_number_text(L"-2.5"), -2.5, 0);
    CHECK_NEAR(*parse_number_text(L"\xFF11\xFF18\xFF0E\xFF12"), 18.2, 1e-12);   // 全角"１８．２"
    CHECK_FALSE(parse_number_text(L"abc").has_value());
    CHECK_FALSE(parse_number_text(L"").has_value());
    CHECK_FALSE(parse_number_text(L"   ").has_value());
    CHECK_FALSE(parse_number_text(L"1.2.3").has_value());
    CHECK_FALSE(parse_number_text(L"1e3").has_value());
    CHECK_FALSE(parse_number_text(L"-").has_value());
    CHECK_FALSE(parse_number_text(L".").has_value());
    CHECK_FALSE(parse_number_text(L"1-2").has_value());
    CHECK_FALSE(parse_number_text(L"12 ms").has_value());
    CHECK_FALSE(parse_number_text(L"inf").has_value());
}

TEST(settings_commit_number_clamps_and_rejects) {
    // spec：阈值框输入 9000 → 5000 并提示"范围 1–5000"
    NumberCommit r = commit_number(L"9000", 1, 5000, 1);
    CHECK_EQ(r.kind, NumberCommitKind::Clamped);
    CHECK_NEAR(r.value, 5000.0, 0);
    CHECK_EQ(range_text(1, 5000, 0), L"范围 1–5000");
    r = commit_number(L"0", 1, 5000, 1);
    CHECK_EQ(r.kind, NumberCommitKind::Clamped);
    CHECK_NEAR(r.value, 1.0, 0);
    // 最多一位小数
    r = commit_number(L"60.25", 1, 5000, 1);
    CHECK_EQ(r.kind, NumberCommitKind::Ok);
    CHECK_NEAR(r.value, 60.3, 1e-12);
    CHECK_EQ(format_number(r.value, 1, 0), L"60.3");
    // 整数框
    r = commit_number(L"159.6", 64, 320, 0);
    CHECK_EQ(r.kind, NumberCommitKind::Ok);
    CHECK_NEAR(r.value, 160.0, 0);
    // 非法输入（"椭圆横纵比"框输入 abc）
    r = commit_number(L"abc", 0.5, 3, 2);
    CHECK_EQ(r.kind, NumberCommitKind::Invalid);
    // 默认每度计数 1.0–1000.0，一位小数
    r = commit_number(L"9.1", 1, 1000, 1);
    CHECK_EQ(r.kind, NumberCommitKind::Ok);
    CHECK_EQ(format_number(r.value, 1, 1), L"9.1");
    CHECK_EQ(range_text(1, 1000, 1), L"范围 1.0–1000.0");
    CHECK_EQ(range_text(0.05, 1, 2), L"范围 0.05–1.00");
    CHECK_EQ(range_text(0, 100, 0), L"范围 0–100");
}

TEST(settings_color_text) {
    const auto c = parse_color_text(L"200000");
    CHECK_TRUE(c.has_value());
    CHECK_EQ(c->r, 0x20);
    CHECK_EQ(c->g, 0x00);
    CHECK_EQ(c->b, 0x00);
    CHECK_TRUE(parse_color_text(L"c8c8c8").has_value());
    CHECK_EQ(format_color_text(*parse_color_text(L" c8C8c8 ")), L"C8C8C8");
    CHECK_EQ(format_color_text(Rgb{0xFF, 0x0A, 0x00}), L"FF0A00");
    CHECK_FALSE(parse_color_text(L"#200000").has_value());
    CHECK_FALSE(parse_color_text(L"20000").has_value());
    CHECK_FALSE(parse_color_text(L"2000000").has_value());
    CHECK_FALSE(parse_color_text(L"GG0000").has_value());
    CHECK_FALSE(parse_color_text(L"").has_value());
}

TEST(settings_threshold_order) {
    CHECK_TRUE(thresholds_ordered(60, 360));
    CHECK_FALSE(thresholds_ordered(400, 360));   // spec：中档开始出现改为 400（满强度 360）被拒
    CHECK_FALSE(thresholds_ordered(360, 360));
    CHECK_TRUE(thresholds_ordered(359.9, 360));
}

TEST(settings_edge_alpha_push) {
    // spec：最浓 0.75，把最淡拖到 0.80 → 两者都为 0.80
    AlphaPair p = push_alpha_pair(0.80, 0.75, true);
    CHECK_NEAR(p.min_alpha, 0.80, 0);
    CHECK_NEAR(p.max_alpha, 0.80, 0);
    // 把最浓拖到低于最淡 → 最淡跟下来
    p = push_alpha_pair(0.15, 0.10, false);
    CHECK_NEAR(p.min_alpha, 0.10, 0);
    CHECK_NEAR(p.max_alpha, 0.10, 0);
    // 不冲突时不动另一个
    p = push_alpha_pair(0.20, 0.75, true);
    CHECK_NEAR(p.min_alpha, 0.20, 0);
    CHECK_NEAR(p.max_alpha, 0.75, 0);
    p = push_alpha_pair(0.15, 0.50, false);
    CHECK_NEAR(p.min_alpha, 0.15, 0);
    CHECK_NEAR(p.max_alpha, 0.50, 0);
}
