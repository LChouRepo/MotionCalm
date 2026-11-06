// test_game_rules.cpp — detect/game_detect 纯规则的单元测试：game-detection spec 的全屏、光标、锁定、排除、已知游戏、in_game、手动标记、--force-game 与门控场景。
#include "test_framework.h"

#include <windows.h>

#include <set>
#include <string>
#include <vector>

#include "config/config.h"
#include "detect/game_detect.h"
#include "util/strings.h"

using namespace mc;

namespace {

const RECT kMonitor{0, 0, 1920, 1080};

// 一个"未知的窗口化程序、光标可见、未限制"的基准采样。
GameSample base_sample(std::wstring exe) {
    GameSample s;
    s.exe = std::move(exe);
    s.window = RECT{100, 100, 1380, 820};
    s.monitor_rect = kMonitor;
    s.virtual_screen = kMonitor;
    s.clip = kMonitor;
    s.cursor_showing = true;
    return s;
}

GameSample fullscreen_hidden(std::wstring exe) {
    GameSample s = base_sample(std::move(exe));
    s.window = kMonitor;
    s.cursor_showing = false;
    return s;
}

GameContext eval(const GameSample& s, const DetectLists& lists = DetectLists{},
                 MouseLockRule rule = MouseLockRule::Strict, bool force = false) {
    return evaluate_game(s, lists, rule, force);
}

}  // namespace

TEST(game_rules_fullscreen_tolerance) {
    CHECK_TRUE(is_fullscreen(RECT{0, 0, 1920, 1080}, kMonitor));
    CHECK_TRUE(is_fullscreen(RECT{0, 0, 1919, 1080}, kMonitor));    // 差 1 px
    CHECK_FALSE(is_fullscreen(RECT{0, 0, 1918, 1080}, kMonitor));   // 差 2 px
    CHECK_FALSE(is_fullscreen(RECT{0, 0, 1920, 1040}, kMonitor));   // 最大化、任务栏可见
    CHECK_TRUE(is_fullscreen(RECT{-1, -1, 1921, 1081}, kMonitor));
    const RECT second{1920, 0, 4480, 1440};
    CHECK_TRUE(is_fullscreen(second, second));                        // 副屏全屏
    CHECK_FALSE(is_fullscreen(second, kMonitor));

    GameSample s = base_sample(L"game.exe");
    s.window = RECT{0, 0, 1919, 1080};
    CHECK_TRUE(eval(s).fullscreen);
    s.window = RECT{0, 0, 1918, 1080};
    CHECK_FALSE(eval(s).fullscreen);
}

TEST(game_rules_cursor_hidden_and_clipped) {
    const RECT vs{0, 0, 4480, 1440};
    CHECK_FALSE(is_clipped(vs, vs));                              // 等于虚拟屏幕：未限制
    CHECK_TRUE(is_clipped(RECT{0, 0, 1920, 1080}, vs));           // 限制在主屏游戏窗口
    CHECK_TRUE(is_clipped(RECT{0, 0, 4480, 1439}, vs));           // 任一边不同
    CHECK_TRUE(is_clipped(RECT{1920, 0, 4480, 1440}, vs));

    GameSample s = base_sample(L"game.exe");
    s.cursor_showing = false;
    s.clip = RECT{100, 100, 1380, 820};
    const GameContext c = eval(s);
    CHECK_TRUE(c.cursor_hidden);
    CHECK_TRUE(c.cursor_clipped);
    s.cursor_showing = true;
    s.clip = s.virtual_screen;
    const GameContext d = eval(s);
    CHECK_FALSE(d.cursor_hidden);
    CHECK_FALSE(d.cursor_clipped);
}

