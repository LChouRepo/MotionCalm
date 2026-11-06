# settings-window Specification

## Purpose
设置窗口：给不熟悉 INI 的用户提供一个中文图形界面，覆盖全部配置项。改动即时生效并自动保存，可以预览遮罩效果，可以用按键捕获设置快捷键并当场看到冲突，还可以管理校准记录与程序名单。

## Requirements

### Requirement: 打开方式与单实例
设置窗口 SHALL 可以通过托盘"设置…"、双击托盘图标、热键 `open_settings` 打开。同一时刻最多存在一个设置窗口：已打开时再次请求 SHALL 只把它恢复（若已最小化）并激活到前台，保持当前分页与控件状态。窗口是非模态的：打开期间托盘、热键、overlay 渲染与强度模型照常运行。

#### Scenario: 重复打开
- **GIVEN** 设置窗口已打开并停留在"快捷键"页，且被最小化
- **WHEN** 用户再次点击托盘"设置…"
- **THEN** 不会出现第二个窗口；原窗口恢复并成为前台，仍停留在"快捷键"页

#### Scenario: 打开期间 overlay 照常工作
- **GIVEN** 设置窗口打开着，用户切回已知游戏
- **WHEN** 鼠标以 440°/s 转动
- **THEN** 遮罩照常出现，强度 > 0.8

### Requirement: 窗口外观与分页
窗口标题 SHALL 为"MotionCalm 设置"，带程序图标并在任务栏显示按钮，不置顶。窗口内是一个 Tab 控件，6 个分页依次为：遮罩、参照物、触发、校准、快捷键、通用；右下角有"关闭"按钮，没有"确定 / 取消 / 应用"。所有文字 SHALL 为中文，字体为 "Microsoft YaHei UI" 9 pt。首次打开时窗口居中显示在鼠标指针所在显示器的工作区；之后打开时回到上次关闭时的位置（`[ui] x / y`），该位置不在鼠标指针所在显示器上时仍居中于该显示器。同一次运行内再次打开时回到上次的分页。Ctrl+Tab / Ctrl+Shift+Tab SHALL 切换到下一个 / 上一个分页。

#### Scenario: 首次打开
- **WHEN** 程序启动后第一次打开设置窗口
- **THEN** 显示"遮罩"页，6 个分页标签依次为"遮罩 参照物 触发 校准 快捷键 通用"

#### Scenario: 键盘切换分页
- **GIVEN** 当前在"通用"页
- **WHEN** 按 Ctrl+Tab
- **THEN** 切换到"遮罩"页（循环）

### Requirement: 只用系统通用控件
设置窗口 SHALL 只使用 Windows 自带的通用控件（comctl32 v6，由 manifest 声明，启用视觉样式）：对话框、Tab、按钮 / 复选框 / 单选框、编辑框、组合框、滑块（trackbar）、列表框、列表视图、静态文本。MUST NOT 引入任何第三方 UI 库或 Web 视图；色块只用静态控件自绘纯色矩形。

#### Scenario: 依赖检查
- **WHEN** 用 `dumpbin /dependents MotionCalm.exe` 检查
- **THEN** 不出现系统 DLL 之外的依赖，`comctl32.dll` 加载的是 6.x 版本

### Requirement: DPI 感知布局
设置窗口 SHALL 按所在显示器的 DPI 缩放（Per-Monitor V2）：在 100%、125%、150%、200% 缩放下，所有文字完整可见、控件互不重叠、不出现横向截断。窗口被拖到另一块 DPI 不同的显示器上时，SHALL 在收到 DPI 变化通知后按新 DPI 重新布局。

#### Scenario: 150% 缩放
- **GIVEN** 显示器缩放为 150%
- **WHEN** 打开设置窗口并逐页查看
- **THEN** 每页的标签、数值、按钮文字都没有被截断或重叠，文字清晰（不是位图拉伸）

#### Scenario: 跨显示器拖动
- **WHEN** 把窗口从 100% 的显示器拖到 200% 的显示器
- **THEN** 窗口与控件按 2 倍重新布局，文字清晰

