// ProcessRunner: background shell jobs streamed into the Log.

#include "test_main.h"

#include "app/log.h"
#include "services/process_runner.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// Waits (up to `limit`) for the job to finish, then lets poll() report it.
bool wait_done(ProcessRunner& runner, std::chrono::milliseconds limit = std::chrono::milliseconds(10000)) {
    const auto deadline = Clock::now() + limit;
    while (runner.running() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool done = !runner.running();
    runner.poll();
    return done;
}

std::vector<std::string> lines_of(const Log& log, LogLevel level) {
    std::vector<std::string> out;
    log.visit([&](const LogLine& line) {
        if (line.level == level) out.push_back(line.text);
    });
    return out;
}

bool contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST(process_runner_captures_echo_lines) {
    Log log;
    ProcessRunner runner;
    CHECK_EQ(runner.last_exit_code(), -1);
    REQUIRE(runner.start("Echo", "echo one; echo two; echo 'three  spaced'", test::temp_dir("pr_echo"), log));
    REQUIRE(wait_done(runner));
    const auto build = lines_of(log, LogLevel::Build);
    REQUIRE_EQ(build.size(), size_t{3});
    CHECK_EQ(build[0], "one");
    CHECK_EQ(build[1], "two");
    CHECK_EQ(build[2], "three  spaced");
    CHECK_EQ(runner.last_exit_code(), 0);
    CHECK(contains(lines_of(log, LogLevel::Info), "Echo finished (exit 0, "));
    CHECK(runner.current_label().empty());
    CHECK(!runner.running());
}

TEST(process_runner_reports_exit_codes) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("Fail", "echo before; exit 3", test::temp_dir("pr_exit"), log));
    REQUIRE(wait_done(runner));
    CHECK_EQ(runner.last_exit_code(), 3);
    CHECK(contains(lines_of(log, LogLevel::Error), "Fail finished (exit 3, "));

    // The runner is reusable after a job ends.
    REQUIRE(runner.start("Ok", "true", test::temp_dir("pr_exit2"), log));
    REQUIRE(wait_done(runner));
    CHECK_EQ(runner.last_exit_code(), 0);
    CHECK(contains(lines_of(log, LogLevel::Info), "Ok finished (exit 0, "));
}

TEST(process_runner_captures_stderr_in_order) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("Err", "echo out; echo oops 1>&2; echo out2", test::temp_dir("pr_err"), log));
    REQUIRE(wait_done(runner));
    const auto build = lines_of(log, LogLevel::Build);
    REQUIRE_EQ(build.size(), size_t{3});
    CHECK_EQ(build[0], "out");
    CHECK_EQ(build[1], "oops");
    CHECK_EQ(build[2], "out2");
}

TEST(process_runner_runs_in_cwd) {
    Log log;
    ProcessRunner runner;
    const fs::path dir = test::temp_dir("pr_cwd");
    REQUIRE(runner.start("Pwd", "pwd -P", dir, log));
    REQUIRE(wait_done(runner));
    const auto build = lines_of(log, LogLevel::Build);
    REQUIRE_EQ(build.size(), size_t{1});
    CHECK_EQ(fs::path(build[0]), fs::canonical(dir));
}

TEST(process_runner_handles_carriage_returns_and_partial_lines) {
    Log log;
    ProcessRunner runner;
    // Progress-style "\r" overwrites, CRLF endings, ANSI colors, tabs, and a
    // final line without a newline.
    REQUIRE(runner.start("Progress", "printf '10%%\\r50%%\\r100%%\\ndos\\r\\n\\033[31mred\\033[0m\\ta\\ntail'",
                         test::temp_dir("pr_cr"), log));
    REQUIRE(wait_done(runner));
    const auto build = lines_of(log, LogLevel::Build);
    REQUIRE_EQ(build.size(), size_t{4});
    CHECK_EQ(build[0], "100%");
    CHECK_EQ(build[1], "dos");
    CHECK_EQ(build[2], "red a");
    CHECK_EQ(build[3], "tail");
}

TEST(process_runner_cancel_stops_job_quickly) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("Sleep", "sleep 30", test::temp_dir("pr_cancel"), log));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(runner.running());
    CHECK_EQ(runner.current_label(), "Sleep");
    CHECK_EQ(runner.last_exit_code(), -1);
    const auto t0 = Clock::now();
    runner.cancel();
    REQUIRE(wait_done(runner, std::chrono::milliseconds(5000)));
    const double took = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(took < 1.5);
    CHECK_NE(runner.last_exit_code(), 0);
    CHECK(runner.elapsed_seconds() < 5.0);
    CHECK(contains(lines_of(log, LogLevel::Warning), "Sleep cancelled (exit "));
}

TEST(process_runner_cancel_kills_whole_group_and_escalates) {
    Log log;
    ProcessRunner runner;
    // The shell ignores SIGTERM and has a child: SIGKILL after the grace period.
    REQUIRE(runner.start("Stubborn", "trap '' TERM; sleep 30 & wait", test::temp_dir("pr_kill"), log));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto t0 = Clock::now();
    runner.cancel();
    REQUIRE(wait_done(runner, std::chrono::milliseconds(8000)));
    const double took = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK(took < 4.0);
    CHECK_NE(runner.last_exit_code(), 0);
}

TEST(process_runner_rejects_second_job_while_busy) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("First", "sleep 30", test::temp_dir("pr_busy"), log));
    CHECK(!runner.start("Second", "echo nope", test::temp_dir("pr_busy2"), log));
    CHECK(contains(lines_of(log, LogLevel::Error), "Cannot start Second: First is still running"));
    CHECK_EQ(runner.current_label(), "First");
    runner.cancel();
    REQUIRE(wait_done(runner, std::chrono::milliseconds(5000)));
    CHECK(!contains(lines_of(log, LogLevel::Build), "nope"));
}

TEST(process_runner_rejects_missing_cwd) {
    Log log;
    ProcessRunner runner;
    CHECK(!runner.start("Nowhere", "true", test::temp_dir("pr_nocwd") / "missing", log));
    CHECK(!runner.running());
    CHECK(contains(lines_of(log, LogLevel::Error), "Cannot start Nowhere"));
}

TEST(process_runner_destructor_cancels_running_job) {
    Log log;
    const auto t0 = Clock::now();
    {
        ProcessRunner runner;
        REQUIRE(runner.start("Sleep", "sleep 30", test::temp_dir("pr_dtor"), log));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 3.0);
}

TEST(process_runner_unpolled_job_is_reported_on_next_start) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("A", "exit 0", test::temp_dir("pr_unpolled"), log));
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (runner.running() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(!runner.running());
    REQUIRE(runner.start("B", "exit 0", test::temp_dir("pr_unpolled2"), log));  // no poll() in between
    REQUIRE(wait_done(runner));
    const auto info = lines_of(log, LogLevel::Info);
    CHECK(contains(info, "A finished (exit 0, "));
    CHECK(contains(info, "B finished (exit 0, "));
}

TEST(process_runner_streams_many_lines) {
    Log log;
    ProcessRunner runner;
    REQUIRE(runner.start("Seq", "i=0; while [ $i -lt 2000 ]; do echo line$i; i=$((i+1)); done",
                         test::temp_dir("pr_many"), log));
    REQUIRE(wait_done(runner));
    const auto build = lines_of(log, LogLevel::Build);
    REQUIRE_EQ(build.size(), size_t{2000});
    CHECK_EQ(build.front(), "line0");
    CHECK_EQ(build.back(), "line1999");
}
