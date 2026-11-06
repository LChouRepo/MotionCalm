// log.cpp — util/log 的实现：互斥量保护的单例文件句柄，按累计大小 1 MB 轮转（删 .1 → 改名 → 重开）。
#include "util/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace mc::log {
namespace {

struct State {
    std::mutex mu;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::uint64_t size = 0;
    std::atomic<int> level{static_cast<int>(Level::Info)};
    std::string pending;              // 还没写进文件的 debug 行（见 write）
    ULONGLONG pending_since_ms = 0;   // pending 中最早一行的 GetTickCount64
};

constexpr std::size_t kPendingMaxBytes = 16 * 1024;

State& state() {
    static State s;   // 函数内静态：首次使用时构造，没有全局构造副作用
    return s;
}

HANDLE open_append(const std::wstring& path, std::uint64_t* size) {
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    LARGE_INTEGER li{};
    *size = ::GetFileSizeEx(h, &li) ? static_cast<std::uint64_t>(li.QuadPart) : 0;
    return h;
}

// 调用方持有 mu。
void rotate_locked(State& s) {
    ::CloseHandle(s.file);
    s.file = INVALID_HANDLE_VALUE;
    const std::wstring old_path = s.path + L".1";
    ::DeleteFileW(old_path.c_str());
    const bool moved = ::MoveFileExW(s.path.c_str(), old_path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    std::uint64_t size = 0;
    s.file = open_append(s.path, &size);
    // 改名失败（例如别的程序以不共享删除的方式打开着它）时继续追加，再写满 1 MB 才重试。
    s.size = moved ? size : 0;
}

// 把 pending 写进文件（必要时先轮转）。调用方持有 mu。
void flush_locked(State& s) {
    if (s.pending.empty()) return;
    if (s.file != INVALID_HANDLE_VALUE && s.size >= kRotateBytes) rotate_locked(s);
    if (s.file != INVALID_HANDLE_VALUE) {
        const char* p = s.pending.data();
        std::size_t left = s.pending.size();
        while (left > 0) {
            const DWORD chunk = left > 0x40000000u ? 0x40000000u : static_cast<DWORD>(left);
            DWORD written = 0;
            if (!::WriteFile(s.file, p, chunk, &written, nullptr) || written == 0) break;
            p += written;
            left -= written;
            s.size += written;
        }
    }
    s.pending.clear();
}

}  // namespace

bool init(const std::wstring& path, Level lv) {
    State& s = state();
    std::lock_guard lock(s.mu);
    flush_locked(s);
    if (s.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    s.path = path;
    s.level.store(static_cast<int>(lv));
    std::uint64_t size = 0;
    s.file = open_append(path, &size);
    s.size = size;
    return s.file != INVALID_HANDLE_VALUE;
}

void shutdown() {
    State& s = state();
    std::lock_guard lock(s.mu);
    flush_locked(s);
    if (s.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
}

void set_level(Level lv) { state().level.store(static_cast<int>(lv)); }

Level level() { return static_cast<Level>(state().level.load()); }

bool enabled(Level lv) { return static_cast<int>(lv) >= state().level.load(); }

std::wstring path() {
    State& s = state();
    std::lock_guard lock(s.mu);
    return s.path;
}

void write(Level lv, std::string_view msg) {
    if (!enabled(lv)) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::string line = format_line(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                                   st.wMilliseconds, lv, msg);
    line += "\r\n";

    // debug 行（--verbose 时每帧一行）先攒在内存里，最多攒 kFlushIntervalMs 或 16 KB 再一次写出：逐行 WriteFile
    // 的系统调用（加上杀毒软件的文件过滤）比格式化本身贵得多。info / warn 行立即连同之前攒的行一起写出，
    // 顺序不变；shutdown 时写出剩余的。
    State& s = state();
    std::lock_guard lock(s.mu);
    if (s.file == INVALID_HANDLE_VALUE) return;
    const ULONGLONG now_ms = ::GetTickCount64();
    if (s.pending.empty()) s.pending_since_ms = now_ms;
    s.pending += line;
    if (lv != Level::Debug || s.pending.size() >= kPendingMaxBytes || now_ms - s.pending_since_ms >= kFlushIntervalMs) {
        flush_locked(s);
    }
}

void flush() {
    State& s = state();
    std::lock_guard lock(s.mu);
    flush_locked(s);
}

std::string_view level_name(Level lv) {
    switch (lv) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
    }
    return "info";
}

std::optional<Level> parse_level(std::string_view name) {
    const std::string_view t = trim(name);
    if (iequals(t, "debug")) return Level::Debug;
    if (iequals(t, "info")) return Level::Info;
    if (iequals(t, "warn")) return Level::Warn;
    return std::nullopt;
}

std::string format_line(int year, int month, int day, int hour, int minute, int second, int millis, Level lv,
                        std::string_view msg) {
    char head[48];
    const int n = std::snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d.%03d [", year, month, day, hour,
                                minute, second, millis);
    std::string out;
    out.reserve(static_cast<std::size_t>(n > 0 ? n : 0) + msg.size() + 8);
    if (n > 0) out.append(head, static_cast<std::size_t>(n));
    out += level_name(lv);
    out += "] ";
    out += msg;
    return out;
}

}  // namespace mc::log
