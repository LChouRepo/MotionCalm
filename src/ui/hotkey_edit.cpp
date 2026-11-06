// hotkey_edit.cpp — ui/hotkey_edit 的实现：子类化过程（design D6）。
#include "ui/hotkey_edit.h"

#include <commctrl.h>

#include <memory>

namespace mc::ui {
namespace {

constexpr UINT_PTR kSubclassId = 0x4D43484B;   // 'MCHK'
constexpr const wchar_t* kStateProp = L"MotionCalm.HotkeyEdit";

struct EditState {
    int row = 0;
    HotkeyEditSink* sink = nullptr;
    HotkeyCapture capture;
};

using ImmAssociateContextExFn = BOOL(WINAPI*)(HWND, void*, DWORD);

// 关闭该窗口的输入法（中文输入法默认开着，按键会先变成 VK_PROCESSKEY）。imm32 动态加载，不改链接清单。
void disable_ime(HWND edit) {
    static ImmAssociateContextExFn fn = [] {
        HMODULE m = ::LoadLibraryExW(L"imm32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m != nullptr ? reinterpret_cast<ImmAssociateContextExFn>(::GetProcAddress(m, "ImmAssociateContextEx"))
                            : nullptr;
    }();
    if (fn != nullptr) fn(edit, nullptr, 0);
}

void emit(EditState* st, const CaptureStep& step) {
    if (step.kind != CaptureKind::Ignore && st->sink != nullptr) st->sink->hotkey_step(st->row, step);
}

LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto* st = reinterpret_cast<EditState*>(ref);
    switch (msg) {
        case WM_GETDLGCODE: {
            const LRESULT def = ::DefSubclassProc(hwnd, msg, wp, lp);
            const auto* m = reinterpret_cast<const MSG*>(lp);
            if (m != nullptr && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
                capture_passes_to_dialog(static_cast<std::uint32_t>(m->wParam), current_modifiers())) {
                return def & ~static_cast<LRESULT>(DLGC_WANTALLKEYS | DLGC_WANTTAB);   // Tab / Shift+Tab：焦点导航
            }
            return def | DLGC_WANTALLKEYS;   // Esc、Enter、方向键都交给捕获框
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const bool repeat = (lp & (1 << 30)) != 0;   // 自动重复：不重复提交，也不让按住的修饰键覆盖已提交的值
            if (!repeat) emit(st, st->capture.key_down(static_cast<std::uint32_t>(wp), current_modifiers()));
            return 0;   // 全部吞掉：防止 Alt 激活菜单、提示音
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            emit(st, st->capture.key_up(static_cast<std::uint32_t>(wp), current_modifiers()));
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_UNICHAR:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
        case WM_CONTEXTMENU:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_CHAR: return 0;
        case WM_SETFOCUS: {
            st->capture.reset();
            const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
            ::SendMessageW(hwnd, EM_SETSEL, 0, -1);
            if (st->sink != nullptr) st->sink->hotkey_focus(st->row, true);
            return r;
        }
        case WM_KILLFOCUS: {
            if (st->capture.pending()) {
                st->capture.reset();
                CaptureStep restore;
                restore.kind = CaptureKind::Restore;
                emit(st, restore);
            }
            const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
            if (st->sink != nullptr) st->sink->hotkey_focus(st->row, false);
            return r;
        }
        case WM_NCDESTROY: {
            ::RemoveWindowSubclass(hwnd, edit_proc, id);
            ::RemovePropW(hwnd, kStateProp);
            std::unique_ptr<EditState> owned(st);
            return ::DefSubclassProc(hwnd, msg, wp, lp);
        }
        default: break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

std::uint32_t current_modifiers() {
    std::uint32_t m = 0;
    if (::GetKeyState(VK_CONTROL) < 0) m |= kModCtrl;
    if (::GetKeyState(VK_MENU) < 0) m |= kModAlt;
    if (::GetKeyState(VK_SHIFT) < 0) m |= kModShift;
    if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) m |= kModWin;
    return m;
}

bool attach_hotkey_edit(HWND edit, int row, HotkeyEditSink* sink) {
    auto st = std::make_unique<EditState>();
    st->row = row;
    st->sink = sink;
    if (!::SetWindowSubclass(edit, edit_proc, kSubclassId, reinterpret_cast<DWORD_PTR>(st.get()))) return false;
    ::SetPropW(edit, kStateProp, st.get());
    st.release();   // 由 WM_NCDESTROY 或 detach_hotkey_edit 释放
    disable_ime(edit);
    return true;
}

void detach_hotkey_edit(HWND edit) {
    // 不用 GetWindowSubclass：它不在系统目录的 comctl32 5.x 导出表里（tests.exe 没有 v6 manifest 也要能加载）。
    auto* st = edit != nullptr ? static_cast<EditState*>(::GetPropW(edit, kStateProp)) : nullptr;
    if (st == nullptr) return;
    ::RemoveWindowSubclass(edit, edit_proc, kSubclassId);
    ::RemovePropW(edit, kStateProp);
    delete st;
}

}  // namespace mc::ui
