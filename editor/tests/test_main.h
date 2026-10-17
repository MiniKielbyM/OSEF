#pragma once
// Tiny headless test harness for the editor's model/, text/, services/ and util/.
//
//   #include "test_main.h"
//   TEST(text_buffer_inserts) {
//       CHECK(1 + 1 == 2);          // non-fatal: records a failure, keeps going
//       CHECK_EQ(answer(), 42);     // prints both values when they're printable
//       REQUIRE(ptr != nullptr);    // fatal: aborts the current test
//   }
//
// Each tests/*.cpp self-registers its TESTs through static Registrar objects.
// Run all:  osef_editor_tests          Filter:  osef_editor_tests png text_
// (positional args are substrings; a test runs if its name contains any).

#include <filesystem>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace osef::editor::test {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
    const char* file;
    int line;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, TestFn fn, const char* file, int line) {
        registry().push_back(TestCase{name, fn, file, line});
    }
};

// Thrown by REQUIRE* to abort the current test (never escapes the harness).
struct AbortTest {};

// Records a failure for the running test and prints "file:line: message".
void report_failure(const char* file, int line, const std::string& message);

// A fresh, empty directory under the system temp dir, removed after the run.
std::filesystem::path temp_dir(const std::string& tag);

// The OSEF checkout (compile definition OSEF_ROOT_DIR, overridable with the
// OSEF_ROOT environment variable). Copy examples before modifying them.
std::filesystem::path repo_root();

template <class T>
std::string printable(const T& value) {
    if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
        std::ostringstream os;
        os << value;
        return os.str();
    } else {
        return "<unprintable>";
    }
}

inline std::string printable(const std::string& s) { return "\"" + s + "\""; }
inline std::string printable(const char* s) { return s ? "\"" + std::string(s) + "\"" : "nullptr"; }
inline std::string printable(bool b) { return b ? "true" : "false"; }
inline std::string printable(std::nullptr_t) { return "nullptr"; }

// Integers of mixed signedness compare by value (no -Wsign-compare noise).
template <class T>
inline constexpr bool is_plain_integer_v =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
    !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> && !std::is_same_v<T, char16_t> &&
    !std::is_same_v<T, char32_t>;

template <class A, class B>
bool values_equal(const A& a, const B& b) {
    if constexpr (is_plain_integer_v<A> && is_plain_integer_v<B>)
        return std::cmp_equal(a, b);
    else
        return a == b;
}

template <class A, class B>
bool check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file,
              int line) {
    if (values_equal(a, b)) return true;
    report_failure(file, line,
                   std::string("CHECK_EQ(") + expr_a + ", " + expr_b + ")\n      left:  " +
                       printable(a) + "\n      right: " + printable(b));
    return false;
}

template <class A, class B>
bool check_ne(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file,
              int line) {
    if (!values_equal(a, b)) return true;
    report_failure(file, line,
                   std::string("CHECK_NE(") + expr_a + ", " + expr_b + ")\n      both:  " +
                       printable(a));
    return false;
}

} // namespace osef::editor::test

#define OSEF_TEST_CAT2(a, b) a##b
#define OSEF_TEST_CAT(a, b) OSEF_TEST_CAT2(a, b)

#define TEST(name)                                                                       \
    static void osef_test_##name();                                                      \
    static const ::osef::editor::test::Registrar OSEF_TEST_CAT(osef_test_reg_, name)(    \
        #name, &osef_test_##name, __FILE__, __LINE__);                                   \
    static void osef_test_##name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) ::osef::editor::test::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b) \
    do { ::osef::editor::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__); } while (0)

#define CHECK_NE(a, b) \
    do { ::osef::editor::test::check_ne((a), (b), #a, #b, __FILE__, __LINE__); } while (0)

#define REQUIRE(cond)                                                                       \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            ::osef::editor::test::report_failure(__FILE__, __LINE__, "REQUIRE(" #cond ")"); \
            throw ::osef::editor::test::AbortTest{};                                        \
        }                                                                                   \
    } while (0)

#define REQUIRE_EQ(a, b)                                                                    \
    do {                                                                                    \
        if (!::osef::editor::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__))          \
            throw ::osef::editor::test::AbortTest{};                                        \
    } while (0)