TEST(game_rules_strict_lock) {
    const auto strict = MouseLockRule::Strict;
    CHECK_TRUE(is_cursor_locked(true, true, 0, 0, strict));       // 隐藏并限制（鼠标可以不动）
    CHECK_TRUE(is_cursor_locked(true, false, 20, 1, strict));     // 隐藏并每帧回中
    CHECK_FALSE(is_cursor_locked(true, false, 20, 15, strict));   // 光标跟着鼠标走
    CHECK_FALSE(is_cursor_locked(true, false, 5, 0, strict));     // 位移太小
    CHECK_FALSE(is_cursor_locked(false, true, 20, 0, strict));    // 光标可见
    CHECK_FALSE(is_cursor_locked(true, false, 8, 0, strict));     // 边界：> 8 才算
    CHECK_FALSE(is_cursor_locked(true, false, 20, 3, strict));    // 边界：< 3 才算

    GameSample s = base_sample(L"game.exe");
    s.cursor_showing = false;
    s.raw_path_100ms = 20;
    s.cursor_disp_100ms = 1;
    CHECK_TRUE(eval(s).cursor_locked);
    s.cursor_disp_100ms = 15;
    CHECK_FALSE(eval(s).cursor_locked);
    s.clip = RECT{100, 100, 1380, 820};
    CHECK_TRUE(eval(s).cursor_locked);
}

TEST(game_rules_hidden_mode) {
    const auto hidden = MouseLockRule::Hidden;
    CHECK_TRUE(is_cursor_locked(true, false, 0, 0, hidden));   // 只隐藏不限制、鼠标静止
    CHECK_FALSE(is_cursor_locked(false, true, 100, 0, hidden));
    GameSample s = base_sample(L"game.exe");
    s.cursor_showing = false;
    CHECK_TRUE(eval(s, DetectLists{}, hidden).cursor_locked);
    CHECK_FALSE(eval(s, DetectLists{}, MouseLockRule::Strict).cursor_locked);
}

TEST(game_rules_default_exclude_22) {
    const Config cfg;
    const auto& ex = cfg.detect.exclude;
    CHECK_EQ(ex.size(), 22);
    if (ex.size() == 22) {
        CHECK_EQ(ex.front(), L"chrome.exe");
        CHECK_EQ(ex.back(), L"obs64.exe");
    }
    std::set<std::wstring> unique;
    for (std::size_t i = 0; i < ex.size(); ++i) {
        CHECK_TRUE(ends_with_ci(ex[i], L".exe"));
        CHECK_EQ(ex[i], kDefaultExcludeList[i]);
        unique.insert(to_lower_ascii(ex[i]));
    }
    CHECK_EQ(unique.size(), 22);
    const std::vector<std::wstring> must{L"PotPlayerMini64.exe", L"Video.UI.exe", L"Discord.exe", L"explorer.exe",
                                         L"vlc.exe", L"mpc-hc64.exe"};
    for (const auto& m : must) CHECK_TRUE(list_contains(ex, m));
}

TEST(game_rules_exclude_case_insensitive) {
    const DetectLists lists = Config{}.detect;
    const GameContext pot = eval(fullscreen_hidden(L"potplayermini64.exe"), lists);
    CHECK_TRUE(pot.excluded);
    CHECK_FALSE(pot.in_game);

    // 全屏视频：chrome.exe 全屏且光标自动隐藏。
    const GameContext chrome = eval(fullscreen_hidden(L"chrome.exe"), lists);
    CHECK_TRUE(chrome.excluded);
    CHECK_FALSE(chrome.in_game);
    const Gates g = compute_gates(chrome, true, true, true, true);
    CHECK_TRUE(g == Gates{});

    // 同时在 exclude 与 force_game 中：不排除。
    DetectLists forced = lists;
    forced.force_game.push_back(L"Chrome.EXE");
    const GameContext c2 = eval(fullscreen_hidden(L"chrome.exe"), forced);
    CHECK_FALSE(c2.excluded);
    CHECK_TRUE(c2.known_game);
    CHECK_TRUE(c2.in_game);
}

TEST(game_rules_in_game) {
    CHECK_TRUE(eval(fullscreen_hidden(L"unknown.exe")).in_game);   // 未知的全屏游戏
    GameSample windowed_hidden = base_sample(L"unknown.exe");
    windowed_hidden.cursor_showing = false;
    CHECK_FALSE(eval(windowed_hidden).in_game);                    // 未知的窗口化程序
    GameSample fullscreen_visible = base_sample(L"unknown.exe");
    fullscreen_visible.window = kMonitor;
    CHECK_FALSE(eval(fullscreen_visible).in_game);

    DetectLists lists;
    lists.known_games.push_back(L"newgame.exe");
    const GameContext menu = eval(base_sample(L"newgame.exe"), lists);   // 已知游戏的菜单界面（光标可见、窗口化）
    CHECK_TRUE(menu.known_game);
    CHECK_TRUE(menu.in_game);
}

