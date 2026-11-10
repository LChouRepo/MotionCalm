// grid_pipeline.h — 可回放的截屏估计管线（纯逻辑，不含 Win32 / D3D）：按 CaptureParams 与片段元数据逐帧喂入灰度网格，得到截屏线程会发布的原始估计与三个角速度；片段汇总统计；回放预期（.expect 旁注文件）的解析与检查。
//
// 【必须与 desktop_dup.cpp 保持同步】GridPipeline 逐步复刻 CaptureService::Impl 的
//   apply_params()          MotionEstimator / SimilarityEstimator 的配置、精化门槛（kRefineMin*DegPerS）
//   open_session() + update_hold_geometry()
//                           output_geometry 的坐标轴交换、三个 SpeedHold 的换算参数、pixel_aspect、丢弃前 kDiscardFramesAfterBuild 帧
//   drain()                 MotionEstimator::push → SimilarityEstimator::push（平移提示 = 交换坐标轴之前的 dx/dy，仅 valid 时给）
//                           → 交换坐标轴 → SpeedHold::on_estimate / on_value（zoom_to_deg_per_s / roll_to_deg_per_s）
//   publish()               speed = hold.value(t)、zoom = zoom_hold.value(t)、roll = detect_roll ? roll_hold.value(t) : 0
// 改动 desktop_dup.cpp 里的估计逻辑时必须同步改这里（反之亦然），否则离线回放与实际运行的结果不再一致。
// 有意的差异：
//   * 发布时刻取该帧自己的时间戳 t（实际运行在 drain() 之后用 qpc_now_s() 发布，通常晚 1 帧左右的读回延迟；
//     超时 / 只有指针更新时的额外发布也不复刻）。保持窗口 hold_ms 因此按帧时间计算。
//   * 网格尺寸取自片段（实际运行时由 sanitize_capture_params 收拢到 [64, 320] × [36, 180]），其余参数照样 sanitize。
//   * 会话重建（访问丢失、显示模式变化）不在片段里，回放把整段当成一个会话；需要时调用 reset()。
//   * FrameRateLimiter（高刷新率抽帧）、只有指针更新的帧、staging 环满丢帧都发生在网格产生之前：
//     录下来的片段已经是这些步骤之后的帧序列，回放不再复刻它们。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "capture/capture_policy.h"
#include "capture/grid_recording.h"
#include "capture/motion_estimator.h"
#include "capture/similarity_estimator.h"

namespace mc {

// 一帧的回放结果（对应 CaptureDebug 里同名字段与一次 publish()）。
struct GridPipelineFrame {
    double t_s = 0;                   // 该帧的时间戳
    bool discarded = false;           // 建立后的前 kDiscardFramesAfterBuild 帧：只更新参考帧，不进 SpeedHold
    // MotionEstimator（dx / dy 已按旋转交换为桌面方向，与 CaptureDebug 相同）
    double dx = 0, dy = 0;
    double confidence = 0;
    bool valid = false;
    double dt_s = 0;
    double measured_deg_per_s = 0;    // hold.last_measurement()
    // SimilarityEstimator
    double scale = 1.0;
    double rotation_deg = 0;
    double sim_confidence = 0;
    bool sim_valid = false;
    double sim_dt_s = 0;
    // 发布值（°/s）
    double speed_deg_per_s = 0;
    double zoom_deg_per_s = 0;
    double roll_deg_per_s = 0;        // detect_roll = false 时为 0
};

// 片段汇总（计数规则与 CaptureDebug 一致：estimates = 未丢弃且 dt > 0 的帧，lowconf = 其中无效的）。
struct GridPipelineSummary {
    std::uint64_t frames = 0;
    std::uint64_t discarded = 0;
    std::uint64_t estimates = 0;
    std::uint64_t valid = 0;          // estimates 中 MotionEstimate 有效的
    std::uint64_t lowconf = 0;        // estimates 中无效的（低置信度 / 无纹理 / 间隔过长）
    std::uint64_t sim_valid = 0;      // 未丢弃帧中相似变换估计有效的
    double confidence_sum = 0;        // estimates 的置信度之和
    double first_t_s = 0, last_t_s = 0;
    // 发布值：对全部帧（含丢弃帧）求和 / 取最大
    double speed_sum = 0, speed_max = 0;
    double zoom_sum = 0, zoom_max = 0;
    double roll_sum = 0, roll_max = 0;

