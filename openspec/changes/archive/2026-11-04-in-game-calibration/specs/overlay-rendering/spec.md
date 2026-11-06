# Spec Delta

## ADDED Requirements

### Requirement: 校准提示

当前帧带有校准提示时，overlay SHALL 把它画在屏幕中上方：
- 文字块水平居中，上沿位于 overlay 高度的 22% 处；
- 每行水平居中，用"Microsoft YaHei UI" 18 DIP（乘以 DPI / 96），白色；
- 背景为 alpha 0.6 的黑色圆角矩形，四周留 12 DIP 的内边距。

提示 MUST 画在遮罩和参照点之上，不受遮罩影响；与 toast 同时出现时，两者都要可见（toast 在顶部，提示在其下方）。提示内容变化（例如计数增长）时 MUST 在下一帧更新。提示存在期间，即使不满足"显示条件"，也 SHALL 以仅文字模式显示。

#### Scenario: 1920×1080 上的位置

- **GIVEN** 96 DPI，overlay 为 1920×1080，校准处于 Run1
- **WHEN** 渲染
- **THEN** 提示背景的上沿约在 y = 238（1080 × 22%），文字块水平居中，两行都可读

#### Scenario: 遮罩满强度时仍清晰

- **WHEN** 校准转圈导致强度 = 1（强档）
- **THEN** 提示的文字和背景与 i = 0 时完全相同，没有被压暗

#### Scenario: 计数实时刷新

- **WHEN** 鼠标持续转动
- **THEN** "已转 N 计数"每帧更新，转动停止后数字不再变化

#### Scenario: 与 toast 共存

- **GIVEN** 正在显示校准提示
- **WHEN** 出现一条 toast
- **THEN** toast 在顶部居中，提示仍在原位置，两者互不遮挡

#### Scenario: 校准结束

- **WHEN** 状态回到 Idle
- **THEN** 下一帧起提示消失