TEST(game_rules_own_process_and_empty_foreground) {
    GameSample own = fullscreen_hidden(L"motioncalm.exe");
    own.own_process = true;
    own.clip = RECT{0, 0, 100, 100};
    const GameContext c = eval(own, DetectLists{}, MouseLockRule::Hidden, true);
    CHECK_FALSE(c.in_game);
    CHECK_FALSE(c.cursor_locked || c.known_game || c.fullscreen || c.cursor_hidden || c.excluded);
    CHECK_TRUE(compute_gates(c, true, true, true, true) == Gates{});

    const GameContext none = eval(fullscreen_hidden(L""), DetectLists{}, MouseLockRule::Strict, true);
    CHECK_FALSE(none.in_game);
    CHECK_TRUE(compute_gates(none, true, true, true, true) == Gates{});
}

TEST(game_rules_known_games_record) {
    DetectLists lists = Config{}.detect;
    GameSample s = base_sample(L"newgame.exe");
    s.cursor_showing = false;
    s.clip = RECT{100, 100, 1380, 820};
    const GameContext locked = eval(s, lists);
    CHECK_TRUE(locked.cursor_locked);
    CHECK_TRUE(should_record_known_game(locked, lists, false));        // 首次锁定：追加
    CHECK_FALSE(should_record_known_game(locked, lists, true));        // --force-game 模式不记录
    lists.known_games.push_back(locked.exe);
    CHECK_FALSE(should_record_known_game(eval(s, lists), lists, false));   // 每个 exe 只追加一次

    // 被排除的 exe（浏览器锁定指针）不追加。
    GameSample browser = s;
    browser.exe = L"chrome.exe";
    const GameContext b = eval(browser, lists);
    CHECK_TRUE(b.cursor_locked);
    CHECK_FALSE(should_record_known_game(b, lists, false));

    // 未锁定不追加；已在 force_game 中也不追加。
    CHECK_FALSE(should_record_known_game(eval(base_sample(L"other.exe"), lists), lists, false));
    DetectLists forced;
    forced.force_game.push_back(L"forced.exe");
    GameSample f = s;
    f.exe = L"forced.exe";
    CHECK_FALSE(should_record_known_game(eval(f, forced), forced, false));
}

TEST(game_rules_mark_and_exclude) {
    DetectLists lists = Config{}.detect;
    // 视为游戏：加入 force_game，并从 exclude 移除（大小写不敏感）。
    CHECK_TRUE(mark_as_game(lists, L"Chrome.exe"));
    CHECK_TRUE(list_contains(lists.force_game, L"chrome.exe"));
    CHECK_FALSE(list_contains(lists.exclude, L"chrome.exe"));
    CHECK_EQ(lists.exclude.size(), 21);
    CHECK_TRUE(mark_as_game(lists, L"game.exe"));
    CHECK_FALSE(mark_as_game(lists, L"GAME.exe"));   // 重复标记：名单不变
    CHECK_EQ(lists.force_game.size(), 2);

    // 排除一个被误判的程序：加入 exclude，并从 known_games 与 force_game 移除。
    lists.known_games.push_back(L"tool.exe");
    lists.force_game.push_back(L"Tool.exe");
    CHECK_TRUE(exclude_app(lists, L"tool.exe"));
    CHECK_TRUE(list_contains(lists.exclude, L"tool.exe"));
    CHECK_FALSE(list_contains(lists.known_games, L"tool.exe"));
    CHECK_FALSE(list_contains(lists.force_game, L"tool.exe"));
    CHECK_FALSE(exclude_app(lists, L"TOOL.EXE"));   // 重复排除：不变
    const GameContext c = eval(fullscreen_hidden(L"tool.exe"), lists);
    CHECK_TRUE(c.excluded);   // 立即不再触发
    CHECK_FALSE(c.in_game);

    // 视为游戏后再排除，再视为游戏：三个名单始终无重复。
    CHECK_TRUE(exclude_app(lists, L"game.exe"));
    CHECK_FALSE(list_contains(lists.force_game, L"game.exe"));
    CHECK_TRUE(mark_as_game(lists, L"game.exe"));
    CHECK_FALSE(list_contains(lists.exclude, L"game.exe"));
    CHECK_FALSE(mark_as_game(lists, L""));
    CHECK_FALSE(exclude_app(lists, L""));
}

