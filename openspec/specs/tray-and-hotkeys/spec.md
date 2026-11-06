# tray-and-hotkeys Specification

## Purpose
不打开任何窗口就能操作 MotionCalm：通过中文托盘菜单、可在 INI 中自定义的全局热键及其即时提示来操作，同时负责开机自启和单实例等进程级行为。

## Requirements

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

### Requirement: 状态行

菜单第二行 SHALL 在菜单弹出的那一刻生成，格式为 `游戏：{exe} · {已锁定|未锁定} · 遮罩：{轻|中|强}`。其中 exe 是"当前程序"（定义见 game-detection），并且仅当它最近一次处于前台时被判为 `in_game` 才显示，否则整行为 `游戏：无 · 遮罩：{档位}`。"已锁定"表示该程序在最近一次前台期间曾达到过 `cursor_locked`。`enabled=0` 时，遮罩部分 MUST 显示为"遮罩：已关闭"。

#### Scenario: 在 CS2 中打开菜单

- **GIVEN** 用户在 cs2.exe 中转动过视角（达到锁定），档位为中
- **WHEN** 切到托盘打开菜单
- **THEN** 状态行为"游戏：cs2.exe · 已锁定 · 遮罩：中"

#### Scenario: 不在游戏里

- **WHEN** 最近的前台程序是记事本
- **THEN** 状态行为"游戏：无 · 遮罩：中"

#### Scenario: 已关闭

- **WHEN** enabled = 0
- **THEN** 状态行以"遮罩：已关闭"结尾

### Requirement: 菜单动作

每个菜单项 SHALL 执行与同名热键动作相同的逻辑，弹出相同的 toast，并 MUST 立即保存受影响的配置。动作如下：
- "遮罩强度"子项直接设置档位；
- "触发灵敏度"子项直接设置灵敏度；
- "打开配置文件"和"打开日志"用系统默认程序打开对应文件（没有关联程序时改用记事本）；
- "重新加载配置"重读 INI 并弹出 toast "已重新加载配置"；
- "关于 MotionCalm"弹出信息框，内容包括版本号和"不注入游戏进程、不读取内存、不安装钩子"的说明。

#### Scenario: 选择遮罩强度

- **WHEN** 在"遮罩强度"子菜单中点选"强"
- **THEN** 档位立即变为强，INI 中 `mask_level=strong`，并弹出 toast "遮罩：强"

#### Scenario: 选择灵敏度

- **WHEN** 在"触发灵敏度"子菜单中点选"高"
- **THEN** 阈值立即改为 30 / 200，INI 中 `sensitivity=high`，并弹出 toast "灵敏度：高"

#### Scenario: 打开配置文件

- **WHEN** 点击"打开配置文件"
- **THEN** 当前使用的 INI（默认为 exe 同目录的 MotionCalm.ini）在默认文本编辑器中打开

#### Scenario: 重新加载

- **GIVEN** 用户在记事本里把 `mask_level` 改成 light 并保存（自动重载之前）
- **WHEN** 点击"重新加载配置"
- **THEN** 档位立即变为轻，并弹出 toast "已重新加载配置"

### Requirement: 热键字符串语法

热键字符串 SHALL 由若干以 `+` 分隔的记号组成，记号前后允许空白，不区分大小写。修饰键为 `Ctrl`、`Alt`、`Shift`、`Win`，各自至多出现一次，顺序任意。修饰键之后必须恰好跟一个按键记号，且位于末尾。`None`（不区分大小写）或空串表示不绑定。规范化输出 MUST 按 Ctrl、Alt、Shift、Win 的顺序排列修饰键，再接按键的规范名。可用的按键名：

```
F1–F24   A–Z   0–9   Space Tab Esc Pause Insert Delete Home End PgUp PgDn
Up Down Left Right   Num0–Num9
VK_OEM_1 VK_OEM_PLUS VK_OEM_COMMA VK_OEM_MINUS VK_OEM_PERIOD VK_OEM_2 VK_OEM_3
VK_OEM_4 VK_OEM_5 VK_OEM_6 VK_OEM_7 VK_OEM_8 VK_OEM_102
```

#### Scenario: 规范与宽松写法

- **WHEN** 解析 "Ctrl+Alt+F9"、"ctrl + alt + f9"、"Alt+Ctrl+F9"
- **THEN** 三者结果相同（Ctrl+Alt，F9），规范化输出都是 "Ctrl+Alt+F9"

#### Scenario: 修饰键排序

- **WHEN** 解析 "Win+Shift+a"
- **THEN** 结果为 Shift+Win 加 A，规范化输出 "Shift+Win+A"

#### Scenario: 各类按键名

- **WHEN** 解析 "Ctrl+PgUp"、"Alt+Num5"、"Ctrl+VK_OEM_3"、"Shift+Up"、"Ctrl+0"、"Esc"
- **THEN** 分别得到 Page Up、小键盘 5、反引号键（VK 0xC0）、上方向键、主键盘 0、Esc

#### Scenario: 不绑定

- **WHEN** 值为 "None"、"none" 或空串
- **THEN** 结果为"不绑定"，并且不报错

#### Scenario: 非法字符串

- **WHEN** 值为 "Ctrl+Alt"（缺按键）、"Ctrl+F9+F10"（两个按键）、"Ctrl+Ctrl+F9"（修饰键重复）、"Ctrl+Foo"、"F25"
- **THEN** 解析失败，并给出可读的错误原因

#### Scenario: 往返一致

- **WHEN** 对上面列出的每个按键名 k，以及修饰键的全部 16 种组合 m，先规范化输出再重新解析
- **THEN** 得到的组合与原组合相同

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

### Requirement: 热键触发提示

