// fakegame/main.cpp — 集成测试用"假游戏"：在指定显示器上开全屏无边框窗口（纹理 grid = 深灰底 + 64 px 网格 + 亮色块；
// noise = 非周期的值噪声 + 亮色块，给截屏运动检测用），可在一段时间窗内横向滚动 / 闪白，按 --mode 隐藏、限制并每帧回中光标，
// N 秒后自动退出。滚动开始 / 结束与闪白时在 stdout 打印本地墙钟时刻，供验证脚本与 MotionCalm.log 对齐。
// 变更 5：--zoom / --roll 改画程序化场景（随缩放滚动的多八度值噪声 + 圆斑，1/6 分辨率逐像素计算后拉伸到全屏），
// 在时间窗内以恒定对数速率放大 / 缩小、以恒定角速度旋转；--selfcheck 不开窗，把同样的画面降采样后交给真实的估计器做离线自检。
// 任何退出路径（超时、按键、关窗、Ctrl+C、看门狗、崩溃）都会释放 ClipCursor 并恢复光标。
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "capture/motion_estimator.h"
#include "capture/similarity_estimator.h"

namespace {

enum class Mode { Lock, Hidden, Plain };
enum class ClipMode { Window, Center, None };
enum class Texture { Grid, Noise };

struct Options {
    double seconds = 10.0;
    Mode mode = Mode::Lock;
    int monitor = -1;          // -1 = 主显示器
    double scroll = 0.0;       // 背景横向滚动速度 px/s（正值：画面向左移，相当于向右转）
    double scroll_start = 0.0; // 从第几秒开始滚动
    double scroll_seconds = -1.0;   // 滚动持续多久（<0 = 一直滚到退出）；之后画面停在最后的位置
    Texture texture = Texture::Grid;
    double flash_at = -1.0;    // <0 = 不闪
    int fps = 60;
    ClipMode clip = ClipMode::Window;
    bool list_monitors = false;
    // 变更 5：前进 / 倾斜场景
    double zoom = 0.0;          // 对数缩放速率 d(ln s)/dt（1/s，正 = 放大 = 向前）；0 = 不缩放
    double zoom_start = 0.0;
    double zoom_seconds = -1.0;
    double roll = 0.0;          // 旋转角速度（°/s，正 = 屏幕上顺时针）；0 = 不旋转
    double roll_start = 0.0;
    double roll_seconds = -1.0;
    bool selfcheck = false;     // 不开窗：离线渲染 + 真实估计器，打印估计值与指令值
    bool scene() const { return zoom != 0.0 || roll != 0.0; }
};

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Lock: return "lock";
        case Mode::Hidden: return "hidden";
        case Mode::Plain: return "plain";
    }
    return "?";
}

const char* texture_name(Texture t) { return t == Texture::Noise ? "noise" : "grid"; }

const char* clip_name(ClipMode c) {
    switch (c) {
        case ClipMode::Window: return "window";
        case ClipMode::Center: return "center";
        case ClipMode::None: return "none";
    }
    return "?";
}

void print_help() {
    std::printf(
        "fakegame - borderless-fullscreen fake game window for MotionCalm integration tests\n"
        "\n"
        "usage: fakegame.exe [options]\n"
        "  --seconds N        run time, then exit (default 10, range 0.1..3600)\n"
        "  --mode M           lock | hidden | plain (default lock)\n"
        "                       lock   = hide cursor + ClipCursor + re-center cursor every frame\n"
        "                       hidden = hide cursor only\n"
        "                       plain  = normal visible cursor, no clip\n"
        "  --monitor i        monitor index from --list-monitors (default: primary)\n"
        "  --scroll px_per_s  scroll the background texture horizontally (default 0)\n"
        "  --scroll-start s   start scrolling at that second (default 0)\n"
        "  --scroll-seconds d scroll for d seconds, then hold the last position (default: until exit)\n"
        "  --texture T        grid | noise (default grid)\n"
        "                       grid  = 64 px grid + coloured blocks (periodic: a motion estimator can lock\n"
        "                               onto a wrong multiple of 64 px)\n"
        "                       noise = deterministic non-periodic value noise + coloured blocks, no grid\n"
        "  --flash-at s       at that second flip the background to white for 3 frames\n"
        "  --zoom r           scene mode: zoom the picture at a constant log rate r = d(ln scale)/dt per second\n"
        "                     (positive = expand = moving forward; e.g. 3 -> x1.051 per 60 Hz frame).\n"
        "                     MotionCalm's zoom= should read |r| x hfov/2 (default hfov 90 -> |r| x 45 deg/s)\n"
        "  --zoom-start s     start zooming at that second (default 0)\n"
        "  --zoom-seconds d   zoom for d seconds, then hold (default: until exit)\n"
        "  --roll deg_per_s   scene mode: rotate the picture about the screen centre (positive = clockwise)\n"
        "  --roll-start s     start rotating at that second (default 0)\n"
        "  --roll-seconds d   rotate for d seconds, then hold (default: until exit)\n"
        "                     Scene mode (any of --zoom / --roll) replaces the texture and ignores --scroll:\n"
        "                     procedural multi-octave value noise + soft discs whose octaves follow the zoom,\n"
        "                     evaluated per pixel at 1/6 resolution and stretched to the full monitor.\n"
        "  --selfcheck        no window: render the --zoom / --roll segments offline (60 fps timestamps),\n"
        "                     box-downscale to 160x90 like the capture path, run the real estimators and print\n"
        "                     estimated vs commanded rates (defaults when neither is given: --zoom 2 --roll 60,\n"
        "                     then the large rates --zoom 6.5 --roll 360). Exit code 0 = every segment\n"
        "                     within +-25 %%\n"
        "  --fps N            target frame rate (default 60, range 1..500)\n"
        "  --clip C           lock-mode clip rect: window | center (2x2 px at center) | none\n"
        "                     (default window = the full monitor rect)\n"
        "  --list-monitors    print the monitors and exit\n"
        "  --help             show this help and exit\n"
        "\n"
        "Esc or any key exits early. ClipCursor and the cursor are always restored on exit,\n"
        "including Ctrl+C / console close; a watchdog force-exits at seconds+5.\n"
        "Scroll / zoom / roll start + stop and the flash print a line with wall=<local yyyy-mm-ddThh:mm:ss.mmm>.\n");
}

