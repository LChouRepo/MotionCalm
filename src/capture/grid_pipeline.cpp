// grid_pipeline.cpp — grid_pipeline.h 的实现（纯标准库）：复刻 desktop_dup.cpp 估计部分的可回放管线、汇总、CSV 与回放预期。
#include "capture/grid_pipeline.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace mc {

namespace {

// 与 desktop_dup.cpp 的同名常量一致（那边在匿名命名空间里，改动时两处同步）：
// 低于这两个流速时相似变换估计直接用短基线链的结果（不做长基线精化），见 SimilarityParams。
constexpr double kRefineMinZoomDegPerS = 4.0;
constexpr double kRefineMinRollDegPerS = 4.0;

void set_error(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
}

}  // namespace

// ---------------------------------------------------------------- GridPipeline

GridPipeline::GridPipeline(const CaptureParams& params, const GridClipInfo& info) : info_(info) {
    cur_ = sanitize_capture_params(params);   // CaptureService::start / update_params 同样先 sanitize
    cur_.grid_w = info.grid_w;
    cur_.grid_h = info.grid_h;

    // ---- apply_params()（首次配置：三个分支都走）
    MotionEstimatorParams ep;
    ep.width = cur_.grid_w;
    ep.height = cur_.grid_h;
    ep.max_shift = cur_.max_shift;
    ep.min_correlation = cur_.min_correlation;
    estimator_.configure(ep);
    {
        SimilarityParams sp = similarity_.params();
        sp.width = cur_.grid_w;
        sp.height = cur_.grid_h;
        sp.min_confidence = cur_.min_correlation;
        sp.baseline_s = cur_.baseline_ms / 1000.0;
        similarity_.configure(sp);
    }
    similarity_.set_refine_thresholds(kRefineMinZoomDegPerS / (cur_.assumed_hfov_deg / 2.0), kRefineMinRollDegPerS);

    // ---- open_session()：几何 + update_hold_geometry()
    geom_ = output_geometry(info.rotation_deg, info.tex_w, info.tex_h);
    if (geom_.screen_w <= 0 || geom_.screen_h <= 0) {
        geom_.screen_w = info.screen_w;
        geom_.screen_h = info.screen_h;
    }
    SpeedHoldParams hp;
    hp.grid_w = geom_.swap_axes ? cur_.grid_h : cur_.grid_w;
    hp.grid_h = geom_.swap_axes ? cur_.grid_w : cur_.grid_h;
    hp.screen_w_px = geom_.screen_w;
    hp.screen_h_px = geom_.screen_h;
    hp.assumed_hfov_deg = cur_.assumed_hfov_deg;
    hp.hold_ms = cur_.hold_ms;
    hold_.configure(hp);
    zoom_hold_.configure(hp);
    roll_hold_.configure(hp);
    if (info.tex_w > 0 && info.tex_h > 0 && cur_.grid_w > 0 && cur_.grid_h > 0) {
        const double asp = (static_cast<double>(info.tex_h) / cur_.grid_h) / (static_cast<double>(info.tex_w) / cur_.grid_w);
        if (std::fabs(asp - similarity_.params().pixel_aspect) > 1e-9) {
            SimilarityParams sp = similarity_.params();
            sp.pixel_aspect = asp;
            similarity_.configure(sp);
        }
    }
    discard_ = kDiscardFramesAfterBuild;
}

void GridPipeline::reset() {
    estimator_.reset();
    similarity_.reset();
    hold_.reset();
    zoom_hold_.reset();
    roll_hold_.reset();
    discard_ = kDiscardFramesAfterBuild;
}

GridPipelineFrame GridPipeline::push(const float* gray, double t_s) {
    GridPipelineFrame r;
    if (gray == nullptr) return r;
    r.t_s = t_s;
    // ---- drain() 的循环体
    MotionEstimate e = estimator_.push(gray, t_s);
    const TranslationHint hint{e.dx, e.dy};   // 交换坐标轴之前（纹理方向）
    const SimilarityEstimate se = similarity_.push(gray, t_s, e.valid ? &hint : nullptr);
    if (geom_.swap_axes) std::swap(e.dx, e.dy);
    if (discard_ > 0) {
        --discard_;
        r.discarded = true;
    } else {
        hold_.on_estimate(e, e.dt_s, t_s);
        zoom_hold_.on_value(se.valid ? zoom_to_deg_per_s(se.scale, se.dt_s, cur_.assumed_hfov_deg) : std::nullopt, t_s);
        roll_hold_.on_value(se.valid ? roll_to_deg_per_s(se.rotation_deg, se.dt_s) : std::nullopt, t_s);
    }
    r.dx = e.dx;
    r.dy = e.dy;
    r.confidence = e.confidence;
    r.valid = e.valid;
    r.dt_s = e.dt_s;
    r.measured_deg_per_s = hold_.last_measurement();
    r.scale = se.scale;
    r.rotation_deg = se.rotation_deg;
    r.sim_confidence = se.confidence;
    r.sim_valid = se.valid;
    r.sim_dt_s = se.dt_s;
    // ---- publish()（发布时刻取帧时间，见头文件）
    r.speed_deg_per_s = hold_.value(t_s);
    r.zoom_deg_per_s = zoom_hold_.value(t_s);
    r.roll_deg_per_s = cur_.detect_roll ? roll_hold_.value(t_s) : 0.0;
    return r;
}

