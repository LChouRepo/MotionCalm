// hotkey_edit.h — 按键捕获框：子类化只读 Edit（comctl32 v6 SetWindowSubclass），把 WM_KEYDOWN / WM_SYSKEYDOWN 等交给 HotkeyCapture 状态机，结果回调给快捷键页；关闭该框的输入法、吞掉字符 / 粘贴 / 右键菜单。
#pragma once

#include <windows.h>

#include "ui/hotkey_capture.h"

namespace mc::ui {

// 捕获框的回调（快捷键页实现）。row = 动作表序号。
class HotkeyEditSink {
public:
    virtual ~HotkeyEditSink() = default;
    // 捕获框获得 / 失去键盘焦点。
    virtual void hotkey_focus(int row, bool focused) = 0;
    // 一次按键的结果（kind != Ignore）。
    virtual void hotkey_step(int row, const CaptureStep& step) = 0;
};

// 当前按住的修饰键（kMod* 位，GetKeyState）。
std::uint32_t current_modifiers();

// 把 edit 变成捕获框。edit 应为 ES_READONLY 的单行 Edit；sink 的生命周期须覆盖 edit（或先 detach）。
bool attach_hotkey_edit(HWND edit, int row, HotkeyEditSink* sink);
// 撤销子类化（sink 先于 edit 销毁时调用）。
void detach_hotkey_edit(HWND edit);

}  // namespace mc::ui