bool parse_double(const wchar_t* s, double& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const double v = std::wcstod(s, &end);
    if (errno != 0 || !end || *end != L'\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parse_int(const wchar_t* s, int& out) {
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long v = std::wcstol(s, &end, 10);
    if (errno != 0 || !end || *end != L'\0' || v < -1000000 || v > 1000000) return false;
    out = static_cast<int>(v);
    return true;
}

// 返回 0 = 继续，1 = 已打印帮助（退出 0），2 = 参数错误。
int parse_args(int argc, wchar_t** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto need_value = [&](const wchar_t*& v) -> bool {
            if (i + 1 >= argc) {
                std::printf("fakegame: %ls needs a value\n", a.c_str());
                return false;
            }
            v = argv[++i];
            return true;
        };
        const wchar_t* v = nullptr;
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            print_help();
            return 1;
        } else if (a == L"--list-monitors") {
            o.list_monitors = true;
        } else if (a == L"--seconds") {
            if (!need_value(v) || !parse_double(v, o.seconds) || o.seconds < 0.1 || o.seconds > 3600) {
                std::printf("fakegame: bad --seconds (0.1..3600)\n");
                return 2;
            }
        } else if (a == L"--mode") {
            if (!need_value(v)) return 2;
            const std::wstring m = v;
            if (m == L"lock") o.mode = Mode::Lock;
            else if (m == L"hidden") o.mode = Mode::Hidden;
            else if (m == L"plain") o.mode = Mode::Plain;
            else {
                std::printf("fakegame: bad --mode '%ls' (lock|hidden|plain)\n", v);
                return 2;
            }
        } else if (a == L"--monitor") {
            if (!need_value(v) || !parse_int(v, o.monitor) || o.monitor < 0) {
                std::printf("fakegame: bad --monitor\n");
                return 2;
            }
        } else if (a == L"--scroll") {
            if (!need_value(v) || !parse_double(v, o.scroll) || std::fabs(o.scroll) > 100000) {
                std::printf("fakegame: bad --scroll\n");
                return 2;
            }
        } else if (a == L"--scroll-start") {
            if (!need_value(v) || !parse_double(v, o.scroll_start) || o.scroll_start < 0 || o.scroll_start > 3600) {
                std::printf("fakegame: bad --scroll-start (0..3600)\n");
                return 2;
            }
        } else if (a == L"--scroll-seconds") {
            if (!need_value(v) || !parse_double(v, o.scroll_seconds) || o.scroll_seconds < 0 || o.scroll_seconds > 3600) {
                std::printf("fakegame: bad --scroll-seconds (0..3600)\n");
                return 2;
            }
        } else if (a == L"--texture") {
            if (!need_value(v)) return 2;
            const std::wstring t = v;
            if (t == L"grid") o.texture = Texture::Grid;
            else if (t == L"noise") o.texture = Texture::Noise;
            else {
                std::printf("fakegame: bad --texture '%ls' (grid|noise)\n", v);
                return 2;
            }
        } else if (a == L"--flash-at") {
            if (!need_value(v) || !parse_double(v, o.flash_at) || o.flash_at < 0) {
                std::printf("fakegame: bad --flash-at\n");
                return 2;
            }
        } else if (a == L"--fps") {
            if (!need_value(v) || !parse_int(v, o.fps) || o.fps < 1 || o.fps > 500) {
                std::printf("fakegame: bad --fps (1..500)\n");
                return 2;
            }
        } else if (a == L"--zoom") {
            if (!need_value(v) || !parse_double(v, o.zoom) || std::fabs(o.zoom) > 20) {
                std::printf("fakegame: bad --zoom (-20..20 per second)\n");
                return 2;
            }
        } else if (a == L"--zoom-start") {
            if (!need_value(v) || !parse_double(v, o.zoom_start) || o.zoom_start < 0 || o.zoom_start > 3600) {
                std::printf("fakegame: bad --zoom-start (0..3600)\n");
                return 2;
            }
        } else if (a == L"--zoom-seconds") {
            if (!need_value(v) || !parse_double(v, o.zoom_seconds) || o.zoom_seconds < 0 || o.zoom_seconds > 3600) {
                std::printf("fakegame: bad --zoom-seconds (0..3600)\n");
                return 2;
            }
        } else if (a == L"--roll") {
            if (!need_value(v) || !parse_double(v, o.roll) || std::fabs(o.roll) > 3600) {
                std::printf("fakegame: bad --roll (-3600..3600 deg/s)\n");
                return 2;
            }
        } else if (a == L"--roll-start") {
            if (!need_value(v) || !parse_double(v, o.roll_start) || o.roll_start < 0 || o.roll_start > 3600) {
                std::printf("fakegame: bad --roll-start (0..3600)\n");
                return 2;
            }
        } else if (a == L"--roll-seconds") {
            if (!need_value(v) || !parse_double(v, o.roll_seconds) || o.roll_seconds < 0 || o.roll_seconds > 3600) {
                std::printf("fakegame: bad --roll-seconds (0..3600)\n");
                return 2;
            }
        } else if (a == L"--selfcheck") {
            o.selfcheck = true;
        } else if (a == L"--clip") {
            if (!need_value(v)) return 2;
            const std::wstring c = v;
            if (c == L"window") o.clip = ClipMode::Window;
            else if (c == L"center") o.clip = ClipMode::Center;
            else if (c == L"none") o.clip = ClipMode::None;
            else {
                std::printf("fakegame: bad --clip '%ls' (window|center|none)\n", v);
                return 2;
            }
        } else {
            std::printf("fakegame: unknown argument '%ls' (see --help)\n", a.c_str());
            return 2;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ monitors

struct MonitorEntry {
    HMONITOR handle = nullptr;
    RECT rect{};
    bool primary = false;
    std::wstring name;
};

BOOL CALLBACK enum_monitor_proc(HMONITOR hmon, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        MonitorEntry e;
        e.handle = hmon;
        e.rect = mi.rcMonitor;
        e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        e.name = mi.szDevice;
        list->push_back(e);
    }
    return TRUE;
}

std::vector<MonitorEntry> enum_monitors() {
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, enum_monitor_proc, reinterpret_cast<LPARAM>(&list));
    return list;
}

