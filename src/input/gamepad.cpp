// gamepad.cpp — input/gamepad 的实现：XInputGetState 轮询 + apply_radial_deadzone。
#include "input/gamepad.h"

#include <windows.h>
#include <Xinput.h>

#include <algorithm>

#include "model/intensity.h"
#include "util/log.h"

namespace mc {

Gamepad::State Gamepad::poll(double now_s, double deadzone, double max_deg_per_s) {
    State out;
    for (int i = 0; i < kSlots; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (!connected_[slot] && now_s < next_probe_[slot]) continue;   // 空槽：2 s 内不再探测
        XINPUT_STATE st{};
        const DWORD rc = ::XInputGetState(static_cast<DWORD>(i), &st);
        if (rc != ERROR_SUCCESS) {
            if (connected_[slot]) log::info("gamepad {} disconnected", i);
            connected_[slot] = false;
            next_probe_[slot] = now_s + kReprobeS;
            continue;
        }
        if (!connected_[slot]) log::info("gamepad {} connected", i);
        connected_[slot] = true;
        ++out.connected;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        const double right = apply_radial_deadzone(g.sThumbRX, g.sThumbRY, deadzone);
        const double left = apply_radial_deadzone(g.sThumbLX, g.sThumbLY, deadzone);
        out.pad_deg_per_s = std::max(out.pad_deg_per_s, right * max_deg_per_s);
        out.pad_move = std::max(out.pad_move, left);
    }
    return out;
}

void Gamepad::reset() {
    connected_.fill(false);
    next_probe_.fill(0.0);
}

}  // namespace mc
