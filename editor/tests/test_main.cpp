// Runs every self-registered TEST (see test_main.h) and prints one line each:
//   [ ok ] name
//   [FAIL] name
// Exit code: 0 when all selected tests pass, 1 otherwise.

#include "test_main.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <random>
#include <system_error>

#ifndef OSEF_ROOT_DIR
#define OSEF_ROOT_DIR ""
#endif

namespace osef::editor::test {

namespace {

int g_failures_in_test = 0;
std::vector<std::filesystem::path> g_temp_dirs;

} // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
    ++g_failures_in_test;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

std::filesystem::path temp_dir(const std::string& tag) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = "/tmp";
    std::random_device rd;
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path dir = base / ("osef-editor-test-" + tag + "-" + std::to_string(rd() % 1000000000u));
        if (fs::create_directories(dir, ec) && !ec) {
            g_temp_dirs.push_back(dir);
            return dir;
        }
    }
    report_failure(__FILE__, __LINE__, "cannot create a temp dir for " + tag);
    throw AbortTest{};
}

std::filesystem::path repo_root() {
    if (const char* env = std::getenv("OSEF_ROOT"); env && *env) return env;
    return OSEF_ROOT_DIR;
}

} // namespace osef::editor::test

int main(int argc, char** argv) {
    using namespace osef::editor::test;

    std::vector<std::string> filters(argv + 1, argv + argc);
    auto selected = [&](const char* name) {
        if (filters.empty()) return true;
        for (const std::string& f : filters)
            if (std::string(name).find(f) != std::string::npos) return true;
        return false;
    };

    int passed = 0, failed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const TestCase& tc : registry()) {
        if (!selected(tc.name)) continue;
        g_failures_in_test = 0;
        try {
            tc.fn();
        } catch (const AbortTest&) {
            // already reported
        } catch (const std::exception& e) {
            report_failure(tc.file, tc.line, std::string("uncaught exception: ") + e.what());
        } catch (...) {
            report_failure(tc.file, tc.line, "uncaught non-std exception");
        }
        if (g_failures_in_test == 0) {
            ++passed;
            std::printf("[ ok ] %s\n", tc.name);
        } else {
            ++failed;
            std::printf("[FAIL] %s\n", tc.name);
        }
        std::fflush(stdout);
    }

    for (const auto& dir : g_temp_dirs) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("%d passed, %d failed (%.0f ms)\n", passed, failed, ms);
    if (passed + failed == 0) {
        std::printf("no tests matched\n");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}
