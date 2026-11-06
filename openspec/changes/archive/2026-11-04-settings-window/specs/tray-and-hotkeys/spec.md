# Spec Delta

## MODIFIED Requirements

### Requirement: 托盘图标

程序运行期间 SHALL 在通知区显示 MotionCalm 图标，鼠标悬停提示为 "MotionCalm"。右键单击 SHALL 立即打开托盘菜单；左键单击 SHALL 在系统双击间隔（`GetDoubleClickTime`）内没有第二次点击时打开托盘菜单；左键双击 SHALL 打开（或激活）设置窗口，且不弹出菜单。资源管理器重启后，图标 MUST 自动重新出现；程序退出时 MUST 移除图标。

#### Scenario: 右键打开菜单

- **WHEN** 用户右键单击托盘图标
- **THEN** 在光标附近弹出托盘菜单；点击菜单以外的地方菜单关闭

#### Scenario: 左键单击打开菜单

- **WHEN** 用户左键单击托盘图标一次，之后不再点击
- **THEN** 经过系统双击间隔（默认 500 ms）后弹出托盘菜单

#### Scenario: 双击打开设置

- **WHEN** 用户左键双击托盘图标
- **THEN** 设置窗口打开并成为前台，托盘菜单不弹出

#### Scenario: 资源管理器重启

- **WHEN** explorer.exe 崩溃后重启
- **THEN** 几秒内托盘图标重新出现，并且仍能正常工作

#### Scenario: 退出后不留残影

- **WHEN** 通过菜单"退出"或 `--exit-after` 退出
- **THEN** 托盘图标立即消失，不残留"幽灵图标"

### Requirement: 托盘菜单结构

托盘菜单 SHALL 依次包含下列条目（`----` 表示分隔线）。"设置…" MUST 是菜单默认项（加粗显示）。勾选和单选状态 MUST 反映当前配置。热键列显示该动作当前绑定的规范字符串；绑定为 None 时 MUST 不显示。

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
    设置…                          <open_settings>  （默认项，加粗）
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
- **AND** "设置…"以粗体显示，旁边没有热键文字（`open_settings` 默认为 None），"打开配置文件"仍在它下方

#### Scenario: 自定义热键反映到菜单

- **GIVEN** INI 中 `toggle=Shift+F2`
- **WHEN** 重新加载配置后打开菜单
- **THEN** "启用遮罩"旁显示 "Shift+F2"

#### Scenario: 没有当前程序

- **WHEN** 自启动以来还没有出现过符合条件的前台程序
- **THEN** "把当前程序视为游戏"和"把当前程序加入排除名单"呈灰色不可点

### Requirement: 热键动作表

热键系统 SHALL 由一张静态动作表驱动，每项包含 id、INI 键、中文名、默认组合；INI 的 `[hotkeys]` 节按此表的顺序读写，设置窗口的快捷键页也按此表的顺序和中文名显示。动作表 MUST 正好包含以下 12 项：

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
open_settings       打开设置                  None
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

#### Scenario: 旧 INI 没有 open_settings 键

- **GIVEN** 由更早版本生成的 INI，`[hotkeys]` 中没有 open_settings
- **WHEN** 新版本启动后因任一改动保存
- **THEN** 不为 open_settings 注册热键；`[hotkeys]` 中在 toggle_edge_dots 之后出现 `open_settings=None`

### Requirement: 热键触发提示

每次热键触发 SHALL 立即执行动作，除 `calibrate` 与 `open_settings` 外都弹出一条 toast：
- `toggle`："已启用遮罩" / "已关闭遮罩"
- `cycle_level`：按 轻 → 中 → 强 → 轻 循环，toast 为 "遮罩：轻|中|强"
- `toggle_center_dot`："中心参照点：开|关"
- `toggle_edge_dots`："边缘参照点：开|关"
- `toggle_capture`："截屏运动检测：开|关"；以 `--no-capture` 启动时不改值，toast 为 "截屏运动检测已被 --no-capture 禁用"
- `debug_bar`："调试信息：开|关"
- `calibrate`：推进校准，提示与 toast 由 turn-calibration 决定
- `cycle_sensitivity`：按 低 → 中 → 高 → 低 循环，toast 为 "灵敏度：低|中|高"
- `open_settings`：打开或激活设置窗口，不弹 toast
- `mark_game`："已视为游戏：{exe}"
- `exclude_app`："已排除：{exe}"
- `exit`：不弹 toast，直接退出

除 `debug_bar`、`calibrate`、`open_settings` 外，改动 MUST 立即保存到 INI（`calibrate` 只在结算成功时保存）。

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

#### Scenario: 打开设置不弹 toast

- **GIVEN** `open_settings=Ctrl+Alt+F12`
- **WHEN** 按 Ctrl+Alt+F12
- **THEN** 设置窗口出现并成为前台，没有 toast

### Requirement: 热键注册与冲突

每个已绑定的动作 SHALL 注册为全局热键，长按 MUST NOT 产生自动重复触发。字符串非法时，该动作不绑定，记一条 warn 日志，并在 INI 中原样保留用户写的字符串。被其他程序占用而注册失败时，记 warn 日志，并弹出一条 toast "快捷键 X 已被其他程序占用"（多个失败合并为一条，X 之间用"、"分隔）；但由设置窗口发起的注册失败 MUST NOT 弹 toast，改为在设置窗口快捷键页对应行标红。同一组合绑定给多个动作时，只有动作表中靠前的那个生效，其余记 warn 并弹 toast "快捷键 X 重复绑定，已忽略「中文名」"（设置窗口发起时同样只标红、不弹 toast）。不带修饰键的单键 MUST 允许注册，但要记 warn（会吃掉游戏里的这个键）。重新加载配置时，MUST 先注销全部热键再重新注册。

