// gamepad.h — XInput 手柄轮询（0–3 号）：空槽每 2 s 才重新探测；右摇杆 → pad_deg_per_s，左摇杆 → pad_move（径向死区重映射，多手柄取最大）。
#pragma once

#include <array>

namespace mc {

class Gamepad {
public:
    static constexpr int kSlots = 4;
    static constexpr double kReprobeS = 2.0;   // 未连接槽位的重新探测间隔

    struct State {
        double pad_deg_per_s = 0;   // 右摇杆映射后幅值 × max_deg_per_s（各手柄取最大）
        double pad_move = 0;        // 左摇杆映射后幅值 0..1（各手柄取最大）
        int connected = 0;          // 本次读到的手柄数
    };

    // 轮询一次。deadzone 为径向死区（0..0.9），max_deg_per_s 为右摇杆推满时的角速度。
    State poll(double now_s, double deadzone, double max_deg_per_s);
    // 忘掉全部槽位状态（下次 poll 重新探测全部）。
    void reset();

private:
    std::array<bool, kSlots> connected_{};
    std::array<double, kSlots> next_probe_{};   // 未连接槽位下次探测的时刻；0 = 立即
};

}  // namespace mc
