# Spec Delta

## ADDED Requirements

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