#### Scenario: 被占用

- **GIVEN** 另一程序已注册了 Ctrl+Alt+F9
- **WHEN** MotionCalm 启动
- **THEN** 弹出 toast "快捷键 Ctrl+Alt+F9 已被其他程序占用"，日志中有对应的 warn，其余热键正常工作

#### Scenario: 设置窗口中改到被占用的组合

- **GIVEN** 另一程序已注册了 Ctrl+Alt+K，设置窗口打开着
- **WHEN** 在快捷键页把 `cycle_level` 改为 Ctrl+Alt+K
- **THEN** 日志中有 warn，不弹 toast，快捷键页该行标红

#### Scenario: 内部重复

- **GIVEN** `toggle=Ctrl+Alt+F9` 且 `exit=Ctrl+Alt+F9`
- **WHEN** 程序启动
- **THEN** 按 Ctrl+Alt+F9 只切换遮罩，不会退出；并弹出 toast "快捷键 Ctrl+Alt+F9 重复绑定，已忽略「退出」"

#### Scenario: 长按不重复触发

- **WHEN** 按住 Ctrl+Alt+F10 两秒
- **THEN** 档位只循环一次

#### Scenario: 非法字符串原样保留

- **GIVEN** INI 中 `toggle=Ctrl+Alt+F99`
- **WHEN** 程序启动，之后因为其他改动保存了 INI
- **THEN** toggle 不绑定，日志有 warn，INI 中仍然是 `toggle=Ctrl+Alt+F99`

#### Scenario: 单键绑定

- **GIVEN** `debug_bar=F8`
- **WHEN** 程序启动
- **THEN** F8 注册成功，日志中有一条说明"单键会吃掉游戏里的该键"的 warn

#### Scenario: 重载后换绑

- **WHEN** 把 `toggle` 从 Ctrl+Alt+F9 改为 Ctrl+Alt+F1 并重载
- **THEN** Ctrl+Alt+F9 不再生效，Ctrl+Alt+F1 生效

## ADDED Requirements

### Requirement: 打开设置的动作
托盘"设置…"、左键双击托盘图标、热键 `open_settings` SHALL 执行同一动作：设置窗口不存在时创建并显示，已存在时恢复（若最小化）并激活到前台。该动作 MUST NOT 弹 toast（窗口出现本身就是反馈）。从全屏游戏中按热键时，窗口出现在鼠标指针所在显示器上并获得前台。

#### Scenario: 游戏中按热键
- **GIVEN** `open_settings=Ctrl+Alt+F12`，用户在全屏游戏中
- **WHEN** 按下 Ctrl+Alt+F12
- **THEN** 设置窗口出现在游戏所在显示器上并成为前台，不出现 toast

#### Scenario: 默认不绑定
- **GIVEN** 默认 INI（`open_settings=None`）
- **WHEN** 程序启动
- **THEN** 不为该动作注册热键；托盘"设置…"和双击图标仍可打开设置窗口

### Requirement: 设置窗口改键后立即重新注册
设置窗口提交某动作的新组合键后，系统 SHALL 立即按新配置重新注册热键（旧组合不再生效、新组合生效，其他动作仍是原来的组合；在按键捕获暂停期间提交的，于恢复注册时生效），并把注册结果（成功 / 被占用 / 重复未生效）交给设置窗口显示。注册失败时该动作没有生效的热键，INI 仍保存用户设定的字符串。

#### Scenario: 改键即时生效
- **GIVEN** `toggle=Ctrl+Alt+F9`，设置窗口打开着
- **WHEN** 在快捷键页把 `toggle` 改为 Ctrl+Alt+F1，然后把焦点移出捕获框
- **THEN** 按 Ctrl+Alt+F9 不再有反应，按 Ctrl+Alt+F1 切换遮罩；INI 中 `toggle=Ctrl+Alt+F1`

### Requirement: 按键捕获期间暂停全局热键
设置窗口的任一按键捕获框获得键盘焦点时，系统 SHALL 注销本程序的全部全局热键，让按键能送达捕获框。焦点离开捕获框、设置窗口失去激活或被关闭时，SHALL 立即按当前配置重新注册全部热键。为检测占用，捕获期间提交的新组合 SHALL 先试注册一次再立即注销，结果用于该行的状态显示。

#### Scenario: 捕获当前已被本程序使用的组合
- **GIVEN** `toggle=Ctrl+Alt+F9`，焦点在 `debug_bar` 的捕获框
- **WHEN** 按 Ctrl+Alt+F9
- **THEN** 遮罩不会被切换；捕获框显示 `Ctrl+Alt+F9`，两行按内部重复标红

#### Scenario: 切走后恢复
- **GIVEN** 焦点在某捕获框，全局热键已暂停
- **WHEN** 用户 Alt+Tab 切回游戏并按 Ctrl+Alt+F10
- **THEN** 遮罩档位照常循环（窗口失去激活时已恢复注册）
