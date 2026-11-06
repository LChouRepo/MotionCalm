// clock.h — 单调时钟：QueryPerformanceCounter 换算成秒（与 GameDetector 使用同一时间基准）。
#pragma once

#include <windows.h>

namespace mc {

// QPC 频率（每秒计数），首次调用时查询并缓存。
inline double qpc_frequency() {
    static const double freq = [] {
        LARGE_INTEGER li{};
        ::QueryPerformanceFrequency(&li);
        return li.QuadPart > 0 ? static_cast<double>(li.QuadPart) : 1.0;
    }();
    return freq;
}

// 当前单调时间（秒）。
inline double qpc_now_s() {
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    return static_cast<double>(li.QuadPart) / qpc_frequency();
}

}  // namespace mc