TEST(game_rules_force_game_mode) {
    const DetectLists lists = Config{}.detect;
    const GameContext c = eval(base_sample(L"fakegame.exe"), lists, MouseLockRule::Strict, true);
    CHECK_TRUE(c.known_game);
    CHECK_TRUE(c.cursor_locked);
    CHECK_TRUE(c.in_game);
    CHECK_TRUE(compute_gates(c, true, true, true, true).mouse);   // 鼠标门控打开
    CHECK_FALSE(should_record_known_game(c, lists, true));         // 不写 known_games

    const GameContext explorer = eval(base_sample(L"explorer.exe"), lists, MouseLockRule::Strict, true);
    CHECK_TRUE(explorer.excluded);   // 排除名单仍然有效
    CHECK_FALSE(explorer.in_game);
    CHECK_TRUE(compute_gates(explorer, true, true, true, true) == Gates{});
}

TEST(game_rules_gates) {
    GameContext c;
    c.exe = L"game.exe";
    c.cursor_locked = true;
    c.known_game = true;
    c.fullscreen = true;
    c.cursor_hidden = true;
    c.in_game = true;
    CHECK_TRUE(compute_gates(c, true, true, true, true) == (Gates{true, true, true, false}));
    CHECK_TRUE(compute_gates(c, false, true, true, true) == Gates{});                          // enabled=0
    CHECK_TRUE(compute_gates(c, true, false, true, true) == (Gates{false, true, true, false}));   // mouse=0
    CHECK_TRUE(compute_gates(c, true, true, false, true) == (Gates{true, false, true, false}));   // keyboard=0
    CHECK_TRUE(compute_gates(c, true, true, true, false) == (Gates{true, true, false, false}));   // gamepad=0

    GameContext excluded = c;
    excluded.excluded = true;
    CHECK_TRUE(compute_gates(excluded, true, true, true, true) == Gates{});

    GameContext unlocked = c;
    unlocked.cursor_locked = false;   // 桌面上甩鼠标不触发
    CHECK_FALSE(compute_gates(unlocked, true, true, true, true).mouse);

    GameContext unknown = c;
    unknown.known_game = false;       // 非已知游戏不计键盘
    CHECK_FALSE(compute_gates(unknown, true, true, true, true).keys);

    GameContext windowed_visible = c;
    windowed_visible.fullscreen = false;
    windowed_visible.cursor_hidden = false;   // 手柄要求全屏或光标隐藏
    CHECK_FALSE(compute_gates(windowed_visible, true, true, true, true).pad);
    GameContext windowed_hidden = windowed_visible;
    windowed_hidden.cursor_hidden = true;
    CHECK_TRUE(compute_gates(windowed_hidden, true, true, true, true).pad);
    GameContext fullscreen_visible = windowed_visible;
    fullscreen_visible.fullscreen = true;
    CHECK_TRUE(compute_gates(fullscreen_visible, true, true, true, true).pad);
}

