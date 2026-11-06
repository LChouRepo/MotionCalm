# Tasks

## 1. 文字表与语言选择（纯逻辑）

- [x] 1.1 `src/i18n/strings.h`（`StrId` 枚举，按界面区域分组，308 条）与 `strings_zh_cn.cpp`（v1.1 原文）、`strings_zh_tw.cpp`（台湾用语）、`strings_en.cpp`（Windows 英文术语）三张 `constexpr` 表；`kStrNames` 供测试打印（design D1）。验证：`tests/test_i18n.cpp` 的 `i18n_every_id_has_text_in_all_languages`（打印 "308 strings x 3 languages"）、`i18n_placeholders_match_across_languages`、`i18n_access_keys_present_in_all_languages` 通过。
- [x] 1.2 `src/i18n/i18n.h/.cpp`：`current / set_language`、`tr`（缺项回退简体）、`format` / `format_text`（`{name}` 占位符）、`placeholders`、`lang_from_locale_name`、`detect_system_language`（`GetUserPreferredUILanguages` → `GetUserDefaultUILanguage`）、`resolve`、`parse_lang_setting`、`lang_code`（design D2）。验证：`i18n_format_text`、`i18n_locale_name_mapping`（zh-TW / HK / MO / Hant → 繁体；CN / SG / Hans / zh → 简体；其余 → en）通过。
- [x] 1.3 `[ui] language`（auto | zh-CN | zh-TW | en，默认 auto，`[ui]` 第一项，模板带中文注释）；"恢复默认设置"保留。验证：`i18n_config_language_key`（读写、非法值 warn、恢复默认保留）、`config_access_get_matches_template_for_every_key`（84 个键）、`ini_template_matches_defaults` 通过。

## 2. 界面接入

- [x] 2.1 设置窗口：6 个分页、外壳（标题、分页标签、状态栏、"恢复默认…""关闭"、确认框标题）、版式引擎的气泡提示与"显示高级选项"、`settings_logic`（读数单位、范围、名单校验、已知游戏计数、校准指引 / 空表说明、三个确认框、截屏复选框后缀）、快捷键捕获状态文字与"无"、快捷键页动作名（`hotkey_action_label`）与访问键。验证：`i18n_switching_changes_ui_text`（繁体 "螢幕高度的 60%"、英文 "60% of screen height" / "100 ms" / "Allowed range: 1–5000" 等）通过；既有按简体断言的测试全部仍通过。
- [x] 2.2 通用页"语言"分组与"界面语言"下拉框（`IDC_GEN_LANGUAGE` 1650）；`SettingsWindow::rebuild_for_language()` 在刷新时发现语言变化即重建分页（保留分页、滚动、焦点）；字体按语言（design D3）。验证：`settingsdemo --selftest` 新增 4 项（下拉框 4 项；选 English → 标题 "MotionCalm Settings"、标签 "Mask" / "Hotkeys"、仍在通用页、按钮 "Clear &known games…"；选繁體 → 「MotionCalm 設定」「快速鍵」；选简体 → 复原且"已知游戏：2 个"保留）PASS。
- [x] 2.3 托盘菜单与状态行、全部 toast、调试条（含截屏字段）、校准提示 / toast / 说明框、关于框、第二个实例提示；`App::apply_language()` 在每次载入配置与设置窗口改动语言时生效；日志行、`level_name_zh` 日志用名保持原样（design D2、D5）。验证：`i18n_switching_changes_ui_text`（状态行、截屏菜单项、调试条 "paused"）、`test_calibration` / `test_capture_service` 既有中文断言通过；`build.cmd all release` 0 警告。
- [x] 2.4 overlay 文字：DirectWrite 字体与排版语言随语言重建，字形预热样本按当前语言拼出（design D4）。验证：`build.cmd all debug` 0 警告、D3D 调试层 0 条消息。
- [x] 2.5 版式按语言放宽：标签栏 / 滑块 / 读数 = 简体 168 / 260 / 76、繁体 168 / 232 / 112、英文 196 / 208 / 132；校准列表列宽 210 / 150 / 110；顺带修复隐藏分页中"应隐藏"控件仍显示（design D4、D8）。验证：见 3.1 的截图。

## 3. 截图检查、文档与打包

- [x] 3.1 `settingsdemo --lang`，截图时长页每隔约一屏多截一张；分别运行 `out\release\settingsdemo.exe --lang zh-CN|zh-TW|en --advanced --screenshots test-output\i18n\<lang>`（每次约 6 s，窗口用 BitBlt/CAPTUREBLT 截取后关闭），逐张检查。验证：三种语言共 43 张截图，无截断、无重叠；英文快捷键页 "Screen motion detection on/off (P)"、"Exit (Q)" 用括注访问键。
- [x] 3.2 `README.zh-TW.md`、`README.en.md`（README.md 的完整译文，菜单与设置项名称与对应语言的界面文字一致，链接与 mod 名称原样）；三份 README 顶部加语言链接行；README.md 的通用页说明加上界面语言。验证：人工通读。
- [x] 3.3 `scripts/package.ps1` 打包三份 README。验证：PowerShell 解析无错误（未实际打包）。
- [x] 3.4 `build.cmd all debug` 与 `build.cmd all release`：0 警告（/W4），282 个单元测试全部通过；`fakegame --selfcheck` PASS；`settingsdemo --selftest` 只有 2 项快捷键占用检查失败（本机运行中的 v1.0.0 占用 Ctrl+Alt+F9，与本变更无关）。
- [ ] 3.5 实机检查托盘菜单、toast、调试条、校准提示与关于框在三种语言下的显示（需要运行 MotionCalm.exe，本机有 v1.0.0 实例占用单实例互斥量）。 <!-- 未验证：由主代理在用户退出 v1.0.0 后运行 -->

## 待复核用语（交给用户确认）

- 繁体：分页"通用"译为「一般」（Windows 繁体惯用），"参照物"页译为「參考點」；"轻档 / 中档 / 强档"译为「輕度 / 中度 / 強度」（"強度·最深程度"读起来与"强度"一词重叠，可改「強檔」）；"手柄死区"译为「控制器死區」；"截屏运动检测"译为「螢幕擷取動態偵測」；"录屏可见性"译为「錄影時是否可見」；"画面匹配要求"译为「畫面比對門檻」；"重测"译为「重新測量」；"循环"译为「循環切換」；关于框"进程 / 钩子"译为「處理程序 / 攔截程式（hook）」；README 的"晕车药（茶苯海明等）"译为「暈車藥（例如 Dramamine）」。
- 英文："遮罩"译为 Mask（也可用 Vignette）；"参照物"页为 "Reference dots"；"截屏运动检测"为 "Screen motion detection"；快捷键页动作名用短形式（"Mark current program as game"、"Exclude current program"），托盘菜单用长形式（"Treat current program as a game"、"Add current program to exclusions"）；调试条用 "Pad"、"Capture"；倍数读数写作 "1.25×"；"单键会在游戏里失灵"为 "A key without Ctrl/Alt won't work right in games"；通用页语言标签为 "Display language"（中文"界面语言"），没有用三语并列的"语言 / 語言 / Language"，靠选项用各自语言显示来保证认得出。

## Workflow follow-up

- 主代理在用户退出 v1.0.0 后完成 3.5，勾选并记录。
- 用户复核上面的"待复核用语"后，必要时只改 `src/i18n/strings_zh_tw.cpp` / `strings_en.cpp` 与两份 README。
- 验证通过后归档：`openspec archive localization`（在 `forward-and-roll-detection` 归档之后，两者都修改 configuration 规格的不同需求）。
