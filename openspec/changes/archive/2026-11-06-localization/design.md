# Design

## Context

动机见 proposal.md，行为契约见 `specs/`。现状：界面文字以 `L"…"` 字面量散落在 `src/ui/pages/*`、`src/ui/settings_dlg.cpp`、`src/ui/settings_logic.cpp`、`src/ui/hotkey_capture.cpp`、`src/tray/*`、`src/app/app*.cpp`、`src/calib/calibration.*` 中；不少纯逻辑函数（读数格式、确认框文案、状态行、调试条字段）已有单元测试按简体中文原文断言。设置窗口的控件全部由代码在 `build()` 里创建，没有对话框模板里的文字（标题除外）。约束不变：C++20 / MSVC `/W4` 零警告、零第三方依赖、主线程不持锁。

## Goals / Non-Goals

**Goals:**
- 一处定义、三种语言、编译期就位；缺译、占位符不一致、缺访问键能被单元测试抓到。
- 切换语言不重启：设置窗口重建分页，其余文字本来就是"用时生成"。
- 现有测试（按简体中文断言）不需要改动。

**Non-Goals:**
- 不做资源 DLL / MUI、不读外部语言文件、不支持运行时加新语言。
- 不本地化日志、INI 注释、命令行参数错误（只进日志）、热键字符串解析错误（只进日志）。

## Decisions

### D1. 文字表：枚举 ID + 三个常量数组

`src/i18n/strings.h` 定义 `enum class StrId : uint16_t`（按界面区域分组，末尾 `Count`）；`strings_zh_cn.cpp` / `strings_zh_tw.cpp` / `strings_en.cpp` 各定义 `const wchar_t* const kStrings…[kStrCount]`，每行注释写 ID 名，顺序与枚举一致；`strings_zh_cn.cpp` 另有 `kStrNames`（ID 名字符串，测试报错时打印）。`mc::i18n::tr(StrId)` 取当前语言的文字（缺项回退简体中文），`mc::i18n::format(StrId, {{L"name", value}, …})` 替换 `{name}` 占位符。
- 选择枚举 + 数组而不是 `std::map<string, …>`：零运行时开销、编译期知道条目数、ID 拼错是编译错误。
- 选择命名占位符而不是 `std::format` 的 `{}`：不同语言的语序不同（例如"删除 {exe} 的校准" / "Delete the calibration for {exe}"），命名占位符允许任意顺序，并能被测试按集合比较；也避免把用户提供的 exe 名当成格式串。
- 访问键写在文字里（"预览强度(&P)"、"&Preview strength"）：英文按 Windows 惯例在单词里加 `&`，中文用括注；快捷键页的动作名由代码按每行固定的字母加访问键（英文里字母出现在名字中时就地加 `&`，否则加 " (&X)"）。

### D2. 当前语言与切换

`i18n::current()` / `set_language()` 是主线程上的一个全局值，进程启动时为简体中文（因此不调用 `set_language` 的单元测试照旧按中文断言）。App 在每次载入配置（启动、外部修改自动重载、托盘重新加载）时先调用 `apply_language()`（`resolve([ui] language)`），再做其余应用步骤，使随后的热键冲突 toast 等已是新语言；设置窗口改动 `language` 时 `apply_settings_change` 同样调用它。语言真的变了时 `settings_.refresh()`。
- `auto` 每次载入都重新推断（`GetUserPreferredUILanguages` 第一项，失败时 `GetUserDefaultUILanguage` → `LCIDToLocaleName`），系统改了显示语言后重新加载配置即可跟上。映射是纯函数 `lang_from_locale_name`，单测覆盖。

### D3. 设置窗口重建

`SettingsWindow` 记住创建分页时的语言 `built_lang_`；`WM_APP_REFRESH`（合并后的全页刷新）发现 `i18n::current() != built_lang_` 时调用 `rebuild_for_language()`：记下当前分页与页内滚动位置、焦点是否在语言下拉框 → 销毁 6 个分页 → 按新语言重建字体（雅黑 / 正黑 / Segoe UI）并换到分页标签、状态栏与按钮上 → 重填分页标签、标题、按钮文字 → 重新排版外壳 → 重新创建分页（Z 序紧跟分页标签，保持 Tab 顺序 = 视觉顺序）→ 切回原分页、恢复滚动、焦点放回下拉框。
- 选择"重建分页"而不是"逐个控件改文字"：分页的控件、行高、辅助说明的换行高度都在 `build()` 里按文字确定，逐个改文字需要每页维护一张"控件 → 文字 ID"表，重建则零额外代码；6 页重建约 20–40 ms，只在切换语言时发生。
- 重建由 posted 刷新消息触发，下拉框的 `CBN_SELCHANGE` 处理完之后才销毁它，不会在控件自己的通知里销毁自己。