### Requirement: 改动即时生效
每个控件的改动 SHALL 立即写入当前配置并触发配置变更通知，效果在 50 ms 内体现在 overlay、强度模型、热键、截屏与游戏判定上，无需点击任何"应用"按钮。滑块在拖动过程中每次位置变化都生效；复选框、单选框、组合框在点选时生效；编辑框在按 Enter 或失去焦点时生效。

#### Scenario: 拖动滑块实时生效
- **GIVEN** 预览强度为 100%，遮罩档位为强
- **WHEN** 拖动"强·最深不透明度"滑块从 0.85 到 0.50
- **THEN** 拖动过程中 overlay 顶边中点的不透明度随之连续变化，松手时为 0.50

#### Scenario: 编辑框在提交时生效
- **WHEN** 在"中·满强度"阈值框里输入 400 但未按 Enter
- **THEN** 强度模型仍用旧值；按 Enter 或切到其他控件后改用 400

### Requirement: 数值控件的取值规则
每个数值控件 SHALL 有固定范围与步长（见各分页需求），范围与配置能力「值校验与回退」的合法范围相同，旁边的数值标签显示当前值与单位。编辑框输入越界时 SHALL 钳制到边界并显示气泡提示"范围 <最小>–<最大>"；无法解析时 SHALL 恢复原值并提示。INI 中不在步长上的值 SHALL 原样保留，滑块停在最接近的位置、数值标签显示实际值，直到用户移动该滑块。

#### Scenario: 越界输入
- **WHEN** 在"中·满强度"阈值框中输入 9000 并按 Enter
- **THEN** 值变为 5000，框内显示 5000，并出现气泡提示"范围 1–5000"

#### Scenario: 非法输入
- **WHEN** 在"椭圆横纵比"框中输入 "abc" 并离开
- **THEN** 框内恢复为原值，配置不变，出现气泡提示

#### Scenario: 不在步长上的值
- **GIVEN** INI 中 `feather=0.355`
- **WHEN** 打开遮罩页但不碰该滑块
- **THEN** 标签显示 0.355，关闭窗口后 INI 中仍是 0.355

### Requirement: 遮罩页开关与档位
遮罩页 SHALL 提供：复选框"启用遮罩" → `[general] enabled`；单选组"遮罩强度"（轻 / 中 / 强）→ `[general] mask_level`（light / medium / strong）；"遮罩颜色"为 6 位十六进制编辑框（RRGGBB，不区分大小写）加一个显示该颜色的色块 → `[mask] color`。颜色值不是 6 位十六进制时 SHALL 恢复原值并提示。

#### Scenario: 切换档位
- **WHEN** 在单选组中点选"强"
- **THEN** INI 中 `mask_level=strong`；托盘"遮罩强度"子菜单显示"强"为选中

#### Scenario: 修改颜色
- **WHEN** 在颜色框中输入 `200000` 并按 Enter
- **THEN** 色块变为深红色，overlay 遮罩颜色随即变为 #200000

### Requirement: 遮罩页各档位参数
遮罩页 SHALL 用三行（轻 / 中 / 强）各两个滑块编辑档位参数："最深不透明度" → `[mask] max_opacity_light` / `max_opacity_medium` / `max_opacity_strong`，范围 0.00–1.00，步长 0.01；"清晰区高度"（满强度时清晰区直径 / 屏幕高度）→ `clear_height_light` / `clear_height_medium` / `clear_height_strong`，范围 0.05–1.00，步长 0.01。

#### Scenario: 默认值
- **WHEN** 使用默认配置打开遮罩页
- **THEN** 三行分别显示 0.60 / 0.65、0.75 / 0.60、0.85 / 0.55

### Requirement: 遮罩页形状与时间
遮罩页 SHALL 提供滑块："椭圆横纵比" → `[mask] ellipse_ratio`，0.50–3.00，步长 0.05；"边缘柔和度" → `feather`，0.05–1.00，步长 0.01；"出现时间" → `attack_ms`，10–2000 ms，步长 10；"淡出时间" → `release_ms`，10–5000 ms，步长 10。

#### Scenario: 修改淡出时间
- **WHEN** 把"淡出时间"拖到 1000 ms
- **THEN** 之后一次转动停止后，强度约 1.0 s 才降到 5% 以下

