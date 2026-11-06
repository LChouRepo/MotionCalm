# Spec Delta

## MODIFIED Requirements

### Requirement: 截屏与录屏可见性

overlay SHALL 按 `hide_from_capture` 决定是否出现在截屏、录屏和屏幕共享中：`always` 一律排除；`never` 一律包含；`auto` 在截屏运动检测处于开启状态（`[trigger] capture=1`、未带 `--no-capture`、未处于"采集失败"）时排除，否则包含。系统不支持排除时（Win10 2004 之前），MUST 记一条 warn 日志并保持可见，截屏检测照常运行。配置重载或开启状态变化后 MUST 在同一次 tick 内按新值生效；从关到开时 MUST 先设好排除，再让截屏线程用帧做估计。

#### Scenario: always

- **GIVEN** `hide_from_capture=always`，overlay 正以 i = 1 显示
- **WHEN** 用 PrintScreen、OBS 显示器采集或 .NET `CopyFromScreen` 截屏
- **THEN** 截到的画面里没有遮罩和参照点，但屏幕上用户仍然看得见

#### Scenario: auto 且截屏检测开启（默认配置）

- **GIVEN** 默认 INI（`capture=1`、`hide_from_capture=auto`），在已知游戏中 overlay 正在显示遮罩
- **WHEN** 用 PrintScreen 或 OBS 显示器采集截取该显示器
- **THEN** 截到的画面中没有遮罩与参照点，屏幕上肉眼仍可见

#### Scenario: auto 且截屏检测未启用

- **GIVEN** `hide_from_capture=auto`，用 `--no-capture` 启动
- **WHEN** verify.ps1 用 `CopyFromScreen` 截屏
- **THEN** 截图中包含遮罩和参照点

#### Scenario: auto 且采集失败

- **GIVEN** `hide_from_capture=auto`，截屏检测进入"采集失败"状态
- **WHEN** 下一次 tick
- **THEN** overlay 不再对截屏隐藏

#### Scenario: auto 且从托盘关闭截屏检测

- **GIVEN** `hide_from_capture=auto`，截屏检测开启
- **WHEN** 用户在托盘取消勾选"截屏运动检测"
- **THEN** 同一次 tick 内 overlay 不再对截屏隐藏

#### Scenario: never 与截屏检测同时开启

- **GIVEN** `hide_from_capture=never`、`capture=1`
- **WHEN** 截屏检测开始取帧
- **THEN** overlay 对截屏可见，日志写一条 warn，说明估计可能受遮罩影响

#### Scenario: 系统不支持排除

- **GIVEN** 系统版本早于 Win10 2004，`hide_from_capture=auto`、`capture=1`
- **WHEN** 截屏检测开启
- **THEN** 日志出现一条 warn，overlay 保持可见，截屏检测照常取帧，程序不退出

#### Scenario: 重载后切换

- **WHEN** 把 INI 改成 `never` 并重载
- **THEN** 下一次截屏就包含 overlay

### Requirement: 调试条

调试信息开启时，overlay SHALL 在底部居中显示两条水平进度条（上为 target，下为平滑后的 value，范围 0..1），以及一行文字：`鼠标 {鼠标°/s 取整}°/s · {移动|静止} · 手柄 {手柄°/s 取整} · 截屏 {截屏字段} · {exe} {锁定|未锁定}`。截屏字段：正在取帧时为 `{截屏°/s 取整}°/s c{最近一次置信度，两位小数}`；暂停取帧时为 `暂停`；截屏检测未开启时为 `关`；采集失败时为 `失败`。当前程序被排除时文字末尾追加 ` · 已排除`，不在游戏时追加 ` · 非游戏`。不满足显示条件时，调试条 MUST 以仅文字模式显示。调试开关 MUST NOT 写入 INI，每次启动默认关闭。

#### Scenario: 转动时显示数值

- **GIVEN** 调试信息已开启，以 `--no-capture` 启动，前台为 cs2.exe 且已锁定
- **WHEN** 鼠标以约 312°/s 转动，同时按住 W
- **THEN** 底部文字为"鼠标 312°/s · 移动 · 手柄 0 · 截屏 关 · cs2.exe 锁定"，两条进度条按 target 和 value 实时变化

#### Scenario: 截屏正在取帧

- **GIVEN** 调试信息已开启，截屏检测开启，前台为已知游戏 cs2.exe
- **WHEN** 鼠标静止，画面以约 337°/s 平移，最近一次置信度为 0.93
- **THEN** 文字为"鼠标 0°/s · 静止 · 手柄 0 · 截屏 337°/s c0.93 · cs2.exe 锁定"

#### Scenario: 截屏采集失败

- **GIVEN** 调试信息已开启，截屏检测处于"采集失败"状态
- **WHEN** 在已知游戏中查看调试条
- **THEN** 截屏字段显示为"截屏 失败"

#### Scenario: 用于诊断非游戏程序

- **GIVEN** 调试信息已开启，截屏检测开启
- **WHEN** 前台是 chrome.exe
- **THEN** 显示仅文字模式的调试条，截屏字段为"截屏 暂停"，文字以"chrome.exe 未锁定 · 已排除"结尾
