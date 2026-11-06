// test_main.cpp — tests.exe 入口：运行 test_framework.h 注册的全部测试，打印 [PASS]/[FAIL] 与汇总，退出码 = 失败的测试数。
//
//   tests.exe                 运行全部测试
//   tests.exe intensity ini   只运行名字包含 "intensity" 或 "ini" 的测试（子串，区分大小写）
//   tests.exe --list          列出全部测试名
//   tests.exe --help
// 过滤后一个测试都没匹配到时返回 1（防止拼错过滤词还以为通过了）。
#include "test_framework.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

bool matches(const char* name, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    for (const auto& f : filters) {
        if (std::strstr(name, f.c_str()) != nullptr) return true;
    }
    return false;
}

void print_help() {
    std::printf(
        "usage: tests.exe [--list] [--help] [filter...]\n"
        "  filter   run only tests whose name contains one of the substrings\n"
        "  --list   list registered tests and exit\n"
        "exit code = number of failed tests (1 if a filter matched nothing)\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);   // 测试名 / 消息可能含中文
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::vector<std::string> filters;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list_only = true;
        } else if (a == "--help" || a == "-h" || a == "/?") {
            print_help();
            return 0;
        } else {
            filters.push_back(a);
        }
    }

    const auto& all = mctest::registry();
    if (list_only) {
        for (const auto& t : all) {
            if (matches(t.name, filters)) std::printf("%s  (%s:%d)\n", t.name, mctest::short_path(t.file).c_str(), t.line);
        }
        return 0;
    }

    int run = 0, passed = 0, failed = 0, total_checks = 0;
    std::string current_file;
    const auto t_start = std::chrono::steady_clock::now();

    for (const auto& t : all) {
        if (!matches(t.name, filters)) continue;

        const std::string file = mctest::short_path(t.file);
        if (file != current_file) {   // 按源文件分组打印
            current_file = file;
            std::printf("-- %s\n", file.c_str());
        }

        mctest::Context& c = mctest::ctx();
        c = mctest::Context{};
        c.test_name = t.name;
        try {
            t.fn();
        } catch (const std::exception& e) {
            mctest::report_failure(t.file, t.line, std::string("uncaught exception: ") + e.what());
        } catch (...) {
            mctest::report_failure(t.file, t.line, "uncaught non-std exception");
        }

        ++run;
        total_checks += c.checks;
        if (c.failures == 0) {
            ++passed;
            std::printf("[PASS] %s\n", t.name);
        } else {
            ++failed;
            std::printf("[FAIL] %s (%s)\n", t.name, c.messages.front().c_str());
            for (std::size_t i = 1; i < c.messages.size(); ++i) {
                std::printf("       also: %s\n", c.messages[i].c_str());
            }
        }
    }

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();

    if (run == 0) {
        std::printf("== no tests matched (%zu registered) ==\n", all.size());
        return filters.empty() ? 0 : 1;
    }
    std::printf("== %d tests: %d passed, %d failed, %d checks, %.1f ms ==\n", run, passed, failed, total_checks, ms);
    return failed;
}