### Requirement: 遮罩页预览强度
遮罩页 SHALL 提供"预览强度"滑块，0–100%，步长 1%，每次打开窗口时为 0%。它不对应任何 INI 键、MUST NOT 被保存；它的值在窗口打开期间作为渲染强度的下限（见 overlay-rendering「设置窗口预览强度」），关闭窗口即失效。

#### Scenario: 预览不保存
- **GIVEN** 预览强度拖到 70%
- **WHEN** 关闭窗口后重新打开
- **THEN** 预览强度为 0%，INI 中没有与预览相关的键

### Requirement: 参照物页
参照物页 SHALL 提供：复选框"显示中心点" → `[dots] center_dot`；滑块"中心点半径" → `center_dot_radius_px`，1–20 px，步长 1；颜色框"中心点颜色" → `center_dot_color`；复选框"中心点描边" → `center_dot_outline`；复选框"显示边缘点" → `edge_dots`；滑块"边缘点半径" → `edge_dot_radius_px`，1–20 px；颜色框"边缘点颜色" → `edge_dot_color`；滑块"边缘点最淡" / "边缘点最浓" → `edge_dot_min_alpha` / `edge_dot_max_alpha`，0.00–1.00，步长 0.01；滑块"边缘点内缩" → `edge_dot_margin_px`，0–400 px，步长 1。

#### Scenario: 关闭中心点时相关控件灰显
- **WHEN** 取消勾选"显示中心点"
- **THEN** "中心点半径""中心点颜色""中心点描边"灰显不可编辑，overlay 上中心点消失

#### Scenario: 最淡不超过最浓
- **GIVEN** 边缘点最浓为 0.75
- **WHEN** 把"边缘点最淡"拖到 0.80
- **THEN** "边缘点最浓"同时被推到 0.80，保存后 `edge_dot_min_alpha=0.80`、`edge_dot_max_alpha=0.80`

### Requirement: 触发页触发源与灵敏度
触发页 SHALL 提供：组合框"触发灵敏度"（低 / 中 / 高）→ `[general] sensitivity`；复选框"鼠标转视角" → `[trigger] mouse`、"键盘移动（WASD / 方向键）" → `keyboard`、"手柄" → `gamepad`、"截屏运动检测" → `capture`；组合框"截屏检测灵敏度"（低 / 中 / 高）→ `capture_sensitivity`；组合框"鼠标锁定判定"（"严格（推荐）" / "仅看光标隐藏"）→ `mouse_lock_rule`（strict / hidden）。

#### Scenario: 命令行禁用截屏
- **GIVEN** 以 `--no-capture` 启动
- **WHEN** 打开触发页
- **THEN** "截屏运动检测"复选框灰显，文字为"截屏运动检测（已由 --no-capture 禁用）"，勾选状态反映 INI 的值

#### Scenario: 采集失败时
- **GIVEN** 截屏检测处于"采集失败"状态
- **WHEN** 打开触发页
- **THEN** 复选框文字为"截屏运动检测（采集失败）"且保持勾选；取消勾选后写入 `capture=0`

### Requirement: 触发页移动与手柄参数
触发页 SHALL 提供滑块："移动强度" → `[trigger] move_intensity`，0.00–1.00，步长 0.05；"手柄最大转速" → `gamepad_max_deg_per_s`，10–2000 °/s，步长 10；"手柄死区" → `gamepad_deadzone`，0.00–0.90，步长 0.01；"速度计算窗口" → `velocity_window_ms`，10–500 ms，步长 5。

#### Scenario: 修改移动强度
- **WHEN** 把"移动强度"拖到 0.50
- **THEN** 已知游戏中只按住 W 时，强度目标为 0.50

### Requirement: 触发页阈值
触发页 SHALL 用 3 行（低 / 中 / 高）× 2 列（开始出现 / 满强度）的数值编辑框（最多一位小数）编辑阈值：`[trigger] v_start_low`、`v_full_low`、`v_start_medium`、`v_full_medium`、`v_start_high`、`v_full_high`，范围 1–5000 °/s。同一档位提交后若"开始出现" ≥ "满强度"，SHALL 拒绝该次输入、恢复原值，并显示气泡提示"开始值必须小于满强度值"。

