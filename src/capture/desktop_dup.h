// desktop_dup.h — 截屏运动检测服务（openspec capture-motion-detection design D1/D8）：独立截屏线程上做 DXGI 桌面复制 → GPU 降采样 → MotionEstimator（平移）+ SimilarityEstimator（前进 / 倾斜，变更 5）→ SpeedHold，以原子 double 把截屏角速度交给主线程。
//
// 主线程用法（每个 tick，全部非阻塞；只有 stop() 会 join）：
//   capture_.start(params);                                   // 启动时（--no-capture 时根本不调用）
//   capture_.set_target(monitor, user_on, wanted);            // 每 tick；只在值变化时唤醒截屏线程
//   in.capture_deg_per_s = capture_.speed(now);               // now = qpc_now_s()；过期 / 暂停 / 失败时为 0
//   in.capture_zoom_deg_per_s = capture_.zoom_speed(now);     // 变更 5：同一规则
//   in.capture_roll_deg_per_s = capture_.roll_speed(now);     // 变更 5：detect_roll=0 时恒为 0
//   capture_.update_params(params);                           // 配置重载、托盘、热键改动之后
//   capture_.stop();                                          // 退出时
// 其中 user_on = !--no-capture && [trigger] capture（不要把 state()==Failed 算进去，否则会反复重试），
//      wanted  = user_on && state() != Failed && gate_capture。
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

#include "capture/capture_policy.h"

namespace mc {

// 调试条 / capturetest 用的快照（debug() 返回一份拷贝）。dx / dy 为桌面方向的格数（旋转 90° / 270° 时符号不保证）。
struct CaptureDebug {
    CaptureState state = CaptureState::Off;
    // 最近一次估计（含低置信度帧）
    double dx = 0, dy = 0;
    double confidence = 0;
    bool valid = false;
    double measured_deg_per_s = 0;    // 最近一次有效测量
    double published_deg_per_s = 0;   // 最近一次发布值（SpeedHold::value）
    // 变更 5：最近一次相似变换估计（含无效帧）与前进 / 倾斜的发布值
    double scale = 1.0;               // 内容放大倍数（参考帧 → 当前帧）
    double rotation_deg = 0;          // 内容旋转角（°，屏幕上顺时针为正）
    double sim_confidence = 0;
    bool sim_valid = false;
    double sim_dt_s = 0;              // 所用参考帧的时间差
    double zoom_deg_per_s = 0;        // 发布值
    double roll_deg_per_s = 0;        // 发布值（detect_roll=0 时为 0）
    // 当前会话
    int screen_w = 0, screen_h = 0;   // 桌面方向的像素尺寸
    int grid_w = 0, grid_h = 0;
    int taps_x = 0, taps_y = 0;
    bool gpu_timing = false;          // 只在日志级别为 debug 时启用
    // start() 以来的累计计数（单调递增；capturetest 做差得到每秒值）
    std::uint64_t frames = 0;         // 读回并送进估计器的新桌面帧
    std::uint64_t estimates = 0;      // 其中有前一帧可比的（不含建立后丢弃的帧）
    std::uint64_t lowconf = 0;        // 其中低置信度 / 无纹理的
    std::uint64_t timeouts = 0;       // AcquireNextFrame 超时
    std::uint64_t pointer_only = 0;   // 只有指针更新的帧
    std::uint64_t dropped = 0;        // staging 环满时丢掉的帧
    std::uint64_t sessions = 0;       // 成功建立的桌面复制会话
    std::uint64_t build_failures = 0; // 建立 / 早夭失败次数
    double confidence_sum = 0;        // estimates 的置信度之和
    double abs_dx_sum = 0, abs_dy_sum = 0;
    double gpu_ms_sum = 0;            // 降采样 GPU 耗时之和
    std::uint64_t gpu_samples = 0;
    double cpu_ms_sum = 0;            // 每帧读回 + 估计的 CPU 耗时之和（按 frames 平均）
    double sim_ms_sum = 0;            // 其中相似变换估计（变更 5）的耗时之和（按 frames 平均）
    std::uint64_t sim_valid_count = 0;   // 相似变换估计有效的帧数
    double submit_ms_sum = 0;         // 每帧提交降采样（D3D 调用 + Flush + ReleaseFrame 之前）的 CPU 耗时之和
    std::uint64_t submits = 0;
    double thread_cpu_s = 0;          // 截屏线程累计 CPU 时间（内核 + 用户，约每 250 ms 刷新）
};

class CaptureService {
public:
    CaptureService();
    ~CaptureService();   // = stop()
    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    // 创建截屏线程（不建设备、不取帧，直到 set_target 给出 feature_on && wanted）。已启动或建线程失败返回 false。
    bool start(const CaptureParams& params);
    // 置停止标志、唤醒并 join（截屏线程每 ≤ 16 ms 检查一次）。释放全部 D3D / DXGI 对象。可重复调用。
    void stop();
    bool started() const;

    // mon：overlay 当前所在的显示器（nullptr → 视同不需要取帧）；
    // feature_on：截屏检测开关（false → 立即释放会话；false → true 时清零失败计数，可从 Failed 重新开始）；
    // wanted：此刻需要取帧（false → 暂停，5 s 后释放会话）。只在有变化时 SetEvent。
    void set_target(HMONITOR mon, bool feature_on, bool wanted);
    // 新参数在截屏线程处理下一个新帧之前生效；网格变化时重建降采样资源并丢弃参考帧。
    void update_params(const CaptureParams& params);

    // 截屏角速度（°/s，≥ 0），无锁原子读。state 为 Off / Paused / Failed，或发布时刻早于 now_s 超过 250 ms 时返回 0。
    // now_s 与 qpc_now_s()（util/clock.h）同一基准。
    double speed(double now_s) const;
    double speed() const;   // 用 qpc_now_s()
    // 变更 5：前进 / 后退的径向流速与倾斜角速度（°/s，≥ 0），规则同 speed()；roll 在 detect_roll=0 时恒为 0。
    double zoom_speed(double now_s) const;
    double roll_speed(double now_s) const;
    CaptureState state() const;
    CaptureDebug debug() const;   // 小结构体拷贝（短暂持锁，仅调试条 / 工具使用）

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mc