TEST(game_rules_capture_gate) {
    // motion-trigger「输入源门控」（变更 3）：gate_capture = enabled && capture && !excluded && known_game；
    // --no-capture / 采集失败由 app 再与 capture_on 相与（不在 compute_gates 里）。
    GameContext c;
    c.exe = L"game.exe";
    c.cursor_locked = true;
    c.known_game = true;
    c.fullscreen = true;
    c.cursor_hidden = true;
    c.in_game = true;
    CHECK_TRUE(compute_gates(c, true, true, true, true, true) == (Gates{true, true, true, true}));
    CHECK_FALSE(compute_gates(c, true, true, true, true, false).capture);   // [trigger] capture=0
    CHECK_FALSE(compute_gates(c, true, true, true, true).capture);          // 缺省参数 = 未开启
    CHECK_FALSE(compute_gates(c, false, true, true, true, true).capture);   // enabled=0
    CHECK_TRUE(compute_gates(c, true, false, false, false, true) == (Gates{false, false, false, true}));   // 与其他源无关

    GameContext unlocked = c;
    unlocked.cursor_locked = false;   // 已知游戏的菜单 / 过场：光标未锁定也照样算截屏
    CHECK_TRUE(compute_gates(unlocked, true, true, true, true, true).capture);

    GameContext excluded = c;
    excluded.excluded = true;
    CHECK_TRUE(compute_gates(excluded, true, true, true, true, true) == Gates{});

    // 非已知游戏不计截屏：全屏、光标隐藏但从未锁定过、也未被标记。
    const GameContext fs_unknown = eval(fullscreen_hidden(L"unknown.exe"));
    CHECK_TRUE(fs_unknown.in_game);
    CHECK_FALSE(fs_unknown.known_game);
    CHECK_FALSE(compute_gates(fs_unknown, true, true, true, true, true).capture);

    // 排除名单（vlc.exe 全屏播放）：不计截屏。
    const GameContext vlc = eval(fullscreen_hidden(L"vlc.exe"), Config{}.detect);
    CHECK_TRUE(compute_gates(vlc, true, true, true, true, true) == Gates{});

    // --force-game 模式：前台视为已知游戏 → 截屏门控打开；排除名单仍有效。
    const GameContext forced = eval(base_sample(L"fakegame.exe"), Config{}.detect, MouseLockRule::Strict, true);
    CHECK_TRUE(compute_gates(forced, true, true, true, true, true).capture);
    const GameContext forced_chrome = eval(base_sample(L"chrome.exe"), Config{}.detect, MouseLockRule::Strict, true);
    CHECK_FALSE(compute_gates(forced_chrome, true, true, true, true, true).capture);
}

TEST(game_rules_exe_name_and_tray_class) {
    CHECK_EQ(exe_name_from_path(L"C:\\Games\\CS2\\cs2.exe"), L"cs2.exe");
    CHECK_EQ(exe_name_from_path(L"D:\\X\\MyGame.EXE"), L"mygame.exe");
    CHECK_EQ(exe_name_from_path(L"plain.exe"), L"plain.exe");
    CHECK_TRUE(is_shell_tray_class(L"Shell_TrayWnd"));
    CHECK_TRUE(is_shell_tray_class(L"Shell_SecondaryTrayWnd"));
    CHECK_TRUE(is_shell_tray_class(L"NotifyIconOverflowWindow"));
    CHECK_TRUE(is_shell_tray_class(L"TopLevelWindowForOverflowXamlIsland"));
    CHECK_FALSE(is_shell_tray_class(L"Notepad"));
    CHECK_FALSE(is_shell_tray_class(L"MotionCalm.Host"));
}

TEST(game_rules_detector_smoke) {
    GameDetector d;
    d.set_rules(MouseLockRule::Strict, false);
    const DetectLists lists = Config{}.detect;
    CHECK_TRUE(d.poll(MouseLockHint{}, lists, true));    // 强制采样
    CHECK_FALSE(d.poll(MouseLockHint{}, lists));         // 16 ms 内再采样被节流
    CHECK_TRUE(d.poll(MouseLockHint{}, lists, true));
    const GameSample& s = d.sample();
    CHECK(s.virtual_screen.right > s.virtual_screen.left);
    CHECK(s.virtual_screen.bottom > s.virtual_screen.top);
    if (s.fg != nullptr) CHECK_FALSE(s.exe.empty());     // 有前台时一定有 exe（或类名）
    CHECK_EQ(d.ctx().exe, s.exe);
    d.reevaluate(lists);
    CHECK_EQ(d.ctx().exe, s.exe);
}
