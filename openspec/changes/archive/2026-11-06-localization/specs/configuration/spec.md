# Spec Delta

## MODIFIED Requirements

### Requirement: [ui] 节
`[ui]` SHALL 保存界面语言与设置窗口自己的界面状态，位于 `[capture]` 之后（`[extra]` 之前），由设置窗口维护：`language`（auto | zh-CN | zh-TW | en，默认 auto，界面语言，规则见 localization「界面语言的选择」；读取时不区分大小写，写回时用这里列出的写法）；`show_advanced`（0 | 1，默认 0，各页是否显示"高级选项"）；`x`、`y`（整数，设置窗口上次关闭时左上角的屏幕坐标，默认留空 = 未设置，范围 −100000–100000）。`language` 为非法值时 SHALL 用 auto 并记一条 warn；缺少该键时用 auto、不记 warn。`x` / `y` 留空时不记 warn；不是整数时记 warn 并按未设置处理；越界截断并记 warn。这些键按设置窗口改动的保存时机写入，"恢复默认设置"不改变它们。

#### Scenario: 默认值
- **WHEN** 读取默认模板
- **THEN** `[ui]` 依次为 `language=auto`、`show_advanced=0`、`x=`、`y=`，默认配置按模板保存仍与模板逐字节相同

#### Scenario: 记住窗口位置
- **GIVEN** 设置窗口被拖到 (630, 196) 后关闭
- **WHEN** 之后再次打开设置窗口，鼠标指针在同一显示器上
- **THEN** INI 中 `x=630`、`y=196`，窗口出现在 (630, 196)

#### Scenario: 旧 INI 没有 [ui]
- **GIVEN** 由更早版本生成的 INI，没有 `[ui]` 节
- **WHEN** 新版本启动后因任一改动保存
- **THEN** 不记 warn，文件末尾（`[capture]` 之后）出现带注释的 `[ui]` 节

#### Scenario: v1.1 的 INI 没有 language
- **GIVEN** v1.1 生成的 INI，`[ui]` 只有 `show_advanced`、`x`、`y`
- **WHEN** 读取配置
- **THEN** 界面语言按 auto 处理，不记 warn；下一次保存时 `language=auto` 补写在 `[ui]` 第一项

#### Scenario: 非法语言
- **GIVEN** INI 中 `language=klingon`
- **WHEN** 读取配置
- **THEN** 按 auto 处理，日志有一条 warn

#### Scenario: 恢复默认保留语言
- **GIVEN** `language=en`
- **WHEN** 执行"恢复默认设置"
- **THEN** `language` 仍为 en，界面保持英文
