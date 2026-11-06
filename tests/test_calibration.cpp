// test_calibration.cpp — calib/calibration 的单元测试：turn-calibration spec 的状态机、开始条件、计数、合法性、15% 一致性、结果与文案、超时 / 离开游戏 / 静默取消，以及按 exe 查表。
#include "test_framework.h"

#include <map>
#include <string>

#include "calib/calibration.h"

using namespace mc;

namespace {

CalibContext game(const wchar_t* exe = L"fakegame.exe", bool locked = true) {
    CalibContext c;
    c.in_game = true;
    c.cursor_locked = locked;
    c.exe = exe;
    return c;
}

CalibContext desktop() {
    CalibContext c;
    c.exe = L"notepad.exe";
    return c;
}

// Idle → Run1，Run1 中累计 dx1，结束 → Between。返回最后一次 on_hotkey 的效果。
CalibEffect run1(Calibration& cal, double& t, long long dx1, const CalibContext& ctx = game()) {
    cal.on_hotkey(t, ctx);
    t += 5;
    cal.on_mouse_dx(dx1);
    t += 5;
    return cal.on_hotkey(t, ctx);
}

// Between → Run2，累计 dx2，结束（结算）。
CalibEffect run2(Calibration& cal, double& t, long long dx2, const CalibContext& ctx = game()) {
    t += 2;
    cal.on_hotkey(t, ctx);
    t += 5;
    cal.on_mouse_dx(dx2);
    t += 5;
    return cal.on_hotkey(t, ctx);
}

// "Esc 无效"：公开接口里只有 on_hotkey 能推进状态，不存在按键 / Esc 入口（编译期确认）。
template <class T>
concept HasKeyEntry = requires(T& t) { t.on_key(0); } || requires(T& t) { t.on_keydown(0); } ||
                      requires(T& t) { t.on_escape(); } || requires(T& t) { t.on_key_event(0, 0); };
static_assert(!HasKeyEntry<Calibration>, "calibration must only be advanced by the calibrate hotkey");

}  // namespace

// ================================================================ 状态机

TEST(calib_full_flow) {
    Calibration cal;
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"");
    double t = 100;
    CHECK_TRUE(cal.on_hotkey(t, game()).empty());   // 开始不弹通用 toast
    CHECK_TRUE(cal.state() == CalibState::Run1);
    CHECK_EQ(cal.target_exe(), L"fakegame.exe");
    CHECK_EQ(cal.current_abs_count(), 0);
    cal.on_mouse_dx(3000);
    cal.on_mouse_dx(3552);
    t += 10;
    CHECK_TRUE(cal.on_hotkey(t, game()).empty());
    CHECK_TRUE(cal.state() == CalibState::Between);
    CHECK_EQ(cal.first_count(), 6552);
    t += 3;
    CHECK_TRUE(cal.on_hotkey(t, game()).empty());
    CHECK_TRUE(cal.state() == CalibState::Run2);
    CHECK_EQ(cal.current_abs_count(), 0);   // 第 2 次从 0 开始
    cal.on_mouse_dx(6552);
    t += 10;
    const CalibEffect done = cal.on_hotkey(t, game());
    CHECK_TRUE(cal.state() == CalibState::Idle);   // Done 是瞬时状态
    CHECK_TRUE(done.save.has_value());
    if (done.save) {
        CHECK_EQ(done.save->exe, L"fakegame.exe");
        CHECK_NEAR(done.save->counts_per_degree, 18.2, 1e-9);
        CHECK_EQ(done.save->c1, 6552);
        CHECK_EQ(done.save->c2, 6552);
    }
    CHECK_EQ(done.toast, L"校准完成：每度 18.2 计数");
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"");   // 回到 Idle：提示消失
    CHECK_EQ(cal.target_exe(), L"");
}

TEST(calib_start_requires_lock) {
    Calibration cal;
    // 在桌面（记事本）按热键
    CalibEffect e = cal.on_hotkey(1, desktop());
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(e.toast, L"请先在游戏里锁定视角再开始校准");
    CHECK_FALSE(e.save.has_value());
    // 已知游戏的菜单界面：in_game 但光标未锁定
    e = cal.on_hotkey(2, game(L"cs2.exe", false));
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(e.toast, L"请先在游戏里锁定视角再开始校准");
    // 锁定但不在游戏里（例如排除名单里的程序）
    CalibContext locked_not_game = game(L"chrome.exe");
    locked_not_game.in_game = false;
    e = cal.on_hotkey(3, locked_not_game);
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(e.toast, L"请先在游戏里锁定视角再开始校准");
    // 锁定后开始：校准对象为 cs2.exe（按小写记录），计数从 0 开始
    e = cal.on_hotkey(4, game(L"CS2.exe"));
    CHECK_TRUE(e.empty());
    CHECK_TRUE(cal.state() == CalibState::Run1);
    CHECK_EQ(cal.target_exe(), L"cs2.exe");
    CHECK_EQ(cal.current_abs_count(), 0);
}

