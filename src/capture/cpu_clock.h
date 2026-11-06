// cpu_clock.h — 精确的线程 / 进程 CPU 时间：QueryThreadCycleTime / QueryProcessCycleTime 的周期数按 TSC 频率换算（TSC 频率用 QPC 校准一次，约 10 ms）。
// GetThreadTimes / GetProcessTimes 按时钟中断（15.6 ms）采样计费，对每帧只忙 0.1 ms 的截屏线程几乎总是读到 0，所以只作后备。
#pragma once

#include <windows.h>
#include <intrin.h>

#include "util/clock.h"

namespace mc {

// TSC 频率（Hz）。第一次调用时校准（阻塞约 10 ms），之后直接返回；不可用时为 0。
inline double tsc_frequency() {
    static const double hz = [] {
        const unsigned long long t0 = __rdtsc();
        const double q0 = qpc_now_s();
        ::Sleep(10);
        unsigned long long t1 = 0;
        double el = 0;
        do {
            t1 = __rdtsc();
            el = qpc_now_s() - q0;
        } while (el < 0.010);
        const double f = static_cast<double>(t1 - t0) / el;
        return f > 1e8 ? f : 0.0;
    }();
    return hz;
}

inline double filetime_seconds(const FILETIME& f) {
    ULARGE_INTEGER u{};
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return static_cast<double>(u.QuadPart) * 1e-7;
}

// 当前线程累计 CPU 时间（内核 + 用户，秒）。
inline double thread_cpu_seconds() {
    const double hz = tsc_frequency();
    ULONG64 cycles = 0;
    if (hz > 0 && ::QueryThreadCycleTime(::GetCurrentThread(), &cycles)) return static_cast<double>(cycles) / hz;
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetThreadTimes(::GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return filetime_seconds(k) + filetime_seconds(u);
}

// 当前进程（所有线程）累计 CPU 时间（秒）。
inline double process_cpu_seconds() {
    const double hz = tsc_frequency();
    ULONG64 cycles = 0;
    if (hz > 0 && ::QueryProcessCycleTime(::GetCurrentProcess(), &cycles)) return static_cast<double>(cycles) / hz;
    FILETIME c{}, e{}, k{}, u{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return filetime_seconds(k) + filetime_seconds(u);
}

}  // namespace mc