#### Scenario: 非法顺序
- **GIVEN** 中档满强度为 360
- **WHEN** 把中档开始出现改为 400 并按 Enter
- **THEN** 框内恢复为 60，配置不变，出现气泡提示

### Requirement: 触发页截屏检测高级参数
触发页 SHALL 在"截屏检测（高级）"分组中提供：滑块"前进 / 后退的灵敏度" → `[capture] forward_gain`（0.5–3.0 倍，步长 0.1）；复选框"识别画面倾斜" → `detect_roll`；前进与倾斜两张阈值表（低 / 中 / 高 × 开始 / 完全，°/s）→ `zoom_*` / `roll_*`，开始必须小于完全；滑块"假定水平视场角" → `assumed_hfov_deg`（30–170°）；"最低置信度" → `min_correlation`（0.10–0.99）；"低置信度保持时间" → `hold_ms`（0–1000 ms）；整数框"最大位移" → `max_shift`（2–40）、"网格宽 / 高" → `grid_w`（64–320）/ `grid_h`（36–180）。改动按截屏配置热更新规则生效。

#### Scenario: 修改视场角
- **GIVEN** 截屏正在取帧
- **WHEN** 把"假定水平视场角"拖到 120
- **THEN** 下一个新帧起截屏角速度按 120° 换算

#### Scenario: 前进 / 后退的灵敏度
- **GIVEN** 设置窗口打开在触发页，已显示高级选项
- **WHEN** 把"前进 / 后退的灵敏度"从默认位置（读数"1.0 倍"）拖到最右
- **THEN** 读数为"3.0 倍"，`forward_gain=3.0` 立即生效，300 ms 后写入 INI；拖到最左时为 0.5

#### Scenario: 关闭倾斜识别
- **WHEN** 取消勾选"识别画面倾斜"
- **THEN** `detect_roll=0`，倾斜角速度从下一个新帧起为 0；再勾选恢复为 1

#### Scenario: 文案与访问键
- **WHEN** 查看这两项
- **THEN** 滑块标签带访问键（"前进 / 后退的灵敏度(&R)"），复选框所在行的标签为"画面倾斜(&T)"，两项下方各有一行灰色辅助说明，用玩家能懂的话说明作用

#### Scenario: 前进 / 倾斜阈值表
- **GIVEN** 设置窗口打开在触发页，已显示高级选项
- **WHEN** 查看前进阈值表的中灵敏度行，然后把开始值改为 200
- **THEN** 两个框默认显示 20 与 150；改为 200（大于 150）时被拒绝，框内恢复 20；改为 30 时 `zoom_start_medium=30` 立即生效

### Requirement: 校准页列表
校准页 SHALL 用列表视图（报表模式、整行选择、单选）列出 `[calibration]` 中的全部记录，列为"游戏"（exe 名）、"每度计数"（两位小数）、"校准日期"（YYYY-MM-DD，无日期时显示"—"），按游戏名升序。没有记录时列表下方显示"尚无校准记录"。校准完成或记录被托盘删除后，列表 SHALL 在 1 个 tick 内刷新。

#### Scenario: 显示旧格式记录
- **GIVEN** INI 中 `[calibration] cs2.exe=18.4`
- **WHEN** 打开校准页
- **THEN** 列表有一行"cs2.exe | 18.40 | —"

#### Scenario: 窗口打开时完成校准
- **GIVEN** 校准页打开着
- **WHEN** 用户在游戏里完成 valorant.exe 的校准
- **THEN** 列表立即出现或更新 valorant.exe 一行，日期为当天

### Requirement: 校准页删除与重测
"删除"与"重测"按钮 SHALL 只在选中一行时可用。"删除"先弹确认框"删除 <exe> 的校准？"，确认后移除该记录，该游戏从下一个 tick 起使用"未校准游戏的每度计数"。"重测"在校准流程空闲时，SHALL 在按钮下方显示指引："请切回 <exe>，锁定视角后按 <calibrate 热键> 开始校准；完成后会覆盖这条记录。"；`calibrate` 绑定为 None 时改为提示先在快捷键页设置。校准进行中时"重测"灰显。

