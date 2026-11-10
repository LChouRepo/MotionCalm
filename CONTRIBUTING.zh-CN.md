# 参与贡献 MotionCalm

[English](CONTRIBUTING.md) | **简体中文** | [繁體中文](CONTRIBUTING.zh-TW.md)

感谢参与！欢迎提交 bug、游戏兼容性反馈（"在某某游戏里能用 / 不能用"）、翻译改进和 Pull Request。
安全问题请看 [SECURITY.md](SECURITY.md)。

## 开发环境

- Windows 10（2004 及以上）或 Windows 11，x64。
- Visual Studio 2022 或 2026（任意版本，或 Build Tools），安装 **"使用 C++ 的桌面开发"** 工作负载
  （MSVC x64 工具 + Windows 10/11 SDK）。没有其他依赖，不用包管理器。
- `scripts\env.cmd` 用 `vswhere.exe` 查找 Visual Studio。找不到时，把 `MC_VS_PATH` 设为 Visual Studio 的安装文件夹
  （里面有 `VC\Auxiliary\Build` 的那个），或把 `MC_VCVARS` 设为 `vcvars64.bat` 的完整路径。
- 可选：Python 3（只有 `tools\make_icon.py` 用到）、Node.js（只有 OpenSpec 命令行工具用到）。

## 构建与测试

在仓库根目录的普通 `cmd` 窗口里运行（开发者命令提示符也可以）：

```bat
build.cmd all release        :: app + 工具 + 测试；tests.exe 自动运行
build.cmd tests debug        :: 目标：all | app | tools | tests；配置：debug | release
out\release\tests.exe        :: 只跑单元测试；失败时退出码非 0
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.0   :: 发布包，输出到 out\
```

输出在 `out\<配置>\`，中间文件在 `out\<配置>\obj\`，不会往源码目录里写东西。CI 在每次 push 和 Pull Request 时跑的就是这些单元测试。

### 实机验证脚本

这些脚本端到端地检查真正的程序。运行期间**会占用你的屏幕、鼠标和键盘约 20 秒**：`fakegame.exe` 打开一个全屏窗口并隐藏、锁住鼠标光标，
`inputsim.exe` 发送模拟的鼠标移动。请先关掉其他全屏程序，运行时不要碰鼠标键盘（任何按键都会让 `fakegame` 提前退出），
也不要在远程桌面里运行。CI **不会**运行它们。截图和日志写到 `test-output\`（已被 git 忽略）。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1             # 鼠标转动 → 遮罩出现并淡出
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Capture    # 截屏运动检测（平移）
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Forward    # 前进 / 后退与倾斜检测
powershell -ExecutionPolicy Bypass -File scripts\verify_calibration.ps1 # 游戏内两次校准
```

每个脚本都会逐项打印 PASS / FAIL。如果你正在运行已发布的 MotionCalm，请先退出：它占着默认快捷键，会让快捷键相关的检查失败。

其他工具：`capturetest.exe`（截屏与估计器耗时）、`fakegame.exe --selfcheck`（不开窗口的估计器自检）、
`settingsdemo.exe --lang <zh-CN|zh-TW|en> --advanced --screenshots test-output\i18n\<语言>`（用假宿主截取设置窗口的每一页）。

要用真实游戏画面调截屏估计器：先用 `capturetest.exe --seconds 15 --record tests\fixtures\recordings\<名字>.mcgrid` 录一段片段，
再用 `capturetest.exe --replay <片段> [--csv OUT]` 离线回放，或把 `MC_REPLAY_DIR` 设为片段所在文件夹后运行 `tests.exe replay_local`，
详见 [tests/fixtures/README.md](tests/fixtures/README.md)。录制文件是商业游戏的画面，**永远不要提交**（`*.mcgrid` 与 `tests/fixtures/recordings/` 已在 `.gitignore` 里）。

## OpenSpec 工作流

