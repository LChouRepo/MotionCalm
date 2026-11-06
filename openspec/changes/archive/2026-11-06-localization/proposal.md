# Proposal

## Why

MotionCalm 到 v1.1 为止只有简体中文界面。用户的朋友圈里有用繁体中文（台湾）和英文 Windows 的玩家，他们要么看不懂设置窗口和托盘菜单，要么看到的是大陆用语（"设置""鼠标""屏幕"），体验很差。本变更（变更 6）为所有界面文字提供三种语言：简体中文（现有文字为源文本）、繁體中文（台湾用语，不是逐字转换）与英文（Windows 标准术语），默认按 Windows 显示语言选择。

## What Changes

- 新增零依赖的界面文字表：每条界面文字有稳定 ID，三种语言各一张编进 exe 的常量表；按 ID 取当前语言的文字，带 `{name}` 占位符的文字用格式化函数填入数值、exe 名、快捷键等。不用资源 DLL / MUI。
- 覆盖范围：设置窗口（全部分页、标签、辅助说明、读数单位、按钮、确认框、状态栏、分页标签、下拉项、气泡提示）、托盘菜单与状态行、全部 toast、校准提示与说明框、调试条、关于框、第二个实例的提示。日志行保持英文 / 原样；INI 模板的注释保持简体中文（不做本地化）。托盘提示文字只有产品名 MotionCalm，不翻译。
- 默认语言：`[ui] language=auto` 时按 `GetUserPreferredUILanguages`（失败时 `GetUserDefaultUILanguage`）的第一项：zh-TW / zh-HK / zh-MO / zh-Hant* → 繁體中文；zh-CN / zh-SG / zh-Hans* / 只有 zh → 简体中文；其余 → English。
- 配置：`[ui]` 新增 `language=auto`（auto | zh-CN | zh-TW | en，写在 `[ui]` 第一项）。"恢复默认设置"保留它（与 `[ui]` 其他键相同）。
- 设置窗口"通用"页最上方新增"语言"分组与"界面语言"下拉框（跟随系统 / 简体中文 / 繁體中文 / English，后三项永远用各自的语言显示）。改动立即生效：设置窗口按新语言重建全部分页（保留当前分页与页内滚动位置，焦点回到下拉框），托盘菜单下次弹出、overlay 文字下一帧即为新语言；按现有的 300 ms 防抖保存。
- 版式：英文与繁体文字更长，设置窗口按语言放宽标签栏与读数宽度、相应收窄滑块，字体按语言选择（Microsoft YaHei UI / Microsoft JhengHei UI / Segoe UI），overlay 的 DirectWrite 字体与排版语言同样随语言变化。
- 文档：新增 `README.zh-TW.md` 与 `README.en.md`（README.md 的完整译文，同样的用语规则），三份 README 顶部都有一行语言切换链接；`scripts/package.ps1` 把三份 README 都打进 zip。
- 工具：`settingsdemo` 新增 `--lang auto|zh-CN|zh-TW|en`，截图时每隔约一屏多截一张，供逐页检查截断与用语。
- 顺带修复：设置窗口初次载入时，隐藏分页里"应隐藏"的控件（通用页"--verbose 运行中"附注）在切到该页后仍然显示的问题。

不在范围内：INI 注释与日志的本地化、其他语言、从右到左排版、语言包热插拔。

## Capabilities

### New Capabilities

- `localization`: 界面语言的选择（自动 / 手动）、三种语言的文字覆盖范围与一致性（每条文字都有三种语言、占位符一致、访问键齐全）、用语与单位规则、切换后立即生效、版式不截断、多语言 README。

### Modified Capabilities

- `configuration`: MODIFIED「[ui] 节」（新增 `language` 键）。
- `settings-window`: ADDED「通用页界面语言」（下拉框、立即重建、保留分页与滚动位置）。

## Impact

- 新文件：`src/i18n/i18n.h/.cpp`、`src/i18n/strings.h`、`src/i18n/strings_zh_cn.cpp`、`strings_zh_tw.cpp`、`strings_en.cpp`、`tests/test_i18n.cpp`、`README.zh-TW.md`、`README.en.md`。
- 修改：`src/ui/*`（全部分页、外壳、纯逻辑文字、快捷键捕获状态）、`src/tray/tray.*`、`src/tray/hotkeys.*`（动作名按语言）、`src/app/app.*`、`src/app/app_settings.cpp`、`src/calib/calibration.*`、`src/overlay/renderer.*`、`src/config/config.*`、`src/res/MotionCalm.default.ini`、`src/res/resource.h`、`tools/settingsdemo/main.cpp`、`scripts/package.ps1`、`README.md`、`tests/test_config_access.cpp`。
- 依赖：无新增。构建脚本按目录自动收集源文件，无需改动。
- 体积：约 300 条文字 × 3 种语言，exe 增加约 60 KB。
