# Spec Delta

## MODIFIED Requirements

### Requirement: 托盘菜单结构

托盘菜单 SHALL 依次包含下列条目（`----` 表示分隔线）。勾选和单选状态 MUST 反映当前配置。热键列显示该动作当前绑定的规范字符串；绑定为 None 时 MUST 不显示。

```
MotionCalm v1.0                              （灰色，不可点）
<状态行>                                      （灰色，不可点）
----
[√] 启用遮罩                       <toggle>
    遮罩强度 ▸ (•)轻 / (•)中 / (•)强  <cycle_level> 循环
    触发灵敏度 ▸ (•)低 / (•)中 / (•)高
[√] 中心参照点                     <toggle_center_dot>
[√] 边缘参照点                     <toggle_edge_dots>
[√] 截屏运动检测                   <toggle_capture>
----
    把当前程序视为游戏              <mark_game>
    把当前程序加入排除名单          <exclude_app>
    校准当前游戏…                   <calibrate>
    校准列表 ▸ 删除 <exe>（每度 <值> 计数） …
    打开配置文件
    重新加载配置
[ ] 显示调试信息                   <debug_bar>
----
[ ] 开机自动启动
    打开日志
    关于 MotionCalm
    退出                           <exit>
```

#### Scenario: 默认配置下的菜单

- **GIVEN** 默认 INI
- **WHEN** 打开托盘菜单
- **THEN** "启用遮罩"已勾选，旁边显示 "Ctrl+Alt+F9"；遮罩强度子菜单选中"中"，旁边显示 "Ctrl+Alt+F10 循环"；中心参照点、边缘参照点和"截屏运动检测"已勾选，"截屏运动检测"旁显示 "Ctrl+Alt+F6"；"显示调试信息"未勾选，显示 "Ctrl+Alt+F8"；"开机自动启动"未勾选
- **AND** "校准当前游戏…"旁显示 "Ctrl+Alt+F7"；"校准列表"子菜单中只有一个灰色的"（无）"
- **AND** 菜单中没有"设置…"（属于后续变更）

#### Scenario: 自定义热键反映到菜单

- **GIVEN** INI 中 `toggle=Shift+F2`
- **WHEN** 重新加载配置后打开菜单
- **THEN** "启用遮罩"旁显示 "Shift+F2"

#### Scenario: 没有当前程序

- **WHEN** 自启动以来还没有出现过符合条件的前台程序
- **THEN** "把当前程序视为游戏"和"把当前程序加入排除名单"呈灰色不可点

### Requirement: 热键动作表

热键系统 SHALL 由一张静态动作表驱动，每项包含 id、INI 键、中文名、默认组合；INI 的 `[hotkeys]` 节按此表的顺序读写。动作表 MUST 正好包含以下 11 项：

```
INI 键              中文名                    默认
toggle              启用 / 关闭遮罩            Ctrl+Alt+F9
cycle_level         循环遮罩强度              Ctrl+Alt+F10
toggle_center_dot   中心参照点开关            Ctrl+Alt+F11
debug_bar           显示调试信息              Ctrl+Alt+F8
calibrate           校准当前游戏              Ctrl+Alt+F7
toggle_capture      截屏运动检测开关          Ctrl+Alt+F6
cycle_sensitivity   循环触发灵敏度            None
toggle_edge_dots    边缘参照点开关            None
mark_game           把当前程序视为游戏        None
exclude_app         把当前程序加入排除名单    None
exit                退出                      None
```

#### Scenario: 默认绑定

- **GIVEN** 默认 INI
- **WHEN** 程序启动
- **THEN** 只注册 6 个全局热键：Ctrl+Alt+F9、Ctrl+Alt+F10、Ctrl+Alt+F11、Ctrl+Alt+F8、Ctrl+Alt+F7、Ctrl+Alt+F6

#### Scenario: 用户绑定原本为 None 的动作

- **GIVEN** INI 中 `exit=Ctrl+Alt+End`
- **WHEN** 程序启动后按下 Ctrl+Alt+End
- **THEN** 程序退出

#### Scenario: 旧 INI 没有 calibrate 键

- **GIVEN** 由 core-overlay 版本生成的 INI，`[hotkeys]` 中没有 calibrate
- **WHEN** 新版本启动
- **THEN** calibrate 按默认值 Ctrl+Alt+F7 注册；下次保存时，`[hotkeys]` 中会在 debug_bar 之后出现 `calibrate=Ctrl+Alt+F7`

#### Scenario: 旧 INI 没有 toggle_capture 键

