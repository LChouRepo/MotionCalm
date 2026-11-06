# Spec Delta

## Purpose

在游戏画面之上叠加一层不可交互的全屏 overlay：按强度把屏幕边缘平滑压暗，常驻固定参照点，并显示简短的文字提示；同时不得干扰游戏的输入、焦点与性能。

## ADDED Requirements

### Requirement: 点击穿透且永不激活

overlay 窗口 MUST 对鼠标完全透明：点击、滚轮、悬停全部落到下方窗口。它 MUST 永远不获得键盘焦点、不成为前台窗口，并且 MUST NOT 出现在任务栏和 Alt-Tab 列表中。

#### Scenario: 点击穿透到游戏

- **GIVEN** overlay 正以强度 1.0 覆盖在游戏窗口上
- **WHEN** 用户在屏幕任意位置点击、拖动或滚动鼠标
- **THEN** 这些事件全部送达下方的游戏窗口
- **AND** 前台窗口仍然是游戏窗口

#### Scenario: 不出现在任务栏与 Alt-Tab

- **GIVEN** overlay 正在显示
- **WHEN** 用户按住 Alt+Tab，或查看任务栏
- **THEN** 两处都看不到 MotionCalm 的 overlay 窗口

#### Scenario: 显示与重新置顶都不抢焦点

- **GIVEN** 游戏是前台窗口
- **WHEN** overlay 从隐藏变为显示，或执行重新置顶
- **THEN** 前台窗口和键盘焦点仍属于游戏

### Requirement: 显示条件

只有当 `enabled=1` 且前台被判定为 `in_game`（见 game-detection）时，overlay 才 SHALL 绘制遮罩和参照点；否则 MUST NOT 绘制。不画遮罩也不画参照点时，overlay 窗口 SHALL 隐藏。唯一的例外是"仅文字"模式（见 toast 与调试条两条需求）。

#### Scenario: 进入游戏后显示参照点

- **GIVEN** `enabled=1`，用户没有任何输入
- **WHEN** 前台切换到一个 `in_game` 为真的程序
- **THEN** 下一个 tick 内（≤ 50 ms），中心点和强度为 0 时的边缘点出现在该程序所在显示器上

#### Scenario: 切到非游戏程序后隐藏

- **GIVEN** overlay 正在游戏上显示
- **WHEN** 前台切换到 `explorer.exe` 或浏览器
- **THEN** 50 ms 内 overlay 不再显示遮罩和参照点，窗口隐藏

#### Scenario: 关闭遮罩

- **WHEN** `enabled` 变为 0
- **THEN** 遮罩和参照点立即消失；此后只会出现 toast 或调试条（仅文字模式）

### Requirement: 只覆盖游戏所在显示器并跟随换屏

overlay SHALL 精确覆盖前台窗口所在显示器的整个矩形（物理像素，按"最近显示器"规则判定）。前台窗口移到另一台显示器后，overlay MUST 在 1 s 内贴合新显示器，其他显示器 MUST NOT 被覆盖。

#### Scenario: 双显示器只盖一块

- **GIVEN** 主屏 1920×1080 位于 (0,0)，副屏 2560×1440 位于 (1920,0)，游戏全屏在副屏上
- **WHEN** overlay 显示
- **THEN** overlay 的矩形正好是 (1920,0)–(4480,1440)
- **AND** 主屏上看不到任何遮罩或参照点

#### Scenario: 游戏换屏

- **WHEN** 用户把游戏窗口从主屏移到副屏（例如 Win+Shift+→）
- **THEN** 1 s 内 overlay 移到副屏，中心点位于副屏中心 (3200,720)

#### Scenario: 分辨率变化

- **WHEN** 游戏所在显示器的分辨率从 1920×1080 改为 2560×1440
- **THEN** 1 s 内 overlay 尺寸变为 2560×1440，遮罩几何按新尺寸重新计算

### Requirement: 按显示器 DPI 缩放像素尺寸

进程 SHALL 以 Per-Monitor V2 方式感知 DPI。所有以 px 配置的尺寸（点半径、描边宽度、边缘点内缩距离、文字字号）MUST 乘以 overlay 所在显示器的 DPI / 96；遮罩几何只按屏幕比例计算，不受 DPI 影响。

#### Scenario: 150% 缩放的显示器