// ---------------------------------------------------------------- 汇总 / 回放 / CSV

void GridPipelineSummary::add(const GridPipelineFrame& f) {
    if (frames == 0) first_t_s = f.t_s;
    last_t_s = f.t_s;
    ++frames;
    if (f.discarded) {
        ++discarded;
    } else {
        if (f.sim_valid) ++sim_valid;
        if (f.dt_s > 0) {
            ++estimates;
            confidence_sum += f.confidence;
            if (f.valid) {
                ++valid;
            } else {
                ++lowconf;
            }
        }
    }
    speed_sum += f.speed_deg_per_s;
    speed_max = std::max(speed_max, f.speed_deg_per_s);
    zoom_sum += f.zoom_deg_per_s;
    zoom_max = std::max(zoom_max, f.zoom_deg_per_s);
    roll_sum += f.roll_deg_per_s;
    roll_max = std::max(roll_max, f.roll_deg_per_s);
}

bool replay_grid_clip(const GridClip& clip, const CaptureParams& params, GridReplayResult* out, std::string* error) {
    if (out == nullptr) return false;
    *out = GridReplayResult{};
    const std::size_t n = static_cast<std::size_t>(std::max(0, clip.info.grid_w)) *
                          static_cast<std::size_t>(std::max(0, clip.info.grid_h));
    if (n == 0) {
        set_error(error, "clip has an empty grid");
        return false;
    }
    GridPipeline pipe(params, clip.info);
    out->frames.reserve(clip.frames.size());
    for (std::size_t k = 0; k < clip.frames.size(); ++k) {
        const GridClipFrame& f = clip.frames[k];
        if (f.gray.size() != n) {
            set_error(error, std::format("frame {} has {} samples, expected {}", k, f.gray.size(), n));
            return false;
        }
        const GridPipelineFrame r = pipe.push(f.gray.data(), f.t_s);
        out->summary.add(r);
        out->frames.push_back(r);
    }
    return true;
}

std::string grid_replay_csv_header() {
    return "frame,t_s,discarded,dt_s,dx,dy,confidence,valid,measured_deg_per_s,scale,rotation_deg,sim_confidence,"
           "sim_valid,sim_dt_s,speed_deg_per_s,zoom_deg_per_s,roll_deg_per_s";
}

std::string grid_replay_csv_row(std::size_t index, const GridPipelineFrame& f) {
    return std::format("{},{:.6f},{},{:.6f},{:.4f},{:.4f},{:.4f},{},{:.3f},{:.6f},{:.4f},{:.4f},{},{:.6f},{:.3f},{:.3f},{:.3f}",
                       index, f.t_s, f.discarded ? 1 : 0, f.dt_s, f.dx, f.dy, f.confidence, f.valid ? 1 : 0,
                       f.measured_deg_per_s, f.scale, f.rotation_deg, f.sim_confidence, f.sim_valid ? 1 : 0,
                       f.sim_dt_s, f.speed_deg_per_s, f.zoom_deg_per_s, f.roll_deg_per_s);
}

// ---------------------------------------------------------------- 回放预期

namespace {

constexpr std::string_view kMetrics[] = {"frames",     "valid_fraction", "lowconf_fraction", "sim_valid_fraction",
                                         "mean_speed", "max_speed",      "mean_zoom",        "max_zoom",
                                         "mean_roll",  "max_roll"};

std::string_view trim(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

bool parse_double(std::string_view v, double* out) {
    if (v.empty()) return false;
    if (v.front() == '+') v.remove_prefix(1);   // from_chars 不接受前导 '+'
    double x = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc{} || ptr != v.data() + v.size() || !std::isfinite(x)) return false;
    *out = x;
    return true;
}

bool parse_int(std::string_view v, int* out) {
    if (v.empty()) return false;
    if (v.front() == '+') v.remove_prefix(1);
    int x = 0;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc{} || ptr != v.data() + v.size()) return false;
    *out = x;
    return true;
}

}  // namespace