TEST(calib_esc_not_handled) {
    // 编译期：Calibration 没有任何按键 / Esc 入口（见文件开头的 static_assert）。
    // 运行期：Run1 中鼠标位移与 tick 都不会推进或取消状态。
    Calibration cal;
    cal.on_hotkey(0, game());
    cal.on_mouse_dx(100);
    CHECK_TRUE(cal.update(1, game()).empty());
    CHECK_TRUE(cal.update(59.9, game()).empty());
    CHECK_TRUE(cal.state() == CalibState::Run1);
    CHECK_EQ(cal.current_abs_count(), 100);   // 计数继续
}

// ================================================================ 计数

TEST(calib_count_signed_sum) {
    {
        Calibration cal;
        double t = 0;
        run1(cal, t, 6552);
        CHECK_EQ(cal.first_count(), 6552);   // 向右转一圈
    }
    {
        Calibration cal;
        double t = 0;
        run1(cal, t, -6552);
        CHECK_EQ(cal.first_count(), 6552);   // 向左转一圈
    }
    {
        Calibration cal;
        cal.on_hotkey(0, game());
        cal.on_mouse_dx(7000);
        cal.on_mouse_dx(-448);   // 来回晃动会抵消
        CHECK_EQ(cal.current_abs_count(), 6552);
        cal.on_hotkey(10, game());
        CHECK_TRUE(cal.state() == CalibState::Between);
        CHECK_EQ(cal.first_count(), 6552);
    }
    {
        // 垂直位移不计：Calibration 只接收 dx（app 只把相对事件的 dx 交给它），dy 没有入口。
        Calibration cal;
        cal.on_hotkey(0, game());
        for (int i = 0; i < 6552; ++i) cal.on_mouse_dx(1);
        cal.on_hotkey(10, game());
        CHECK_EQ(cal.first_count(), 6552);
    }
}

TEST(calib_count_only_in_run) {
    Calibration cal;
    cal.on_mouse_dx(5000);   // Idle：不计
    cal.on_hotkey(0, game());
    CHECK_EQ(cal.current_abs_count(), 0);
    cal.on_mouse_dx(6552);
    cal.on_hotkey(10, game());
    CHECK_TRUE(cal.state() == CalibState::Between);
    cal.on_mouse_dx(9999);   // Between：不计
    CHECK_EQ(cal.current_abs_count(), 0);
    cal.on_hotkey(12, game());
    CHECK_TRUE(cal.state() == CalibState::Run2);
    CHECK_EQ(cal.current_abs_count(), 0);
    cal.on_mouse_dx(6600);
    CHECK_EQ(cal.current_abs_count(), 6600);
    // 很长的累计也不会溢出（64 位）
    Calibration big;
    big.on_hotkey(0, game());
    for (int i = 0; i < 1000; ++i) big.on_mouse_dx(5'000'000);
    CHECK_EQ(big.current_abs_count(), 5'000'000'000LL);
}

// ================================================================ 合法性

TEST(calib_valid_bounds) {
    CHECK_FALSE(calib_count_valid(799));
    CHECK_TRUE(calib_count_valid(800));
    CHECK_TRUE(calib_count_valid(400000));
    CHECK_FALSE(calib_count_valid(400001));
    CHECK_FALSE(calib_count_valid(0));
    CHECK_FALSE(calib_count_valid(-1000));
    // 状态机里用同一规则：799 不合法，800 合法
    Calibration a;
    double t = 0;
    CHECK_EQ(run1(a, t, 799).toast, L"计数异常，请重试");
    CHECK_TRUE(a.state() == CalibState::Idle);
    Calibration b;
    t = 0;
    CHECK_TRUE(run1(b, t, -800).empty());
    CHECK_TRUE(b.state() == CalibState::Between);
    Calibration c;
    t = 0;
    CHECK_TRUE(run1(c, t, 400000).empty());
    CHECK_TRUE(c.state() == CalibState::Between);
    Calibration d;
    t = 0;
    CHECK_EQ(run1(d, t, 400001).toast, L"计数异常，请重试");
    CHECK_TRUE(d.state() == CalibState::Idle);
}

TEST(calib_run1_invalid_to_idle) {
    Calibration cal;
    double t = 0;
    const CalibEffect e = run1(cal, t, 500);
    CHECK_EQ(e.toast, L"计数异常，请重试");
    CHECK_FALSE(e.save.has_value());
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"");
    // 可以重新开始
    CHECK_TRUE(cal.on_hotkey(t + 1, game()).empty());
    CHECK_TRUE(cal.state() == CalibState::Run1);
}

