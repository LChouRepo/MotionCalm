# Spec Delta

## MODIFIED Requirements

### Requirement: 显示条件

只有当 `enabled=1` 且前台被判定为 `in_game`（见 game-detection）时，overlay 才 SHALL 绘制遮罩和参照点；否则 MUST NOT 绘制。不画遮罩也不画参照点时，overlay 窗口 SHALL 隐藏。例外有两个："仅文字"模式（见 toast 与调试条两条需求），以及设置窗口打开且预览强度大于 0 时的预览模式（见「设置窗口预览强度」）。

#### Scenario: 进入游戏后显示参照点

- **GIVEN** `enabled=1`，用户没有任何输入
- **WHEN** 前台切换到一个 `in_game` 为真的程序
- **THEN** 下一个 tick 内（≤ 50 ms），中心点和强度为 0 时的边缘点出现在该程序所在显示器上

#### Scenario: 切到非游戏程序后隐藏

- **GIVEN** overlay 正在游戏上显示，设置窗口未打开
- **WHEN** 前台切换到 `explorer.exe` 或浏览器
- **THEN** 50 ms 内 overlay 不再显示遮罩和参照点，窗口隐藏

#### Scenario: 关闭遮罩

- **GIVEN** 设置窗口未打开
- **WHEN** `enabled` 变为 0
- **THEN** 遮罩和参照点立即消失；此后只会出现 toast 或调试条（仅文字模式）

#### Scenario: 预览模式例外

- **GIVEN** 前台是设置窗口（`in_game` 为假），`enabled=0`
- **WHEN** 把预览强度拖到 50%
- **THEN** overlay 在设置窗口所在显示器上绘制遮罩与参照点

## ADDED Requirements

### Requirement: 设置窗口预览强度
设置窗口打开且预览强度 p > 0 时，overlay SHALL 进入预览模式：`in_game` 为真时显示在游戏所在显示器，否则显示在设置窗口所在显示器；绘制遮罩与参照点，不受 `enabled` 与 `in_game` 限制；绘制所用强度为 max(强度模型输出 value, p)，档位、颜色与点样式取当前配置。p 只作用于渲染，MUST NOT 改变强度模型的内部状态。p 回到 0 或设置窗口关闭后，SHALL 在下一个 tick 恢复正常的显示条件与强度。

#### Scenario: 桌面上满强度预览
- **GIVEN** 1920×1080 显示器，档位为强，设置窗口在该显示器上，前台不是游戏
- **WHEN** 把预览强度拖到 100%
- **THEN** 50 ms 内 overlay 出现在该显示器上，顶边中点的遮罩 alpha 为 0.85（误差 ±0.005），中心点可见

#### Scenario: 半强度预览
- **GIVEN** 同上
- **WHEN** 预览强度为 50%
- **THEN** 四个角的 alpha 为 0.425（误差 ±0.005），顶边中点 alpha 为 0

#### Scenario: 游戏中取较大值
- **GIVEN** 设置窗口开着但前台是已知游戏，预览强度为 30%
- **WHEN** 鼠标转动使强度模型输出 0.8
- **THEN** overlay 按强度 0.8 绘制；转动停止、模型输出降到 0 后，overlay 停在强度 0.3

#### Scenario: 关闭窗口后恢复
- **GIVEN** 前台是设置窗口，预览强度为 60%
- **WHEN** 关闭设置窗口，前台变为桌面
- **THEN** 下一个 tick 内遮罩消失、overlay 隐藏；强度模型的 value 一直保持为 0