// ------------------------------------------------------------------ GDI canvas

struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;

    bool create(HWND ref, int width, int height) {
        HDC screen = GetDC(ref);
        dc = CreateCompatibleDC(screen);
        bmp = CreateCompatibleBitmap(screen, width, height);
        ReleaseDC(ref, screen);
        if (!dc || !bmp) return false;
        old = SelectObject(dc, bmp);
        w = width;
        h = height;
        return true;
    }
    void destroy() {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bmp = nullptr;
        old = nullptr;
    }
};

void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    const RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// 格点哈希 → [0, 1)（固定种子，每次运行画面一致）。
double lattice_value(std::uint32_t ix, std::uint32_t iy, std::uint32_t octave) {
    std::uint32_t h = ix * 0x8DA6B343u ^ iy * 0xD8163841u ^ (octave + 1u) * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0xC2B2AE35u;
    h ^= h >> 15;
    return static_cast<double>(h & 0xFFFFFFu) / 16777216.0;
}

// 一个八度的值噪声：边长 cell 的格点取随机值，smoothstep 双线性插值；x 方向每 period 个格点回绕（横向无缝）。
double value_noise(int x, int y, int cell, std::uint32_t period, std::uint32_t octave) {
    const int gx = x / cell, gy = y / cell;
    double tx = static_cast<double>(x - gx * cell) / cell;
    double ty = static_cast<double>(y - gy * cell) / cell;
    tx = tx * tx * (3.0 - 2.0 * tx);
    ty = ty * ty * (3.0 - 2.0 * ty);
    const std::uint32_t x0 = static_cast<std::uint32_t>(gx) % period, x1 = (x0 + 1u) % period;
    const std::uint32_t y0 = static_cast<std::uint32_t>(gy), y1 = y0 + 1u;
    const double a = lattice_value(x0, y0, octave), b = lattice_value(x1, y0, octave);
    const double c = lattice_value(x0, y1, octave), d = lattice_value(x1, y1, octave);
    const double top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

// noise 底图：4 个八度（128 / 64 / 32 / 16 px）的值噪声，灰度 ≈ 0.12–0.88，略带冷色调。没有任何规则网格：
// 唯一的周期是纹理宽度（≥ 屏幕宽，远超截屏位移搜索范围）。tex.w 须是 128 的倍数。
void paint_noise(Canvas& tex) {
    static constexpr int kCells[] = {128, 64, 32, 16};
    static constexpr double kAmps[] = {0.45, 0.28, 0.17, 0.10};
    std::vector<std::uint32_t> px(static_cast<std::size_t>(tex.w) * static_cast<std::size_t>(tex.h));
    for (int y = 0; y < tex.h; ++y) {
        for (int x = 0; x < tex.w; ++x) {
            double v = 0;
            for (int o = 0; o < 4; ++o) {
                const std::uint32_t period = static_cast<std::uint32_t>(tex.w / kCells[o]);
                v += kAmps[o] * value_noise(x, y, kCells[o], period, static_cast<std::uint32_t>(o));
            }
            v = 0.12 + 0.76 * v;   // 振幅和 = 1 → v ∈ [0.12, 0.88]
            const int g = static_cast<int>(v * 255.0 + 0.5);
            const int r = g * 92 / 100, b = std::min(255, g * 108 / 100);
            px[static_cast<std::size_t>(y) * static_cast<std::size_t>(tex.w) + static_cast<std::size_t>(x)] =
                static_cast<std::uint32_t>((r << 16) | (g << 8) | b);   // 0x00RRGGBB = BI_RGB 32 bpp 的 BGRA
        }
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = tex.w;
    bi.bmiHeader.biHeight = -tex.h;   // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(tex.dc, 0, 0, static_cast<DWORD>(tex.w), static_cast<DWORD>(tex.h), 0, 0, 0,
                      static_cast<UINT>(tex.h), px.data(), &bi, DIB_RGB_COLORS);
}

// 纹理：grid 宽度向上取整到 64 的倍数、noise 到 128 的倍数，横向无缝（越界的色块在另一侧补画）。
void build_texture(Canvas& tex, Texture kind) {
    if (kind == Texture::Noise) {
        paint_noise(tex);
    } else {
        fill(tex.dc, 0, 0, tex.w, tex.h, RGB(36, 38, 42));
        HBRUSH minor = CreateSolidBrush(RGB(70, 74, 82));
        HBRUSH major = CreateSolidBrush(RGB(112, 118, 130));
        for (int x = 0; x < tex.w; x += 64) {
            const RECT r{x, 0, x + 1, tex.h};
            FillRect(tex.dc, &r, (x / 64) % 4 == 0 ? major : minor);
        }
        for (int y = 0; y < tex.h; y += 64) {
            const RECT r{0, y, tex.w, y + 1};
            FillRect(tex.dc, &r, (y / 64) % 4 == 0 ? major : minor);
        }
        DeleteObject(minor);
        DeleteObject(major);
    }

    // 确定性的"随机"亮色块（LCG，固定种子 → 每次运行画面一致）。
    std::uint32_t seed = 0x2545F491u;
    auto rnd = [&seed]() -> std::uint32_t {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    static const COLORREF palette[] = {RGB(255, 96, 64),  RGB(255, 208, 64), RGB(96, 224, 96),
                                       RGB(64, 192, 255), RGB(176, 112, 255), RGB(255, 96, 192),
                                       RGB(236, 236, 236), RGB(255, 160, 32)};
    const int palette_n = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    const long long area = static_cast<long long>(tex.w) * tex.h;
    int count = static_cast<int>(area / 45000);
    if (count < 24) count = 24;
    for (int i = 0; i < count; ++i) {
        const int w = 16 + static_cast<int>(rnd() % 144u);
        const int h = 16 + static_cast<int>(rnd() % 144u);
        const int x = static_cast<int>(rnd() % static_cast<std::uint32_t>(tex.w));
        const int y_range = tex.h > h ? tex.h - h : 1;
        const int y = static_cast<int>(rnd() % static_cast<std::uint32_t>(y_range));
        const COLORREF c = palette[static_cast<int>(rnd() % static_cast<std::uint32_t>(palette_n))];
        fill(tex.dc, x, y, w, h, c);
        if (x + w > tex.w) fill(tex.dc, x - tex.w, y, w, h, c);   // 横向回绕
        // 一部分色块加一个深色内框，给运动估计更多边缘
        if (rnd() % 3u == 0) fill(tex.dc, x + w / 4, y + h / 4, w / 2, h / 2, RGB(20, 20, 24));
    }
}

// ------------------------------------------------------------------ scene mode（变更 5）

// 随缩放滚动的八度（Shepard 式）：八度 k 的屏幕周期 = kBasePeriod·S / 2^k（S = 当前放大倍数，低分辨率像素），
// 记 u = k − log2 S，权重 w(u) = exp(−(u − kCenterU)² / 2)，只算 |u − kCenterU| ≤ kSpanU 的八度。
// 于是任意时刻屏幕上的频谱都一样（无限放大也不会"用完"细节），而每个八度本身严格按相似变换运动。
constexpr double kBasePeriod = 48.0;
constexpr double kCenterU = 0.5;
constexpr double kSpanU = 2.6;

struct SceneRenderer {
    int lw = 0, lh = 0;
    std::vector<float> table;          // 256×256 格点随机值
    std::vector<std::uint32_t> px;     // 0x00RRGGBB，自上而下

    void init(int w, int h) {
        lw = w;
        lh = h;
        px.assign(static_cast<std::size_t>(lw) * static_cast<std::size_t>(lh), 0u);
        table.resize(256u * 256u);
        std::uint32_t seed = 0x1234567u;
        for (float& v : table) {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(seed >> 8) / 16777216.0f;
        }
    }
    float lat(int ix, int iy, int k) const {
        return table[static_cast<std::size_t>(((iy + k * 151) & 255) * 256 + ((ix + k * 73) & 255))];
    }
    // 一个八度在格坐标 (x, y) 处的值 ∈ 约 [−0.5, 0.5]：smoothstep 值噪声 + 每格至多一个软边圆斑（edge = 边缘宽度，格单位）。
    float octave(float x, float y, int k, float edge) const {
        const float fx = std::floor(x), fy = std::floor(y);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        float tx = x - fx, ty = y - fy;
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const float a = lat(ix, iy, k), b = lat(ix + 1, iy, k), c = lat(ix, iy + 1, k), d = lat(ix + 1, iy + 1, k);
        const float top = a + (b - a) * sx, bot = c + (d - c) * sx;
        float v = top + (bot - top) * sy - 0.5f;
        const float h = lat(ix + 17, iy + 29, k + 7);
        if (h < 0.16f) {
            const float dx = tx - 0.5f, dy = ty - 0.5f;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float m = std::clamp((0.32f - dist) / edge + 0.5f, 0.0f, 1.0f);
            v += (h < 0.08f ? 0.7f : -0.7f) * m;
        }
        return v;
    }
    // log2_s = log2(放大倍数)，rot_deg = 内容旋转角（屏幕上顺时针为正）。
    void render(double log2_s, double rot_deg) {
        struct Oct {
            int k;
            float w, m, edge;
        };
        Oct oct[16];
        int n = 0;
        double wsum2 = 0;
        const int k_lo = static_cast<int>(std::ceil(log2_s + kCenterU - kSpanU));
        const int k_hi = static_cast<int>(std::floor(log2_s + kCenterU + kSpanU));
        for (int k = k_lo; k <= k_hi && n < 16; ++k) {
            const double u = k - log2_s;
            const double w = std::exp(-0.5 * (u - kCenterU) * (u - kCenterU));
            const double period = kBasePeriod / std::exp2(u);   // 屏幕周期（低分辨率像素）
            oct[n++] = Oct{k, static_cast<float>(w), static_cast<float>(1.0 / period),
                           static_cast<float>(std::max(1.2 / period, 0.02))};
            wsum2 += w * w;
        }
        const float norm = static_cast<float>(0.75 / std::sqrt(std::max(wsum2, 1e-9)));
        const double th = rot_deg * 3.14159265358979323846 / 180.0;
        const float c = static_cast<float>(std::cos(th)), sn = static_cast<float>(std::sin(th));
        const float hx = 0.5f * lw, hy = 0.5f * lh;
        for (int j = 0; j < lh; ++j) {
            std::uint32_t* row = px.data() + static_cast<std::size_t>(j) * static_cast<std::size_t>(lw);
            const float py = static_cast<float>(j) + 0.5f - hy;
            for (int i = 0; i < lw; ++i) {
                const float pxv = static_cast<float>(i) + 0.5f - hx;
                // 内容旋转 θ：当前像素显示的是未旋转画面中 R(−θ)·p 处的内容（缩放已经体现在各八度的屏幕周期里）
                const float qx = c * pxv + sn * py;
                const float qy = -sn * pxv + c * py;
                float v = 0.0f;
                for (int o = 0; o < n; ++o) v += oct[o].w * octave(qx * oct[o].m, qy * oct[o].m, oct[o].k, oct[o].edge);
                v = std::clamp(0.5f + norm * v, 0.04f, 0.96f);
                const int g = static_cast<int>(v * 255.0f + 0.5f);
                const int r = g * 92 / 100, b = std::min(255, g * 108 / 100);
                row[i] = static_cast<std::uint32_t>((r << 16) | (g << 8) | b);
            }
        }
    }
    // 截屏路径的近似：低分辨率 → 拉伸 → GPU 盒式平均到 gw×gh 的灰度（0.299R + 0.587G + 0.114B）。
    void downscale(int gw, int gh, std::vector<float>& out) const {
        out.assign(static_cast<std::size_t>(gw) * static_cast<std::size_t>(gh), 0.0f);
        for (int y = 0; y < gh; ++y) {
            const int y0 = y * lh / gh, y1 = std::max(y0 + 1, (y + 1) * lh / gh);
            for (int x = 0; x < gw; ++x) {
                const int x0 = x * lw / gw, x1 = std::max(x0 + 1, (x + 1) * lw / gw);
                double acc = 0;
                for (int yy = y0; yy < y1; ++yy) {
                    for (int xx = x0; xx < x1; ++xx) {
                        const std::uint32_t p = px[static_cast<std::size_t>(yy) * static_cast<std::size_t>(lw) +
                                                   static_cast<std::size_t>(xx)];
                        acc += (0.299 * ((p >> 16) & 255) + 0.587 * ((p >> 8) & 255) + 0.114 * (p & 255)) / 255.0;
                    }
                }
                out[static_cast<std::size_t>(y) * static_cast<std::size_t>(gw) + static_cast<std::size_t>(x)] =
                    static_cast<float>(acc / ((y1 - y0) * (x1 - x0)));
            }
        }
    }
};

// 时间窗内的累计量：窗前 0，窗内线性增长，窗后保持。
double window_amount(double t, double rate, double start, double seconds) {
    if (rate == 0.0) return 0.0;
    const double end = seconds >= 0 ? start + seconds : 1e300;
    return rate * (std::clamp(t, start, end) - start);
}

// --selfcheck：离线渲染 1920×1080 的 1/6（320×180），降到 160×90 交给真实估计器（与 CaptureService 相同的调用方式）。
int run_selfcheck(Options o) {
    // 默认（两者都没给）：日常游动量级 --zoom 2（90 °/s 前进流速，中档目标强度约 0.56）与 --roll 60，
    // 再加上 v1.1 初版的大幅速度 --zoom 6.5 / --roll 360（确认大运动仍能收敛）。
    const bool defaults = !o.scene();
    if (defaults) {
        o.zoom = 2.0;
        o.roll = 60.0;
    }
    const int lw = 320, lh = 180, gw = 160, gh = 90;
    const double fps = 60.0, hfov = 90.0;
    SceneRenderer sr;
    sr.init(lw, lh);
    std::printf("[fakegame] selfcheck: %dx%d scene -> %dx%d grid, %.0f fps, hfov %.0f\n", lw, lh, gw, gh, fps, hfov);
    int failures = 0;
    auto run = [&](const char* name, double zoom_rate, double roll_rate) {
        mc::MotionEstimator pan;
        mc::SimilarityEstimator sim;
        sim.set_refine_thresholds(4.0 / (hfov / 2.0), 4.0);   // 与 CaptureService 相同的省算力门槛
        std::vector<float> grid;
        std::vector<double> zooms, rolls, pans;
        int valid = 0, total = 0;
        double render_ms = 0, sim_ms = 0;
        LARGE_INTEGER f{}, a{}, b{};
        QueryPerformanceFrequency(&f);
        const int frames = 90;   // 1.5 s；前 10 帧不计（历史环未满）
        for (int k = 0; k < frames; ++k) {
            const double t = k / fps;
            QueryPerformanceCounter(&a);
            sr.render(zoom_rate * t / std::log(2.0), roll_rate * t);
            QueryPerformanceCounter(&b);
            render_ms += 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
            sr.downscale(gw, gh, grid);
            const mc::MotionEstimate pe = pan.push(grid.data(), t);
            const mc::TranslationHint hint{pe.dx, pe.dy};
            QueryPerformanceCounter(&a);
            const mc::SimilarityEstimate se = sim.push(grid.data(), t, pe.valid ? &hint : nullptr);
            QueryPerformanceCounter(&b);
            sim_ms += 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
            if (k < 10) continue;
            ++total;
            if (pe.valid) {
                const auto w = mc::shift_to_deg_per_s(pe.dx, pe.dy, gw, gh, 1920, 1080, hfov, pe.dt_s);
                if (w) pans.push_back(*w);
            }
            if (!se.valid) continue;
            ++valid;
            if (const auto z = mc::zoom_to_deg_per_s(se.scale, se.dt_s, hfov)) zooms.push_back(*z);
            if (const auto r = mc::roll_to_deg_per_s(se.rotation_deg, se.dt_s)) rolls.push_back(*r);
        }
        auto median = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double zexp = std::fabs(zoom_rate) * hfov / 2.0, rexp = std::fabs(roll_rate);
        const double zm = median(zooms), rm = median(rolls), pm = median(pans);
        // 没有指令的一项必须低于前进 / 倾斜阈值最低一档的起点（高灵敏度：前进 10 °/s、倾斜 8 °/s）
        const bool zok = zexp == 0.0 ? zm < 5.0 : std::fabs(zm - zexp) <= 0.25 * zexp;
        const bool rok = rexp == 0.0 ? rm < 5.0 : std::fabs(rm - rexp) <= 0.25 * rexp;
        const bool vok = valid >= total * 9 / 10;
        std::printf("[fakegame] selfcheck %-5s: valid %d/%d  zoom median %.1f deg/s (expected %.1f) %s  "
                    "roll median %.1f deg/s (expected %.1f) %s  pan median %.1f deg/s  render %.2f ms/frame  "
                    "estimator %.3f ms/frame\n",
                    name, valid, total, zm, zexp, zok ? "ok" : "FAIL", rm, rexp, rok ? "ok" : "FAIL", pm,
                    render_ms / frames, sim_ms / frames);
        if (!zok || !rok || !vok) ++failures;
    };
    if (o.zoom != 0.0) run("zoom", o.zoom, 0.0);
    if (o.roll != 0.0) run("roll", 0.0, o.roll);
    if (defaults) {
        run("zoom+", 6.5, 0.0);
        run("roll+", 0.0, 360.0);
    }
    run("still", 0.0, 0.0);
    std::printf("[fakegame] selfcheck %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

// ------------------------------------------------------------------ global state

Options g_opt;
HWND g_hwnd = nullptr;
std::atomic<bool> g_quit{false};
std::atomic<int> g_quit_reason{0};   // 0 timeout, 1 key, 2 close, 3 ctrl-c
bool g_active = false;
bool g_clip_applied = false;
bool g_cursor_hidden = false;
RECT g_clip_rect{};
POINT g_center{};

void release_clip() {
    ClipCursor(nullptr);
    g_clip_applied = false;
}

void hide_cursor() {
    if (g_cursor_hidden) return;
    int guard = 0;
    while (ShowCursor(FALSE) >= 0 && ++guard < 64) {
    }
    g_cursor_hidden = true;
}

void show_cursor() {
    if (!g_cursor_hidden) return;
    int guard = 0;
    while (ShowCursor(TRUE) < 0 && ++guard < 64) {
    }
    g_cursor_hidden = false;
}

void restore_everything() {
    release_clip();
    show_cursor();
}

BOOL WINAPI console_ctrl_handler(DWORD type) {
    // 在系统创建的线程里执行：ClipCursor 是全局的，可直接释放；其余清理交给主线程。
    ClipCursor(nullptr);
    g_quit_reason = 3;
    g_quit = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        Sleep(500);   // 返回后进程会被结束，给主线程一点时间收尾
    }
    return TRUE;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS*) {
    ClipCursor(nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR:
            if (g_opt.mode != Mode::Plain && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // bit 30 = 之前已按下（自动重复 / 窗口出现前就按着的键）→ 忽略，只认新按下的键
            if ((lp & (1LL << 30)) == 0) {
                g_quit_reason = 1;
                g_quit = true;
            }
            return 0;
        case WM_SYSCHAR:
        case WM_CHAR:
            return 0;
        case WM_ACTIVATE:
            g_active = LOWORD(wp) != WA_INACTIVE;
            if (!g_active) release_clip();   // 像真游戏一样：切走就放开光标
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            g_quit_reason = 2;
            g_quit = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void bring_to_foreground(HWND hwnd) {
    if (GetForegroundWindow() == hwnd) return;
    SetForegroundWindow(hwnd);
    if (GetForegroundWindow() == hwnd) return;

    // 回退 1：挂到当前前台线程的输入队列上再设置（前台锁的常用绕法）。
    const HWND fg = GetForegroundWindow();
    const DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    if (fg_thread != 0 && fg_thread != me && AttachThreadInput(me, fg_thread, TRUE)) {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(me, fg_thread, FALSE);
    }
    if (GetForegroundWindow() == hwnd) return;

    // 回退 2：SwitchToThisWindow（Alt-Tab 同款切换）。
    SwitchToThisWindow(hwnd, TRUE);
}

// 每帧：前台且激活时保持 ClipCursor 并把光标拉回中心；否则放开。
void update_lock() {
    if (g_opt.mode != Mode::Lock) return;
    const bool want = g_active && GetForegroundWindow() == g_hwnd;
    if (!want) {
        if (g_clip_applied) release_clip();
        return;
    }
    if (g_opt.clip != ClipMode::None) {
        RECT cur{};
        GetClipCursor(&cur);
        if (!EqualRect(&cur, &g_clip_rect)) ClipCursor(&g_clip_rect);
        g_clip_applied = true;
    }
    SetCursorPos(g_center.x, g_center.y);
}

double qpc_seconds(LONGLONG ticks, LONGLONG freq) {
    return static_cast<double>(ticks) / static_cast<double>(freq);
}

// 本地墙钟 "yyyy-mm-ddThh:mm:ss.mmm"（与 inputsim 的 t0= / t1= 相同写法）。
std::string wall_now() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    SYSTEMTIME utc{}, loc{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", loc.wYear, loc.wMonth, loc.wDay, loc.wHour,
                  loc.wMinute, loc.wSecond, loc.wMilliseconds);
    return buf;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const int pr = parse_args(argc, argv, g_opt);
    if (pr == 1) return 0;
    if (pr != 0) return 2;
    if (g_opt.selfcheck) return run_selfcheck(g_opt);

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();

    const auto monitors = enum_monitors();
    if (g_opt.list_monitors) {
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            const RECT& r = monitors[i].rect;
            std::printf("monitor %zu: %ls  %ld,%ld  %ldx%ld%s\n", i, monitors[i].name.c_str(), r.left, r.top,
                        r.right - r.left, r.bottom - r.top, monitors[i].primary ? "  (primary)" : "");
        }
        return 0;
    }
    if (monitors.empty()) {
        std::printf("fakegame: no monitors found\n");
        return 1;
    }
    std::size_t mon_index = 0;
    if (g_opt.monitor >= 0) {
        if (static_cast<std::size_t>(g_opt.monitor) >= monitors.size()) {
            std::printf("fakegame: --monitor %d out of range (have %zu, see --list-monitors)\n", g_opt.monitor,
                        monitors.size());
            return 2;
        }
        mon_index = static_cast<std::size_t>(g_opt.monitor);
    } else {
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].primary) mon_index = i;
        }
    }
    const MonitorEntry& mon = monitors[mon_index];
    const int W = mon.rect.right - mon.rect.left;
    const int H = mon.rect.bottom - mon.rect.top;
    g_center = POINT{mon.rect.left + W / 2, mon.rect.top + H / 2};
    g_clip_rect = g_opt.clip == ClipMode::Center
                      ? RECT{g_center.x - 1, g_center.y - 1, g_center.x + 1, g_center.y + 1}
                      : mon.rect;

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(crash_filter);
    timeBeginPeriod(1);

    // 看门狗：主循环万一卡住，到点也一定释放光标并退出。
    const DWORD watchdog_ms = static_cast<DWORD>((g_opt.seconds + 5.0) * 1000.0);
    std::thread([watchdog_ms]() {
        Sleep(watchdog_ms);
        ClipCursor(nullptr);
        std::printf("[fakegame] watchdog: main loop overran, forcing exit\n");
        ExitProcess(3);
    }).detach();

    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = g_opt.mode == Mode::Plain ? LoadCursorW(nullptr, IDC_ARROW) : nullptr;
    wc.lpszClassName = L"MotionCalmFakeGame";
    if (!RegisterClassExW(&wc)) {
        std::printf("fakegame: RegisterClassEx failed (%lu)\n", GetLastError());
        timeEndPeriod(1);
        return 1;
    }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"FAKEGAME", WS_POPUP, mon.rect.left, mon.rect.top, W, H,
                             nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) {
        std::printf("fakegame: CreateWindowEx failed (%lu)\n", GetLastError());
        timeEndPeriod(1);
        return 1;
    }

    Canvas back, tex;
    const int tex_unit = g_opt.texture == Texture::Noise ? 128 : 64;
    const int tex_w = ((W + tex_unit - 1) / tex_unit) * tex_unit;
    if (!back.create(g_hwnd, W, H) || !tex.create(g_hwnd, tex_w, H)) {
        std::printf("fakegame: cannot create %dx%d GDI buffers\n", W, H);
        DestroyWindow(g_hwnd);
        timeEndPeriod(1);
        return 1;
    }
    build_texture(tex, g_opt.texture);
    // 场景模式：1/6 分辨率逐像素渲染，StretchDIBits 拉伸到全屏（最近邻）。
    SceneRenderer scene;
    BITMAPINFO scene_bi{};
    if (g_opt.scene()) {
        scene.init(std::max(16, (W + 3) / 6), std::max(9, (H + 3) / 6));
        scene_bi.bmiHeader.biSize = sizeof scene_bi.bmiHeader;
        scene_bi.bmiHeader.biWidth = scene.lw;
        scene_bi.bmiHeader.biHeight = -scene.lh;
        scene_bi.bmiHeader.biPlanes = 1;
        scene_bi.bmiHeader.biBitCount = 32;
        scene_bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(back.dc, COLORONCOLOR);
    }

    HFONT font_title = CreateFontW(-72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT font_small = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    char flash_buf[32] = "off";
    if (g_opt.flash_at >= 0) std::snprintf(flash_buf, sizeof flash_buf, "%.2fs", g_opt.flash_at);
    char scroll_buf[64] = "always";
    if (g_opt.scroll_seconds >= 0) {
        std::snprintf(scroll_buf, sizeof scroll_buf, "%.2f-%.2fs", g_opt.scroll_start, g_opt.scroll_start + g_opt.scroll_seconds);
    } else if (g_opt.scroll_start > 0) {
        std::snprintf(scroll_buf, sizeof scroll_buf, "from %.2fs", g_opt.scroll_start);
    }
    std::printf("[fakegame] monitor %zu %ls %ld,%ld %dx%d%s | mode=%s clip=%s seconds=%.1f fps=%d texture=%s "
                "scroll=%.0fpx/s (%s) flash_at=%s\n",
                mon_index, mon.name.c_str(), mon.rect.left, mon.rect.top, W, H, mon.primary ? " (primary)" : "",
                mode_name(g_opt.mode), clip_name(g_opt.clip), g_opt.seconds, g_opt.fps, texture_name(g_opt.texture),
                g_opt.scroll, g_opt.scroll != 0.0 ? scroll_buf : "off", flash_buf);
    if (g_opt.scene()) {
        std::printf("[fakegame] scene %dx%d (stretched): zoom=%.3f/s from %.2fs for %s, roll=%.1f deg/s from %.2fs for %s "
                    "(texture/scroll ignored)\n",
                    scene.lw, scene.lh, g_opt.zoom, g_opt.zoom_start,
                    g_opt.zoom_seconds >= 0 ? (std::to_string(g_opt.zoom_seconds) + "s").c_str() : "ever", g_opt.roll,
                    g_opt.roll_start, g_opt.roll_seconds >= 0 ? (std::to_string(g_opt.roll_seconds) + "s").c_str() : "ever");
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    bring_to_foreground(g_hwnd);
    g_active = GetForegroundWindow() == g_hwnd;
    if (g_opt.mode != Mode::Plain) hide_cursor();
    std::printf("[fakegame] foreground=%s\n", g_active ? "yes" : "no (will keep retrying for 2 s)");

    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const LONGLONG q0 = now.QuadPart;
    auto elapsed = [&]() {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return qpc_seconds(c.QuadPart - q0, freq.QuadPart);
    };

    const double frame_dt = 1.0 / g_opt.fps;
    double next_frame = 0.0;
    long long frames = 0;
    int flash_left = 0;
    bool flash_done = g_opt.flash_at < 0;
    const double scroll_end = g_opt.scroll_seconds >= 0 ? g_opt.scroll_start + g_opt.scroll_seconds : 1e300;
    bool scroll_started = g_opt.scroll == 0.0;   // 不滚动时不打印开始 / 结束行
    bool scroll_stopped = g_opt.scroll == 0.0;
    bool zoom_started = g_opt.zoom == 0.0, zoom_stopped = g_opt.zoom == 0.0 || g_opt.zoom_seconds < 0;
    bool roll_started = g_opt.roll == 0.0, roll_stopped = g_opt.roll == 0.0 || g_opt.roll_seconds < 0;
    double render_ms_sum = 0;
    long long scene_frames = 0;
    bool fg_reported = g_active;
    wchar_t text[256];

    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        const double t = elapsed();
        if (t >= g_opt.seconds) {
            g_quit_reason = 0;
            break;
        }

        if (t >= next_frame) {
            // 前 2 秒里如果没拿到前台，继续尝试（启动器的前台权限可能晚一点才到）。
            if (GetForegroundWindow() != g_hwnd && t < 2.0 && frames % 10 == 0) bring_to_foreground(g_hwnd);
            if (!fg_reported && GetForegroundWindow() == g_hwnd) {
                fg_reported = true;
                g_active = true;
                std::printf("[fakegame] foreground=yes at t=%.2fs\n", t);
            }
            update_lock();

            if (!flash_done && t >= g_opt.flash_at) {
                flash_done = true;
                flash_left = 3;
                std::printf("[fakegame] flash at t=%.3fs (frame %lld) wall=%s\n", t, frames, wall_now().c_str());
            }
            if (!scroll_started && t >= g_opt.scroll_start) {
                scroll_started = true;
                std::printf("[fakegame] scroll start t=%.3fs (frame %lld) wall=%s\n", t, frames, wall_now().c_str());
            }
            if (scroll_started && !scroll_stopped && t >= scroll_end) {
                scroll_stopped = true;
                std::printf("[fakegame] scroll stop t=%.3fs (frame %lld) wall=%s\n", t, frames, wall_now().c_str());
            }
            if (!zoom_started && t >= g_opt.zoom_start) {
                zoom_started = true;
                std::printf("[fakegame] zoom start t=%.3fs (frame %lld) rate=%.3f/s wall=%s\n", t, frames, g_opt.zoom,
                            wall_now().c_str());
            }
            if (zoom_started && !zoom_stopped && t >= g_opt.zoom_start + g_opt.zoom_seconds) {
                zoom_stopped = true;
                std::printf("[fakegame] zoom stop t=%.3fs (frame %lld) wall=%s\n", t, frames, wall_now().c_str());
            }
            if (!roll_started && t >= g_opt.roll_start) {
                roll_started = true;
                std::printf("[fakegame] roll start t=%.3fs (frame %lld) rate=%.1fdeg/s wall=%s\n", t, frames, g_opt.roll,
                            wall_now().c_str());
            }
            if (roll_started && !roll_stopped && t >= g_opt.roll_start + g_opt.roll_seconds) {
                roll_stopped = true;
                std::printf("[fakegame] roll stop t=%.3fs (frame %lld) wall=%s\n", t, frames, wall_now().c_str());
            }

            const bool flashing = flash_left > 0;
            if (flashing) {
                PatBlt(back.dc, 0, 0, W, H, WHITENESS);
                --flash_left;
            } else if (g_opt.scene()) {
                LARGE_INTEGER r0{}, r1{};
                QueryPerformanceCounter(&r0);
                const double ln_s = window_amount(t, g_opt.zoom, g_opt.zoom_start, g_opt.zoom_seconds);
                const double rot = window_amount(t, g_opt.roll, g_opt.roll_start, g_opt.roll_seconds);
                scene.render(ln_s / std::log(2.0), rot);
                StretchDIBits(back.dc, 0, 0, W, H, 0, 0, scene.lw, scene.lh, scene.px.data(), &scene_bi, DIB_RGB_COLORS,
                              SRCCOPY);
                QueryPerformanceCounter(&r1);
                render_ms_sum += 1000.0 * qpc_seconds(r1.QuadPart - r0.QuadPart, freq.QuadPart);
                ++scene_frames;
            } else {
                int off = 0;
                if (g_opt.scroll != 0.0) {
                    // 滚动时间窗内匀速移动；窗前停在 0，窗后停在最后的位置。
                    const double ts = std::clamp(t, g_opt.scroll_start, scroll_end) - g_opt.scroll_start;
                    double o = std::fmod(ts * g_opt.scroll, static_cast<double>(tex.w));
                    if (o < 0) o += tex.w;
                    off = static_cast<int>(o);
                    if (off < 0 || off >= tex.w) off = 0;
                }
                const int first = tex.w - off;   // 纹理 [off, tex.w) 这一段
                if (first >= W) {
                    BitBlt(back.dc, 0, 0, W, H, tex.dc, off, 0, SRCCOPY);
                } else {
                    BitBlt(back.dc, 0, 0, first, H, tex.dc, off, 0, SRCCOPY);
                    BitBlt(back.dc, first, 0, W - first, H, tex.dc, 0, 0, SRCCOPY);
                }
            }

            SetBkMode(back.dc, TRANSPARENT);
            SetTextColor(back.dc, flashing ? RGB(0, 0, 0) : RGB(240, 240, 240));
            HGDIOBJ old_font = SelectObject(back.dc, font_title);
            RECT r_title{40, 24, W - 40, 120};
            DrawTextW(back.dc, L"FAKEGAME", -1, &r_title, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(back.dc, font_small);
            std::swprintf(text, 256, L"frame %lld   t=%6.2f / %.1f s", frames, t, g_opt.seconds);
            RECT r_counter{40, 32, W - 40, 64};
            DrawTextW(back.dc, text, -1, &r_counter, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            std::swprintf(text, 256, L"mode=%hs  clip=%hs  texture=%hs  scroll=%.0f px/s  fg=%ls", mode_name(g_opt.mode),
                          clip_name(g_opt.clip), texture_name(g_opt.texture), g_opt.scroll,
                          GetForegroundWindow() == g_hwnd ? L"yes" : L"no");
            RECT r_info{40, 64, W - 40, 96};
            DrawTextW(back.dc, text, -1, &r_info, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(back.dc, old_font);

            HDC wdc = GetDC(g_hwnd);
            BitBlt(wdc, 0, 0, W, H, back.dc, 0, 0, SRCCOPY);
            ReleaseDC(g_hwnd, wdc);
            GdiFlush();

            ++frames;
            next_frame += frame_dt;
            if (next_frame < t) next_frame = t + frame_dt;   // 落后太多就不追帧
        }

        const double wait_s = next_frame - elapsed();
        if (wait_s > 0) {
            const DWORD ms = static_cast<DWORD>(std::ceil(wait_s * 1000.0));
            MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    const double total = elapsed();
    restore_everything();
    DeleteObject(font_title);
    DeleteObject(font_small);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    back.destroy();
    tex.destroy();
    timeEndPeriod(1);

    static const char* reasons[] = {"timeout", "key", "close", "ctrl-c"};
    const int reason = g_quit_reason.load();
    std::printf("[fakegame] exit reason=%s frames=%lld elapsed=%.2fs avg_fps=%.1f\n",
                reasons[(reason >= 0 && reason < 4) ? reason : 0], frames, total,
                total > 0 ? static_cast<double>(frames) / total : 0.0);
    if (scene_frames > 0) {
        std::printf("[fakegame] scene render %.2f ms/frame (%lld frames)\n", render_ms_sum / static_cast<double>(scene_frames),
                    scene_frames);
    }
    return 0;
}