每次热键触发 SHALL 立即执行动作。除 `calibrate`（提示由 turn-calibration 决定）、`open_settings`（打开或激活设置窗口）和 `exit`（直接退出）外，每次都 MUST 弹出一条 toast，文案见"热键 toast 文案"。除 `debug_bar`、`calibrate`、`open_settings` 外，改动 MUST 立即保存到 INI（`calibrate` 只在结算成功时保存）。

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

### Requirement: 热键 toast 文案

热键 toast SHALL 使用以下文案：
- `toggle`："已启用遮罩" / "已关闭遮罩"
- `cycle_level`（轻 → 中 → 强 → 轻）："遮罩：轻|中|强"
- `toggle_center_dot`："中心参照点：开|关"；`toggle_edge_dots`："边缘参照点：开|关"
- `toggle_capture`："截屏运动检测：开|关"；`--no-capture` 启动时不改值，文案为 "截屏运动检测已被 --no-capture 禁用"
- `debug_bar`："调试信息：开|关"
- `cycle_sensitivity`（低 → 中 → 高 → 低）："灵敏度：低|中|高"
- `mark_game`："已视为游戏：{exe}"；`exclude_app`："已排除：{exe}"

#### Scenario: 循环灵敏度

- **GIVEN** 当前触发灵敏度为高
- **WHEN** 触发 `cycle_sensitivity`
- **THEN** 灵敏度变为低，toast 为 "灵敏度：低"

#### Scenario: 截屏检测被命令行禁用

- **GIVEN** 以 `--no-capture` 启动，INI 中 `capture=1`
- **WHEN** 按 Ctrl+Alt+F6
- **THEN** INI 保持 `capture=1`，toast 为 "截屏运动检测已被 --no-capture 禁用"

### Requirement: 开机自启

`[general] autostart` SHALL 是开机自启的唯一依据。为 1 时，`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下名为 `MotionCalm` 的值 MUST 为带引号的当前 exe 完整路径；为 0 时 MUST 删除该值。程序启动时、配置重载后、托盘勾选变化时都 SHALL 按此同步注册表。写注册表失败时，弹出 toast "无法设置开机自启"，记 warn，并把 `autostart` 恢复为原值。

#### Scenario: 勾选开机自启

- **WHEN** 在托盘中勾选"开机自动启动"
- **THEN** INI 中 `autostart=1`，注册表 Run\MotionCalm = `"D:\Tools\MotionCalm\MotionCalm.exe"`（实际路径）

#### Scenario: exe 被移动后的修正

- **GIVEN** autostart=1，用户把整个文件夹搬到了别处
- **WHEN** 从新位置启动
- **THEN** Run\MotionCalm 被更新为新路径

#### Scenario: 取消

- **WHEN** 取消勾选
- **THEN** Run\MotionCalm 值被删除，INI 中 `autostart=0`

### Requirement: 单实例

程序 SHALL 用命名互斥量 `Local\MotionCalm.Instance` 保证同一会话只运行一个实例。第二个实例 MUST 通知第一个实例，然后在 1 s 内以退出码 0 退出，期间不创建托盘图标、overlay 和日志写入。第一个实例收到通知后弹出 toast "MotionCalm 已在运行"。

#### Scenario: 重复启动

- **GIVEN** MotionCalm 已在运行
- **WHEN** 用户再次双击 MotionCalm.exe
- **THEN** 新进程 1 s 内退出，通知区仍只有一个图标，屏幕顶部出现 toast "MotionCalm 已在运行"

### Requirement: 退出

无论通过"退出"菜单、`exit` 热键还是 `--exit-after` 到时，程序都 SHALL 有序退出：注销热键、移除托盘图标、取消前台事件监听、释放渲染资源、关闭日志，并以退出码 0 结束。退出过程 MUST NOT 改动 INI，除非还有尚未写盘的改动。

#### Scenario: 菜单退出

- **WHEN** 点击"退出"
- **THEN** 1 s 内进程结束，退出码 0，托盘图标消失，之前注册的组合键可被其他程序注册

### Requirement: 校准菜单项

"校准当前游戏…" SHALL 弹出一个置顶的信息框，说明操作方法：回到游戏，锁定视角，准星对准参照物，按 `{calibrate 当前绑定}` 开始，转一整圈后再按一次，共测两次。`calibrate` 绑定为 None 时，信息框 MUST 改为提示"请先在 MotionCalm.ini 的 [hotkeys] 中为 calibrate 设置快捷键"。这个菜单项本身 MUST NOT 改变校准状态。"校准列表 ▸" SHALL 按 exe 字母序列出 `[calibration]` 中的每一项，文字为"删除 {exe}（每度 {值，1 位小数} 计数）"。点击后直接删除该项，立即保存，并弹 toast "已删除校准：{exe}"。列表为空时只显示灰色的"（无）"。

#### Scenario: 托盘发起校准

- **WHEN** 点击"校准当前游戏…"
- **THEN** 弹出说明框，其中包含"Ctrl+Alt+F7"；校准状态仍为 Idle

#### Scenario: 未绑定热键

- **GIVEN** `calibrate=None`
- **WHEN** 点击"校准当前游戏…"
- **THEN** 说明框提示需要先在 INI 中设置 calibrate 快捷键

#### Scenario: 删除一项

- **GIVEN** `[calibration]` 中有 cs2.exe=18.400 和 valorant.exe=35.714
- **WHEN** 在"校准列表"中点击"删除 cs2.exe（每度 18.4 计数）"
- **THEN** INI 中只剩 valorant.exe 一项，toast 为"已删除校准：cs2.exe"，cs2.exe 立即回到默认的 18.2

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