#### Scenario: 删除记录
- **GIVEN** 选中 cs2.exe（18.40）
- **WHEN** 点"删除"并确认
- **THEN** 列表中不再有 cs2.exe，INI 的 `[calibration]` 中也没有该键，cs2.exe 改用 `default_counts_per_degree`

#### Scenario: 重测指引
- **GIVEN** `calibrate=Ctrl+Alt+F7`，选中 cs2.exe
- **WHEN** 点"重测"
- **THEN** 显示"请切回 cs2.exe，锁定视角后按 Ctrl+Alt+F7 开始校准；完成后会覆盖这条记录。"

### Requirement: 校准页默认每度计数
校准页 SHALL 提供编辑框"未校准游戏的每度计数" → `[trigger] default_counts_per_degree`，范围 1.0–1000.0，一位小数，旁边注明"默认 18.2（约 800 DPI、常见灵敏度）"。

#### Scenario: 修改默认值
- **WHEN** 输入 9.1 并按 Enter
- **THEN** 未校准的已知游戏中，同样的鼠标位移得到的角速度是原来的 2 倍

### Requirement: 快捷键页列表
快捷键页 SHALL 按热键注册表的顺序为每个动作显示一行（`toggle`、`cycle_level`、`toggle_center_dot`、`debug_bar`、`calibrate`、`toggle_capture`、`cycle_sensitivity`、`toggle_edge_dots`、`open_settings`、`mark_game`、`exclude_app`、`exit`，共 12 行）：左边是注册表中的中文名，中间是按键捕获框（显示规范化的组合键文字或"None"），右边是状态文字。页顶说明："点击输入框后按下新的组合键；按 Backspace 或 Delete 清除。"

#### Scenario: 默认绑定
- **WHEN** 使用默认配置打开快捷键页
- **THEN** 第一行捕获框显示 `Ctrl+Alt+F9`，`open_settings` 一行显示 `None`

### Requirement: 按键捕获
捕获框获得焦点后 SHALL 记录按下的修饰键（Ctrl、Alt、Shift、Win），只按修饰键时显示"Ctrl+Alt+…"之类的半成品。按下热键语法支持的非修饰键时，SHALL 立即以"当前修饰键 + 该键"提交。不带修饰键的 Backspace 或 Delete SHALL 提交为 None。Tab 键用于切换焦点，不被捕获；不带修饰键的 Esc 取消本次捕获、恢复原值，不关闭窗口。语法不支持的键（如 Enter、PrintScreen）不改值，状态显示"不支持该按键"。

#### Scenario: 捕获组合键
- **WHEN** 焦点在"循环遮罩强度"的捕获框，用户按住 Ctrl+Shift 再按 F10
- **THEN** 框内显示 `Ctrl+Shift+F10`，INI 中 `cycle_level=Ctrl+Shift+F10`

#### Scenario: 清除绑定
- **WHEN** 焦点在某个捕获框，按 Delete
- **THEN** 框内显示 `None`，该动作不再注册热键

#### Scenario: 带修饰键的 Delete
- **WHEN** 按 Ctrl+Shift+Delete
- **THEN** 提交为 `Ctrl+Shift+Delete`，而不是 None

#### Scenario: 只按修饰键后松开
- **WHEN** 按下 Ctrl+Alt 后全部松开，没有按其他键
- **THEN** 框内恢复原值，配置不变

### Requirement: 快捷键冲突与警告
每次提交和每次打开快捷键页时，系统 SHALL 逐行评估：与本程序其他动作组合键相同 → 红色底，状态"与「<对方中文名>」重复"，动作表中靠后的一行再加"（未生效）"（只有靠前的生效）；试注册失败（被其他程序或系统占用）→ 红色底，状态"已被其他程序占用"；不带任何修饰键 → 黄色底，状态"无修饰键：游戏里按这个键也会触发"。有冲突或警告的值照常保存。颜色之外 SHALL 始终配有状态文字。

