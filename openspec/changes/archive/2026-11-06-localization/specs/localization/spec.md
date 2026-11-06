# Spec Delta

## Purpose

MotionCalm 的界面文字以简体中文、繁體中文（台湾用语）与英文三种语言提供：按 Windows 显示语言自动选择或由用户指定，切换后立即生效，且任何一种语言下界面都不截断、不缺字。

## ADDED Requirements

### Requirement: 界面语言的选择
系统 SHALL 支持三种界面语言：简体中文（zh-CN）、繁體中文（zh-TW）、English（en）。`[ui] language` 为 `zh-CN` / `zh-TW` / `en` 时 SHALL 使用该语言；为 `auto`（默认）时 SHALL 取用户首选 Windows 显示语言列表的第一项（取不到时用用户默认 UI 语言），按以下规则映射：语言名为 `zh-TW`、`zh-HK`、`zh-MO` 或以 `zh-Hant` 开头 → 繁體中文；`zh-CN`、`zh-SG`、以 `zh-Hans` 开头或只有 `zh` → 简体中文；其余一律 → English。语言名比较不区分大小写，`_` 与 `-` 等价。

#### Scenario: 台湾系统
- **GIVEN** `[ui] language=auto`，Windows 显示语言为 `zh-TW`
- **WHEN** MotionCalm 启动
- **THEN** 托盘菜单、设置窗口、toast 都是繁體中文

#### Scenario: 香港与繁体脚本
- **WHEN** 系统显示语言为 `zh-HK`、`zh-MO` 或 `zh-Hant-HK`
- **THEN** 界面语言为繁體中文

#### Scenario: 新加坡与简体脚本
- **WHEN** 系统显示语言为 `zh-SG`、`zh-Hans-HK` 或 `zh`
- **THEN** 界面语言为简体中文

#### Scenario: 其他语言
- **WHEN** 系统显示语言为 `en-US`、`ja-JP` 或无法取得
- **THEN** 界面语言为 English

#### Scenario: 手动指定
- **GIVEN** 系统显示语言为 `en-US`
- **WHEN** `[ui] language=zh-CN`
- **THEN** 界面语言为简体中文

### Requirement: 三种语言的文字覆盖范围
以下所有用户可见文字 SHALL 按当前界面语言显示：设置窗口（标题、分页标签、页面说明、分组标题、标签、复选框与单选框文字、下拉项、辅助说明、滑块读数及单位、编辑框占位提示、按钮、气泡提示、确认框、状态栏、快捷键行的状态文字、校准列表的列名与空表说明）、托盘菜单与状态行、全部 toast、校准提示与校准说明框、调试条、关于框、第二个实例启动时第一个实例显示的提示。以下内容 MUST NOT 随界面语言变化：日志文件的每一行、INI 模板中的注释（保持简体中文）、产品名 MotionCalm（包括托盘图标的提示文字）、exe 名、快捷键写法（如 `Ctrl + Alt + F9`）。

#### Scenario: 英文托盘菜单
- **GIVEN** 界面语言为 English
- **WHEN** 右键托盘图标
- **THEN** 菜单项为 "Enable mask"、"Mask strength"、"Trigger sensitivity"、"Center dot"、"Edge dots"、"Screen motion detection"、"Treat current program as a game"、"Add current program to exclusions"、"Calibrate current game…"、"Calibrations"、"Settings…"（默认项）、"Open config file"、"Reload config"、"Show debug info"、"Start with Windows"、"Open log"、"About MotionCalm"、"Exit"，状态行形如 "Game: cs2.exe · locked · Mask: Medium"

#### Scenario: 繁体 toast
- **GIVEN** 界面语言为繁體中文
- **WHEN** 按 Ctrl+Alt+F10 把遮罩切到强
- **THEN** toast 为「遮罩：強」

#### Scenario: 日志不变
- **GIVEN** 界面语言为 English
- **WHEN** 出现快捷键冲突
- **THEN** toast 为英文，日志里对应的行与简体中文界面下逐字相同

#### Scenario: INI 注释不变
- **GIVEN** 界面语言为 English
- **WHEN** 首次运行生成 MotionCalm.ini 或任何一次保存
- **THEN** 文件中的注释与 v1.1 模板相同（简体中文）

### Requirement: 文字表的一致性
每一条界面文字 SHALL 在三种语言中都有非空译文；带占位符（`{名称}`）的文字在三种语言中的占位符集合 SHALL 相同；简体中文文字带访问键（`(&X)`）时，另外两种语言的同一条文字 SHALL 也带访问键。单元测试 SHALL 检查这三条。

