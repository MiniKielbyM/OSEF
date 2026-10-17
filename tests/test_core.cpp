// Unit tests for the freestanding OSEF core library (osef/core.h).
//
// Hosted test: std is used for reference results (snprintf, std::stable_sort)
// and the core headers are deliberately mixed with std headers in the same
// translation unit. Global operator new/delete are replaced to count live
// allocations, and a Counted type tracks constructions/destructions, so leaks
// and double destroys show up as failed checks.
//
// Build & run (from the repo root):
//   c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined \
//       -Iengine/include tests/test_core.cpp engine/src/core/*.cpp -o build/test_core && build/test_core

#include <new>
#include <string>
#include <vector>

#include <osef/core.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

static void check_str(const char* file, int line, const char* expr, osef::StringView got, osef::StringView want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n    got  \"%.*s\"\n    want \"%.*s\"\n", file, line, expr,
                    static_cast<int>(got.size()), got.data(), static_cast<int>(want.size()), want.data());
    }
}
#define CHECK_STR(got, want) check_str(__FILE__, __LINE__, #got " == " #want, (got), (want))

// assert_fail is normally provided by the platform layer. Here it throws, so
// tests can verify that bounds checks fire. Weak so a real platform
// definition wins if this test is linked against the whole engine.
struct AssertFailure {
    const char* expr;
};
namespace osef {
[[gnu::weak]] void assert_fail(const char*, int, const char* expr) { throw AssertFailure{expr}; }
} // namespace osef