TEST(calib_run2_invalid_to_between_keeps_c1) {
    Calibration cal;
    double t = 0;
    run1(cal, t, 6552);
    const CalibEffect e = run2(cal, t, 0);
    CHECK_EQ(e.toast, L"计数异常，请重试");
    CHECK_FALSE(e.save.has_value());
    CHECK_TRUE(cal.state() == CalibState::Between);
    CHECK_EQ(cal.first_count(), 6552);
    CHECK_NEAR(cal.entered_at(), t, 0);   // Between 重新计时
    // 再测第 2 次即可结算
    const CalibEffect ok = run2(cal, t, 6552);
    CHECK_TRUE(ok.save.has_value());
    CHECK_TRUE(cal.state() == CalibState::Idle);
}

// ================================================================ 一致性

TEST(calib_inconsistent_6000_7000) {
    CHECK_FALSE(calib_runs_consistent(6000, 7000));
    Calibration cal;
    double t = 0;
    run1(cal, t, 6000);
    const CalibEffect e = run2(cal, t, 7000);
    CHECK_EQ(e.toast, L"两次结果相差超过 15%，请重新校准");
    CHECK_FALSE(e.save.has_value());   // INI 不变
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(cal.first_count(), 0);   // 两次结果都被丢弃
}

TEST(calib_consistent_6000_6800) {
    CHECK_TRUE(calib_runs_consistent(6000, 6800));
    Calibration cal;
    double t = 0;
    run1(cal, t, 6000);
    const CalibEffect e = run2(cal, t, -6800);
    CHECK_TRUE(e.save.has_value());
    if (e.save) CHECK_NEAR(e.save->counts_per_degree, 6400.0 / 360.0, 1e-9);
}

TEST(calib_consistent_mean_base_6000_6950) {
    // 相对平均约 14.7%（相对较小值约 15.8%）：以平均为基准，判为一致。
    CHECK_TRUE(calib_runs_consistent(6000, 6950));
    CHECK_TRUE(calib_runs_consistent(6950, 6000));
    Calibration cal;
    double t = 0;
    run1(cal, t, 6950);
    CHECK_TRUE(run2(cal, t, 6000).save.has_value());
    // 边界：|d| / mean 恰好 0.15 算一致（≤），略超就不一致。
    CHECK_TRUE(calib_runs_consistent(37000, 43000));    // 6000 / 40000 = 0.15
    CHECK_FALSE(calib_runs_consistent(37000, 43001));
    CHECK_FALSE(calib_runs_consistent(0, 0));
}

// ================================================================ 结果与文案

TEST(calib_cpd_average) {
    CHECK_NEAR(calib_counts_per_degree(6552, 6700), 18.406, 0.001);
    CHECK_NEAR(calib_counts_per_degree(6552, 6552), 18.2, 1e-12);
    CHECK_NEAR(calib_counts_per_degree(13104, 13104), 36.4, 1e-12);
    Calibration cal;
    double t = 0;
    run1(cal, t, 6552);
    const CalibEffect e = run2(cal, t, 6700);
    CHECK_TRUE(e.save.has_value());
    if (e.save) {
        CHECK_NEAR(e.save->counts_per_degree, 6626.0 / 360.0, 1e-12);
        CHECK_EQ(calib_format_cpd(e.save->counts_per_degree), "18.406");
    }
    CHECK_EQ(e.toast, L"校准完成：每度 18.4 计数");
}

TEST(calib_toast_text) {
    CHECK_EQ(calib_result_toast(18.2), L"校准完成：每度 18.2 计数");
    CHECK_EQ(calib_result_toast(36.4), L"校准完成：每度 36.4 计数");
    CHECK_EQ(calib_format_date(2026, 11, 3), L"2026-11-03");
    CHECK_EQ(calib_format_date(2027, 1, 31), L"2027-01-31");
    // 两次 13104 → 36.400
    Calibration cal;
    double t = 0;
    run1(cal, t, 13104);
    const CalibEffect e = run2(cal, t, 13104);
    CHECK_TRUE(e.save.has_value());
    if (e.save) CHECK_EQ(calib_format_cpd(e.save->counts_per_degree), "36.400");
    CHECK_EQ(e.toast, L"校准完成：每度 36.4 计数");
}

