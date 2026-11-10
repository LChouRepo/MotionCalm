# 參與貢獻 MotionCalm

[English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | **繁體中文**

感謝參與！歡迎回報 bug、提供遊戲相容性回饋（「在某某遊戲裡能用 / 不能用」）、改進翻譯和送出 Pull Request。
安全性問題請見 [SECURITY.md](SECURITY.md)。

## 開發環境

- Windows 10（2004 以上）或 Windows 11，x64。
- Visual Studio 2022 或 2026（任何版本，或 Build Tools），安裝 **「使用 C++ 的桌面開發」** 工作負載
  （MSVC x64 工具 + Windows 10/11 SDK）。沒有其他相依套件，不需要套件管理員。
- `scripts\env.cmd` 用 `vswhere.exe` 尋找 Visual Studio。找不到時，把 `MC_VS_PATH` 設為 Visual Studio 的安裝資料夾
  （裡面有 `VC\Auxiliary\Build` 的那個），或把 `MC_VCVARS` 設為 `vcvars64.bat` 的完整路徑。
- 選用：Python 3（只有 `tools\make_icon.py` 會用到）、Node.js（只有 OpenSpec 命令列工具會用到）。

## 建置與測試

在存放庫根目錄的一般 `cmd` 視窗中執行（開發人員命令提示字元也可以）：

```bat
build.cmd all release        :: app + 工具 + 測試；tests.exe 自動執行
build.cmd tests debug        :: 目標：all | app | tools | tests；組態：debug | release
out\release\tests.exe        :: 只執行單元測試；失敗時結束代碼非 0
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.1.0   :: 發行套件，輸出到 out\
```

輸出在 `out\<組態>\`，中繼檔在 `out\<組態>\obj\`，不會寫入原始碼資料夾。CI 在每次 push 和 Pull Request 時執行的就是這些單元測試。

### 實機驗證指令碼

這些指令碼會端對端地檢查真正的程式。執行期間**會占用你的螢幕、滑鼠和鍵盤約 20 秒**：`fakegame.exe` 會開啟一個全螢幕視窗並隱藏、鎖住滑鼠游標，
`inputsim.exe` 會送出模擬的滑鼠移動。請先關閉其他全螢幕程式，執行時不要碰滑鼠鍵盤（任何按鍵都會讓 `fakegame` 提前結束），
也不要在遠端桌面中執行。CI **不會**執行它們。擷取畫面和記錄檔會寫到 `test-output\`（已被 git 忽略）。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1             # 滑鼠轉動 → 遮罩出現並淡出
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Capture    # 螢幕擷取動態偵測（平移）
powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Forward    # 前進 / 後退與傾斜偵測
powershell -ExecutionPolicy Bypass -File scripts\verify_calibration.ps1 # 遊戲內兩次校準
```

每個指令碼都會逐項印出 PASS / FAIL。如果你正在執行已發行的 MotionCalm，請先結束它：它占用了預設快速鍵，會讓快速鍵相關的檢查失敗。

其他工具：`capturetest.exe`（螢幕擷取與估計器耗時）、`fakegame.exe --selfcheck`（不開視窗的估計器自我檢查）、
`settingsdemo.exe --lang <zh-CN|zh-TW|en> --advanced --screenshots test-output\i18n\<語言>`（用假的宿主擷取設定視窗的每一頁）。

要用真實遊戲畫面調整螢幕擷取估計器：先用 `capturetest.exe --seconds 15 --record tests\fixtures\recordings\<名稱>.mcgrid` 錄一段片段，
再用 `capturetest.exe --replay <片段> [--csv OUT]` 離線重播，或把 `MC_REPLAY_DIR` 設為片段所在的資料夾後執行 `tests.exe replay_local`，
詳見 [tests/fixtures/README.md](tests/fixtures/README.md)。錄製檔是商業遊戲的畫面，**絕對不要提交**（`*.mcgrid` 與 `tests/fixtures/recordings/` 已列在 `.gitignore`）。

## OpenSpec 工作流程

程式行為用 [OpenSpec](https://github.com/Fission-AI/OpenSpec) 描述。規格與變更文件以簡體中文撰寫（見 `openspec/config.yaml`），
結構性標題和 SHALL / MUST 關鍵字保持英文。

- `openspec/specs/<功能>/spec.md`：目前的需求，每個功能一個資料夾。
- `openspec/changes/<變更>/`：進行中的變更：`proposal.md`、`design.md`、`tasks.md`，以及 `specs/` 下的增量規格。
- `openspec/changes/archive/<日期>-<變更>/`：已完成的變更；封存時會把增量規格合併進 `openspec/specs/`。

比修 bug 更大的改動：先提出變更，依任務實作並在 `tasks.md` 裡打勾，然後封存。送出 Pull Request 前執行 `openspec validate --strict`。
存放庫裡的 `.claude/skills/` 和 `.claude/commands/opsx/` 是給 Claude Code 用的輔助指令（`/opsx:propose`、`/opsx:apply`、`/opsx:archive` 等），
可用可不用，直接用 `openspec` 命令列效果一樣。小修正不需要建立變更資料夾。

[DESIGN.md](DESIGN.md) 是整個專案的共同理解。實作需要偏離它時，先修改 DESIGN.md 並說明原因。

## 程式碼規範

摘自 [DESIGN.md §13](DESIGN.md)：

- C++20、MSVC、`/W4 /permissive- /utf-8`。Win32 + Direct3D 11 + DirectComposition + Direct2D / DirectWrite。**不引入任何第三方相依套件。**
- COM 物件用 `Microsoft::WRL::ComPtr`；所有控制代碼都用 RAII 管理。
- 預期內的錯誤不用例外：初始化失敗時回傳 `bool` 並寫入記錄檔。
- Win32 字串用 `std::wstring`；原始檔為 UTF-8；寬字元字串常值 `L"…"`。
- 每個原始檔開頭用一行註解說明它的職責。
- 能做成純邏輯（不含 Win32）的部分就做成純邏輯，例如 `model/intensity` 和螢幕擷取估計器，並在 `tests/` 裡加上單元測試。
- 永遠不注入遊戲、不讀取遊戲記憶體、不安裝低階 hook（`WH_MOUSE_LL` / `WH_KEYBOARD_LL`）。MotionCalm 只使用 Raw Input、XInput 和桌面複製。
- `.cmd` 檔只能有 ASCII 字元，行尾為 CRLF（`.gitattributes` 會處理行尾）。
- 設定視窗的版面與文案規範：[docs/settings-ui.md](docs/settings-ui.md)。

## 新增或修改介面文字

所有給使用者看的文字都在 `src/i18n/`；記錄檔內容不翻譯。

1. 在 `src/i18n/strings.h` 的 `enum class StrId` 對應分組裡加一個 ID。
2. 在三張表的**同一位置**各加一行：`strings_zh_cn.cpp`（來源文字）、`strings_zh_tw.cpp`（台灣用語，不是簡繁轉換：設定、滑鼠、螢幕、快速鍵、程式、預設……）、
   `strings_en.cpp`（Windows 標準術語，句首大寫，會開啟對話方塊的命令加上「…」）。
3. 三種語言的 `{name}` 預留位置要一致；來源文字有便捷鍵（`&`）時，每種語言都要有。
4. 用 `i18n::tr(StrId::…)` 取得文字，有預留位置的用 `i18n::format(StrId::…, {…})`（見 `src/i18n/i18n.h`）。
5. 執行 `build.cmd tests`：任一張表裡某個 ID 為空或預留位置不一致時，`tests/test_i18n.cpp` 會失敗。
6. 設定視窗裡的文字要檢查有沒有被截斷：
   `out\release\settingsdemo.exe --lang zh-TW --advanced --screenshots test-output\i18n\zh-TW`（`zh-CN`、`en` 也照樣執行一次）。

如果改動影響到使用說明，三份都要修改：`docs/user-guide.zh-CN.md`、`docs/user-guide.zh-TW.md`、`docs/user-guide.en.md`。

## Pull Request 檢查清單

- [ ] `build.cmd all release` 成功，所有單元測試通過。
- [ ] 新邏輯有單元測試。
- [ ] 修改了輸入、遊戲判定、螢幕擷取、校準或繪製時，執行過實機驗證指令碼（在 PR 裡寫明執行了哪些、結果如何）。
- [ ] 新增的介面文字三種語言都有；需要時三份使用說明都已更新。
- [ ] 行為變更已新增 / 更新 OpenSpec 變更，`openspec validate --strict` 通過。
- [ ] `CHANGELOG.md` 的「Unreleased」下已更新。
- [ ] 程式碼、文件、指令碼裡沒有個人路徑、使用者名稱或本機特有的設定。

## 回報 bug

請使用 [bug 回報表單](https://github.com/LChouRepo/MotionCalm/issues/new/choose)，寫明遊戲、Windows 版本、顯示模式
（無邊框 / 視窗化 / 獨占全螢幕）、介面語言、MotionCalm 版本，並附上 `MotionCalm.log`（系統匣選單 → 開啟記錄檔）。
記錄檔裡有程式名稱（例如遊戲的 `.exe`），不想公開的內容請先刪除。

送出貢獻即表示你同意以 [MIT 授權條款](LICENSE) 授權你的貢獻。
