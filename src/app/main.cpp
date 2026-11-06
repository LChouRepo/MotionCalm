// main.cpp — MotionCalm.exe 入口（wWinMain）：单实例互斥量（第二个实例通知第一个实例后以 0 退出），然后构造 App 并运行。
#include <windows.h>

#include "app/app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmdline*/, int /*show*/) {
    // 单实例（design D12）：在日志初始化之前检查；第二个实例不建托盘、不建 overlay、不写日志。
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, mc::kInstanceMutexName);
    if (mutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 第一个实例可能还在启动：最多等 0.5 s 找它的 host 窗口。
        for (int i = 0; i < 10; ++i) {
            if (HWND host = ::FindWindowW(mc::kHostClassName, nullptr)) {
                ::PostMessageW(host, mc::WM_APP_ALREADY_RUNNING, 0, 0);
                break;
            }
            ::Sleep(50);
        }
        ::CloseHandle(mutex);
        return 0;
    }

    int rc = 0;
    {
        mc::App app(instance);
        rc = app.run();
    }
    if (mutex != nullptr) ::CloseHandle(mutex);   // 互斥量保持到进程结束
    return rc;
}
