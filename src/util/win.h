// win.h — Win32 公共头与 RAII 小工具：ComPtr 别名、UniqueHandle（CloseHandle）、UniqueHwnd（DestroyWindow）、ScopeExit。
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <utility>

namespace mc {

// Microsoft::WRL::ComPtr 的短别名。
template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// 独占持有一个内核对象句柄，析构时 CloseHandle。nullptr 与 INVALID_HANDLE_VALUE 都视为空。
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // 当前句柄（可能为空）。
    HANDLE get() const noexcept { return h_; }
    // 是否持有有效句柄。
    bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }
    // 关闭旧句柄并接管 h。
    void reset(HANDLE h = nullptr) noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = h;
    }
    // 放弃所有权并返回句柄。
    HANDLE release() noexcept { return std::exchange(h_, nullptr); }

private:
    HANDLE h_ = nullptr;
};

// 独占持有一个窗口，析构时 DestroyWindow（只能在创建它的线程上析构）。
class UniqueHwnd {
public:
    UniqueHwnd() noexcept = default;
    explicit UniqueHwnd(HWND h) noexcept : h_(h) {}
    ~UniqueHwnd() { reset(); }
    UniqueHwnd(UniqueHwnd&& o) noexcept : h_(o.release()) {}
    UniqueHwnd& operator=(UniqueHwnd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueHwnd(const UniqueHwnd&) = delete;
    UniqueHwnd& operator=(const UniqueHwnd&) = delete;

    // 当前窗口句柄（可能为空）。
    HWND get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
    // 销毁旧窗口并接管 h。
    void reset(HWND h = nullptr) noexcept {
        if (h_ != nullptr) ::DestroyWindow(h_);
        h_ = h;
    }
    // 放弃所有权并返回句柄（例如窗口已在 WM_DESTROY 中自行销毁时）。
    HWND release() noexcept { return std::exchange(h_, nullptr); }

private:
    HWND h_ = nullptr;
};

// 作用域结束时调用 f（除非先 dismiss）。用法：auto guard = mc::ScopeExit([&] { ... });
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) noexcept(noexcept(F(std::move(f)))) : f_(std::move(f)) {}
    ~ScopeExit() {
        if (active_) f_();
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    // 取消这次清理。
    void dismiss() noexcept { active_ = false; }

private:
    F f_;
    bool active_ = true;
};

}  // namespace mc