TEST(calib_message_texts) {
    Calibration cal;
    cal.on_hotkey(0, game());
    cal.on_mouse_dx(3271);
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"),
             L"校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7\n已转 3271 计数");
    cal.on_mouse_dx(-3271 - 100);   // |Σdx| 取整数绝对值，随输入实时更新
    CHECK_EQ(cal.message(L"Shift+F7"),
             L"校准 1/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Shift+F7\n已转 100 计数");
    cal.on_mouse_dx(100 + 6552);
    cal.on_hotkey(10, game());
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"第 1 次：6552 计数\n准星重新对准参照物，按 Ctrl+Alt+F7 开始第 2 次");
    CHECK_EQ(cal.message(L"Shift+F7"), L"第 1 次：6552 计数\n准星重新对准参照物，按 Shift+F7 开始第 2 次");
    cal.on_hotkey(12, game());
    cal.on_mouse_dx(42);
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"),
             L"校准 2/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Ctrl+Alt+F7\n已转 42 计数");
    CHECK_EQ(cal.message(L"Shift+F7"),
             L"校准 2/2：准星对准一个参照物，水平转一整圈回到同一点，然后再按 Shift+F7\n已转 42 计数");
}

TEST(calib_format_cpd_3dp) {
    CHECK_EQ(calib_format_cpd(18.4), "18.400");
    CHECK_EQ(calib_format_cpd(35.714), "35.714");
    CHECK_EQ(calib_format_cpd(18.2), "18.200");
    CHECK_EQ(calib_format_cpd(1111.111), "1111.111");
    // 800 / 360 = 2.2222…：直接四舍五入为 2.222（× 360 = 799.92，读回来不合法），改写 2.223
    CHECK_EQ(calib_format_cpd(800.0 / 360.0), "2.223");
    CHECK_EQ(calib_format_cpd(400000.0 / 360.0), "1111.111");
    CHECK_TRUE(calib_cpd_valid(800.0 / 360.0));
    CHECK_TRUE(calib_cpd_valid(2.223));
    CHECK_FALSE(calib_cpd_valid(2.222));
    CHECK_FALSE(calib_cpd_valid(1.0));
    CHECK_FALSE(calib_cpd_valid(2000));
    CHECK_TRUE(calib_cpd_valid(1111.111));
    CHECK_FALSE(calib_cpd_valid(1111.112));
}

// ================================================================ 取消

TEST(calib_timeout_per_state) {
    {
        // Run1 超时：60.1 s 后回到 Idle，toast "校准超时，已取消"，提示消失
        Calibration cal;
        cal.on_hotkey(10, game());
        CHECK_TRUE(cal.update(70.0, game()).empty());   // 恰好 60 s 不算超过
        const CalibEffect e = cal.update(70.1, game());
        CHECK_EQ(e.toast, L"校准超时，已取消");
        CHECK_FALSE(e.save.has_value());
        CHECK_TRUE(cal.state() == CalibState::Idle);
        CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"");
    }
    {
        // 每个状态单独计时：Run1 停 50 s 后进入 Between，再停 50 s 不超时
        Calibration cal;
        cal.on_hotkey(0, game());
        cal.on_mouse_dx(6552);
        CHECK_TRUE(cal.update(50, game()).empty());
        cal.on_hotkey(50, game());
        CHECK_TRUE(cal.state() == CalibState::Between);
        CHECK_TRUE(cal.update(100, game()).empty());
        CHECK_TRUE(cal.state() == CalibState::Between);
        CHECK_EQ(cal.update(110.2, game()).toast, L"校准超时，已取消");
        CHECK_TRUE(cal.state() == CalibState::Idle);
    }
    {
        // Run2 也会超时，且丢弃第 1 次的结果
        Calibration cal;
        double t = 0;
        run1(cal, t, 6552);
        cal.on_hotkey(t, game());
        CHECK_TRUE(cal.state() == CalibState::Run2);
        CHECK_EQ(cal.update(t + 61, game()).toast, L"校准超时，已取消");
        CHECK_EQ(cal.first_count(), 0);
    }
    {
        // 超时后才按热键：按超时取消处理，不再推进
        Calibration cal;
        cal.on_hotkey(0, game());
        cal.on_mouse_dx(6552);
        const CalibEffect e = cal.on_hotkey(61, game());
        CHECK_EQ(e.toast, L"校准超时，已取消");
        CHECK_TRUE(cal.state() == CalibState::Idle);
    }
}

