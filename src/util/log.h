// log.h — 线程安全的文件日志：UTF-8 行 "YYYY-MM-DD HH:MM:SS.mmm [level] msg"，1 MB 轮转为 <path>.1；std::format 风格接口。
#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "util/strings.h"

namespace mc::log {

enum class Level { Debug = 0, Info = 1, Warn = 2 };

// 文件达到该大小（字节）后，下一次写入前轮转。
inline constexpr std::uint64_t kRotateBytes = 1024ull * 1024ull;
// debug 行最多在内存里攒这么久（毫秒）再写进文件；info / warn 行总是立即写出（连同之前攒下的 debug 行）。
inline constexpr unsigned long long kFlushIntervalMs = 100;

// 打开（追加，不存在则新建）日志文件并设定级别；共享读 / 写 / 删除。失败返回 false，之后的写入静默丢弃。
bool init(const std::wstring& path, Level level);
// 关闭日志文件；之后的写入静默丢弃，直到再次 init。
void shutdown();
// 修改当前级别（线程安全）。
void set_level(Level level);
// 当前级别。
Level level();
// 当前级别下 lv 级别的行是否会写出（不判断文件是否打开）。
bool enabled(Level lv);
// 当前日志文件路径；未初始化时为空。
std::wstring path();
// 写一行：加时间戳与级别前缀、CRLF 结尾；msg 为 UTF-8。低于当前级别时丢弃。必要时先轮转。
// debug 行先缓冲（≤ kFlushIntervalMs 或 16 KB），info / warn 行立即写出。
void write(Level lv, std::string_view msg);
// 立即写出缓冲的 debug 行。
void flush();

// 级别的小写名："debug" / "info" / "warn"。
std::string_view level_name(Level lv);
// 解析级别名（ASCII 大小写不敏感）；未知返回 nullopt。
std::optional<Level> parse_level(std::string_view name);
// 组装一行（不含换行）："YYYY-MM-DD HH:MM:SS.mmm [level] msg"。纯函数，供测试。
std::string format_line(int year, int month, int day, int hour, int minute, int second, int millis, Level lv,
                        std::string_view msg);

// 写 debug 行，std::format 语法（UTF-8 窄串）。级别不够时不做格式化。
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
// 写 info 行，std::format 语法（UTF-8 窄串）。
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
// 写 warn 行，std::format 语法（UTF-8 窄串）。
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

// 宽字符版本：格式化成 UTF-16 后转 UTF-8 写出（方便直接传 std::wstring 的 exe 名、路径）。
template <class... Args>
void debug(std::wformat_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, wide_to_utf8(std::format(fmt, std::forward<Args>(args)...)));
}
template <class... Args>
void info(std::wformat_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, wide_to_utf8(std::format(fmt, std::forward<Args>(args)...)));
}
template <class... Args>
void warn(std::wformat_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, wide_to_utf8(std::format(fmt, std::forward<Args>(args)...)));
}

}  // namespace mc::log
