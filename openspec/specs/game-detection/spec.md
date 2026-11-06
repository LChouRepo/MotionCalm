# game-detection Specification

## Purpose
判断前台程序是不是"正在玩的游戏"、鼠标是否被游戏锁住，用来决定 overlay 是否显示以及各输入源是否计入强度；同时保证浏览器、视频播放器等程序永远不会触发遮罩。

## Requirements

### Requirement: 前台程序识别

系统 SHALL 在每个 tick 采样前台窗口，但两次采样间隔 MUST ≥ 16 ms（即最高 60 Hz）。它取得前台窗口所属进程的 exe 文件名（小写，不含路径），并按进程 ID 缓存。拿不到 exe 名时 MUST 改用窗口类名（小写）。前台窗口为空，或属于 MotionCalm 自身进程时，SHALL 视为"没有游戏"（`in_game=false`，所有门控为 false）。

#### Scenario: 普通游戏

- **WHEN** 前台是 `C:\Games\CS2\cs2.exe` 的窗口
- **THEN** exe = "cs2.exe"

#### Scenario: 大小写归一

- **WHEN** 前台进程的文件名是 `MyGame.EXE`
- **THEN** exe = "mygame.exe"

#### Scenario: 拿不到进程名

- **WHEN** 无法查询前台进程的映像名
- **THEN** exe 取该窗口类名的小写形式，其余判定照常进行

#### Scenario: 本进程在前台

- **WHEN** 托盘菜单弹出，前台变成 MotionCalm 自己的窗口
- **THEN** in_game = false，overlay 不显示遮罩和参照点

### Requirement: 全屏判定

前台窗口矩形的四条边与其所在显示器矩形的对应边相差都 ≤ 1 px 时，`fullscreen` SHALL 为 true。

#### Scenario: 无边框全屏

- **GIVEN** 显示器矩形为 (0,0)–(1920,1080)
- **WHEN** 窗口矩形为 (0,0)–(1920,1080) 或 (0,0)–(1919,1080)
- **THEN** fullscreen = true

#### Scenario: 差 2 px

- **WHEN** 窗口矩形为 (0,0)–(1918,1080)
- **THEN** fullscreen = false

#### Scenario: 最大化的普通窗口

- **WHEN** 窗口最大化后为 (0,0)–(1920,1040)（任务栏可见）
- **THEN** fullscreen = false

#### Scenario: 副屏全屏

- **WHEN** 副屏矩形为 (1920,0)–(4480,1440)，窗口矩形与之相同
- **THEN** fullscreen = true

### Requirement: 光标状态

系统光标当前不在显示状态时，`cursor_hidden` SHALL 为 true。光标限制矩形严格小于虚拟屏幕矩形（任一边不同）时，`cursor_clipped` SHALL 为 true。

#### Scenario: 游戏隐藏光标

- **WHEN** 游戏把光标隐藏
- **THEN** cursor_hidden = true

#### Scenario: 游戏限制光标

- **WHEN** 游戏把光标限制在自己的窗口矩形内，而该矩形小于虚拟屏幕
- **THEN** cursor_clipped = true

#### Scenario: 未限制

- **WHEN** 没有程序限制光标，限制矩形等于虚拟屏幕
- **THEN** cursor_clipped = false

### Requirement: 光标锁定判定（strict，默认）

`mouse_lock_rule=strict` 时，系统 SHALL 按 `cursor_locked = cursor_hidden && (cursor_clipped || (raw_path_100ms > 8 && cursor_disp_100ms < 3))` 判定。`raw_path_100ms` 是最近 100 ms 内各原始相对位移事件的 √(dx²+dy²) 之和（单位：计数）；`cursor_disp_100ms` 是当前光标位置与约 100 ms 前那次采样位置之间的距离（单位：像素）。

#### Scenario: 隐藏并限制

- **WHEN** cursor_hidden 和 cursor_clipped 都为 true（鼠标可以不动）
- **THEN** cursor_locked = true

#### Scenario: 隐藏并每帧回中

- **GIVEN** 光标隐藏但未被限制，游戏每帧把光标放回中心
- **WHEN** 100 ms 内原始位移路径为 20 计数，光标位置只变了 1 px
- **THEN** cursor_locked = true

#### Scenario: 光标跟着鼠标走

- **GIVEN** 光标隐藏、未限制（例如视频播放器自动隐藏了光标）
- **WHEN** 100 ms 内原始位移 20 计数，光标位置移动了 15 px
- **THEN** cursor_locked = false

#### Scenario: 位移太小

- **GIVEN** 光标隐藏、未限制
- **WHEN** 100 ms 内原始位移只有 5 计数
- **THEN** cursor_locked = false

#### Scenario: 光标可见

- **WHEN** 光标可见但被限制在窗口内
- **THEN** cursor_locked = false

### Requirement: 光标锁定判定（hidden 模式）

`mouse_lock_rule=hidden` 时，系统 SHALL 直接取 `cursor_locked = cursor_hidden`。

#### Scenario: 只隐藏不限制

- **GIVEN** `mouse_lock_rule=hidden`
- **WHEN** 光标隐藏、未被限制，鼠标静止
- **THEN** cursor_locked = true

### Requirement: 排除名单