- **GIVEN** 显示器有效 DPI 为 144，配置为默认值（`center_dot_radius_px=3`、`edge_dot_margin_px=24`）
- **WHEN** overlay 在该显示器上显示
- **THEN** 中心点半径为 4.5 px，描边宽 1.5 px，边缘点中心离屏幕边缘 36 px

#### Scenario: 跨不同 DPI 的显示器移动

- **WHEN** 游戏从 96 DPI 的显示器移到 144 DPI 的显示器
- **THEN** overlay 仍精确覆盖新显示器的物理像素矩形，不采用系统建议的缩放后矩形；点的尺寸按 144 DPI 重新计算

### Requirement: 置顶维持

overlay SHALL 保持在置顶层的最上方：前台窗口每次变化时 MUST 立即重新置顶；此外每 1 s 兜底重新置顶一次。重新置顶 MUST NOT 移动 overlay、改变它的尺寸或激活它。

#### Scenario: 前台变化后立即重新置顶

- **GIVEN** 另一个置顶窗口刚盖过 overlay
- **WHEN** 用户点回游戏，使前台发生变化
- **THEN** overlay 在这次前台变化的回调里就回到置顶层最上方

#### Scenario: 1 s 兜底

- **GIVEN** 前台始终是游戏，没有变化
- **WHEN** 某个置顶窗口自己把自己提到 overlay 之上
- **THEN** 不超过 1 s，overlay 重新回到最上方

### Requirement: 遮罩几何与不透明度

遮罩 SHALL 是以 overlay 中心为圆心、横向更宽的椭圆羽化暗角，颜色取 `[mask] color`（默认 000000）。每个像素的 alpha MUST 按下列公式计算，并以预乘 alpha 输出（rgb = color × a）。其中 i 为平滑后的强度（0..1），W、H 为 overlay 的像素宽高，p 为像素中心相对 (W/2, H/2) 的偏移，level 为当前档位。

```
ry   = clear_height[level] * H / 2
rx   = ry * ellipse_ratio                 // 默认 1.25
d    = length(p / (rx, ry))               // d = 1 即满强度时的清晰区边界
R    = length((W/2, H/2) / (rx, ry))      // 屏幕角的归一化距离
r_in = lerp(R + feather, 1.0, i)          // feather 默认 0.35
a    = max_opacity[level] * i * smoothstep(r_in, r_in + feather, d)
smoothstep(e0, e1, x) = t*t*(3 - 2*t),  t = clamp((x - e0) / (e1 - e0), 0, 1)
```

#### Scenario: 强度为 0 时完全透明

- **WHEN** i = 0（任意档位）
- **THEN** 所有像素的遮罩 alpha 为 0

#### Scenario: 强档满强度（1920×1080，默认参数）

- **GIVEN** 档位为强（max_opacity 0.85，clear_height 0.55），因此 ry = 297 px、rx = 371.25 px
- **WHEN** i = 1
- **THEN** 屏幕中心像素的 alpha = 0
- **AND** 中心正上方 297 px 处（d = 1.0）alpha = 0
- **AND** 中心正上方 ≥ 401 px 处（d ≥ 1.35）、顶边中点和四个角的 alpha = 0.85（误差 ±0.005）

#### Scenario: 中档与轻档满强度

- **GIVEN** 1920×1080，i = 1
- **WHEN** 档位为中（0.75 / 0.60）或轻（0.60 / 0.65）
- **THEN** 顶边中点的 alpha 分别为 0.75 和 0.60
- **AND** 中心正上方 324 px（中档）或 351 px（轻档）以内的 alpha 为 0

#### Scenario: 半强度时只压暗角落

- **GIVEN** 1920×1080，强档，i = 0.5（R ≈ 3.161，r_in ≈ 2.256）
- **WHEN** 计算遮罩
- **THEN** 四个角的 alpha = 0.425（误差 ±0.005）
- **AND** 顶边中点（d ≈ 1.818 < r_in）的 alpha = 0

#### Scenario: 遮罩颜色

- **GIVEN** `[mask] color=200000`
- **WHEN** 某像素 a = 0.5
- **THEN** 输出的预乘颜色为 (0x20/255 × 0.5, 0, 0)，alpha 为 0.5

### Requirement: 中心参照点

