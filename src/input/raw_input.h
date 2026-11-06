// raw_input.h — Raw Input 后台采集（RIDEV_INPUTSINK，无钩子）：鼠标相对位移进速度窗口与 100 ms 路径窗口，跟踪 WASD / 方向键按下状态。
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "model/intensity.h"

namespace mc {

class RawInput {
public:
    static constexpr double kPathWindowS = 0.100;   // 锁定判定用的原始位移路径窗口
    static constexpr double kStaleKeysS = 5.0;      // 这么久没有任何键盘消息就清空按下状态
    static constexpr double kMaxHoldS = 0.025;      // 速度窗口的数据地平线保持上限（见 VelocityWindow）
    // 最近 100 ms 内的原始鼠标事件不超过这么多（≤ 600 Hz）时，主循环值得为每个事件醒来打时间戳（见 wants_event_wake）。
    static constexpr std::uint32_t kWakeMaxEvents100ms = 60;

    // 在 target 窗口上注册鼠标（0x01/0x02）与键盘（0x01/0x06），RIDEV_INPUTSINK。失败返回 false（已记日志）。
    bool register_devices(HWND target);
    // 撤销注册（RIDEV_REMOVE）。
    void unregister_devices();

    // 可选回调：每次记入相对鼠标位移时，以这批位移的水平分量之和（有符号计数，绝对坐标事件已被过滤）调用；
    // handle() 每个事件调用一次，drain() 每次读取调用一次（Σdx ≠ 0 时）。在消息处理中同步执行（变更 2 的校准计数）。
    void set_on_relative_dx(std::function<void(long long dx)> fn) { on_relative_dx_ = std::move(fn); }

    // 处理一条 WM_INPUT（lparam 为 HRAWINPUT），时间戳 = 处理时刻。调用方处理完 MUST 调用 DefWindowProc。
    void handle(LPARAM lparam);
    // 用 GetRawInputBuffer 一次读出队列里的全部原始输入。这批鼠标位移作为一段记入速度窗口，时间段由
    // BatchTimeline 决定（连续移动时从上一批位移开始，停顿后从上一次读取开始），窗口按重叠比例求和，
    // 所以读取节拍（逐事件唤醒、16.7 ms tick、30 ms 空闲轮询、不规则）都不会让速度偏高或偏低。
    void drain(double now_s);
    // 是否值得让主循环为每个原始输入事件醒来（QS_RAWINPUT）：后台接收的鼠标输入在 Windows 11 上被系统合并成
    // 约 125 Hz 一包（每包 ~8 ms 的位移），只在 tick 时批量读取的话，50 ms 窗口里只有 6–7 包，
    // 窗口边界落在包与包之间造成 ±1 包（约 ±16%）的跳动；逐包醒来记下到达时刻就能按真实时间段积分。
    // 事件很密（> 600 Hz，未被合并的 1–8 kHz 鼠标）时批量读取已经足够准，逐个醒来反而太贵，返回 false。
    bool wants_event_wake(double now_s) const { return path_.event_count(now_s) <= kWakeMaxEvents100ms; }

    // 速度窗口长度（秒）。
    void set_velocity_window(double window_s) { velocity_.set_window(window_s); }
    // |Σ(dx, dy)| / window（计数 / 秒），范围为最近 velocity_window 内。
    double counts_per_s(double now_s) const { return velocity_.counts_per_s(now_s); }
    // 最近 100 ms 原始位移路径 Σ√(dx²+dy²)（计数）。
    double raw_path_100ms(double now_s) const { return path_.path_length(now_s); }
    // 8 个移动键是否有任一按下；距最后一条键盘消息 ≥ 5 s 时先清空（防卡键）。
    bool moving(double now_s);
    // 清空全部按下状态（前台变化时调用）。
    void clear_keys();

    // 统计：累计收到的相对鼠标事件数、忽略的绝对坐标事件数。
    std::uint64_t mouse_events() const { return mouse_events_; }
    std::uint64_t absolute_events() const { return absolute_events_; }

private:
    void on_keyboard(const RAWKEYBOARD& k, double t);
    // 一次读取得到的相对位移（n 个事件之和，path = 各事件长度之和）记入两个窗口；n = 0 时只推进时间线。
    void add_batch(double now_s, double dx, double dy, double path, std::uint32_t n);
    // 把 Σdx 交给 on_relative_dx_（若设置了且 dx ≠ 0）。
    void notify_dx(long long dx) const;

    VelocityWindow velocity_{0.050};
    VelocityWindow path_{kPathWindowS};
    BatchTimeline timeline_;
    std::array<bool, 8> keys_{};          // W A S D Up Down Left Right
    double last_key_t_ = -1e9;
    bool registered_ = false;
    std::uint64_t mouse_events_ = 0;
    std::uint64_t absolute_events_ = 0;
    std::vector<std::uint64_t> buffer_;   // GetRawInputBuffer 的缓冲（8 字节对齐）
    std::function<void(long long)> on_relative_dx_;
};

}  // namespace mc