TEST(calib_leave_game_cancels) {
    {
        // Run1 中 Alt+Tab 到浏览器
        Calibration cal;
        cal.on_hotkey(0, game(L"cs2.exe"));
        cal.on_mouse_dx(3000);
        CHECK_TRUE(cal.update(1, game(L"CS2.EXE")).empty());   // 同一 exe（大小写不同）不算离开
        CalibContext browser;
        browser.exe = L"chrome.exe";
        const CalibEffect e = cal.update(2, browser);
        CHECK_EQ(e.toast, L"已离开游戏，校准取消");
        CHECK_FALSE(e.save.has_value());
        CHECK_TRUE(cal.state() == CalibState::Idle);
    }
    {
        // Between 中切到 MotionCalm 自己的托盘菜单
        Calibration cal;
        double t = 0;
        run1(cal, t, 6552, game(L"cs2.exe"));
        CHECK_TRUE(cal.state() == CalibState::Between);
        CalibContext own;
        own.exe = L"motioncalm.exe";
        CHECK_EQ(cal.update(t + 1, own).toast, L"已离开游戏，校准取消");
        CHECK_TRUE(cal.state() == CalibState::Idle);
        CHECK_EQ(cal.first_count(), 0);
    }
    {
        // 热键路径上先检查离开游戏：在别的程序里按热键结束 Run1 → 取消，而不是进入 Between
        Calibration cal;
        cal.on_hotkey(0, game(L"cs2.exe"));
        cal.on_mouse_dx(6552);
        const CalibEffect e = cal.on_hotkey(5, game(L"valorant.exe"));
        CHECK_EQ(e.toast, L"已离开游戏，校准取消");
        CHECK_TRUE(cal.state() == CalibState::Idle);
    }
    {
        // 没有前台窗口也算离开；Run 中短暂未锁定不算
        Calibration cal;
        cal.on_hotkey(0, game());
        CHECK_TRUE(cal.update(1, game(L"fakegame.exe", false)).empty());
        CHECK_EQ(cal.update(2, CalibContext{}).toast, L"已离开游戏，校准取消");
    }
}

TEST(calib_cancel_silent) {
    Calibration cal;
    double t = 0;
    run1(cal, t, 6552);
    CHECK_TRUE(cal.active());
    cal.cancel();
    CHECK_FALSE(cal.active());
    CHECK_TRUE(cal.state() == CalibState::Idle);
    CHECK_EQ(cal.first_count(), 0);
    CHECK_EQ(cal.target_exe(), L"");
    CHECK_EQ(cal.message(L"Ctrl+Alt+F7"), L"");
    CHECK_TRUE(cal.update(t + 100, desktop()).empty());   // Idle：update 不再产生任何效果
    cal.cancel();                                          // 重复取消无害
    CHECK_TRUE(cal.state() == CalibState::Idle);
}

// ================================================================ 查表

TEST(calib_lookup_fallback_and_case) {
    const std::map<std::wstring, double> table{{L"cs2.exe", 20.0}, {L"valorant.exe", 35.714}};
    CHECK_NEAR(counts_per_degree_for(L"cs2.exe", table, 18.2), 20.0, 0);
    CHECK_NEAR(counts_per_degree_for(L"CS2.exe", table, 18.2), 20.0, 0);   // 不区分大小写
    CHECK_NEAR(counts_per_degree_for(L"VALORANT.EXE", table, 18.2), 35.714, 0);
    CHECK_NEAR(counts_per_degree_for(L"other.exe", table, 18.2), 18.2, 0);   // 查不到 → 回退
    CHECK_NEAR(counts_per_degree_for(L"", table, 18.2), 18.2, 0);
    CHECK_NEAR(counts_per_degree_for(L"cs2.exe", {}, 18.2), 18.2, 0);
    const std::map<std::wstring, double> bad{{L"x.exe", 1.0}};
    CHECK_NEAR(counts_per_degree_for(L"x.exe", bad, 18.2), 18.2, 0);   // 不合法的值不用
    CHECK_EQ(std::string(calib_state_name(CalibState::Between)), "Between");
}