overlay 显示且 `center_dot=1` 时，SHALL 在 overlay 正中心画一个半径为 `center_dot_radius_px` 的实心圆，颜色 `center_dot_color`，alpha 0.9。`center_dot_outline=1` 时，紧贴圆外侧再画一圈 1 px、alpha 0.6 的黑色描边。中心点 MUST 盖在遮罩之上，大小和颜色不随强度变化，位置永不移动；边缘做约 1 px 抗锯齿。

#### Scenario: 静止时也在

- **GIVEN** overlay 在 1920×1080 的游戏上显示，i = 0
- **THEN** (960,540) 处可见半径 3 px 的白点，外围有一圈深色描边

#### Scenario: 满强度时不被遮罩盖住

- **WHEN** i = 1，任意档位
- **THEN** 中心点的颜色、alpha 和大小与 i = 0 时相同

#### Scenario: 关闭中心点和描边

- **WHEN** `center_dot=0`
- **THEN** 不画中心点
- **AND** `center_dot=1, center_dot_outline=0` 时只画白点，不画描边

### Requirement: 边缘参照点

overlay 显示且 `edge_dots=1` 时，SHALL 画 8 个点：四个角和四条边的中点，各自向内缩 `edge_dot_margin_px`。点的半径为 `edge_dot_radius_px`，颜色 `edge_dot_color`，alpha = lerp(`edge_dot_min_alpha`, `edge_dot_max_alpha`, i)。边缘点 MUST 盖在遮罩之上，位置固定，永不移动。

#### Scenario: 1920×1080、96 DPI 下的位置

- **WHEN** 使用默认配置
- **THEN** 8 个点的中心依次为 (24,24)、(960,24)、(1896,24)、(24,540)、(1896,540)、(24,1056)、(960,1056)、(1896,1056)，半径 3 px，颜色 #C8C8C8

#### Scenario: 透明度随强度变化

- **WHEN** i 分别为 0、0.5、1
- **THEN** 边缘点的 alpha 分别为 0.15、0.45、0.75

#### Scenario: 关闭边缘点

- **WHEN** `edge_dots=0`
- **THEN** 不画任何边缘点，中心点不受影响

### Requirement: toast 提示

toast SHALL 显示在 overlay 顶部居中：黑色圆角底，白色文字，字体 "Microsoft YaHei UI"。每条 toast 总共显示 1.5 s，前 1.2 s 不透明，最后 0.3 s 线性淡出。新 toast MUST 立即替换正在显示的旧 toast 并重新计时。即使当前不满足"显示条件"，toast 也 MUST 可见：此时 overlay 以仅文字模式出现在前台窗口所在的显示器上，不画遮罩和参照点。

#### Scenario: 热键触发 toast

- **WHEN** 用户在游戏里按下 `cycle_level` 热键，档位切到强
- **THEN** 下一帧顶部居中出现"遮罩：强"
- **AND** 1.2 s 后开始淡出，1.5 s 时完全消失

#### Scenario: 新 toast 替换旧 toast

- **GIVEN** "遮罩：中"已显示 1.0 s
- **WHEN** 再次触发，显示"遮罩：强"
- **THEN** 屏幕上只有"遮罩：强"，并且从这一刻起重新完整显示 1.5 s

#### Scenario: 不在游戏时仍可见

- **GIVEN** 前台是桌面，overlay 处于隐藏状态
- **WHEN** 产生一条 toast（例如 `toggle` 热键关闭遮罩时的"已关闭遮罩"）
- **THEN** toast 出现在桌面所在显示器的顶部居中，没有遮罩和参照点
- **AND** toast 消失后 overlay 窗口重新隐藏

### Requirement: 调试条

调试信息开启时，overlay SHALL 在底部居中显示两条水平进度条（上为 target，下为平滑后的 value，范围 0..1），以及一行文字：`鼠标 {鼠标°/s 取整}°/s · {移动|静止} · 手柄 {手柄°/s 取整} · 截屏 0 · {exe} {锁定|未锁定}`。当前程序被排除时文字末尾追加 ` · 已排除`，不在游戏时追加 ` · 非游戏`。不满足显示条件时，调试条 MUST 以仅文字模式显示。调试开关 MUST NOT 写入 INI，每次启动默认关闭。

#### Scenario: 转动时显示数值

- **GIVEN** 调试信息已开启，前台为 cs2.exe 且已锁定
- **WHEN** 鼠标以约 312°/s 转动，同时按住 W
- **THEN** 底部文字为"鼠标 312°/s · 移动 · 手柄 0 · 截屏 0 · cs2.exe 锁定"，两条进度条按 target 和 value 实时变化