#### Scenario: 内部重复
- **GIVEN** `toggle=Ctrl+Alt+F9`
- **WHEN** 把 `debug_bar` 设为 `Ctrl+Alt+F9`
- **THEN** 这两行都变为红色底；"启用 / 关闭遮罩"一行状态为"与「显示调试信息」重复"，"显示调试信息"一行状态为"与「启用 / 关闭遮罩」重复（未生效）"
- **AND** 按 Ctrl+Alt+F9 只切换遮罩

#### Scenario: 被其他程序占用
- **GIVEN** 另一个程序已注册 `Ctrl+Alt+K`
- **WHEN** 把某动作设为 `Ctrl+Alt+K`
- **THEN** 该行红色底，状态"已被其他程序占用"，INI 中仍保存 `Ctrl+Alt+K`

#### Scenario: 无修饰键
- **WHEN** 把 `toggle_center_dot` 设为 `F11`
- **THEN** 该行黄色底并显示警告文字，热键照常注册

### Requirement: 通用页开机自启
通用页 SHALL 提供复选框"开机自动启动"，绑定 `[general] autostart`，并与托盘"开机自动启动"走同一套逻辑：`autostart` 是唯一依据，勾选时把 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下的 `MotionCalm` 值写为带引号的当前 exe 完整路径，取消时删除该值。写注册表失败时 SHALL 把 `autostart` 与复选框都恢复原状态，弹出 toast "无法设置开机自启"，并记 warn。

#### Scenario: 勾选
- **WHEN** 勾选"开机自动启动"
- **THEN** 注册表 Run 键下出现 `MotionCalm="<exe 完整路径>"`，INI 中 `autostart=1`，托盘该项也显示为勾选

#### Scenario: 写注册表失败
- **GIVEN** Run 键被组策略设为只读
- **WHEN** 勾选"开机自动启动"
- **THEN** 复选框恢复为未勾选，INI 中仍为 `autostart=0`，出现 toast "无法设置开机自启"

### Requirement: 通用页录屏可见性与日志级别
通用页 SHALL 提供组合框"录屏可见性" → `[general] hide_from_capture`：选项为"自动（截屏检测开启时隐藏）"= auto、"始终对截屏隐藏"= always、"始终可见"= never；组合框"日志级别" → `[general] log_level`：选项为"调试"= debug、"信息"= info、"警告"= warn。以 `--verbose` 启动时，日志级别组合框旁 SHALL 注明"（--verbose 运行中，实际为调试）"。

#### Scenario: 切到始终可见
- **WHEN** 选择"始终可见"
- **THEN** 下一次截屏中包含 overlay，INI 中 `hide_from_capture=never`

### Requirement: 通用页程序名单
通用页 SHALL 提供"排除名单"（`[detect] exclude`）与"视为游戏"（`[detect] force_game`）两个列表，各带输入框与"添加""删除"按钮；以及文字"已知游戏：<N> 个"和"清空已知游戏列表"按钮（`[detect] known_games`）。添加时去掉首尾空白；空名称、含 `;` 或 `=` 的名称、已在该列表中（不区分大小写）的名称 SHALL 被拒绝并给出气泡提示。加入排除名单时 SHALL 同时从 `force_game` 和 `known_games` 移除；加入"视为游戏"时 SHALL 同时从排除名单移除。

#### Scenario: 加入排除名单
- **GIVEN** tool.exe 在 `known_games` 中
- **WHEN** 在排除名单输入框中输入 `tool.exe` 并点"添加"
- **THEN** 排除名单出现 tool.exe，`known_games` 中没有它，"已知游戏"计数减 1，它从下一次采样起不再触发

#### Scenario: 重复添加
- **WHEN** 向排除名单添加 `Chrome.EXE`
- **THEN** 被拒绝，提示"已在名单中"

#### Scenario: 清空已知游戏
- **WHEN** 点"清空已知游戏列表"并在确认框中选"是"
- **THEN** `known_games` 为空，显示"已知游戏：0 个"，`force_game` 不变

