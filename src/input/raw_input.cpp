// raw_input.cpp — input/raw_input 的实现：RegisterRawInputDevices + WM_INPUT / GetRawInputBuffer 解析（只计相对位移，跟踪 8 个移动键）。
#include "input/raw_input.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "util/clock.h"
#include "util/log.h"

namespace mc {
namespace {

constexpr std::size_t kBufferBytes = 32 * 1024;   // 约 680 个鼠标包；不够时循环再读

// 虚拟键 → keys_ 下标；不是移动键返回 -1。
int move_key_index(USHORT vk) {
    switch (vk) {
        case 'W': return 0;
        case 'A': return 1;
        case 'S': return 2;
        case 'D': return 3;
        case VK_UP: return 4;
        case VK_DOWN: return 5;
        case VK_LEFT: return 6;
        case VK_RIGHT: return 7;
        default: return -1;
    }
}

// GetRawInputBuffer 缓冲里的下一个包（等价于 NEXTRAWINPUTBLOCK：x64 上按 8 字节对齐；该宏依赖的 QWORD 在
// WIN32_LEAN_AND_MEAN 下没有定义）。
RAWINPUT* next_block(RAWINPUT* ri) {
    const auto p = reinterpret_cast<std::uintptr_t>(ri) + ri->header.dwSize;
    return reinterpret_cast<RAWINPUT*>((p + 7u) & ~static_cast<std::uintptr_t>(7u));
}

}  // namespace

bool RawInput::register_devices(HWND target) {
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;   // 鼠标
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = target;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;   // 键盘
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = target;
    if (!::RegisterRawInputDevices(rid, static_cast<UINT>(std::size(rid)), sizeof(RAWINPUTDEVICE))) {
        log::warn("RegisterRawInputDevices failed: error={}", ::GetLastError());
        return false;
    }
    registered_ = true;
    timeline_.reset(qpc_now_s());
    velocity_.set_max_hold(kMaxHoldS);
    return true;
}

void RawInput::unregister_devices() {
    if (!registered_) return;
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_REMOVE;
    ::RegisterRawInputDevices(rid, static_cast<UINT>(std::size(rid)), sizeof(RAWINPUTDEVICE));
    registered_ = false;
}

void RawInput::add_batch(double now_s, double dx, double dy, double path, std::uint32_t n) {
    const double start = timeline_.on_read(now_s, n > 0);
    if (n == 0) return;
    velocity_.add_span(start, now_s, dx, dy, path, n);
    path_.add_span(start, now_s, dx, dy, path, n);
    mouse_events_ += n;
}

void RawInput::notify_dx(long long dx) const {
    if (dx != 0 && on_relative_dx_) on_relative_dx_(dx);
}

void RawInput::on_keyboard(const RAWKEYBOARD& k, double t) {
    if (k.VKey == 0xFF) return;   // 转义序列的一部分，不是真实按键
    last_key_t_ = t;              // 任何键盘消息（含自动重复）都刷新
    const int idx = move_key_index(k.VKey);
    if (idx >= 0) keys_[static_cast<std::size_t>(idx)] = (k.Flags & RI_KEY_BREAK) == 0;
}

void RawInput::handle(LPARAM lparam) {
    // RAWINPUT 含指针 / 64 位成员，用 RAWINPUT 本身做缓冲保证对齐；鼠标与键盘包都不超过它。
    RAWINPUT ri;
    UINT size = sizeof(ri);
    const UINT got = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &ri, &size,
                                       sizeof(RAWINPUTHEADER));
    if (got == static_cast<UINT>(-1) || got < sizeof(RAWINPUTHEADER)) return;   // 已被 drain 读走
    const double now = qpc_now_s();   // 处理时刻的 QPC（GetMessageTime 只有毫秒且会回绕）
    if (ri.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = ri.data.mouse;
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {   // 远程桌面、绘图板等绝对坐标：忽略
            ++absolute_events_;
            return;
        }
        if (m.lLastX == 0 && m.lLastY == 0) return;   // 只有按键 / 滚轮
        const double dx = static_cast<double>(m.lLastX);
        const double dy = static_cast<double>(m.lLastY);
        add_batch(now, dx, dy, std::sqrt(dx * dx + dy * dy), 1);
        notify_dx(m.lLastX);
    } else if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        on_keyboard(ri.data.keyboard, now);
    }
}

void RawInput::drain(double now_s) {
    if (!registered_) return;
    if (buffer_.empty()) buffer_.resize(kBufferBytes / sizeof(std::uint64_t));
    double sx = 0, sy = 0, path = 0;
    long long sum_dx = 0;   // 整数 Σdx（校准计数用）
    std::uint32_t n_mouse = 0;
    for (int round = 0; round < 64; ++round) {
        UINT size = static_cast<UINT>(buffer_.size() * sizeof(std::uint64_t));
        auto* first = reinterpret_cast<RAWINPUT*>(buffer_.data());
        const UINT n = ::GetRawInputBuffer(first, &size, sizeof(RAWINPUTHEADER));
        if (n == 0 || n == static_cast<UINT>(-1)) break;
        RAWINPUT* ri = first;
        for (UINT i = 0; i < n; ++i) {
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
                    ++absolute_events_;
                } else if (m.lLastX != 0 || m.lLastY != 0) {
                    const double dx = static_cast<double>(m.lLastX);
                    const double dy = static_cast<double>(m.lLastY);
                    sx += dx;
                    sy += dy;
                    sum_dx += m.lLastX;
                    path += std::sqrt(dx * dx + dy * dy);
                    ++n_mouse;
                }
            } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                on_keyboard(ri->data.keyboard, now_s);
            }
            ri = next_block(ri);
        }
    }
    // 这批位移在 (start, now] 内均匀发生（start 见 BatchTimeline）；速度窗口按重叠比例求和。
    add_batch(now_s, sx, sy, path, n_mouse);
    notify_dx(sum_dx);
}

bool RawInput::moving(double now_s) {
    if (now_s - last_key_t_ >= kStaleKeysS) clear_keys();
    for (bool k : keys_) {
        if (k) return true;
    }
    return false;
}

void RawInput::clear_keys() { keys_.fill(false); }

}  // namespace mc