#### Scenario: 用于诊断非游戏程序

- **GIVEN** 调试信息已开启
- **WHEN** 前台是 chrome.exe
- **THEN** 显示仅文字模式的调试条，文字以"chrome.exe 未锁定 · 已排除"结尾

### Requirement: 截屏与录屏可见性

overlay SHALL 按 `hide_from_capture` 决定是否出现在截屏、录屏和屏幕共享中：`always` 一律排除；`never` 一律包含；`auto` 在截屏运动检测启用时排除，否则包含（本变更中截屏检测恒为未启用，所以 auto 等于包含）。系统不支持排除时（Win10 2004 之前），MUST 记一条 warn 日志并保持可见。配置重载后 MUST 立即按新值生效。

#### Scenario: always

- **GIVEN** `hide_from_capture=always`，overlay 正以 i = 1 显示
- **WHEN** 用 PrintScreen、OBS 显示器采集或 .NET `CopyFromScreen` 截屏
- **THEN** 截到的画面里没有遮罩和参照点，但屏幕上用户仍然看得见

#### Scenario: auto 且截屏检测未启用

- **GIVEN** `hide_from_capture=auto`，用 `--no-capture` 启动（或截屏检测尚未实现）
- **WHEN** verify.ps1 用 `CopyFromScreen` 截屏
- **THEN** 截图中包含遮罩和参照点

#### Scenario: 重载后切换

- **WHEN** 把 INI 改成 `never` 并重载
- **THEN** 下一次截屏就包含 overlay

### Requirement: 按需 Present

只有当帧内容与上一次 Present 不同时（强度、档位、遮罩 / 点参数、点开关、文字内容或透明度、尺寸、DPI 变化），overlay 才 SHALL Present 新帧。强度回到 0 且没有文字时，MUST 再 Present 一帧"强度 0 的画面"（只含开启的参照点；点全关时全透明），然后停止 Present，直到内容再次变化。

#### Scenario: 静止不 Present

- **GIVEN** 在游戏里，i = 0，没有文字
- **WHEN** 10 s 内没有任何输入
- **THEN** 这 10 s 内 Present 次数为 0（进入该状态时那一帧除外）

#### Scenario: 淡出后的最后一帧

- **GIVEN** 刚完成一次转动，强度正在下降
- **WHEN** 强度到达 0
- **THEN** 屏幕上只剩中心点和 alpha 0.15 的边缘点，没有任何遮罩残影
- **AND** 之后停止 Present

### Requirement: 渲染节拍与资源占用

overlay 可见，并且强度 > 0、toast 正在显示或调试条开启时，进程 SHALL 按显示器刷新率节拍运行；其余时间 MUST 每 30 ms 轮询一次且不 Present。任何状态下都 MUST NOT 忙等。目标：活动时 CPU < 2%（单核占比），空闲时 < 0.2%，1080p 下 GPU 每帧 < 0.2 ms。

#### Scenario: 集成验证中的 CPU

- **WHEN** verify.ps1 跑完 20 s 的完整流程（含 3 s 持续转动）
- **THEN** MotionCalm 进程的 CPU 时间（内核 + 用户）小于 0.4 s（20 s 的 2%）

#### Scenario: 桌面空闲

- **GIVEN** 前台是非游戏程序
- **WHEN** 静置 60 s
- **THEN** MotionCalm 的 CPU 时间增量小于 0.12 s

#### Scenario: GPU 耗时

- **GIVEN** 以 `--verbose` 运行，在 1920×1080 上 i = 1
- **WHEN** 查看日志中每 5 s 一条的 GPU 耗时统计
- **THEN** 平均每帧 GPU 时间 < 0.2 ms

### Requirement: 渲染设备丢失恢复

当显卡设备被移除或重置（驱动更新、TDR）时，overlay SHALL 重建全部渲染资源并在 2 s 内恢复显示，MUST NOT 崩溃，并记一条 warn 日志。重建失败时每 2 s 重试一次。

#### Scenario: 驱动重置

- **GIVEN** overlay 正在显示
- **WHEN** 显卡驱动重置，导致 Present 返回设备移除错误
- **THEN** 进程继续运行，2 s 内 overlay 恢复正常显示，日志中有一条说明设备丢失的 warn
