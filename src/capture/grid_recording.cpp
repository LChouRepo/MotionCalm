// grid_recording.cpp — grid_recording.h 的实现（纯标准库）：网格片段的编码 / 解码 / 校验与文件读写。
#include "capture/grid_recording.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace mc {

namespace {

constexpr std::uint8_t kMagic[8] = {'M', 'C', 'G', 'C', 'L', 'I', 'P', 0x1A};
constexpr std::uint32_t kMaxHeaderSize = 4096;
// 文件长度上限：最大扩展头 + 最多帧数（最小网格 8×8 时）的时间戳 + 全部样本。
constexpr std::uint64_t kMaxFrames = kGridClipMaxSamples / (kGridClipMinDim * kGridClipMinDim);
constexpr std::uint64_t kMaxFileBytes = kMaxHeaderSize + kMaxFrames * 8 + kGridClipMaxSamples * 2;

void set_error(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
}

std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ---- 小端序列化
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put_f64(std::vector<std::uint8_t>& b, double v) {
    const std::uint64_t u = std::bit_cast<std::uint64_t>(v);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}
std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
double get_f64(const std::uint8_t* p) {
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return std::bit_cast<double>(u);
}

// 元数据校验（编码、解码共用）。
bool check_info(const GridClipInfo& in, std::string* error) {
    auto dim_ok = [](long long v) { return v >= kGridClipMinDim && v <= kGridClipMaxDim; };
    auto px_ok = [](long long v) { return v >= 1 && v <= kGridClipMaxPixels; };
    if (!dim_ok(in.grid_w) || !dim_ok(in.grid_h)) {
        set_error(error, std::format("grid size {}x{} out of range [{}, {}]", in.grid_w, in.grid_h, kGridClipMinDim,
                                     kGridClipMaxDim));
        return false;
    }
    if (!px_ok(in.tex_w) || !px_ok(in.tex_h)) {
        set_error(error, std::format("texture size {}x{} out of range", in.tex_w, in.tex_h));
        return false;
    }
    if (!px_ok(in.screen_w) || !px_ok(in.screen_h)) {
        set_error(error, std::format("screen size {}x{} out of range", in.screen_w, in.screen_h));
        return false;
    }
    if (in.rotation_deg != 0 && in.rotation_deg != 90 && in.rotation_deg != 180 && in.rotation_deg != 270) {
        set_error(error, std::format("rotation {} is not 0/90/180/270", in.rotation_deg));
        return false;
    }
    return true;
}

}  // namespace

double grid_clip_sample_scale(const GridClip& clip) {
    double hi = 1.0;
    for (const GridClipFrame& f : clip.frames) {
        for (const float v : f.gray) {
            if (std::isfinite(v) && v > hi) hi = v;
        }
    }
    return hi;
}

bool encode_grid_clip(const GridClip& clip, std::vector<std::uint8_t>* out, std::string* error) {
    if (out == nullptr) return false;
    out->clear();
    const GridClipInfo& in = clip.info;
    if (!check_info(in, error)) return false;
    const std::size_t n = static_cast<std::size_t>(in.grid_w) * static_cast<std::size_t>(in.grid_h);
    if (clip.frames.size() > 0xFFFFFFFFull ||
        static_cast<std::uint64_t>(clip.frames.size()) * n > kGridClipMaxSamples) {
        set_error(error, std::format("too many frames ({})", clip.frames.size()));
        return false;
    }
    const double t0 = clip.frames.empty() ? 0.0 : clip.frames.front().t_s;
    double prev_t = 0;
    for (std::size_t k = 0; k < clip.frames.size(); ++k) {
        const GridClipFrame& f = clip.frames[k];
        if (f.gray.size() != n) {
            set_error(error, std::format("frame {} has {} samples, expected {}", k, f.gray.size(), n));
            return false;
        }
        const double t = f.t_s - t0;
        if (!std::isfinite(t) || (k > 0 && t < prev_t)) {
            set_error(error, std::format("frame {} timestamp is not finite or goes backwards", k));
            return false;
        }
        prev_t = t;
    }
    const double scale = grid_clip_sample_scale(clip);
    if (!(scale <= kGridClipMaxScale)) {
        set_error(error, std::format("sample values too large (max {})", scale));
        return false;
    }
    const double src_t0 = in.source_t0_s + t0;

    const std::size_t record = 8 + 2 * n;
    out->reserve(kGridClipHeaderSize + clip.frames.size() * record);
    out->insert(out->end(), std::begin(kMagic), std::end(kMagic));
    put_u32(*out, kGridClipVersion);
    put_u32(*out, static_cast<std::uint32_t>(kGridClipHeaderSize));
    put_u32(*out, static_cast<std::uint32_t>(in.grid_w));
    put_u32(*out, static_cast<std::uint32_t>(in.grid_h));
    put_u32(*out, static_cast<std::uint32_t>(in.tex_w));
    put_u32(*out, static_cast<std::uint32_t>(in.tex_h));
    put_u32(*out, static_cast<std::uint32_t>(in.screen_w));
    put_u32(*out, static_cast<std::uint32_t>(in.screen_h));
    put_u32(*out, static_cast<std::uint32_t>(in.rotation_deg));
    put_u32(*out, static_cast<std::uint32_t>(clip.frames.size()));
    put_f64(*out, scale);
    put_f64(*out, std::isfinite(src_t0) ? src_t0 : 0.0);

    const double k_q = 65535.0 / scale;
    for (const GridClipFrame& f : clip.frames) {
        put_f64(*out, f.t_s - t0);
        const std::size_t at = out->size();
        out->resize(at + 2 * n);
        std::uint8_t* p = out->data() + at;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = f.gray[i];
            double q = 0;
            if (std::isfinite(v) && v > 0) q = std::min(65535.0, std::round(v * k_q));
            put_u16(p + 2 * i, static_cast<std::uint16_t>(q));
        }
    }
    return true;
}

