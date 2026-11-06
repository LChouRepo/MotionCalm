// test_framework.h — 极简单元测试框架（零第三方依赖）：TEST 自动注册 + CHECK* 非致命断言。
//
// 用法：
//   #include "test_framework.h"
//   TEST(intensity_rises_within_100ms) {
//       CHECK(model.value() > 0.9f);
//       CHECK_EQ(parse("Ctrl+F9").vk, VK_F9);
//       CHECK_NEAR(x, 0.35, 1e-6);
//       CHECK_TRUE(ok); CHECK_FALSE(bad);
//   }
// 规则：
//   * TEST(name) 的 name 必须是合法标识符；同一 .cpp 内唯一（跨文件可重名，但不建议）。
//   * CHECK* 失败不会中断当前测试（继续执行后面的检查），全部失败点都会打印。
//   * 测试函数抛出的异常算作失败。
//   * tests.exe [filter...]：只跑名字包含任一子串的测试；--list 列出；退出码 = 失败的测试数。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mctest {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    const char* file;
    int line;
    TestFn fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, const char* file, int line, TestFn fn) {
        registry().push_back(TestCase{name, file, line, fn});
    }
};

// 当前正在运行的测试的状态（单线程）。
struct Context {
    const char* test_name = nullptr;
    int checks = 0;
    int failures = 0;
    std::vector<std::string> messages;   // "file:line: what"
    int capture_depth = 0;               // >0 时失败只计数到 captured，不算测试失败（框架自测用）
    int captured = 0;
};

inline Context& ctx() {
    static Context c;
    return c;
}

// __FILE__ 往往是绝对路径；截成 tests\xxx.cpp / src\xxx\yyy.cpp 便于阅读。
inline std::string short_path(const char* file) {
    std::string f = file ? file : "?";
    for (const char* marker : {"\\tests\\", "/tests/", "\\src\\", "/src/"}) {
        const auto pos = f.rfind(marker);
        if (pos != std::string::npos) return f.substr(pos + 1);
    }
    const auto slash = f.find_last_of("\\/");
    return slash == std::string::npos ? f : f.substr(slash + 1);
}

// UTF-16 → UTF-8（不依赖 windows.h），用于打印 std::wstring。
inline std::string utf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(w[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            const std::uint32_t lo = static_cast<std::uint16_t>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

namespace detail {

template <class T>
using bare_t = std::remove_cvref_t<T>;

template <class T>
inline constexpr bool is_char_type_v =
    std::is_same_v<T, char> || std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> ||
    std::is_same_v<T, wchar_t> || std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> ||
    std::is_same_v<T, char32_t>;

// 可以用 std::cmp_equal 比较的整数（排除 bool 与字符类型）。
template <class T>
inline constexpr bool is_cmp_int_v =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !is_char_type_v<T>;

template <class T>
inline constexpr bool is_narrow_str_v =
    !std::is_same_v<bare_t<T>, std::nullptr_t> && std::is_convertible_v<const bare_t<T>&, std::string_view>;

template <class T>
inline constexpr bool is_wide_str_v =
    !std::is_same_v<bare_t<T>, std::nullptr_t> && std::is_convertible_v<const bare_t<T>&, std::wstring_view>;

template <class T>
concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

}  // namespace detail

template <class T>
std::string to_str(const T& v) {
    using U = detail::bare_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        return "nullptr";
    } else if constexpr (std::is_enum_v<U>) {
        return to_str(static_cast<std::underlying_type_t<U>>(v));
    } else if constexpr (detail::is_char_type_v<U>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (detail::is_wide_str_v<U>) {
        return "L\"" + utf8(std::wstring_view(v)) + "\"";
    } else if constexpr (detail::is_narrow_str_v<U>) {
        return "\"" + std::string(std::string_view(v)) + "\"";
    } else if constexpr (std::is_floating_point_v<U>) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
        return buf;
    } else if constexpr (detail::Streamable<U>) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<unprintable>";
    }
}

template <class A, class B>
bool values_equal(const A& a, const B& b) {
    using UA = detail::bare_t<A>;
    using UB = detail::bare_t<B>;
    if constexpr (detail::is_cmp_int_v<UA> && detail::is_cmp_int_v<UB>) {
        return std::cmp_equal(a, b);   // 无符号 / 有符号混比不报 C4389，也不出错
    } else if constexpr (detail::is_narrow_str_v<UA> && detail::is_narrow_str_v<UB>) {
        return std::string_view(a) == std::string_view(b);   // const char* 按内容比
    } else if constexpr (detail::is_wide_str_v<UA> && detail::is_wide_str_v<UB>) {
        return std::wstring_view(a) == std::wstring_view(b);
    } else {
        return a == b;
    }
}

inline void report_failure(const char* file, int line, const std::string& what) {
    Context& c = ctx();
    if (c.capture_depth > 0) {
        ++c.captured;
        return;
    }
    ++c.failures;
    c.messages.push_back(short_path(file) + ":" + std::to_string(line) + ": " + what);
}

inline bool check_impl(bool ok, const char* expr, const char* file, int line) {
    ++ctx().checks;
    if (!ok) report_failure(file, line, expr);
    return ok;
}

template <class A, class B>
bool check_eq_impl(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++ctx().checks;
    const bool ok = values_equal(a, b);
    if (!ok) {
        report_failure(file, line,
                       std::string("CHECK_EQ(") + ea + ", " + eb + ") -> " + to_str(a) + " != " + to_str(b));
    }
    return ok;
}

inline bool check_near_impl(double a, double b, double eps, const char* ea, const char* eb, const char* ee,
                            const char* file, int line) {
    ++ctx().checks;
    const double diff = std::fabs(a - b);
    const bool ok = !std::isnan(diff) && diff <= eps;   // NaN 永远失败
    if (!ok) {
        report_failure(file, line,
                       std::string("CHECK_NEAR(") + ea + ", " + eb + ", " + ee + ") -> " + to_str(a) + " vs " +
                           to_str(b) + " (|diff| " + to_str(diff) + " > " + to_str(eps) + ")");
    }
    return ok;
}

// 框架自测用：作用域内的失败只计数、不让当前测试失败。
class CaptureFailures {
public:
    CaptureFailures() : start_(ctx().captured) { ++ctx().capture_depth; }
    ~CaptureFailures() { --ctx().capture_depth; }
    CaptureFailures(const CaptureFailures&) = delete;
    CaptureFailures& operator=(const CaptureFailures&) = delete;
    int count() const { return ctx().captured - start_; }

private:
    int start_;
};

}  // namespace mctest

#define MC_TEST_FN_(name) mc_test_fn_##name
#define MC_TEST_REG_(name) mc_test_reg_##name

#define TEST(name)                                                                                   \
    static void MC_TEST_FN_(name)();                                                                 \
    static const ::mctest::Registrar MC_TEST_REG_(name){#name, __FILE__, __LINE__, &MC_TEST_FN_(name)}; \
    static void MC_TEST_FN_(name)()

#define CHECK(expr) ::mctest::check_impl(static_cast<bool>(expr), "CHECK(" #expr ")", __FILE__, __LINE__)
#define CHECK_TRUE(expr) ::mctest::check_impl(static_cast<bool>(expr), "CHECK_TRUE(" #expr ")", __FILE__, __LINE__)
#define CHECK_FALSE(expr) ::mctest::check_impl(!static_cast<bool>(expr), "CHECK_FALSE(" #expr ")", __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::mctest::check_eq_impl((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps)                                                                              \
    ::mctest::check_near_impl(static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps), #a, \
                              #b, #eps, __FILE__, __LINE__)
