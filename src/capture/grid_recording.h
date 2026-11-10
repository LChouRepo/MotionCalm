// grid_recording.h — 灰度网格片段（clip）文件格式的纯逻辑（只用标准库，不含 Win32）：把截屏线程送进估计器的网格序列存成带版本号的小端二进制文件，并带校验地读回，供离线回放与测试使用。
//
// 文件布局（version 1，全部小端）：
//   偏移  类型      字段
//   0     u8[8]     magic = "MCGCLIP" 0x1A
//   8     u32       version = 1
//   12    u32       header_size（≥ 64；读取时跳过 64 之后的扩展字节，留给以后的次要扩展）
//   16    u32 × 2   grid_w, grid_h            网格尺寸（格，纹理 / 扫描输出方向）
//   24    u32 × 2   tex_w, tex_h              复制得到的桌面纹理尺寸（扫描输出方向）
//   32    u32 × 2   screen_w, screen_h        桌面方向的像素尺寸（角度换算用）
//   40    u32       rotation_deg              0 / 90 / 180 / 270
//   44    u32       frame_count
//   48    f64       sample_scale              量化标度：样本值 = q / 65535 × sample_scale（> 0）
//   56    f64       source_t0_s               首帧的原始时间戳（秒，QPC 基准；仅供参考，回放不用）
//   64…   帧记录 × frame_count，每条 8 + 2·grid_w·grid_h 字节：
//         f64 t_s（相对首帧的秒数，首帧 = 0，单调不减）+ u16 × (grid_w·grid_h)（行主序量化样本）
// 文件长度必须恰好等于 header_size + frame_count × 记录长度（截断或多余字节都算损坏）。
//
// 量化：源头是 8 位桌面在每格上的区域平均，16 位量化（误差 ≤ sample_scale / 131070）实际上无损；
// HDR 桌面的亮度可以超过 1.0，所以 sample_scale = max(1, 全片段最大样本)，而不是截断到 1。负值与 NaN 记 0。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc {

inline constexpr std::uint32_t kGridClipVersion = 1;
inline constexpr std::size_t kGridClipHeaderSize = 64;
// 读取时的合理性上限（文件来自磁盘，不可信）。
inline constexpr int kGridClipMinDim = 8;                     // 网格每轴至少 8 格（SimilarityEstimator 的下限）
inline constexpr int kGridClipMaxDim = 1024;                  // 网格每轴至多 1024 格
inline constexpr int kGridClipMaxPixels = 65536;              // 纹理 / 屏幕每轴至多 65536 像素
inline constexpr std::uint64_t kGridClipMaxSamples = 1ull << 27;   // 全片段至多 1.34 亿个样本（解码后 512 MB）
inline constexpr double kGridClipMaxScale = 1e6;

// 片段元数据（回放需要的全部几何信息）。
struct GridClipInfo {
    int grid_w = 0, grid_h = 0;       // 网格尺寸（纹理方向）
    int tex_w = 0, tex_h = 0;         // 复制得到的桌面纹理尺寸（扫描输出方向）
    int screen_w = 0, screen_h = 0;   // 桌面方向的像素尺寸
    int rotation_deg = 0;             // 0 / 90 / 180 / 270
    double source_t0_s = 0;           // 读取：首帧的原始时间戳；写入：与 frames[0].t_s 相加后写进文件头
};

struct GridClipFrame {
    double t_s = 0;              // 时间戳（秒）。读回时相对首帧（首帧 = 0）
    std::vector<float> gray;     // grid_w × grid_h，行主序
};

struct GridClip {
    GridClipInfo info;
    std::vector<GridClipFrame> frames;
};

// 编码到内存。写入的时间戳 = frames[k].t_s − frames[0].t_s；文件头 source_t0_s = info.source_t0_s + frames[0].t_s。
// 元数据非法（尺寸越界、旋转不是 0/90/180/270）、某帧样本数不等于 grid_w·grid_h、时间戳非有限或递减时返回 false 并写 error。
bool encode_grid_clip(const GridClip& clip, std::vector<std::uint8_t>* out, std::string* error);

// 从内存解码并校验全部字段（magic、版本、尺寸、长度、时间戳）。失败返回 false、写 error，*out 内容未定义。
bool decode_grid_clip(const std::uint8_t* data, std::size_t size, GridClip* out, std::string* error);

// 文件版本（路径用 std::filesystem::path，非 ASCII 路径没问题）。写入先写到 "<path>.tmp" 再改名，失败时不留半个文件。
bool write_grid_clip(const std::filesystem::path& path, const GridClip& clip, std::string* error);
bool read_grid_clip(const std::filesystem::path& path, GridClip* out, std::string* error);

// 量化标度：max(1, 全部有限样本的最大值)。
double grid_clip_sample_scale(const GridClip& clip);

}  // namespace mc