### Requirement: 通用页配置文件与恢复默认
通用页 SHALL 提供"打开配置文件"按钮：先保存尚未落盘的改动，再用系统默认程序打开 `MotionCalm.ini`。以及"恢复默认设置"按钮：先弹确认框（说明会保留校准记录、"视为游戏"、已知游戏和开机自启），确认后按配置能力的规则恢复默认值，并刷新所有分页的控件。

#### Scenario: 恢复默认
- **GIVEN** 用户把档位改为强、`toggle` 改为 `Ctrl+Alt+K`，并有一条 cs2.exe 的校准
- **WHEN** 点"恢复默认设置"并确认
- **THEN** 遮罩页显示"中"，快捷键页 `toggle` 显示 `Ctrl+Alt+F9`，校准页仍有 cs2.exe

#### Scenario: 打开配置文件前先保存
- **WHEN** 拖动一个滑块后 100 ms 内点"打开配置文件"
- **THEN** 打开的文件里已经是新值

### Requirement: 与其他入口的双向同步
设置窗口打开期间，配置因其他来源改变时（托盘、热键、外部修改 INI 后自动重载、校准完成、已知游戏自动追加），窗口内所有分页的控件 SHALL 在 1 个 tick 内刷新为新值。唯一的例外是正有键盘焦点、且有未提交输入的编辑框，它保留用户正在输入的内容。

#### Scenario: 热键改档位
- **GIVEN** 设置窗口停在遮罩页，档位为中
- **WHEN** 用户按 `Ctrl+Alt+F10`
- **THEN** 单选组立即变为"强"

#### Scenario: 外部编辑 INI
- **WHEN** 用记事本把 `feather` 改为 0.50 并保存
- **THEN** 自动重载后，"边缘柔和度"滑块与标签显示 0.50

### Requirement: 关闭窗口
点"关闭"、标题栏关闭按钮、Alt+F4，或焦点不在捕获框时按 Esc，SHALL 关闭设置窗口：立即保存尚未落盘的改动，清除预览强度下限，如全局热键处于暂停则恢复注册，然后销毁窗口；程序继续在托盘运行。从托盘退出程序时，若设置窗口打开着，SHALL 先按同样的步骤关闭它。

#### Scenario: 关闭后状态
- **GIVEN** 预览强度为 60%，刚改过一个滑块（不到 300 ms）
- **WHEN** 点"关闭"
- **THEN** INI 已包含新值，下一个 tick 内预览遮罩消失（不在游戏时 overlay 隐藏）

### Requirement: 通用页界面语言
通用页 SHALL 在页面说明之后、"启动与日志"之前提供"语言"分组：下拉框"界面语言"（访问键 N）→ `[ui] language`，四项依次为"跟随系统"= auto、"简体中文"= zh-CN、"繁體中文"= zh-TW、"English"= en；后三项无论当前界面语言如何都 SHALL 用各自的语言显示，第一项按当前界面语言显示（"跟随系统" / "跟隨系統" / "Use system language"）。下方一行辅助说明写明"切换后立即生效，跟随系统按 Windows 的显示语言选择"。选择后 SHALL 立即生效：窗口标题、分页标签、状态栏按钮与全部分页按新语言重建，保留当前分页（通用页）与页内滚动位置，键盘焦点回到"界面语言"下拉框；改动按设置窗口改动的保存时机写入 INI。

#### Scenario: 切换为英文
- **GIVEN** 设置窗口打开在通用页，界面语言为简体中文
- **WHEN** 在"界面语言"中选择 "English"
- **THEN** 窗口标题变为 "MotionCalm Settings"，分页标签为 "Mask"、"Reference dots"、"Trigger"、"Calibration"、"Hotkeys"、"General"，仍显示 General 页，`[ui] language=en` 在 300 ms 后写入 INI

#### Scenario: 选项文字
- **GIVEN** 界面语言为 English
- **WHEN** 展开"Display language"下拉框
- **THEN** 四项为 "Use system language"、"简体中文"、"繁體中文"、"English"

#### Scenario: 切换后页内状态保留
- **GIVEN** 通用页显示"已知游戏：2 个"
- **WHEN** 依次切换到 English、繁體中文、简体中文
- **THEN** 计数依次显示 "Known games: 2"、「已知遊戲：2 個」、"已知游戏：2 个"