bool decode_grid_clip(const std::uint8_t* data, std::size_t size, GridClip* out, std::string* error) {
    if (out == nullptr) return false;
    *out = GridClip{};
    if (data == nullptr || size < kGridClipHeaderSize) {
        set_error(error, std::format("file too short for a header ({} bytes)", size));
        return false;
    }
    if (std::memcmp(data, kMagic, sizeof kMagic) != 0) {
        set_error(error, "not a MotionCalm grid clip (bad magic)");
        return false;
    }
    const std::uint32_t version = get_u32(data + 8);
    if (version != kGridClipVersion) {
        set_error(error, std::format("unsupported clip version {} (this build reads version {})", version,
                                     kGridClipVersion));
        return false;
    }
    const std::uint32_t header_size = get_u32(data + 12);
    if (header_size < kGridClipHeaderSize || header_size > kMaxHeaderSize || header_size > size) {
        set_error(error, std::format("bad header size {}", header_size));
        return false;
    }
    // u32 → long long 再比较范围，避免大值转 int 溢出。
    const long long gw = get_u32(data + 16), gh = get_u32(data + 20);
    const long long tw = get_u32(data + 24), th = get_u32(data + 28);
    const long long sw = get_u32(data + 32), sh = get_u32(data + 36);
    const long long rot = get_u32(data + 40);
    const std::uint64_t count = get_u32(data + 44);
    const double scale = get_f64(data + 48);
    const double src_t0 = get_f64(data + 56);
    const long long lim_dim = kGridClipMaxDim, lim_px = kGridClipMaxPixels;
    if (gw > lim_dim || gh > lim_dim || tw > lim_px || th > lim_px || sw > lim_px || sh > lim_px || rot > 270) {
        set_error(error, std::format("absurd dimensions in header (grid {}x{}, texture {}x{}, screen {}x{}, "
                                     "rotation {})",
                                     gw, gh, tw, th, sw, sh, rot));
        return false;
    }
    GridClipInfo in;
    in.grid_w = static_cast<int>(gw);
    in.grid_h = static_cast<int>(gh);
    in.tex_w = static_cast<int>(tw);
    in.tex_h = static_cast<int>(th);
    in.screen_w = static_cast<int>(sw);
    in.screen_h = static_cast<int>(sh);
    in.rotation_deg = static_cast<int>(rot);
    in.source_t0_s = std::isfinite(src_t0) ? src_t0 : 0.0;
    if (!check_info(in, error)) return false;
    if (!(scale > 0) || !(scale <= kGridClipMaxScale)) {
        set_error(error, std::format("bad sample scale {}", scale));
        return false;
    }
    const std::uint64_t n = static_cast<std::uint64_t>(gw) * static_cast<std::uint64_t>(gh);
    if (count * n > kGridClipMaxSamples) {
        set_error(error, std::format("too many samples ({} frames of {}x{})", count, gw, gh));
        return false;
    }
    const std::uint64_t record = 8 + 2 * n;
    const std::uint64_t expected = header_size + count * record;
    if (size < expected) {
        set_error(error, std::format("truncated clip: {} bytes, expected {} ({} frames)", size, expected, count));
        return false;
    }
    if (size > expected) {
        set_error(error, std::format("clip has {} unexpected trailing bytes", size - expected));
        return false;
    }

    out->info = in;
    out->frames.resize(static_cast<std::size_t>(count));
    const double k_v = scale / 65535.0;
    const std::uint8_t* p = data + header_size;
    double prev_t = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        GridClipFrame& f = out->frames[static_cast<std::size_t>(k)];
        f.t_s = get_f64(p);
        if (!std::isfinite(f.t_s) || (k > 0 && f.t_s < prev_t)) {
            set_error(error, std::format("frame {} timestamp is not finite or goes backwards", k));
            out->frames.clear();
            return false;
        }
        prev_t = f.t_s;
        p += 8;
        f.gray.resize(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
            f.gray[i] = static_cast<float>(get_u16(p + 2 * i) * k_v);
        }
        p += 2 * n;
    }
    return true;
}

bool write_grid_clip(const std::filesystem::path& path, const GridClip& clip, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!encode_grid_clip(clip, &bytes, error)) return false;
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            set_error(error, "cannot create " + path_utf8(tmp));
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            set_error(error, "write failed: " + path_utf8(tmp));
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        set_error(error, "cannot rename to " + path_utf8(path) + ": " + ec.message());
        return false;
    }
    return true;
}

bool read_grid_clip(const std::filesystem::path& path, GridClip* out, std::string* error) {
    if (out == nullptr) return false;
    *out = GridClip{};
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        set_error(error, "cannot open " + path_utf8(path) + ": " + ec.message());
        return false;
    }
    if (size > kMaxFileBytes) {
        set_error(error, std::format("file too large ({} bytes)", size));
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        set_error(error, "cannot open " + path_utf8(path));
        return false;
    }
    if (!bytes.empty()) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(f.gcount()) != size) {
        set_error(error, "read failed: " + path_utf8(path));
        return false;
    }
    return decode_grid_clip(bytes.data(), bytes.size(), out, error);
}

}  // namespace mc