当 exe 出现在 `[detect] exclude` 中（不区分大小写），且不在 `[detect] force_game` 中时，`excluded` SHALL 为 true。被排除的程序 MUST NOT 被判为 `in_game`，所有门控 MUST 为 false。默认名单 MUST 恰好是以下 22 项：

```
chrome.exe msedge.exe firefox.exe brave.exe opera.exe opera_gx.exe vivaldi.exe arc.exe
vlc.exe PotPlayerMini64.exe PotPlayer64.exe PotPlayerMini.exe mpc-hc64.exe mpc-hc.exe
mpv.exe wmplayer.exe Video.UI.exe Netflix.exe Spotify.exe Discord.exe explorer.exe obs64.exe
```

#### Scenario: 全屏视频

- **WHEN** 前台为 chrome.exe，在全屏播放视频，光标已自动隐藏
- **THEN** excluded = true，in_game = false，不显示遮罩和参照点

#### Scenario: 大小写不敏感

- **WHEN** 前台 exe 为 "potplayermini64.exe"，名单中写的是 "PotPlayerMini64.exe"
- **THEN** excluded = true

#### Scenario: 被强制视为游戏

- **WHEN** chrome.exe 同时出现在 `exclude` 和 `force_game` 中
- **THEN** excluded = false

### Requirement: 已知游戏与持久化

当 exe 在 `force_game` 或 `known_games` 中时，`known_game` SHALL 为 true。某个未被排除的 exe 第一次达到 `cursor_locked` 时，系统 MUST 把它追加到 `[detect] known_games` 并立即保存 INI；每个 exe 只追加一次。被排除的 exe MUST NOT 被追加。

#### Scenario: 新游戏第一次锁定

- **GIVEN** newgame.exe 不在任何名单中
- **WHEN** 它第一次达到 cursor_locked
- **THEN** `known_games` 末尾追加 "newgame.exe"，INI 立即写回磁盘

#### Scenario: 重启后仍然记得

- **GIVEN** `known_games` 中已有 newgame.exe
- **WHEN** 重启 MotionCalm 后切到 newgame.exe 的主菜单（光标可见、窗口化）
- **THEN** known_game = true，in_game = true

#### Scenario: 浏览器锁定指针

- **WHEN** 浏览器里的网页游戏让 chrome.exe 达到 cursor_locked
- **THEN** chrome.exe 不会被追加到 `known_games`

### Requirement: 是否在游戏中

系统 SHALL 按 `in_game = !excluded && (known_game || (fullscreen && cursor_hidden))` 判定。overlay 的遮罩和参照点只在 `enabled && in_game` 时显示（见 overlay-rendering）。

#### Scenario: 未知的全屏游戏

- **WHEN** 一个未知的 exe 全屏并隐藏了光标
- **THEN** in_game = true

#### Scenario: 未知的窗口化程序

- **WHEN** 一个未知的 exe 以窗口化运行并隐藏了光标
- **THEN** in_game = false

#### Scenario: 已知游戏的菜单界面

- **WHEN** 已知游戏处于菜单界面（光标可见、窗口化）
- **THEN** in_game = true，overlay 显示参照点

### Requirement: 手动标记当前程序

"把当前程序视为游戏" SHALL 把当前程序的 exe 加入 `force_game`，同时从 `exclude` 中移除。"把当前程序加入排除名单" SHALL 把它加入 `exclude`，同时从 `force_game` 和 `known_games` 中移除。两者都 MUST 立即保存 INI，并从下一次采样起生效。"当前程序"是最近一个既不属于 MotionCalm、也不是任务栏 / 通知区窗口的前台程序。还没有这样的程序时，这两个操作不可用。

#### Scenario: 托盘里把游戏标记为游戏

- **GIVEN** 用户刚从 game.exe 切到托盘图标并打开菜单
- **WHEN** 点击"把当前程序视为游戏"
- **THEN** `force_game` 中加入 game.exe（不是 explorer.exe 或 MotionCalm.exe），INI 已保存

#### Scenario: 排除一个被误判的程序

- **GIVEN** tool.exe 在 `known_games` 中
- **WHEN** 用 `exclude_app` 热键排除它
- **THEN** `exclude` 中加入 tool.exe，`known_games` 和 `force_game` 中都没有它，它立即不再触发

#### Scenario: 重复标记

- **WHEN** 对已在 `force_game` 中的 exe 再次执行"视为游戏"
- **THEN** 名单中不出现重复项

### Requirement: 测试模式 --force-game

以 `--force-game` 启动时，系统 SHALL 把每次采样到的前台程序（不属于 MotionCalm 自身时）视为 `known_game = true` 且 `cursor_locked = true`。该模式 MUST NOT 向 `known_games` 写入任何内容，排除名单照常生效；启动时 MUST 记一条 warn 日志，注明处于测试模式。

#### Scenario: 集成验证

- **GIVEN** 以 `--force-game` 启动
- **WHEN** fakegame.exe 成为前台
- **THEN** 鼠标门控打开，in_game = true
- **AND** 退出后 INI 的 `known_games` 与启动前相同

#### Scenario: 排除名单仍然有效

- **GIVEN** 以 `--force-game` 启动
- **WHEN** explorer.exe 在前台
- **THEN** in_game = false