程序行为用 [OpenSpec](https://github.com/Fission-AI/OpenSpec) 描述。规格与变更文档用简体中文书写（见 `openspec/config.yaml`），
结构性标题和 SHALL / MUST 关键字保持英文。

- `openspec/specs/<能力>/spec.md`：当前的需求，每个能力一个文件夹。
- `openspec/changes/<变更>/`：进行中的变更：`proposal.md`、`design.md`、`tasks.md`，以及 `specs/` 下的增量规格。
- `openspec/changes/archive/<日期>-<变更>/`：已完成的变更；归档时会把增量规格合并进 `openspec/specs/`。

比修 bug 更大的改动：先提出变更，按任务实现并在 `tasks.md` 里打勾，然后归档。提交 Pull Request 前运行 `openspec validate --strict`。
仓库里的 `.claude/skills/` 和 `.claude/commands/opsx/` 是给 Claude Code 用的辅助命令（`/opsx:propose`、`/opsx:apply`、`/opsx:archive` 等），
可用可不用，直接用 `openspec` 命令行效果一样。小修复不需要建变更文件夹。

[DESIGN.md](DESIGN.md) 是整个项目的共同理解。实现需要偏离它时，先改 DESIGN.md 并说明原因。

## 代码规范

摘自 [DESIGN.md §13](DESIGN.md)：

- C++20、MSVC、`/W4 /permissive- /utf-8`。Win32 + Direct3D 11 + DirectComposition + Direct2D / DirectWrite。**不引入任何第三方依赖。**
- COM 对象用 `Microsoft::WRL::ComPtr`；所有句柄用 RAII 管理。
- 预期内的错误不用异常：初始化失败返回 `bool` 并写日志。
- Win32 字符串用 `std::wstring`；源文件 UTF-8；宽字符串字面量 `L"…"`。
- 每个源文件开头一行注释说明它的职责。
- 能做成纯逻辑（不含 Win32）的部分就做成纯逻辑，例如 `model/intensity` 和截屏估计器，并在 `tests/` 里加单元测试。
- 永远不注入游戏、不读游戏内存、不装低级钩子（`WH_MOUSE_LL` / `WH_KEYBOARD_LL`）。MotionCalm 只用 Raw Input、XInput 和桌面复制。
- `.cmd` 文件只能有 ASCII 字符，行尾 CRLF（`.gitattributes` 会处理行尾）。
- 设置窗口的版式与文案规范：[docs/settings-ui.md](docs/settings-ui.md)。

## 新增或修改界面文字

所有面向用户的文字都在 `src/i18n/`；日志行不翻译。

1. 在 `src/i18n/strings.h` 的 `enum class StrId` 对应分组里加一个 ID。
2. 在三张表的**同一位置**各加一行：`strings_zh_cn.cpp`（源文本）、`strings_zh_tw.cpp`（台湾用语，不是简繁转换：設定、滑鼠、螢幕、快速鍵、程式、預設……）、
   `strings_en.cpp`（Windows 标准术语，句首大写，会打开对话框的命令带"…"）。
3. 三种语言的 `{name}` 占位符要一致；源文本有访问键（`&`）时，每种语言都要有。
4. 用 `i18n::tr(StrId::…)` 取文字，带占位符的用 `i18n::format(StrId::…, {…})`（见 `src/i18n/i18n.h`）。
5. 运行 `build.cmd tests`：任一张表里某个 ID 为空或占位符不一致时，`tests/test_i18n.cpp` 会失败。
6. 设置窗口里的文字要检查有没有被截断：
   `out\release\settingsdemo.exe --lang en --advanced --screenshots test-output\i18n\en`（`zh-CN`、`zh-TW` 同样跑一遍）。

如果改动影响到使用说明，三份都要改：`docs/user-guide.zh-CN.md`、`docs/user-guide.zh-TW.md`、`docs/user-guide.en.md`。

## Pull Request 检查清单

- [ ] `build.cmd all release` 成功，所有单元测试通过。
- [ ] 新逻辑有单元测试。
- [ ] 改了输入、游戏判定、截屏、校准或渲染时跑过实机验证脚本（在 PR 里写明跑了哪些、结果如何）。
- [ ] 新增的界面文字三种语言都有；需要时三份使用说明都已更新。
- [ ] 行为变化已新增 / 更新 OpenSpec 变更，`openspec validate --strict` 通过。
- [ ] `CHANGELOG.md` 的 "Unreleased" 下已更新。
- [ ] 代码、文档、脚本里没有个人路径、用户名或本机特有的设置。

## 报告 bug

请使用 [bug 报告表单](https://github.com/LChouRepo/MotionCalm/issues/new/choose)，写明游戏、Windows 版本、显示模式
（无边框 / 窗口化 / 独占全屏）、界面语言、MotionCalm 版本，并附上 `MotionCalm.log`（托盘菜单 → 打开日志）。
日志里有程序名（比如游戏的 `.exe`），不想公开的内容请先删掉。

提交贡献即表示你同意以 [MIT 许可证](LICENSE) 授权你的贡献。