- **GIVEN** 由更早版本生成的 INI，`[hotkeys]` 中没有 toggle_capture
- **WHEN** 新版本启动
- **THEN** toggle_capture 按默认值 Ctrl+Alt+F6 注册；下次保存时，`[hotkeys]` 中会在 calibrate 之后出现 `toggle_capture=Ctrl+Alt+F6`

### Requirement: 热键触发提示

每次热键触发 SHALL 立即执行动作，除 `calibrate` 外都弹出一条 toast：
- `toggle`："已启用遮罩" / "已关闭遮罩"
- `cycle_level`：按 轻 → 中 → 强 → 轻 循环，toast 为 "遮罩：轻|中|强"
- `toggle_center_dot`："中心参照点：开|关"
- `toggle_edge_dots`："边缘参照点：开|关"
- `toggle_capture`："截屏运动检测：开|关"；以 `--no-capture` 启动时不改值，toast 为 "截屏运动检测已被 --no-capture 禁用"
- `debug_bar`："调试信息：开|关"
- `calibrate`：推进校准，提示与 toast 由 turn-calibration 决定
- `cycle_sensitivity`：按 低 → 中 → 高 → 低 循环，toast 为 "灵敏度：低|中|高"
- `mark_game`："已视为游戏：{exe}"
- `exclude_app`："已排除：{exe}"
- `exit`：不弹 toast，直接退出

除 `debug_bar`、`calibrate` 外，改动 MUST 立即保存到 INI（`calibrate` 只在结算成功时保存）。

#### Scenario: 关闭遮罩

- **GIVEN** 在游戏中，遮罩已启用
- **WHEN** 按 Ctrl+Alt+F9
- **THEN** 遮罩和参照点消失，顶部出现 toast "已关闭遮罩"（仅文字模式），INI 中 `enabled=0`

#### Scenario: 循环档位

- **GIVEN** 当前为强
- **WHEN** 按 Ctrl+Alt+F10
- **THEN** 档位变为轻，toast 为 "遮罩：轻"

#### Scenario: 没有当前程序时标记

- **WHEN** 还没有"当前程序"时触发 `mark_game`
- **THEN** 不修改任何名单，toast 为 "没有可标记的程序"

#### Scenario: 开始校准不弹通用 toast

- **GIVEN** 在游戏中，光标已锁定
- **WHEN** 按 Ctrl+Alt+F7
- **THEN** 屏幕中上方出现"校准 1/2：…"提示，没有通用 toast

#### Scenario: 切换截屏检测

- **GIVEN** `capture=1`
- **WHEN** 按 Ctrl+Alt+F6
- **THEN** toast 为 "截屏运动检测：关"，INI 中立即写入 `capture=0`

## ADDED Requirements

### Requirement: 截屏运动检测的切换
托盘项"截屏运动检测"与热键 `toggle_capture` SHALL 执行同一动作：翻转 `[trigger] capture`、立即保存 INI、按截屏检测开启状态的规则启动或停止截屏，并弹出与热键相同的 toast。截屏检测处于"采集失败"时，菜单项文字 SHALL 为"截屏运动检测（采集失败）"，仍按 INI 显示勾选。以 `--no-capture` 启动时，菜单项文字为"截屏运动检测（已由 --no-capture 禁用）"并灰显。

#### Scenario: 从托盘关闭
- **GIVEN** `capture=1`，菜单项显示为勾选
- **WHEN** 用户点击"截屏运动检测"
- **THEN** INI 中写入 `capture=0`，截屏线程在 100 ms 内停止取帧，弹出 toast "截屏运动检测：关"

#### Scenario: 热键开启
- **GIVEN** `capture=0`，用户在已知游戏中
- **WHEN** 按下 `Ctrl+Alt+F6`
- **THEN** INI 中写入 `capture=1`，截屏开始取帧，toast 为 "截屏运动检测：开"

#### Scenario: 采集失败后再切换
- **GIVEN** `capture=1` 但截屏检测处于"采集失败"
- **WHEN** 用户点击该菜单项两次
- **THEN** 第一次写入 `capture=0`；第二次写入 `capture=1` 并重新尝试采集，失败计数从 0 开始

#### Scenario: 命令行禁用
- **GIVEN** 以 `--no-capture` 启动
- **WHEN** 打开托盘菜单，并按下 `Ctrl+Alt+F6`
- **THEN** 菜单项灰显且文字为"截屏运动检测（已由 --no-capture 禁用）"；按热键后 INI 不变，toast 为 "截屏运动检测已被 --no-capture 禁用"