#define CHECK_ASSERTS(stmt)                                                      \
    do {                                                                         \
        bool fired_ = false;                                                     \
        try {                                                                    \
            stmt;                                                                \
        } catch (const AssertFailure&) {                                         \
            fired_ = true;                                                       \
        }                                                                        \
        ++g_checks;                                                              \
        if (!fired_) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: expected assertion: %s\n", __FILE__, __LINE__, #stmt); \
        }                                                                        \
    } while (0)

// --- allocation counting ----------------------------------------------------
static long g_live_allocs = 0;
static long g_total_allocs = 0;
// When > 0, the g_fail_alloc_in-th next allocation throws std::bad_alloc.
static int g_fail_alloc_in = 0;

void* operator new(std::size_t n) {
    if (g_fail_alloc_in > 0 && --g_fail_alloc_in == 0) throw std::bad_alloc();
    void* p = std::malloc(n ? n : 1);
    if (!p) std::abort();
    ++g_live_allocs;
    ++g_total_allocs;
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
    if (!p) return;
    --g_live_allocs;
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

// Checks that a scope frees everything it allocated.
struct LeakScope {
    const char* name;
    long base;
    explicit LeakScope(const char* n) : name(n), base(g_live_allocs) {}
    ~LeakScope() {
        ++g_checks;
        if (g_live_allocs != base) {
            ++g_failures;
            std::printf("  FAIL leak in %s: %ld allocation(s) outstanding\n", name, g_live_allocs - base);
        }
    }
};

// --- instrumented element type -----------------------------------------------
struct Counted {
    static constexpr int kAlive = 0x600DF00D;
    static constexpr int kDead = 0x0DEADBAD;
    static int alive;
    static int constructions;
    static int destructions;
    static int bad;

    int value;
    int magic;

    Counted(int v = 0) : value(v), magic(kAlive) { ++alive, ++constructions; }
    Counted(const Counted& o) : value(o.value), magic(kAlive) {
        if (o.magic != kAlive) ++bad;
        ++alive, ++constructions;
    }
    Counted(Counted&& o) noexcept : value(o.value), magic(kAlive) {
        if (o.magic != kAlive) ++bad;
        o.value = -1;
        ++alive, ++constructions;
    }
    Counted& operator=(const Counted& o) {
        if (o.magic != kAlive || magic != kAlive) ++bad;
        value = o.value;
        return *this;
    }
    Counted& operator=(Counted&& o) noexcept {
        if (o.magic != kAlive || magic != kAlive) ++bad;
        value = o.value;
        if (&o != this) o.value = -1;
        return *this;
    }
    ~Counted() {
        if (magic != kAlive) ++bad; // double destroy or destroying garbage
        magic = kDead;
        --alive, ++destructions;
    }
    bool operator==(const Counted& o) const { return value == o.value; }
    bool operator<(const Counted& o) const { return value < o.value; }
};
int Counted::alive = 0;
int Counted::constructions = 0;
int Counted::destructions = 0;
int Counted::bad = 0;

static void reset_counted() {
    Counted::alive = Counted::constructions = Counted::destructions = Counted::bad = 0;
}
static bool counted_clean() { return Counted::alive == 0 && Counted::bad == 0; }

// Not trivially relocatable: must be moved with its constructor.
struct SelfRef {
    SelfRef* self;
    int v;
    SelfRef(int x = 0) : self(this), v(x) {}
    SelfRef(const SelfRef& o) : self(this), v(o.v) {}
    SelfRef& operator=(const SelfRef& o) {
        v = o.v;
        return *this;
    }
    ~SelfRef() { self = nullptr; }
    bool ok() const { return self == this; }
};

// Defeats -Wself-assign / -Wself-move for intentional self-assignment tests.
template <class T>
T& same(T& x) {
    return x;
}

using osef::Deque;
using osef::Function;
using osef::Map;
using osef::Pair;
using osef::SharedPtr;
using osef::String;
using osef::StringView;
using osef::UniquePtr;
using osef::Vector;


// ---------------------------------------------------------------------------
// base.h
// ---------------------------------------------------------------------------
struct TB {};
struct TD : TB {};
struct NonTrivial {
    NonTrivial(const NonTrivial&) {}
};
static_assert(osef::is_same_v<osef::remove_reference_t<int&&>, int>);
static_assert(osef::is_same_v<osef::remove_cv_t<const volatile int>, int>);
static_assert(osef::is_same_v<osef::decay_t<const int&>, int>);
static_assert(osef::is_same_v<osef::decay_t<int[3]>, int*>);
static_assert(osef::is_same_v<osef::decay_t<int(int)>, int (*)(int)>);
static_assert(osef::is_same_v<osef::decay_t<const char (&)[4]>, const char*>);
static_assert(osef::is_base_of_v<TB, TD> && !osef::is_base_of_v<TD, TB>);
static_assert(osef::is_convertible_v<TD*, TB*> && !osef::is_convertible_v<TB*, TD*>);
static_assert(osef::is_convertible_v<const char*, StringView>);
static_assert(!osef::is_convertible_v<StringView, String>); // explicit
static_assert(osef::is_convertible_v<void, void> && !osef::is_convertible_v<int, void>);
static_assert(osef::is_integral_v<const unsigned long long> && osef::is_integral_v<bool> &&
              !osef::is_integral_v<float> && !osef::is_integral_v<int*>);
static_assert(osef::is_trivially_copyable_v<int> && !osef::is_trivially_copyable_v<NonTrivial>);
static_assert(osef::is_trivially_destructible_v<int> && !osef::is_trivially_destructible_v<String>);
static_assert(osef::is_pointer_v<int* const> && !osef::is_pointer_v<int>);
static_assert(osef::is_function_v<void()> && !osef::is_function_v<void (*)()>);
static_assert((osef::min)(3, 4) == 3 && (osef::max)(3, 4) == 4);
static_assert(OSEF_HOSTED == 1);

static void test_base() {
    String a("first"), b("second");
    osef::swap(a, b);
    CHECK_STR(a, "second");
    CHECK_STR(b, "first");
    int x = 5;
    CHECK(osef::exchange(x, 9) == 5 && x == 9);
    String moved = osef::move(a);
    CHECK_STR(moved, "second");
    CHECK(a.empty());
    CHECK(osef::forward<int&>(x) == 9);
}

// ---------------------------------------------------------------------------
// StringView
// ---------------------------------------------------------------------------
static void test_string_view() {
    constexpr StringView cv("hello");
    static_assert(cv.size() == 5);
    StringView v("hello world");
    CHECK(v.size() == 11 && !v.empty());
    CHECK(v[0] == 'h' && v.front() == 'h' && v.back() == 'd');
    CHECK(StringView().empty() && StringView().data() != nullptr);
    CHECK(StringView(static_cast<const char*>(nullptr)).empty());
    CHECK(StringView("abc", 2) == "ab");
    CHECK_STR(v.substr(6), "world");
    CHECK_STR(v.substr(6, 3), "wor");
    CHECK_STR(v.substr(6, 100), "world");
    CHECK(v.substr(11).empty());
    CHECK(v.substr(50).empty()); // clamped
    CHECK(v.find('o') == 4);
    CHECK(v.find('o', 5) == 7);
    CHECK(v.find('z') == StringView::npos);
    CHECK(v.find('d', 11) == StringView::npos);
    CHECK(v.find("world") == 6);
    CHECK(v.find("") == 0);
    CHECK(v.find("", 11) == 11);
    CHECK(v.find("", 12) == StringView::npos);
    CHECK(v.find("worlds") == StringView::npos);
    CHECK(v.find("o w") == 4);
    CHECK(v.find("l", 4) == 9);
    CHECK(v.rfind('o') == 7);
    CHECK(v.rfind('o', 6) == 4);
    CHECK(v.rfind('h', 0) == 0);
    CHECK(v.rfind('z') == StringView::npos);
    CHECK(StringView().rfind('a') == StringView::npos);
    CHECK(v.rfind("l") == 9);
    CHECK(v.rfind("l", 8) == 3);
    CHECK(v.rfind("hello") == 0);
    CHECK(v.rfind("") == 11);
    CHECK(v.rfind("", 3) == 3);
    CHECK(v.rfind("hello world!") == StringView::npos);
    CHECK(v.starts_with("hello") && v.starts_with("") && !v.starts_with("world") && v.starts_with('h'));
    CHECK(v.ends_with("world") && v.ends_with("") && !v.ends_with("hello") && v.ends_with('d'));
    CHECK(!StringView().starts_with('a') && !StringView().ends_with('a'));
    CHECK(!StringView("ab").starts_with("abc"));
    CHECK(v.contains("lo w") && v.contains('w') && !v.contains("xyz"));
    CHECK(v.find_first_of("ow") == 4);
    CHECK(v.find_first_not_of("hel") == 4);
    CHECK(v.find_last_of("lo") == 9);
    CHECK(v.find_last_not_of("dlr") == 7);
    CHECK(v.find_first_of("xyz") == StringView::npos);
    CHECK(StringView("aaa").find_first_not_of("a") == StringView::npos);
    CHECK_STR(StringView("  \t hi there \r\n").trimmed(), "hi there");
    CHECK(StringView("   ").trimmed().empty());
    StringView r = v;
    r.remove_prefix(6);
    CHECK_STR(r, "world");
    r.remove_suffix(2);
    CHECK_STR(r, "wor");
    r.remove_prefix(99);
    CHECK(r.empty());

    CHECK(StringView("abc").compare("abd") < 0);
    CHECK(StringView("abd").compare("abc") > 0);
    CHECK(StringView("ab").compare("abc") < 0);
    CHECK(StringView("abc").compare("ab") > 0);
    CHECK(StringView("abc").compare("abc") == 0);
    CHECK(StringView("").compare("") == 0);
    CHECK(StringView("\xff").compare("a") > 0); // unsigned byte order
    CHECK(StringView("a") < StringView("b") && StringView("b") > StringView("a"));
    CHECK(StringView("a") <= StringView("a") && StringView("a") >= StringView("a"));
    CHECK(StringView("a") != StringView("b"));
    const char embedded[] = {'a', '\0', 'b'};
    CHECK(StringView(embedded, 3) != StringView("a"));
    CHECK(StringView(embedded, 3).size() == 3);

    int n = 0;
    for (char c : StringView("xyz")) n += c == 'y';
    CHECK(n == 1);
    CHECK_ASSERTS((void)v[11]);
    CHECK_ASSERTS((void)StringView().front());
    CHECK_ASSERTS((void)StringView().back());
}

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------
static void test_string_basics() {
    LeakScope leak("string basics");
    String e;
    CHECK(e.empty() && e.size() == 0 && e.c_str()[0] == '\0' && e.capacity() >= 15);
    String a("abc");
    CHECK(a.size() == 3 && std::strcmp(a.c_str(), "abc") == 0 && a.data() == a.c_str());
    CHECK(String(static_cast<const char*>(nullptr)).empty());
    CHECK_STR(String("abcdef", 3), "abc");
    CHECK_STR(String(StringView("view")), "view");
    CHECK_STR(String(4, 'z'), "zzzz");
    CHECK(String(0, 'z').empty());

    // Small-string boundary: 15 inline, 16 on the heap.
    long before = g_total_allocs;
    String s15("123456789012345");
    CHECK(g_total_allocs == before);
    String s16("1234567890123456");
    CHECK(g_total_allocs == before + 1);
    CHECK(s15.size() == 15 && s16.size() == 16 && s16.c_str()[16] == '\0');

    // Copies are deep and independent.
    for (const char* text : {"short", "a considerably longer string that lives on the heap"}) {
        String orig(text);
        String copy(orig);
        CHECK(copy == orig && copy.data() != orig.data());
        copy[0] = '#';
        CHECK(orig[0] == text[0]);
        String assigned;
        assigned = orig;
        CHECK(assigned == orig);
        same(assigned) = assigned; // self copy-assignment
        CHECK_STR(assigned, text);
        String moved(osef::move(copy));
        CHECK(copy.empty() && copy.c_str()[0] == '\0'); // moved-from is empty and usable
        CHECK(moved[0] == '#');
        copy = "reuse after move";
        CHECK_STR(copy, "reuse after move");
        String target("old contents that are long enough for the heap");
        target = osef::move(moved);
        CHECK(target[0] == '#' && moved.empty());
        target = osef::move(same(target)); // self move-assignment
        CHECK(target[0] == '#' && target.size() == std::strlen(text));
        osef::swap(target, assigned);
        CHECK_STR(target, text);
        CHECK(assigned[0] == '#');
        target.swap(same(target));
        CHECK_STR(target, text);
    }

    // Swap every inline/heap combination.
    String sa("tiny"), sb("this one is definitely on the heap");
    sa.swap(sb);
    CHECK_STR(sa, "this one is definitely on the heap");
    CHECK_STR(sb, "tiny");
    String sc("x"), sd("y");
    sc.swap(sd);
    CHECK(sc == "y" && sd == "x");

    String asg;
    asg = "literal";
    CHECK_STR(asg, "literal");
    asg = StringView("view!");
    CHECK_STR(asg, "view!");
    asg.assign("abcdef", 2);
    CHECK_STR(asg, "ab");
    String al("0123456789abcdefghijXYZ");
    al.assign(al.data() + 10, 10); // aliasing assign
    CHECK_STR(al, "abcdefghij");
    al.assign(al.c_str() + 2); // aliasing assign through const char*
    CHECK_STR(al, "cdefghij");
}

static void test_string_modify() {
    LeakScope leak("string modify");
    String s;
    for (int i = 0; i < 1000; ++i) s.push_back(static_cast<char>('a' + i % 26));
    CHECK(s.size() == 1000 && s[25] == 'z' && s[26] == 'a' && s.c_str()[1000] == '\0');
    CHECK(s.capacity() >= 1000);
    s.pop_back();
    CHECK(s.size() == 999 && s.back() == 'k');
    s.clear();
    CHECK(s.empty() && s.c_str()[0] == '\0' && s.capacity() >= 999);
    s.shrink_to_fit();
    CHECK(s.capacity() == 15);

    String a("hello");
    a += ' ';
    a += "world";
    a += StringView("!!", 1);
    a += String("?");
    a.append("abc", 2).append(StringView("_")).append("cstr").append(3, '.');
    CHECK_STR(a, "hello world!?ab_cstr...");
    a.append(nullptr, 0);
    a.append(static_cast<const char*>(nullptr));
    CHECK_STR(a, "hello world!?ab_cstr...");

    // Self-append through every overload, inline and heap.
    String sa("abc");
    sa += sa;
    CHECK_STR(sa, "abcabc");
    sa.append(sa.data(), sa.size());
    CHECK_STR(sa, "abcabcabcabc");
    sa += sa.c_str() + 9;
    CHECK_STR(sa, "abcabcabcabcabc");
    sa += StringView(sa).substr(0, 3);
    CHECK_STR(sa, "abcabcabcabcabcabc");
    sa.reserve(100);
    sa += StringView(sa).substr(15);
    CHECK_STR(sa, "abcabcabcabcabcabcabc");

    String r("12345");
    r.resize(8, 'x');
    CHECK_STR(r, "12345xxx");
    r.resize(2);
    CHECK_STR(r, "12");
    r.resize(4);
    CHECK(r.size() == 4 && r[2] == '\0' && r[3] == '\0');
    r.reserve(200);
    CHECK(r.capacity() >= 200 && r.size() == 4);
    r.reserve(10);
    CHECK(r.capacity() >= 200);

    String e("0123456789");
    e.erase(2, 3);
    CHECK_STR(e, "0156789");
    e.erase(5);
    CHECK_STR(e, "01567");
    e.erase(10, 2); // pos past end: no-op
    CHECK_STR(e, "01567");
    e.erase(1, String::npos);
    CHECK_STR(e, "0");
    e.erase();
    CHECK(e.empty());

    String in("world");
    in.insert(0, "hello ");
    CHECK_STR(in, "hello world");
    in.insert(5, ",");
    CHECK_STR(in, "hello, world");
    in.insert(100, "!"); // clamped to end
    CHECK_STR(in, "hello, world!");
    in.insert(0, StringView(in).substr(7, 5)); // aliasing insert
    CHECK_STR(in, "worldhello, world!");
    in.insert(5, 3, '-');
    CHECK_STR(in, "world---hello, world!");
    in.insert(0, 0, 'x');
    CHECK_STR(in, "world---hello, world!");

    String rp("the quick brown fox");
    rp.replace(4, 5, "slow");
    CHECK_STR(rp, "the slow brown fox");
    rp.replace(0, 3, "a much longer replacement than before, forcing a reallocation of");
    CHECK_STR(rp, "a much longer replacement than before, forcing a reallocation of slow brown fox");
    rp.replace(2, 1000, "");
    CHECK_STR(rp, "a ");
    rp.replace(1, 0, StringView(rp)); // aliasing replace
    CHECK_STR(rp, "aa  ");

    CHECK(a.front() == 'h');
    a.front() = 'H';
    a.back() = '!';
    CHECK(a.starts_with("Hello") && a.ends_with("..!") && a.starts_with('H') && a.ends_with('!'));
    int count = 0;
    for (char& c : a)
        if (c == 'l') ++count;
    CHECK(count == 3);

    String empty;
    CHECK_ASSERTS(empty.pop_back());
    CHECK_ASSERTS((void)empty.front());
    CHECK_ASSERTS((void)empty.back());
    CHECK(empty[0] == '\0'); // index size() is the terminator
    CHECK_ASSERTS((void)empty[1]);
}

static void test_string_search_compare() {
    LeakScope leak("string search");
    String s("one two three two one");
    CHECK(s.find('t') == 4);
    CHECK(s.find("two") == 4);
    CHECK(s.find("two", 5) == 14);
    CHECK(s.find("four") == String::npos);
    CHECK(s.rfind("two") == 14);
    CHECK(s.rfind('o') == 18);
    CHECK(s.rfind("one", 17) == 0);
    CHECK(s.find_first_of("wh") == 5);
    CHECK(s.find_last_of("t") == 14);
    CHECK(s.find_first_not_of("one ") == 4);
    CHECK(s.find_last_not_of("one ") == 15);
    CHECK(s.contains("three") && !s.contains("four") && s.contains('h'));
    CHECK_STR(s.substr(4, 3), "two");
    CHECK_STR(s.substr(18), "one");
    CHECK_STR(s.substr(18, 999), "one");
    CHECK(s.substr(21).empty());
    CHECK(s.substr(1000).empty());
    CHECK_STR(s.substr(), s);
    CHECK(String::npos == static_cast<size_t>(-1) && StringView::npos == String::npos);

    String a("apple"), b("banana");
    CHECK(a < b && !(b < a) && a != b && a == String("apple"));
    CHECK(a == "apple" && "apple" == a && a != "apples" && "apples" != a);
    CHECK(a == StringView("apple") && StringView("apple") == a);
    CHECK(a < "b" && "a" < a && a <= "apple" && a >= "apple" && b > a);
    CHECK(a.compare("apple") == 0 && a.compare("apricot") < 0 && a.compare("ap") > 0);
    StringView v = a;
    CHECK(v.data() == a.data() && v.size() == 5);
    CHECK(a.view() == v);

    // String with embedded NUL compares by length, not by C string.
    String z("a\0b", 3);
    CHECK(z.size() == 3 && z != "a" && z == StringView("a\0b", 3));
}

static void test_string_concat() {
    LeakScope leak("string concat");
    String s("ab");
    StringView v("cd");
    const char* c = "ef";
    CHECK_STR(s + s, "abab");
    CHECK_STR(s + v, "abcd");
    CHECK_STR(v + s, "cdab");
    CHECK_STR(s + c, "abef");
    CHECK_STR(c + s, "efab");
    CHECK_STR(s + 'x', "abx");
    CHECK_STR('x' + s, "xab");
    CHECK_STR(v + v, "cdcd");
    CHECK_STR(v + c, "cdef");
    CHECK_STR(c + v, "efcd");
    CHECK_STR(v + 'y', "cdy");
    CHECK_STR('y' + v, "ycd");
    CHECK_STR(String("12") + s, "12ab");
    CHECK_STR(String("12") + v, "12cd");
    CHECK_STR(String("12") + c, "12ef");
    CHECK_STR(String("12") + 'z', "12z");
    CHECK_STR(s + "lit" + 'c' + v + s + String("!"), "ablitccdab!");
    CHECK_STR("x" + s + "y", "xaby");
    String self("ab");
    String r = osef::move(self) + self; // rvalue left operand aliasing the right
    CHECK_STR(r, "abab");
    String longer = String(40, 'q') + String(40, 'w');
    CHECK(longer.size() == 80 && longer[39] == 'q' && longer[40] == 'w');
}

static void test_string_numbers() {
    LeakScope leak("string numbers");
    CHECK_STR(osef::to_string(0), "0");
    CHECK_STR(osef::to_string(-1), "-1");
    CHECK_STR(osef::to_string(INT_MAX), "2147483647");
    CHECK_STR(osef::to_string(INT_MIN), "-2147483648");
    CHECK_STR(osef::to_string(UINT_MAX), "4294967295");
    CHECK_STR(osef::to_string(LONG_MIN), std::to_string(LONG_MIN).c_str());
    CHECK_STR(osef::to_string(ULONG_MAX), std::to_string(ULONG_MAX).c_str());
    CHECK_STR(osef::to_string(LLONG_MIN), "-9223372036854775808");
    CHECK_STR(osef::to_string(LLONG_MAX), "9223372036854775807");
    CHECK_STR(osef::to_string(ULLONG_MAX), "18446744073709551615");
    CHECK_STR(osef::to_string(static_cast<size_t>(42)), "42");
    CHECK_STR(osef::to_string(static_cast<int64_t>(-42)), "-42");
    CHECK_STR(osef::to_string(static_cast<uint8_t>(200)), "200");
    CHECK_STR(osef::to_string(static_cast<short>(-7)), "-7");
    CHECK_STR(osef::to_string(true), "1");

    long long out = 12345;
    struct Case {
        const char* text;
        bool ok;
        long long value;
    };
    const Case cases[] = {
        {"0", true, 0},
        {"-0", true, 0},
        {"+0", true, 0},
        {"42", true, 42},
        {"+42", true, 42},
        {"-42", true, -42},
        {"007", true, 7},
        {"9223372036854775807", true, LLONG_MAX},
        {"-9223372036854775808", true, LLONG_MIN},
        {"9223372036854775808", false, 0},
        {"-9223372036854775809", false, 0},
        {"99999999999999999999999", false, 0},
        {"0x0", true, 0},
        {"0x10", true, 16},
        {"0XfF", true, 255},
        {"-0x10", true, -16},
        {"+0x7fffffffffffffff", true, LLONG_MAX},
        {"-0x8000000000000000", true, LLONG_MIN},
        {"0x8000000000000000", false, 0},
        {"0x1ffffffffffffffff", false, 0},
        {"0x", false, 0},
        {"-0x", false, 0},
        {"0xg", false, 0},
        {"", false, 0},
        {"-", false, 0},
        {"+", false, 0},
        {"--1", false, 0},
        {"+-1", false, 0},
        {" 1", false, 0},
        {"1 ", false, 0},
        {"12a", false, 0},
        {"ff", false, 0},
        {"1.5", false, 0},
        {"x10", false, 0},
    };
    for (const Case& c : cases) {
        out = 12345;
        const bool ok = osef::parse_int(c.text, out);
        ++g_checks;
        if (ok != c.ok || (ok && out != c.value) || (!ok && out != 12345)) {
            ++g_failures;
            std::printf("  FAIL parse_int(\"%s\") -> ok=%d out=%lld\n", c.text, ok, out);
        }
    }
    CHECK(osef::parse_int(StringView("123456", 3), out) && out == 123); // not NUL-terminated
    CHECK(osef::to_int("17") == 17);
    CHECK(osef::to_int("nope", -5) == -5);
    CHECK(osef::to_int("") == 0);
    CHECK(osef::to_int("0x20", 1) == 32);
    CHECK(osef::to_int("-9223372036854775809", 3) == 3);
}

// ---------------------------------------------------------------------------
// Vector
// ---------------------------------------------------------------------------
static void test_vector_basics() {
    LeakScope leak("vector basics");
    reset_counted();
    {
        Vector<int> e;
        CHECK(e.empty() && e.size() == 0 && e.capacity() == 0 && e.begin() == e.end());
        Vector<int> v{1, 2, 3};
        CHECK(v.size() == 3 && v[0] == 1 && v.front() == 1 && v.back() == 3 && v.data()[1] == 2);
        Vector<int> n(4);
        CHECK(n.size() == 4 && n[0] == 0 && n[3] == 0);
        Vector<int> f(3, 7);
        CHECK(f.size() == 3 && f[2] == 7);
        Vector<int> five(5, 3); // (count, value), not the iterator range ctor
        CHECK(five.size() == 5 && five[4] == 3);
        int raw[] = {4, 5, 6};
        Vector<int> range(raw, raw + 3);
        CHECK(range == Vector<int>({4, 5, 6}));
        std::vector<int> sv{9, 8};
        Vector<int> from_std(sv.begin(), sv.end());
        CHECK(from_std.size() == 2 && from_std[0] == 9);
        CHECK(v != range);
        int sum = 0;
        for (int x : v) sum += x;
        CHECK(sum == 6);
        const Vector<int>& cv = v;
        CHECK(*cv.begin() == 1 && cv.end() - cv.begin() == 3);

        // Growth is amortized: doubling keeps reallocations logarithmic.
        Vector<int> g;
        long before = g_total_allocs;
        for (int i = 0; i < 100000; ++i) g.push_back(i);
        CHECK(g_total_allocs - before < 30);
        CHECK(g.size() == 100000 && g[99999] == 99999);
        g.clear();
        CHECK(g.empty() && g.capacity() >= 100000);
        g.shrink_to_fit();
        CHECK(g.capacity() == 0);
        g.reserve(10);
        CHECK(g.capacity() == 10);
        g.reserve(5);
        CHECK(g.capacity() == 10);

        Vector<int> r{1, 2, 3};
        r.resize(5);
        CHECK(r.size() == 5 && r[4] == 0);
        r.resize(7, 9);
        CHECK(r.size() == 7 && r[6] == 9 && r[4] == 0);
        r.resize(1);
        CHECK(r == Vector<int>{1});
        r = {5, 6};
        CHECK(r == Vector<int>({5, 6}));

        CHECK_ASSERTS((void)e[0]);
        CHECK_ASSERTS(e.pop_back());
        CHECK_ASSERTS((void)e.front());
        CHECK_ASSERTS((void)e.back());
        CHECK_ASSERTS((void)v[3]);
        CHECK_ASSERTS(v.erase(v.end()));
    }
    CHECK(counted_clean());
}

static void test_vector_nontrivial() {
    LeakScope leak("vector non-trivial");
    reset_counted();
    {
        Vector<Counted> v;
        for (int i = 0; i < 10; ++i) v.emplace_back(i);
        CHECK(v.size() == 10 && Counted::alive == 10);
        Counted& ref = v.emplace_back(10);
        CHECK(ref.value == 10 && &ref == &v.back());

        // Insert in the middle with spare capacity and when full.
        v.reserve(20);
        v.insert(v.begin() + 3, Counted(100));
        CHECK(v.size() == 12 && v[3].value == 100 && v[2].value == 2 && v[4].value == 3 && v[11].value == 10);
        v.shrink_to_fit();
        CHECK(v.capacity() == v.size());
        Counted* it = v.insert(v.begin() + 1, Counted(200));
        CHECK(it == v.begin() + 1 && it->value == 200 && v[0].value == 0 && v[2].value == 1);
        it = v.insert(v.end(), Counted(300));
        CHECK(it == v.end() - 1 && v.back().value == 300);
        it = v.emplace(v.begin(), 400);
        CHECK(v.front().value == 400 && it == v.begin());
        CHECK(Counted::alive == static_cast<int>(v.size()));

        // Erase in the middle.
        it = v.erase(v.begin() + 2);
        CHECK(it->value == 1 && v[1].value == 0);
        it = v.erase(v.begin() + 1, v.begin() + 4);
        CHECK(it == v.begin() + 1 && it->value == 100);
        it = v.erase(v.begin() + 1, v.begin() + 1);
        CHECK(it == v.begin() + 1);
        it = v.erase(v.end() - 1);
        CHECK(it == v.end());
        CHECK(Counted::alive == static_cast<int>(v.size()));
        int expect[] = {400, 100, 3, 4, 5, 6, 7, 8, 9, 10};
        CHECK(v.size() == 10);
        for (size_t i = 0; i < v.size() && i < 10; ++i) CHECK(v[i].value == expect[i]);

        // Arguments that alias elements, with and without reallocation.
        v.shrink_to_fit();
        v.push_back(v[0]);
        CHECK(v.back().value == 400 && v[0].value == 400);
        v.shrink_to_fit();
        v.emplace_back(v[1]);
        CHECK(v.back().value == 100);
        v.shrink_to_fit();
        v.insert(v.begin(), v.back());
        CHECK(v[0].value == 100);
        v.reserve(v.size() + 5);
        v.insert(v.begin() + 1, v[5]);
        CHECK(v[1].value == v[6].value);
        v.shrink_to_fit();
        v.resize(v.size() + 3, v[0]);
        CHECK(v.back().value == 100 && v[v.size() - 3].value == 100);
        v.push_back(osef::move(v[2]));
        CHECK(v.back().value == 400 && v[2].value == -1);
        CHECK(Counted::alive == static_cast<int>(v.size()));

        // Range insert, including from the vector itself.
        Vector<Counted> w{Counted(1), Counted(2), Counted(3)};
        w.insert(w.begin() + 1, w.begin(), w.end());
        CHECK(w.size() == 6 && w[0].value == 1 && w[1].value == 1 && w[3].value == 3 && w[4].value == 2);
        w.reserve(20);
        w.insert(w.end(), w.begin(), w.begin() + 2);
        CHECK(w.size() == 8 && w[6].value == 1 && w[7].value == 1);
        w.insert(w.begin(), {Counted(-5), Counted(-6)});
        CHECK(w.size() == 10 && w[0].value == -5 && w[1].value == -6 && w[2].value == 1);
        w.insert(w.begin(), w.begin(), w.begin()); // empty range
        CHECK(w.size() == 10);
        Counted* at = w.insert(w.begin() + 2, 3, w[0]); // count copies of an element
        CHECK(at == w.begin() + 2 && w.size() == 13 && w[2].value == -5 && w[4].value == -5 && w[5].value == 1);
        w.erase(w.begin() + 2, w.begin() + 5);
        Vector<int> ci{1, 2};
        ci.insert(ci.begin() + 1, 3, 9);
        CHECK(ci == Vector<int>({1, 9, 9, 9, 2}));
        ci.insert(ci.end(), 0, 7);
        CHECK(ci.size() == 5);

        // Copy / move / self-assignment.
        Vector<Counted> c(v);
        CHECK(c == v && c.data() != v.data());
        Vector<Counted> m(osef::move(c));
        CHECK(c.empty() && m == v);
        c = m;
        CHECK(c == m);
        c = osef::move(same(c));
        CHECK(c == m);
        c = same(c);
        CHECK(c == m);
        c = osef::move(w);
        CHECK(c.size() == 10 && w.empty());
        w.push_back(Counted(1)); // moved-from vector is usable
        CHECK(w.size() == 1);
        osef::swap(c, w);
        CHECK(c.size() == 1 && w.size() == 10);

        v.resize(3);
        CHECK(v.size() == 3);
        v.pop_back();
        CHECK(v.size() == 2);
        CHECK(Counted::alive == static_cast<int>(v.size() + m.size() + c.size() + w.size()));
        v.clear();
    }
    CHECK(counted_clean());
    CHECK(Counted::constructions == Counted::destructions);

    // Elements that must not be memcpy'd.
    {
        Vector<SelfRef> s;
        for (int i = 0; i < 100; ++i) s.emplace_back(i);
        s.insert(s.begin() + 50, SelfRef(-1));
        s.erase(s.begin() + 10);
        Vector<SelfRef> copy = s;
        bool all_ok = true;
        for (const SelfRef& x : s) all_ok = all_ok && x.ok();
        for (const SelfRef& x : copy) all_ok = all_ok && x.ok();
        CHECK(all_ok && s[49].v == -1 && copy[49].v == -1);
    }
}

static void test_vector_nested() {
    LeakScope leak("vector nested");
    reset_counted();
    {
        Vector<Vector<String>> grid;
        for (int i = 0; i < 20; ++i) {
            Vector<String> row;
            for (int j = 0; j <= i; ++j) row.push_back(osef::to_string(i * 100 + j) + " with a long suffix");
            grid.push_back(osef::move(row));
        }
        grid.insert(grid.begin() + 5, grid[7]);
        grid.erase(grid.begin());
        CHECK(grid.size() == 20);
        CHECK_STR(grid[4][0], "700 with a long suffix");
        CHECK(grid[4].size() == 8);
        Vector<Vector<String>> copy = grid;
        copy[0][0] = "changed";
        CHECK_STR(grid[0][0], "100 with a long suffix");

        // Assigning from an element's own member (owned by *this).
        struct Node {
            Vector<Node> children;
            int id = 0;
        };
        Vector<Node> tree(1);
        tree[0].children.resize(3);
        tree[0].children[1].id = 7;
        tree = osef::move(tree[0].children);
        CHECK(tree.size() == 3 && tree[1].id == 7);
        Vector<Node> tree2(1);
        tree2[0].children.resize(2);
        tree2[0].children[0].id = 5;
        tree2 = tree2[0].children;
        CHECK(tree2.size() == 2 && tree2[0].id == 5);

        Vector<UniquePtr<Counted>> owners;
        for (int i = 0; i < 50; ++i) owners.push_back(osef::make_unique<Counted>(i));
        owners.erase(owners.begin() + 10, owners.begin() + 20);
        owners.insert(owners.begin(), osef::make_unique<Counted>(-1));
        CHECK(Counted::alive == 41 && owners[0]->value == -1 && owners[11]->value == 20);
    }
    CHECK(counted_clean());
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------
static void test_map() {
    LeakScope leak("map");
    reset_counted();
    {
        Map<String, int> m;
        CHECK(m.empty() && m.find("x") == m.end() && !m.contains("x") && m.count("x") == 0);
        m["delta"] = 4;
        m["alpha"] = 1;
        m["charlie"] = 3;
        m["bravo"] = 2;
        CHECK(m.size() == 4);
        const char* order[] = {"alpha", "bravo", "charlie", "delta"};
        int i = 0;
        bool ordered = true;
        for (auto& [k, v] : m) {
            ordered = ordered && k == order[i] && v == i + 1;
            ++i;
        }
        CHECK(ordered && i == 4);

        // Heterogeneous lookup: StringView, const char*, String.
        CHECK(m.find(StringView("charlie"))->second == 3);
        CHECK(m.find("bravo")->second == 2);
        CHECK(m.find(String("delta"))->second == 4);
        const char* key = "alpha";
        CHECK(m.find(key) == m.begin());
        CHECK(m.contains(StringView("alphabet", 5)) && !m.contains(StringView("alphabet")));
        CHECK(m.count("delta") == 1 && m.count("echo") == 0);
        CHECK(m.at("charlie") == 3);
        CHECK_ASSERTS((void)m.at("zulu"));

        // operator[] default-inserts; emplace/insert never overwrite.
        CHECK(m["echo"] == 0 && m.size() == 5);
        CHECK(!m.emplace("alpha", 100) && m["alpha"] == 1);
        CHECK(m.emplace(StringView("foxtrot"), 6) && m["foxtrot"] == 6);
        CHECK(m.insert(String("golf"), 7) && !m.insert("golf", 70) && m["golf"] == 7);
        CHECK(m.insert(Pair<String, int>{"hotel", 8}) && m.at("hotel") == 8);
        CHECK(!m.insert_or_assign("alpha", 11) && m["alpha"] == 11);
        CHECK(m.insert_or_assign("india", 9) && m["india"] == 9);
        CHECK(m.size() == 9);

        CHECK(m.lower_bound("b")->first == "bravo");
        CHECK(m.lower_bound("bravo")->first == "bravo");
        CHECK(m.upper_bound("bravo")->first == "charlie");
        CHECK(m.lower_bound("zzz") == m.end());
        CHECK(m.lower_bound("") == m.begin());

        CHECK(m.erase("nope") == 0);
        CHECK(m.erase("echo") == 1 && !m.contains("echo") && m.size() == 8);
        CHECK(m.erase(StringView("foxtrot")) == 1);

        // Erase while iterating, using the returned iterator.
        for (auto it = m.begin(); it != m.end();) {
            if (it->second % 2 == 1) it = m.erase(it);
            else ++it;
        }
        CHECK(m.size() == 3);
        CHECK(m.contains("bravo") && m.contains("delta") && m.contains("hotel"));
        const Map<String, int>& cm = m;
        CHECK(cm.find("delta")->second == 4 && cm.at("bravo") == 2 && cm.begin()->first == "bravo");
        Map<String, int>::const_iterator cit = cm.find("hotel");
        CHECK(cit != cm.end());
        Map<String, int> copy = m;
        copy.erase(copy.find("bravo"));
        CHECK(copy.size() == 2 && m.size() == 3);
        m.clear();
        CHECK(m.empty());

        // Integer keys, random insertion order vs std::map.
        Map<int, int> mi;
        std::map<int, int> ref;
        osef::Random rng(7);
        for (int n = 0; n < 2000; ++n) {
            int k = static_cast<int>(rng.below(500)) - 250;
            int op = static_cast<int>(rng.below(3));
            if (op == 0) {
                mi.erase(k);
                ref.erase(k);
            } else if (op == 1) {
                mi.insert_or_assign(k, n);
                ref[k] = n;
            } else {
                mi.emplace(k, n);
                ref.emplace(k, n);
            }
        }
        bool same_contents = mi.size() == ref.size();
        auto rit = ref.begin();
        for (const auto& [k, v] : mi) {
            same_contents = same_contents && rit != ref.end() && rit->first == k && rit->second == v;
            ++rit;
        }
        CHECK(same_contents);

        Map<String, int> init{{"b", 2}, {"a", 1}, {"b", 3}};
        CHECK(init.size() == 2 && init["a"] == 1 && init["b"] == 2);
    }

    // Move-only values and nested containers.
    {
        Map<String, UniquePtr<Counted>> owners;
        owners["one"] = osef::make_unique<Counted>(1);
        CHECK(owners.emplace("two", osef::make_unique<Counted>(2)));
        CHECK(!owners.emplace("two", osef::make_unique<Counted>(22))); // rejected value is destroyed
        CHECK(owners.insert_or_assign(String("three"), osef::make_unique<Counted>(3)));
        CHECK(!owners.insert_or_assign("one", osef::make_unique<Counted>(11)));
        CHECK(Counted::alive == 3 && owners["one"]->value == 11);
        for (int k = 0; k < 30; ++k) owners[osef::to_string(k)] = osef::make_unique<Counted>(k);
        CHECK(Counted::alive == 33);
        owners.erase("two");
        CHECK(Counted::alive == 32);
        Map<String, UniquePtr<Counted>> moved = osef::move(owners);
        CHECK(owners.empty() && moved.size() == 32 && Counted::alive == 32);
        owners = osef::move(moved);
        owners = osef::move(same(owners));
        CHECK(owners.size() == 32);

        Map<String, Vector<String>> groups;
        groups["fruit"].push_back("apple");
        groups["fruit"].push_back("pear");
        groups["veg"] = {"leek"};
        Map<String, Vector<String>> gcopy = groups;
        gcopy["fruit"][0] = "plum";
        CHECK_STR(groups["fruit"][0], "apple");
        CHECK(gcopy["fruit"].size() == 2 && gcopy.size() == 2);
        Map<int, Map<String, Counted>> deep;
        deep[1]["a"] = Counted(5);
        deep[2]["b"] = Counted(6);
        deep[1]["c"] = Counted(7);
        Map<int, Map<String, Counted>> deep_copy = deep;
        deep.erase(1);
        CHECK(deep_copy[1]["c"].value == 7 && deep.size() == 1);
    }
    CHECK(counted_clean());
}

// ---------------------------------------------------------------------------
// Deque
// ---------------------------------------------------------------------------
static void test_deque() {
    LeakScope leak("deque");
    reset_counted();
    {
        Deque<int> d;
        CHECK(d.empty() && d.size() == 0 && d.begin() == d.end());
        CHECK_ASSERTS(d.pop_front());
        CHECK_ASSERTS(d.pop_back());
        CHECK_ASSERTS((void)d.front());
        CHECK_ASSERTS((void)d.back());
        CHECK_ASSERTS((void)d[0]);

        // Mixed pushes/pops against std::deque, forcing wrap-around and growth.
        std::vector<int> ref; // front at index 0
        osef::Random rng(99);
        bool match = true;
        for (int i = 0; i < 20000; ++i) {
            const uint64_t op = rng.below(5);
            if (op == 0) {
                d.push_back(i);
                ref.push_back(i);
            } else if (op == 1) {
                d.push_front(i);
                ref.insert(ref.begin(), i);
            } else if (op == 2 && !ref.empty()) {
                match = match && d.front() == ref.front();
                d.pop_front();
                ref.erase(ref.begin());
            } else if (op == 3 && !ref.empty()) {
                match = match && d.back() == ref.back();
                d.pop_back();
                ref.pop_back();
            } else if (!ref.empty()) {
                const size_t k = static_cast<size_t>(rng.below(ref.size()));
                match = match && d[k] == ref[k];
            }
            match = match && d.size() == ref.size();
        }
        size_t idx = 0;
        for (int x : d) match = match && x == ref[idx++];
        CHECK(match && idx == ref.size());

        Deque<String> ds;
        for (int i = 0; i < 6; ++i) ds.push_back("item " + osef::to_string(i) + " padded out to the heap");
        for (int i = 0; i < 4; ++i) ds.pop_front();
        for (int i = 0; i < 6; ++i) ds.push_back("again " + osef::to_string(i)); // wraps inside capacity 8
        for (int i = 0; i < 3; ++i) ds.push_front("front " + osef::to_string(i)); // grows while wrapped
        CHECK(ds.size() == 11);
        CHECK_STR(ds.front(), "front 2");
        CHECK_STR(ds[3], "item 4 padded out to the heap");
        CHECK_STR(ds.back(), "again 5");
        ds.push_back(ds.front()); // aliasing push
        ds.push_front(ds.back());
        CHECK_STR(ds.front(), "front 2");
        CHECK_STR(ds.back(), "front 2");
        ds.erase_at(1);
        CHECK_STR(ds[1], "front 1");
        ds.erase_at(ds.size() - 2);
        CHECK_STR(ds[ds.size() - 2], "again 4");
        auto it = ds.erase(ds.begin());
        CHECK(it == ds.begin());
        CHECK_STR(*it, "front 1");
        CHECK(it->size() == 7);
        Deque<String> dcopy = ds;
        Deque<String> dmove = osef::move(dcopy);
        CHECK(dcopy.empty() && dmove.size() == ds.size());
        bool equal = true;
        for (size_t i = 0; i < ds.size(); ++i) equal = equal && ds[i] == dmove[i];
        CHECK(equal);
        dcopy = dmove;
        dcopy = same(dcopy);
        dcopy = osef::move(same(dcopy));
        CHECK(dcopy.size() == ds.size());
        const Deque<String>& cds = ds;
        size_t n = 0;
        for (const String& s : cds) n += s.size() > 0;
        CHECK(n == ds.size());
        Deque<String>::const_iterator ci = ds.begin(); // iterator -> const_iterator
        CHECK(ci == cds.begin());

        Deque<Counted> dc{Counted(1), Counted(2)};
        for (int i = 0; i < 100; ++i) {
            dc.push_front(Counted(i));
            dc.emplace_back(i);
            if (i % 3 == 0) dc.pop_front();
        }
        CHECK(Counted::alive == static_cast<int>(dc.size()));
        dc.erase_at(5);
        dc.erase_at(dc.size() - 5);
        CHECK(Counted::alive == static_cast<int>(dc.size()));
        dc.clear();
        CHECK(Counted::alive == 0 && dc.empty());
        dc.push_back(Counted(3));
        CHECK(dc.front().value == 3);
    }
    CHECK(counted_clean());
}

// ---------------------------------------------------------------------------
// Function
// ---------------------------------------------------------------------------
static int add_one(int x) { return x + 1; }
static int g_side = 0;
static void bump() { ++g_side; }

static void test_function() {
    LeakScope leak("function");
    reset_counted();
    {
        Function<int(int)> empty;
        CHECK(!empty && empty == nullptr && nullptr == empty);
        CHECK_ASSERTS(empty(1));
        Function<int(int)> null_fn = nullptr;
        CHECK(!null_fn);
        int (*null_ptr)(int) = nullptr;
        Function<int(int)> from_null_ptr = null_ptr;
        CHECK(!from_null_ptr);

        Function<int(int)> fp = add_one;
        CHECK(fp && fp != nullptr && fp(41) == 42);
        Function<int(int)> fp2 = &add_one;
        CHECK(fp2(1) == 2);
        Function<void()> vf = bump;
        vf();
        CHECK(g_side == 1);

        // Captures, copied state is independent.
        int counter = 0;
        Function<int()> next = [counter]() mutable { return ++counter; };
        CHECK(next() == 1 && next() == 2);
        Function<int()> copy = next;
        CHECK(copy() == 3 && copy() == 4 && next() == 3);
        Function<int()> moved = osef::move(next);
        CHECK(!next && moved() == 4);
        next = moved;
        CHECK(next() == 5 && moved() == 5);
        next = same(next);
        CHECK(next() == 6);
        next = osef::move(same(next));
        CHECK(next() == 7);
        next = nullptr;
        CHECK(!next);
        next = [] { return 99; };
        CHECK(next() == 99);

        // Reference capture and reference parameters.
        int total = 0;
        Function<void(int)> acc = [&total](int x) { total += x; };
        acc(5);
        acc(6);
        CHECK(total == 11);
        Function<void(String&)> app = [](String& s) { s += "!"; };
        String s("hi");
        app(s);
        CHECK_STR(s, "hi!");
        Function<String(const String&, StringView)> join = [](const String& a, StringView b) { return a + b; };
        CHECK_STR(join(String("ab"), "cd"), "abcd");
        // By-value move-only argument.
        Function<int(UniquePtr<Counted>)> take = [](UniquePtr<Counted> p) { return p->value; };
        CHECK(take(osef::make_unique<Counted>(8)) == 8);

        // void result discards a value; convertible results convert.
        Function<void(int)> discard = add_one;
        discard(1);
        Function<long(int)> widen = add_one;
        CHECK(widen(2) == 3L);

        // Captured state is destroyed exactly once, including copies.
        Counted c(10);
        {
            Function<int()> f1 = [c] { return c.value; };
            Function<int()> f2 = f1;
            Function<int()> f3 = osef::move(f1);
            CHECK(f2() == 10 && f3() == 10);
            CHECK(Counted::alive == 3);
            f2 = f3;
            CHECK(Counted::alive == 3);
        }
        CHECK(Counted::alive == 1);
        String big(100, 'x');
        Function<size_t()> sz = [big] { return big.size(); };
        Vector<Function<size_t()>> fns;
        for (int i = 0; i < 20; ++i) fns.push_back(sz);
        fns.erase(fns.begin() + 3);
        CHECK(fns.size() == 19 && fns[7]() == 100);
    }
    // Overload resolution only accepts matching callables.
    static_assert(osef::is_invocable_r_v<int, int (*)(int), int>);
    static_assert(!osef::is_invocable_r_v<int, int (*)(int), const char*>);
    static_assert(osef::is_invocable_r_v<void, int (*)(int), int>);
    static_assert(!osef::is_convertible_v<int, Function<int(int)>>);
    CHECK(counted_clean());
}

// ---------------------------------------------------------------------------
// UniquePtr / SharedPtr
// ---------------------------------------------------------------------------
struct Animal {
    static int destroyed;
    virtual ~Animal() { ++destroyed; }
    virtual int legs() const { return 0; }
};
int Animal::destroyed = 0;
struct Dog : Animal {
    Counted tag{4};
    int legs() const override { return 4; }
};
// Base without a virtual destructor: SharedPtr(new Derived) must still run
// ~Derived (it remembers the original type).
struct PlainBase {
    int x = 1;
};
struct PlainDerived : PlainBase {
    Counted c{7};
};

static void test_unique_ptr() {
    LeakScope leak("unique_ptr");
    reset_counted();
    Animal::destroyed = 0;
    {
        UniquePtr<Counted> e;
        CHECK(!e && e == nullptr && nullptr == e && e.get() == nullptr);
        CHECK_ASSERTS((void)*e);
        UniquePtr<Counted> p = osef::make_unique<Counted>(5);
        CHECK(p && p != nullptr && p->value == 5 && (*p).value == 5 && Counted::alive == 1);
        UniquePtr<Counted> q = osef::move(p);
        CHECK(!p && q->value == 5 && Counted::alive == 1);
        q = osef::move(same(q)); // self-move keeps the object
        CHECK(q && q->value == 5 && Counted::alive == 1);
        q.reset(new Counted(6));
        CHECK(q->value == 6 && Counted::alive == 1);
        q.reset(q.get()); // same pointer: no double delete
        CHECK(Counted::alive == 1);
        Counted* raw = q.release();
        CHECK(!q && Counted::alive == 1);
        delete raw;
        CHECK(Counted::alive == 0);
        q = osef::make_unique<Counted>(1);
        q = nullptr;
        CHECK(Counted::alive == 0);
        UniquePtr<Counted> a = osef::make_unique<Counted>(1), b = osef::make_unique<Counted>(2);
        a.swap(b);
        CHECK(a->value == 2 && b->value == 1 && a != b);

        UniquePtr<Animal> an = osef::make_unique<Dog>(); // converting move
        CHECK(an->legs() == 4 && Counted::alive == 3);
        UniquePtr<Dog> dog = osef::make_unique<Dog>();
        an = osef::move(dog); // converting move-assign destroys the old Dog
        CHECK(!dog && Animal::destroyed == 1 && Counted::alive == 3);
        static_assert(!osef::is_convertible_v<UniquePtr<Animal>&&, UniquePtr<Dog>>);
        static_assert(!osef::is_convertible_v<const UniquePtr<Dog>&, UniquePtr<Dog>>);

        struct Agg {
            int a;
            String b;
        };
        UniquePtr<Agg> agg = osef::make_unique<Agg>(1, String("two"));
        CHECK(agg->a == 1 && agg->b == "two");
    }
    CHECK(Animal::destroyed == 2);
    CHECK(counted_clean());
}

static void test_shared_ptr() {
    LeakScope leak("shared_ptr");
    reset_counted();
    Animal::destroyed = 0;
    {
        SharedPtr<Counted> e;
        CHECK(!e && e.use_count() == 0 && e == nullptr && nullptr == e);
        CHECK_ASSERTS((void)*e);
        SharedPtr<Counted> null_owned(static_cast<Counted*>(nullptr));
        CHECK(!null_owned && null_owned.use_count() == 0);

        long before = g_total_allocs;
        SharedPtr<Counted> a = osef::make_shared<Counted>(3);
        CHECK(g_total_allocs - before <= 1); // object and count share one allocation (may be elided at -O2)
        CHECK(a.use_count() == 1 && a.unique() && a->value == 3 && (*a).value == 3);
        {
            SharedPtr<Counted> b = a;
            CHECK(a.use_count() == 2 && b.use_count() == 2 && a == b && b.get() == a.get());
            SharedPtr<Counted> c = osef::move(b);
            CHECK(!b && c.use_count() == 2);
            c = same(c);
            CHECK(c.use_count() == 2);
            c = osef::move(same(c));
            CHECK(c && c.use_count() == 2);
            b = c;
            CHECK(a.use_count() == 3);
            b = nullptr;
            CHECK(a.use_count() == 2);
        }
        CHECK(a.use_count() == 1 && Counted::alive == 1);

        SharedPtr<Counted> owned(new Counted(4));
        CHECK(owned.use_count() == 1 && Counted::alive == 2);
        owned = a; // releases the old object
        CHECK(Counted::alive == 1 && a.use_count() == 2);
        owned.reset();
        CHECK(!owned && a.use_count() == 1);
        owned.reset(new Counted(9));
        CHECK(owned->value == 9 && Counted::alive == 2);
        owned.swap(a);
        CHECK(owned->value == 3 && a->value == 9);
        a.reset();
        owned.reset();
        CHECK(Counted::alive == 0);

        // Converting construction / assignment between Derived and Base.
        SharedPtr<Dog> dog = osef::make_shared<Dog>();
        SharedPtr<Animal> animal = dog;
        CHECK(animal.use_count() == 2 && animal->legs() == 4 && animal == dog);
        SharedPtr<Animal> animal2 = osef::move(dog);
        CHECK(!dog && animal2.use_count() == 2);
        SharedPtr<Animal> animal3;
        animal3 = animal2;
        CHECK(animal.use_count() == 3);
        animal3 = SharedPtr<Dog>(new Dog);
        CHECK(animal3.use_count() == 1 && animal.use_count() == 2 && Counted::alive == 2);
        SharedPtr<Dog> back = osef::static_pointer_cast<Dog>(animal);
        CHECK(back.use_count() == 3 && back->tag.value == 4);
        animal.reset();
        animal2.reset();
        CHECK(back.use_count() == 1 && Animal::destroyed == 0);
        back.reset();
        CHECK(Animal::destroyed == 1);
        animal3 = nullptr;
        CHECK(Animal::destroyed == 2 && Counted::alive == 0);
        static_assert(!osef::is_convertible_v<SharedPtr<Animal>, SharedPtr<Dog>>);

        // From UniquePtr, and the remembered deleter type.
        UniquePtr<Dog> u = osef::make_unique<Dog>();
        SharedPtr<Animal> from_unique = osef::move(u);
        CHECK(!u && from_unique.use_count() == 1);
        from_unique.reset();
        CHECK(Animal::destroyed == 3);
        {
            SharedPtr<PlainBase> pb(new PlainDerived);
            CHECK(pb->x == 1 && Counted::alive == 1);
        }
        CHECK(Counted::alive == 0); // ~PlainDerived ran through SharedPtr<PlainBase>

        // Shared ownership inside containers.
        Vector<SharedPtr<Counted>> many;
        SharedPtr<Counted> one = osef::make_shared<Counted>(1);
        for (int i = 0; i < 100; ++i) many.push_back(one);
        CHECK(one.use_count() == 101);
        many.erase(many.begin(), many.begin() + 50);
        CHECK(one.use_count() == 51);
        Map<String, SharedPtr<Counted>> by_name;
        by_name["x"] = one;
        by_name["y"] = one;
        CHECK(one.use_count() == 53);
        many.clear();
        by_name.clear();
        CHECK(one.use_count() == 1);
    }
    CHECK(counted_clean());
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
static void check_format(int line, const char* want, int want_len, const char* got, size_t got_len) {
    ++g_checks;
    if (want_len < 0 || static_cast<size_t>(want_len) != got_len ||
        std::memcmp(want, got, std::strlen(want) + 1) != 0) {
        ++g_failures;
        std::printf("  FAIL format line %d: got \"%s\" (%zu), want \"%s\" (%d)\n", line, got, got_len, want,
                    want_len);
    }
}

// Whether the host C library is a trustworthy reference for this format.
// Microsoft's UCRT differs from C99/glibc/BSD in implementation-defined and
// alternate-form output (%p prints 00007FF6..., some '#' cases differ), so
// on Windows those conversions are only checked against glibc/BSD in CI.
static bool host_reference(const char* fmt) {
#if defined(_WIN32)
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') continue;
        if (p[1] == '%') {
            ++p;
            continue;
        }
        for (const char* q = p + 1; *q; ++q) {
            if (*q == '#') return false;
            if (*q == 'p') return false;
            if ((*q >= 'a' && *q <= 'z' && *q != 'h' && *q != 'l' && *q != 'j' && *q != 'z' && *q != 't') ||
                (*q >= 'A' && *q <= 'Z' && *q != 'L'))
                break;
        }
    }
#else
    (void)fmt;
#endif
    return true;
}
static const char* format_of(const char* fmt, ...) { return fmt; }

// Formats with the host snprintf and osef::format_to, osef::format, compares.
#define FMT(...)                                                                 \
    if (host_reference(format_of(__VA_ARGS__))) do {                             \
        char want_[512];                                                         \
        char got_[512];                                                          \
        const int wl_ = std::snprintf(want_, sizeof want_, __VA_ARGS__);         \
        const size_t gl_ = osef::format_to(got_, sizeof got_, __VA_ARGS__);      \
        check_format(__LINE__, want_, wl_, got_, gl_);                           \
        String gs_ = osef::format(__VA_ARGS__);                                  \
        ++g_checks;                                                              \
        if (gs_.size() != gl_ || std::strcmp(gs_.c_str(), want_) != 0) {         \
            ++g_failures;                                                        \
            std::printf("  FAIL format() line %d: \"%s\"\n", __LINE__, gs_.c_str()); \
        }                                                                        \
    } while (0)

// Truncated output into an n-byte buffer: same bytes, same return value,
// nothing written past n.
#define FMT_N(n, ...)                                                            \
    if (host_reference(format_of(__VA_ARGS__))) do {                             \
        char want_[64];                                                          \
        char got_[64];                                                           \
        std::memset(want_, '#', sizeof want_);                                   \
        std::memset(got_, '#', sizeof got_);                                     \
        const int wl_ = std::snprintf((n) ? want_ : nullptr, (n), __VA_ARGS__);  \
        const size_t gl_ = osef::format_to((n) ? got_ : nullptr, (n), __VA_ARGS__); \
        ++g_checks;                                                              \
        if (wl_ < 0 || static_cast<size_t>(wl_) != gl_ || std::memcmp(want_, got_, sizeof want_) != 0) { \
            ++g_failures;                                                        \
            std::printf("  FAIL format_to n=%d line %d: ret %zu vs %d, \"%.*s\" vs \"%.*s\"\n", \
                        static_cast<int>(n), __LINE__, gl_, wl_, 20, got_, 20, want_); \
        }                                                                        \
    } while (0)

#define EXPECT_FMT(want, ...)                                                    \
    do {                                                                         \
        char got_[256];                                                          \
        const size_t gl_ = osef::format_to(got_, sizeof got_, __VA_ARGS__);      \
        check_format(__LINE__, want, static_cast<int>(std::strlen(want)), got_, gl_); \
    } while (0)

static void test_format_integers() {
    const int ints[] = {0, 1, -1, 7, 42, -42, 255, 1000, -99999, INT_MAX, INT_MIN};
    for (int v : ints) {
        FMT("%d", v);
        FMT("%i", v);
        FMT("[%5d]", v);
        FMT("[%-5d]", v);
        FMT("[%05d]", v);
        FMT("[%+d]", v);
        FMT("[% d]", v);
        FMT("[%.3d]", v);
        FMT("[%.0d]", v);
        FMT("[%8.3d]", v);
        FMT("[%-8.3d]", v);
        FMT("[%+08d]", v);
        FMT("[% 08d]", v);
        FMT("[%+.0d]", v);
        FMT("[%08.3d]", v); // precision disables the 0 flag
        FMT("[%-+12d]", v);
        FMT("[%- 12d]", v);
        FMT("[%*d]", 9, v);
        FMT("[%*d]", -9, v); // negative width: left-justify
        FMT("[%-*d]", 9, v);
        FMT("[%.*d]", 6, v);
        FMT("[%.*d]", -3, v); // negative precision: as if omitted
        FMT("[%*.*d]", 12, 8, v);
        FMT("[%0*d]", 10, v);
        FMT("[%.20d]", v);
        FMT("[%40d]", v);
        FMT("[%040d]", v);
    }
    const unsigned uns[] = {0u, 1u, 8u, 15u, 16u, 255u, 0x1234abcdu, UINT_MAX, 0x80000000u};
    for (unsigned v : uns) {
        FMT("%u", v);
        FMT("%x", v);
        FMT("%X", v);
        FMT("%o", v);
        FMT("[%#x]", v);
        FMT("[%#X]", v);
        FMT("[%#o]", v);
        FMT("[%#.0o]", v);
        FMT("[%#.0x]", v);
        FMT("[%.0x]", v);
        FMT("[%.0u]", v);
        FMT("[%.0o]", v);
        FMT("[%08x]", v);
        FMT("[%#08x]", v);
        FMT("[%#-8x]", v);
        FMT("[%#10.4x]", v);
        FMT("[%#10.4X]", v);
        FMT("[%#5o]", v);
        FMT("[%#05o]", v);
        FMT("[%.5o]", v);
        FMT("[%#.5o]", v);
        FMT("[%#.1o]", v);
        FMT("[%#12.3o]", v);
        FMT("[%-12u]", v);
        FMT("[%012u]", v);
        FMT("[%.12x]", v);
        FMT("[%#.12x]", v);
        FMT("[%*x]", 11, v);
    }
    // Length modifiers.
    FMT("%hhd %hhd %hhd", static_cast<signed char>(-1), static_cast<signed char>(127), static_cast<signed char>(-128));
    FMT("%hhu %hhx %hho", static_cast<unsigned char>(255), static_cast<unsigned char>(171), static_cast<unsigned char>(8));
    FMT("%hd %hd %hu %hx", static_cast<short>(-12345), static_cast<short>(SHRT_MIN), static_cast<unsigned short>(65535),
        static_cast<unsigned short>(0xbeef));
    FMT("%ld %ld %ld", 0L, LONG_MIN, LONG_MAX);
    FMT("%lu %lx %lo %#lX", ULONG_MAX, ULONG_MAX, ULONG_MAX, ULONG_MAX);
    FMT("%lld %lld %lld", 0LL, LLONG_MIN, LLONG_MAX);
    FMT("%lld", static_cast<long long>(INT64_MIN));
    FMT("[%30lld] [%-30lld] [%030lld] [%+lld]", LLONG_MIN, LLONG_MIN, LLONG_MIN, LLONG_MAX);
    FMT("%llu %llx %llX %llo %#llo %#llx", ULLONG_MAX, ULLONG_MAX, ULLONG_MAX, ULLONG_MAX, ULLONG_MAX, ULLONG_MAX);
    FMT("%zu %zx %zd %zd", static_cast<size_t>(SIZE_MAX), static_cast<size_t>(0xabc), static_cast<ptrdiff_t>(-5),
        static_cast<ptrdiff_t>(PTRDIFF_MAX));
    FMT("%jd %jd %ju %jx", static_cast<intmax_t>(INTMAX_MIN), static_cast<intmax_t>(INTMAX_MAX),
        static_cast<uintmax_t>(UINTMAX_MAX), static_cast<uintmax_t>(0xfeed));
    FMT("%td %td %td", static_cast<ptrdiff_t>(-7), static_cast<ptrdiff_t>(PTRDIFF_MIN), static_cast<ptrdiff_t>(0));
    FMT("%d %ld %lld %u %hhd %zu %s", 1, 2L, 3LL, 4u, static_cast<signed char>(5), static_cast<size_t>(6), "seven");
}

static void test_format_other() {
    // Characters and strings.
    FMT("%c%c%c", 'a', 'B', '!');
    FMT("[%5c] [%-5c] [%c]", 'x', 'y', ' ');
    FMT("[%s] [%10s] [%-10s] [%.2s] [%10.2s] [%-10.2s] [%.0s] [%.10s]", "hello", "hello", "hello", "hello", "hello",
        "hello", "hello", "hello");
    FMT("[%s] [%5s] [%.*s] [%*s] [%-*s]", "", "", 3, "abcdef", 6, "ab", 6, "ab");
    FMT("[%.*s]", -1, "negative precision is ignored");
    const char unterminated[3] = {'a', 'b', 'c'}; // precision must bound the read (ASan checks)
    FMT("[%.3s] [%.2s]", unterminated, unterminated);
    FMT("%s and %s", "a very long string argument that goes on for a while", "another");
    // %%, %p and mixtures.
    FMT("100%% sure %%%d%%", 5);
    int local = 0;
    void* p1 = &local;
    void* p2 = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    FMT("%p %p", p1, p2);
    FMT("[%20p] [%-20p]", p1, p2);
    FMT("a%db%sc%xd%%e%cf%ug", -3, "str", 0xfe, 'z', 9u);

    // Behaviour that differs between C libraries or is OSEF-specific.
    EXPECT_FMT("0x0", "%p", static_cast<void*>(nullptr));
    EXPECT_FMT("[     0x0]", "[%8p]", static_cast<void*>(nullptr));
    const char* null_str = nullptr;
    EXPECT_FMT("(null)", "%s", null_str);
    EXPECT_FMT("? 7", "%f %d", 1.5, 7);
    EXPECT_FMT("[    ?] ?|?|?|8", "[%5.2f] %e|%g|%Lf|%d", 2.0, 3.0, 4.0, 5.0L, 8);
    EXPECT_FMT("?? ok", "%a%G %s", 1.0, 2.0, "ok");
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wformat"
#  pragma clang diagnostic ignored "-Wformat-invalid-specifier"
#  pragma clang diagnostic ignored "-Wformat-extra-args"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat"
#  pragma GCC diagnostic ignored "-Wformat-extra-args"
#endif
    EXPECT_FMT("abc%", "abc%");
    EXPECT_FMT("abc%-5", "abc%-5");
    EXPECT_FMT("%y %5.2y x", "%y %5.2y %s", "x");
    int sink = 0;
    EXPECT_FMT("ab", "a%nb", &sink); // %n consumes its argument, writes nothing
    CHECK(sink == 0);
    EXPECT_FMT("12", "%n%d", &sink, 12);
    // Flag combinations compilers warn about but C defines.
    FMT("[%-05d] [%+ d] [% +d] [%-#08x]", 42, 42, 42, 0x2a);
    FMT("[%hhd] [%hhu] [%hd]", 300, 300, 70000);
    EXPECT_FMT("[    %] [%  ]", "[%5%] [%-3%]"); // width applies to %% as in BSD/macOS libc
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

    // Truncation, return value and termination.
    for (size_t n = 0; n <= 14; ++n) {
        FMT_N(n, "hello %s %d", "world", 42);
        FMT_N(n, "%08x|%-4s|", 0xbeefu, "ab");
    }
    FMT_N(1, "%s", "");
    FMT_N(0, "%s", "");
    char tiny[4] = {'x', 'x', 'x', 'x'};
    CHECK(osef::format_to(tiny, 0, "abc") == 3 && tiny[0] == 'x'); // n == 0 writes nothing
    CHECK(osef::format_to(nullptr, 0, "%d", 12345) == 5);
    CHECK(osef::format_to(tiny, 1, "abc") == 3 && tiny[0] == '\0');
    CHECK(osef::format_to(tiny, sizeof tiny, "%s", "abcdef") == 6 && std::strcmp(tiny, "abc") == 0);

    // osef::format for outputs larger than its internal stack buffer.
    {
        LeakScope leak("format string");
        String big = osef::format("%300d|%s", 7, "end");
        CHECK(big.size() == 304 && big[299] == '7' && big.ends_with("|end") && big[0] == ' ');
        String huge = osef::format("%-5000s.", "x");
        CHECK(huge.size() == 5001 && huge[0] == 'x' && huge[4999] == ' ' && huge.back() == '.');
        String empty = osef::format("%s", "");
        CHECK(empty.empty());
        String exact = osef::format("%255d", 1); // fits exactly with the NUL
        CHECK(exact.size() == 255 && exact.back() == '1');
        String over = osef::format("%256d", 1);
        CHECK(over.size() == 256 && over.back() == '1');
    }
}

// ---------------------------------------------------------------------------
// Algorithms and Random
// ---------------------------------------------------------------------------
static void test_algorithm() {
    LeakScope leak("algorithm");
    reset_counted();
    {
        osef::Random rng(1234);
        for (int size : {0, 1, 2, 3, 15, 16, 17, 31, 100, 1000, 5000}) {
            Vector<Pair<int, int>> v; // (key, original index) checks stability
            std::vector<std::pair<int, int>> ref;
            for (int i = 0; i < size; ++i) {
                const int k = static_cast<int>(rng.below(static_cast<uint64_t>(size / 4 + 1)));
                v.push_back(Pair<int, int>{k, i});
                ref.emplace_back(k, i);
            }
            osef::sort(v.begin(), v.end(), [](const Pair<int, int>& a, const Pair<int, int>& b) { return a.first < b.first; });
            std::stable_sort(ref.begin(), ref.end(),
                             [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
            bool same_order = true;
            for (int i = 0; i < size; ++i)
                same_order = same_order && v[i].first == ref[i].first && v[i].second == ref[i].second;
            ++g_checks;
            if (!same_order) {
                ++g_failures;
                std::printf("  FAIL stable sort of %d elements\n", size);
            }
        }
        Vector<int> desc{5, 1, 4, 2, 3};
        osef::sort(desc.begin(), desc.end(), [](int a, int b) { return a > b; });
        CHECK(desc == Vector<int>({5, 4, 3, 2, 1}));
        osef::sort(desc.begin(), desc.end());
        CHECK(desc == Vector<int>({1, 2, 3, 4, 5}));

        Vector<String> words{"pear", "apple", "fig", "banana", "cherry", "date", "elderberry", "grape",
                             "kiwi", "lemon", "mango", "nectarine", "orange", "papaya", "quince", "rhubarb",
                             "strawberry", "tangerine", "ugli", "vanilla", "watermelon"};
        osef::sort(words.begin(), words.end());
        bool sorted = true;
        for (size_t i = 1; i < words.size(); ++i) sorted = sorted && !(words[i] < words[i - 1]);
        CHECK(sorted && words.front() == "apple" && words.back() == "watermelon" && words.size() == 21);

        Vector<Counted> cs;
        for (int i = 0; i < 300; ++i) cs.emplace_back(static_cast<int>(rng.below(1000)));
        osef::sort(cs.begin(), cs.end());
        bool csorted = true;
        for (size_t i = 1; i < cs.size(); ++i) csorted = csorted && cs[i - 1].value <= cs[i].value;
        CHECK(csorted && Counted::alive == 300 && Counted::bad == 0);

        int raw[] = {3, 1, 2};
        osef::sort(raw, raw + 3);
        CHECK(raw[0] == 1 && raw[1] == 2 && raw[2] == 3);

        Vector<int> v{1, 2, 3, 4, 5, 6};
        auto even = [](int x) { return x % 2 == 0; };
        CHECK(*osef::find_if(v.begin(), v.end(), even) == 2);
        CHECK(osef::find_if(v.begin(), v.end(), [](int x) { return x > 10; }) == v.end());
        CHECK(osef::find(v.begin(), v.end(), 4) == v.begin() + 3);
        CHECK(osef::find(v.begin(), v.end(), 9) == v.end());
        CHECK(osef::any_of(v.begin(), v.end(), even));
        CHECK(!osef::all_of(v.begin(), v.end(), even));
        CHECK(osef::all_of(v.begin(), v.end(), [](int x) { return x > 0; }));
        CHECK(osef::none_of(v.begin(), v.end(), [](int x) { return x > 6; }));
        CHECK(osef::all_of(v.begin(), v.begin(), even) && !osef::any_of(v.begin(), v.begin(), even));
        CHECK(osef::count_if(v.begin(), v.end(), even) == 3);
        osef::reverse(v.begin(), v.end());
        CHECK(v == Vector<int>({6, 5, 4, 3, 2, 1}));
        Vector<int> odd{1, 2, 3};
        osef::reverse(odd.begin(), odd.end());
        CHECK(odd == Vector<int>({3, 2, 1}));
        Deque<int> dq{1, 2, 3};
        CHECK(osef::count_if(dq.begin(), dq.end(), [](int x) { return x > 1; }) == 2);
    }
    CHECK(counted_clean());
}

static void test_random() {
    osef::Random a(42), b(42), c(43);
    bool same_seq = true;
    bool differs = false;
    for (int i = 0; i < 100; ++i) {
        const uint64_t x = a.next();
        same_seq = same_seq && x == b.next();
        differs = differs || x != c.next();
    }
    CHECK(same_seq && differs);
    // splitmix64 reference value for seed 0.
    osef::Random z(0);
    CHECK(z.next() == 0xE220A8397B1DCDAFull);
    CHECK(a.below(0) == 0 && a.below(1) == 0);
    int buckets[10] = {};
    bool in_range = true;
    for (int i = 0; i < 100000; ++i) {
        const uint64_t r = a.below(10);
        in_range = in_range && r < 10;
        if (r < 10) ++buckets[r];
    }
    bool balanced = true;
    for (int bucket : buckets) balanced = balanced && bucket > 9000 && bucket < 11000;
    CHECK(in_range && balanced);
    bool big_ok = true;
    for (int i = 0; i < 1000; ++i) big_ok = big_ok && a.below(UINT64_MAX) < UINT64_MAX;
    CHECK(big_ok);
    bool between_ok = true;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t r = a.between(5, 8);
        between_ok = between_ok && r >= 5 && r <= 8;
    }
    CHECK(between_ok && a.between(3, 3) == 3 && a.between(9, 2) == 9);
    (void)a.between(0, UINT64_MAX);
}

// ---------------------------------------------------------------------------
// Regressions found by review
// ---------------------------------------------------------------------------
namespace osef {
// A type living in namespace osef, like every engine type.
struct ReviewEngineThing {
    int id;
    String name;
};
} // namespace osef

// Throws from its default/copy constructor once the budget runs out.
static int g_throw_budget = 0;
struct ThrowingElement {
    String payload{"a payload long enough to live on the heap"};
    ThrowingElement() {
        if (--g_throw_budget < 0) throw 42;
    }
    ThrowingElement(const ThrowingElement& o) : payload(o.payload) {
        if (--g_throw_budget < 0) throw 42;
    }
    ThrowingElement& operator=(const ThrowingElement&) = default;
};

static void test_review_regressions() {
    // 1. std algorithms / `using std::swap; swap(a, b)` on osef types used to
    //    be ambiguous (osef::swap<T> vs std::swap<T>). This is a compile test.
    {
        std::vector<Pair<int, int>> pairs{{3, 0}, {1, 1}, {2, 2}};
        std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        CHECK(pairs[0].first == 1 && pairs[1].first == 2 && pairs[2].first == 3);
        std::vector<osef::ReviewEngineThing> things{{2, "b"}, {1, "a"}};
        std::sort(things.begin(), things.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        CHECK(things[0].id == 1 && things[0].name == "a");
        std::vector<Vector<int>> vecs{{3}, {1}, {2}};
        std::sort(vecs.begin(), vecs.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
        CHECK(vecs[0][0] == 1 && vecs[2][0] == 3);
        std::vector<UniquePtr<int>> ups;
        ups.push_back(osef::make_unique<int>(2));
        ups.push_back(osef::make_unique<int>(1));
        std::sort(ups.begin(), ups.end(), [](const auto& a, const auto& b) { return *a < *b; });
        CHECK(*ups[0] == 1 && *ups[1] == 2);
        std::vector<Function<int()>> fns{[] { return 1; }, [] { return 2; }};
        std::reverse(fns.begin(), fns.end());
        CHECK(fns[0]() == 2);
        std::vector<StringView> views{"b", "a"};
        std::sort(views.begin(), views.end());
        CHECK(views[0] == "a");
        using std::swap;
        StringView v1("x"), v2("y");
        swap(v1, v2);
        CHECK(v1 == "y" && v2 == "x");
        Map<int, int> m1{{1, 1}}, m2;
        swap(m1, m2);
        CHECK(m1.empty() && m2.size() == 1);
        // osef::swap: member swap when available, moves otherwise.
        std::string s1 = "one", s2 = "two";
        osef::swap(s1, s2);
        CHECK(s1 == "two" && s2 == "one");
        String a = "short", b = "a string long enough for the heap";
        osef::swap(a, b);
        CHECK_STR(a, "a string long enough for the heap");
        CHECK_STR(b, "short");
        Deque<int> d1{1, 2}, d2{3};
        osef::swap(d1, d2);
        CHECK(d1.size() == 1 && d2.size() == 2);
        int x = 1, y = 2;
        osef::swap(x, y);
        CHECK(x == 2 && y == 1);
        static_assert(noexcept(osef::swap(x, y)));
        static_assert(osef::detail::has_member_swap<String> && !osef::detail::has_member_swap<StringView>);
    }

    // 2. A throwing element constructor inside a container constructor used
    //    to leak the buffer and the elements built so far.
    {
        LeakScope leak("container ctor exception");
        g_throw_budget = 2;
        bool threw = false;
        try {
            Vector<ThrowingElement> v(5);
        } catch (int) {
            threw = true;
        }
        CHECK(threw);
        g_throw_budget = 100;
        ThrowingElement proto;
        Vector<ThrowingElement> src(4);
        Deque<ThrowingElement> dsrc;
        for (int i = 0; i < 4; ++i) dsrc.emplace_back();
        ThrowingElement arr[4];
        Map<int, ThrowingElement> msrc;
        for (int i = 0; i < 4; ++i) msrc[i];
        int throws = 0;
        g_throw_budget = 2;
        try { Vector<ThrowingElement> v(4, proto); } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Vector<ThrowingElement> v(src); } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Vector<ThrowingElement> v(arr, arr + 4); } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Vector<ThrowingElement> v{proto, proto, proto, proto}; } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Deque<ThrowingElement> d(dsrc); } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Deque<ThrowingElement> d{proto, proto, proto, proto}; } catch (int) { ++throws; }
        g_throw_budget = 2;
        try { Map<int, ThrowingElement> m(msrc); } catch (int) { ++throws; }
        CHECK(throws == 7);
        g_throw_budget = 0;
    }

    // 3. SharedPtr(U*) / reset(U*) must delete p if the control block
    //    allocation throws (as std::shared_ptr does).
    {
        LeakScope leak("SharedPtr control block bad_alloc");
        reset_counted();
        bool threw = false;
        try {
            Counted* raw = new Counted(1);
            g_fail_alloc_in = 1;
            SharedPtr<Counted> p(raw);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            SharedPtr<Counted> p = osef::make_shared<Counted>(2);
            Counted* raw = new Counted(3);
            g_fail_alloc_in = 1;
            p.reset(raw);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw);
        g_fail_alloc_in = 0;
        CHECK(counted_clean());
    }

    // 4. String::reserve(npos) used to allocate npos + 1 == 0 bytes and then
    //    write past the end of the buffer.
    {
        LeakScope leak("String::reserve(npos)");
        String s("hello");
        CHECK_ASSERTS(s.reserve(String::npos));
        CHECK_STR(s, "hello");
    }
}

int main() {
    std::printf("osef core tests\n");
    test_base();
    test_string_view();
    test_string_basics();
    test_string_modify();
    test_string_search_compare();
    test_string_concat();
    test_string_numbers();
    test_vector_basics();
    test_vector_nontrivial();
    test_vector_nested();
    test_map();
    test_deque();
    test_function();
    test_unique_ptr();
    test_shared_ptr();
    test_format_integers();
    test_format_other();
    test_algorithm();
    test_random();
    test_review_regressions();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