#### Scenario: 单元测试
- **WHEN** 执行 `build.cmd tests release`
- **THEN** 文字表测试打印条目数并全部通过；任何一条缺译、占位符不一致或缺访问键都会使测试失败并打印该条的 ID

### Requirement: 繁体中文用语
繁體中文文字 SHALL 使用台湾用语（例如 設定、滑鼠、螢幕、快速鍵、程式、預設、影片、設定檔、記錄檔、系統匣、控制器、擷取畫面、錄影、檔案、資料夾、選單、視窗、資訊、支援、載入、儲存、還原預設值、進階、使用者、開啟），引号用「」，MUST NOT 残留大陆用语。

#### Scenario: 繁体不残留大陆用语
- **WHEN** 扫描繁體中文文字表
- **THEN** 没有任何一条包含"设置""鼠標""屏幕""快捷鍵""默認""視頻""配置文件""日誌""托盤""手柄""菜單""窗口""信息""支持""加載""保存""用戶""打開"

#### Scenario: 繁体托盘
- **GIVEN** 界面语言为繁體中文
- **WHEN** 右键托盘图标
- **THEN** 有「設定…」「開機時自動啟動」「開啟記錄檔」「結束」，没有"设置""退出"

### Requirement: 英文用语
英文文字 SHALL 使用 Windows 标准术语与句首大写（Settings、Hotkeys、Calibration、Trigger、General、Reference dots、Mask、Restore defaults、Show advanced options、Start with Windows），打开对话框或需要进一步操作的命令带省略号（"Settings…"、"Restore defaults…"）。

#### Scenario: 英文按钮
- **GIVEN** 界面语言为 English
- **WHEN** 查看设置窗口底部
- **THEN** 按钮为 "Restore defaults…" 与 "Close"

### Requirement: 读数单位
滑块读数的单位 SHALL 按语言显示：简体 "°/秒"、"毫秒"、"像素"、"屏高的 60%"；繁体 "°/秒"、"毫秒"、"像素"、"螢幕高度的 60%"；英文 "°/s"、"ms"、"px"、"60% of screen height"。

#### Scenario: 繁体读数
- **GIVEN** 界面语言为繁體中文
- **WHEN** 查看遮罩页"中度·清晰區大小"滑块
- **THEN** 读数为"螢幕高度的 60%"

#### Scenario: 英文读数
- **GIVEN** 界面语言为 English
- **WHEN** 查看遮罩页的清晰区与出现速度滑块
- **THEN** 读数为 "60% of screen height" 与 "100 ms"

### Requirement: 切换语言立即生效
界面语言改变（设置窗口、外部修改 INI 后自动重载、托盘"重新加载配置"）后 SHALL 立即生效：设置窗口在下一次刷新时按新语言重建全部分页，保留当前分页与页内滚动位置；托盘菜单从下一次弹出起、overlay 文字（toast、调试条、校准提示）从下一帧起为新语言。已经显示中的 toast 不必重新翻译。

#### Scenario: 外部改 INI
- **GIVEN** 设置窗口打开在"快捷键"页，界面语言为简体中文
- **WHEN** 用户用记事本把 `[ui] language` 改为 `en` 并保存
- **THEN** 约 0.5 秒内设置窗口变为英文，仍在 "Hotkeys" 页

### Requirement: 不同语言下版式不截断
三种语言下，设置窗口的所有标签、读数、按钮、下拉项与状态栏文字 SHALL 完整显示（不被截断、不重叠）；辅助说明可以换行。设置窗口的字体 SHALL 按语言选择：简体中文 Microsoft YaHei UI、繁體中文 Microsoft JhengHei UI、English Segoe UI；overlay 文字同样按语言选择字体与排版语言。

#### Scenario: 逐页截图检查
- **WHEN** 分别以 `settingsdemo --lang zh-CN|zh-TW|en --advanced --screenshots test-output\i18n\<lang>` 截取全部分页（长页每隔约一屏一张）
- **THEN** 每张截图中没有被截断或重叠的文字

### Requirement: 多语言说明文档
发布包 SHALL 包含三份使用说明：`README.md`（简体中文）、`README.zh-TW.md`（繁體中文，台湾用语）、`README.en.md`（English），后两份是前者的完整译文，菜单与设置项的名字与对应语言的界面文字一致，链接与 mod 名称保持原样。每份 README 顶部 SHALL 有一行指向另外两份的语言链接。

#### Scenario: 打包
- **WHEN** 运行 `scripts\package.ps1`
- **THEN** zip 根目录有 `README.md`、`README.zh-TW.md`、`README.en.md`
