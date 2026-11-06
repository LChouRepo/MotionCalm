// test_framework_selftest.cpp — 测试框架自身的冒烟测试：各 CHECK 宏的通过路径，以及失败能被正确检出。
#include "test_framework.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {
enum class Color { Red = 1, Green = 2 };
}

TEST(framework_basic_checks_pass) {
    CHECK(1 + 1 == 2);
    CHECK_TRUE(true);
    CHECK_FALSE(false);
    CHECK_EQ(Color::Green, Color::Green);
}

TEST(framework_check_eq_mixed_types) {
    const std::vector<int> v{1, 2, 3};
    CHECK_EQ(v.size(), 3);                       // size_t vs int：内部用 std::cmp_equal，无符号/有符号不告警
    CHECK_EQ(-1, static_cast<std::int64_t>(-1));
    CHECK_EQ(std::string("abc"), "abc");
    const char* p = "xyz";
    CHECK_EQ(p, std::string("xyz"));             // const char* 按内容比较
    CHECK_EQ(std::wstring(L"宽字符"), L"宽字符");
    CHECK_NEAR(0.1 + 0.2, 0.3, 1e-12);
    CHECK_NEAR(1.0f / 3.0f, 0.333333, 1e-6);
}

TEST(framework_detects_failures) {
    int captured = 0;
    {
        mctest::CaptureFailures cap;   // 作用域内的失败只计数，不让本测试失败
        CHECK(1 == 2);
        CHECK_FALSE(true);
        CHECK_EQ(3, 4);
        CHECK_EQ(std::string("a"), "b");
        CHECK_NEAR(1.0, 1.1, 0.01);
        CHECK_NEAR(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0);   // NaN 永远失败
        CHECK(2 + 2 == 4);                                                 // 这条通过，不计入
        captured = cap.count();
    }
    CHECK_EQ(captured, 6);
}