### D4. 版式

`layout::label_w()` / `slider_w()` / `readout_w()` 按当前语言返回 96 DPI 像素：简体 168 / 260 / 76（与 v1.1 相同），繁体 168 / 232 / 112（"螢幕高度的 100%" 约 100 px），英文 196 / 208 / 132（最长标签约 190 px，"100% of screen height" 约 125 px）。总宽 ≤ 分页可用宽度（约 600 px，含滚动条）。校准列表列宽改为 210 / 150 / 110（"Counts per degree" 放得下）。辅助说明本来就按宽度换行。英文状态栏文字缩短为 "Changes apply and save automatically · Saved 21:04:33"，避免省略号。overlay 的 DirectWrite 文本格式按语言选字体与排版语言（zh-CN / zh-TW / en-US），语言变化时重建；初始化时的字形预热样本改为从当前语言的常用 toast / 调试条 / 校准提示拼出。

### D5. 不本地化的部分

日志（`log::*`）保持英文 / 原样，包括其中夹带的中文档位名（`level_name_zh`），便于对照旧日志与验证脚本；INI 模板注释保持简体中文（模板逐字节写回的规则不变）；托盘提示只有 "MotionCalm"。`HotkeyActionInfo::name` 保留简体中文源文本（测试与日志用），界面一律走 `hotkey_action_label()`。

### D6. 文字来源与用语

简体中文为源文本：v1.1 界面上的原文逐字搬进表里（新增的通用页"语言"分组与 v1.1 前进 / 倾斜阈值表的行标签除外）。繁体按台湾用语改写（见 specs「用语与单位」；例如"通用"页在 Windows 繁体中惯称"一般"、"参照物"页称"參考點"、"手柄"称"控制器"、"死区"保留"死區"），英文按 Windows 术语与句首大写。单元测试扫描繁体表中的大陆用语。

### D7. 截图验证

`settingsdemo --lang <L> --advanced --screenshots test-output\i18n\<L>` 逐页截图，长页每隔约 2/3 窗口高度多截一张（`_s1`、`_s2`…），最后一张滚到底（`_end`），每次运行约 6 s；截图后人工检查截断与用语。`--selftest` 新增"通用页切换语言"检查（英文 → 繁体 → 简体，标题、分页标签、页内状态）。

### D8. 顺带修复：隐藏分页中的"应隐藏"控件

版式引擎用 `IsWindowVisible` 判断控件当前是否显示；分页隐藏时它对子控件总是 FALSE，导致在分页隐藏期间被标为"隐藏"的控件（通用页"--verbose 运行中"附注）保留 `WS_VISIBLE`，切到该页时仍然显示。改为看控件自己的 `WS_VISIBLE` 样式。

## Risks / Trade-offs

- [新增文字忘记三语] → 单元测试检查每个 ID 三种语言非空、占位符一致、访问键齐全；数组长度固定为 `kStrCount`，少写的条目为 `nullptr` 会被测试抓到。
- [默认 auto 在英文系统上显示英文] 开发者本机（英文 Windows）默认会看到英文界面；朋友的中文系统按系统语言显示。需要固定语言时在通用页选择或写 `[ui] language`。
- [繁体用语的判断] 少数词（"死區"、"處理程序"、"攔截程式（hook）"、"等級"）是在几种常见说法之间的选择，见 tasks.md 的"待复核用语"，交给用户复核。
- [英文访问键] 英文标签的访问键字母与中文不同（例如 "Fade-&in time"），每页内仍互不重复。

## Migration Plan

旧 INI 没有 `[ui] language` → 按 auto 处理、不记 warn；下次保存按模板补写。无需迁移其他数据。