    void add(const GridPipelineFrame& f);
    double duration_s() const { return frames > 1 ? last_t_s - first_t_s : 0.0; }
    double valid_fraction() const { return estimates > 0 ? static_cast<double>(valid) / static_cast<double>(estimates) : 0.0; }
    double lowconf_fraction() const { return estimates > 0 ? static_cast<double>(lowconf) / static_cast<double>(estimates) : 0.0; }
    double sim_valid_fraction() const {
        const std::uint64_t n = frames - discarded;
        return n > 0 ? static_cast<double>(sim_valid) / static_cast<double>(n) : 0.0;
    }
    double confidence_avg() const { return estimates > 0 ? confidence_sum / static_cast<double>(estimates) : 0.0; }
    double mean_speed() const { return frames > 0 ? speed_sum / static_cast<double>(frames) : 0.0; }
    double mean_zoom() const { return frames > 0 ? zoom_sum / static_cast<double>(frames) : 0.0; }
    double mean_roll() const { return frames > 0 ? roll_sum / static_cast<double>(frames) : 0.0; }
};

class GridPipeline {
public:
    // params 先经 sanitize_capture_params，再把网格尺寸换成 info.grid_w × info.grid_h。
    GridPipeline(const CaptureParams& params, const GridClipInfo& info);

    // 喂入一帧（行主序 grid_w × grid_h，纹理方向；t_s 单调递增，任意基准）。gray 为 nullptr 时返回默认结果、不改变状态。
    GridPipelineFrame push(const float* gray, double t_s);
    // 相当于会话重建：估计器丢弃参考帧、SpeedHold 归零、重新丢弃前 kDiscardFramesAfterBuild 帧。
    void reset();

    const CaptureParams& params() const { return cur_; }
    const GridClipInfo& info() const { return info_; }
    const OutputGeometry& geometry() const { return geom_; }
    const SimilarityParams& similarity_params() const { return similarity_.params(); }
    const SpeedHoldParams& hold_params() const { return hold_.params(); }

private:
    CaptureParams cur_;
    GridClipInfo info_;
    OutputGeometry geom_;
    MotionEstimator estimator_;
    SimilarityEstimator similarity_;
    SpeedHold hold_, zoom_hold_, roll_hold_;
    int discard_ = 0;
};

struct GridReplayResult {
    std::vector<GridPipelineFrame> frames;
    GridPipelineSummary summary;
};

// 把整个片段跑一遍。某帧样本数不等于 grid_w·grid_h 时返回 false 并写 error。
bool replay_grid_clip(const GridClip& clip, const CaptureParams& params, GridReplayResult* out, std::string* error);

// CSV（逗号分隔、点作小数点，与区域设置无关）：表头一行，每帧一行，均不含换行符。
std::string grid_replay_csv_header();
std::string grid_replay_csv_row(std::size_t index, const GridPipelineFrame& f);

// ---------------------------------------------------------------- 回放预期（tests/fixtures/README.md）
//
// 片段 foo.mcgrid 旁边的 foo.expect：UTF-8 文本，每行 key=value，# 之后为注释（整行或行尾），空行忽略，键区分大小写。
//   回放参数（可选，覆盖默认 CaptureParams）：hfov、max_shift、min_corr、hold_ms、baseline_ms、detect_roll（0/1）
//   界限：<metric>_min=<x> / <metric>_max=<x>，metric 为
//     frames、valid_fraction、lowconf_fraction、sim_valid_fraction、mean_speed、max_speed、mean_zoom、max_zoom、mean_roll、max_roll
// 未知键、重复键、无法解析的数值都算错误（防止拼错的键被悄悄忽略）。
struct ReplayBound {
    std::string metric;
    bool is_min = true;   // true：metric ≥ value；false：metric ≤ value
    double value = 0;
};

struct ReplayExpectations {
    std::optional<double> hfov, min_corr;
    std::optional<int> max_shift, hold_ms, baseline_ms;
    std::optional<bool> detect_roll;
    std::vector<ReplayBound> bounds;

    // 把参数覆盖写进 p（没给的保持不变）。
    void apply_to(CaptureParams* p) const;
};

bool parse_replay_expectations(std::string_view text, ReplayExpectations* out, std::string* error);
// 读文件再解析（文件不存在也算失败；先用 std::filesystem::exists 判断是否有旁注文件）。
bool read_replay_expectations(const std::filesystem::path& path, ReplayExpectations* out, std::string* error);
// metric 的当前值；未知名字返回 nullopt。
std::optional<double> replay_metric(const GridPipelineSummary& s, std::string_view metric);
// 不满足的界限，每条一句英文说明（空 = 全部满足）。
std::vector<std::string> check_replay_expectations(const ReplayExpectations& e, const GridPipelineSummary& s);
// foo.mcgrid → foo.expect
std::filesystem::path replay_expectations_path(const std::filesystem::path& clip_path);

}  // namespace mc