void ReplayExpectations::apply_to(CaptureParams* p) const {
    if (p == nullptr) return;
    if (hfov) p->assumed_hfov_deg = *hfov;
    if (min_corr) p->min_correlation = *min_corr;
    if (max_shift) p->max_shift = *max_shift;
    if (hold_ms) p->hold_ms = *hold_ms;
    if (baseline_ms) p->baseline_ms = *baseline_ms;
    if (detect_roll) p->detect_roll = *detect_roll;
}

bool parse_replay_expectations(std::string_view text, ReplayExpectations* out, std::string* error) {
    if (out == nullptr) return false;
    *out = ReplayExpectations{};
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);   // 记事本可能加 BOM
    std::vector<std::string_view> seen;   // 指向 text 内部，只在本函数内使用
    int line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        line = trim(line.substr(0, line.find('#')));   // # 之后是注释（整行或行尾）
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            set_error(error, std::format("line {}: expected key=value", line_no));
            return false;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view val = trim(line.substr(eq + 1));
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            set_error(error, std::format("line {}: duplicate key '{}'", line_no, key));
            return false;
        }
        seen.push_back(key);
        const auto bad_value = [&] {
            set_error(error, std::format("line {}: bad value '{}' for '{}'", line_no, val, key));
            return false;
        };
        double d = 0;
        int i = 0;
        if (key == "hfov") {
            if (!parse_double(val, &d)) return bad_value();
            out->hfov = d;
        } else if (key == "min_corr") {
            if (!parse_double(val, &d)) return bad_value();
            out->min_corr = d;
        } else if (key == "max_shift") {
            if (!parse_int(val, &i)) return bad_value();
            out->max_shift = i;
        } else if (key == "hold_ms") {
            if (!parse_int(val, &i)) return bad_value();
            out->hold_ms = i;
        } else if (key == "baseline_ms") {
            if (!parse_int(val, &i)) return bad_value();
            out->baseline_ms = i;
        } else if (key == "detect_roll") {
            if (val == "1" || val == "true") {
                out->detect_roll = true;
            } else if (val == "0" || val == "false") {
                out->detect_roll = false;
            } else {
                return bad_value();
            }
        } else {
            const bool is_min = key.ends_with("_min");
            const bool is_max = key.ends_with("_max");
            const std::string_view metric = (is_min || is_max) ? key.substr(0, key.size() - 4) : std::string_view{};
            if (metric.empty() || std::find(std::begin(kMetrics), std::end(kMetrics), metric) == std::end(kMetrics)) {
                set_error(error, std::format("line {}: unknown key '{}'", line_no, key));
                return false;
            }
            if (!parse_double(val, &d)) return bad_value();
            out->bounds.push_back(ReplayBound{std::string(metric), is_min, d});
        }
    }
    return true;
}

bool read_replay_expectations(const std::filesystem::path& path, ReplayExpectations* out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        const std::u8string u = path.u8string();
        set_error(error, "cannot open " + std::string(reinterpret_cast<const char*>(u.data()), u.size()));
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_replay_expectations(ss.str(), out, error);
}

std::optional<double> replay_metric(const GridPipelineSummary& s, std::string_view metric) {
    if (metric == "frames") return static_cast<double>(s.frames);
    if (metric == "valid_fraction") return s.valid_fraction();
    if (metric == "lowconf_fraction") return s.lowconf_fraction();
    if (metric == "sim_valid_fraction") return s.sim_valid_fraction();
    if (metric == "mean_speed") return s.mean_speed();
    if (metric == "max_speed") return s.speed_max;
    if (metric == "mean_zoom") return s.mean_zoom();
    if (metric == "max_zoom") return s.zoom_max;
    if (metric == "mean_roll") return s.mean_roll();
    if (metric == "max_roll") return s.roll_max;
    return std::nullopt;
}

std::vector<std::string> check_replay_expectations(const ReplayExpectations& e, const GridPipelineSummary& s) {
    std::vector<std::string> failures;
    for (const ReplayBound& b : e.bounds) {
        const std::optional<double> v = replay_metric(s, b.metric);
        if (!v) {
            failures.push_back(std::format("unknown metric '{}'", b.metric));
            continue;
        }
        const bool ok = b.is_min ? (*v >= b.value) : (*v <= b.value);   // NaN 两边都不满足
        if (!ok) {
            failures.push_back(std::format("{} = {:.4f} {} {} {:.4f}", b.metric, *v, b.is_min ? "<" : ">",
                                           b.is_min ? "min" : "max", b.value));
        }
    }
    return failures;
}

std::filesystem::path replay_expectations_path(const std::filesystem::path& clip_path) {
    std::filesystem::path p = clip_path;
    p.replace_extension(L".expect");
    return p;
}

}  // namespace mc
